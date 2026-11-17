#!/usr/bin/env python3
"""Exercise the packaged media engines with owned, prerecorded device fixtures."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[4]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default=str(Path.home() / "Android/Sdk/platform-tools/adb"))
    parser.add_argument("--runtime", type=Path, default=ROOT / "builds/android/runtime/arm64-v8a")
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--sample", type=Path, required=True)
    args = parser.parse_args()
    adb = [args.adb, "-d"]
    run_id = uuid.uuid4().hex
    remote = "/data/local/tmp/pilon-media-probe-" + run_id
    proof = ROOT / "build/android/media-runtime/proof" / run_id
    proof.mkdir(parents=True)

    def command(*values, timeout=60, check=True):
        result = subprocess.run(adb + list(values), capture_output=True, timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f"Media probe failed: {values}\n{result.stderr.decode(errors='replace')}")
        return result

    def shell(*values, **kwargs):
        return command("shell", shlex.join([str(value) for value in values]), **kwargs)

    def fetch(name):
        return command("exec-out", "cat", remote + "/" + name).stdout

    def audio_duration(name):
        with wave.open(io.BytesIO(fetch(name))) as audio:
            assert audio.getnchannels() == 1 and audio.getframerate() == 16000
            assert audio.getsampwidth() == 2
            return audio.getnframes() / audio.getframerate()

    processes = []
    try:
        assert command("get-state").stdout.strip() == b"device"
        shell("mkdir", remote)
        for name in ("ffmpeg", "ffprobe", "whisper-cli"):
            command("push", str(args.runtime / ("libpilon_" + name + ".so")), remote + "/" + name)
            shell("chmod", "700", remote + "/" + name)
        whisper_libraries = sorted(args.runtime.glob("libggml*.so")) + list(args.runtime.glob("libwhisper.so"))
        for library in whisper_libraries:
            command("push", str(library), remote + "/" + library.name)
        if whisper_libraries:
            standard = Path.home() / "Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
            command("push", str(standard), remote + "/libc++_shared.so")
        command("push", str(args.model), remote + "/model.bin")
        command("push", str(args.sample), remote + "/speech.wav")
        initial = hashlib.sha256(args.sample.read_bytes()).hexdigest()
        ffmpeg = remote + "/ffmpeg"
        ffprobe = remote + "/ffprobe"
        whisper = remote + "/whisper-cli"
        versions = {
            "ffmpeg": shell(ffmpeg, "-version").stdout.decode().splitlines()[0],
            "ffprobe": shell(ffprobe, "-version").stdout.decode().splitlines()[0],
            "whisper": shell(whisper, "--help").stderr.decode().splitlines()[:2],
        }
        # The app converts arbitrary audio inputs to mono PCM and segments long recordings.
        shell(ffmpeg, "-nostdin", "-v", "error", "-i", remote + "/speech.wav",
              "-c:a", "aac", "-b:a", "64k", remote + "/field notes.m4a")
        shell(ffmpeg, "-nostdin", "-v", "error", "-i", remote + "/field notes.m4a",
              "-vn", "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", remote + "/decoded.wav")
        assert audio_duration("decoded.wav") > 10
        command("push", str(ROOT / "addons/notes/tests/fixtures/android-ocr/printed.png"), remote + "/frame.png")
        shell(ffmpeg, "-nostdin", "-v", "error", "-loop", "1", "-i", remote + "/frame.png",
              "-i", remote + "/speech.wav", "-t", "1.2", "-vf", "scale=320:240", "-pix_fmt", "yuv420p",
              "-c:v", "mpeg4", "-c:a", "aac", remote + "/clip.mp4")
        duration = shell(ffprobe, "-v", "error", "-show_entries", "format=duration",
                         "-of", "default=nw=1:nk=1", remote + "/clip.mp4").stdout
        assert abs(float(duration) - 1.2) < .1, duration
        started = time.monotonic()
        recognized = shell(whisper, "-m", remote + "/model.bin", "-f", remote + "/decoded.wav",
                           "-t", "4", "-l", "en", "-ng", "-otxt", "-of", remote + "/recognized")
        elapsed = time.monotonic() - started
        (proof / "whisper.log").write_bytes(recognized.stdout + recognized.stderr)
        text = fetch("recognized.txt").decode().strip()
        assert "ask not what your country" in text.lower(), text
        long_audio = proof / "long.wav"
        with wave.open(str(long_audio), "wb") as audio:
            audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(16000)
            audio.writeframes(struct.pack("<h", 0) * 500000)
        command("push", str(long_audio), remote + "/long.wav")
        shell(ffmpeg, "-nostdin", "-v", "error", "-i", remote + "/long.wav",
              "-af", "asetnsamples=n=16000:p=0", "-f", "segment", "-segment_time", "30",
              "-c:a", "pcm_s16le", remote + "/part-%03d.wav")
        chunks = [audio_duration("part-000.wav"), audio_duration("part-001.wav")]
        assert all(0 < value <= 30 for value in chunks) and abs(sum(chunks) - 31.25) < .001
        shell(ffmpeg, "-nostdin", "-v", "error", "-i", remote + "/long.wav",
              "-af", "atempo=1.5", "-c:a", "pcm_s16le", remote + "/fast.wav")
        assert abs(audio_duration("fast.wav") - 31.25 / 1.5) < .1
        failed = shell(ffmpeg, "-nostdin", "-v", "error", "-i", remote + "/missing",
                       remote + "/invalid.wav", check=False)
        assert failed.returncode != 0
        pidfile = remote + "/cancel.pid"
        invocation = shlex.join([ffmpeg, "-nostdin", "-v", "error", "-stream_loop", "-1",
                                 "-i", remote + "/speech.wav", "-f", "null", "-"])
        process = subprocess.Popen(adb + ["shell", "-T", "echo $$ > " + shlex.quote(pidfile) + "; exec " + invocation],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        processes.append((process, pidfile, ffmpeg))
        for _ in range(20):
            pid = shell("cat", pidfile, check=False).stdout.decode().strip()
            if pid.isdigit():
                break
            time.sleep(.05)
        assert pid.isdigit() and shell("readlink", "/proc/" + pid + "/exe").stdout.decode().strip() == ffmpeg
        shell("kill", "-KILL", pid)
        process.communicate(timeout=5)
        assert process.returncode != 0
        shell(ffmpeg, "-nostdin", "-v", "error", "-i", remote + "/decoded.wav", "-f", "null", "-")
        assert hashlib.sha256(fetch("speech.wav")).hexdigest() == initial
        result = {"scope": "isolated adb-shell engines; installed-app recording is a separate gate",
                  "versions": versions, "transcription": text, "transcriptionSeconds": elapsed,
                  "segmentDurations": chunks, "videoDuration": float(duration), "cancelAndRecovery": True, "sourcePreserved": True}
        (proof / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("Media conversion, offline transcription, segmentation, speed, cancellation and recovery passed:", proof)
    finally:
        for process, pidfile, executable in processes:
            if process.poll() is None:
                pid = shell("cat", pidfile, check=False).stdout.decode().strip()
                if pid.isdigit() and shell("readlink", "/proc/" + pid + "/exe", check=False).stdout.decode().strip() == executable:
                    shell("kill", "-KILL", pid, check=False)
                process.communicate(timeout=5)
        assert re.fullmatch(r"/data/local/tmp/pilon-media-probe-[0-9a-f]{32}", remote)
        shell("rm", "-rf", remote)


if __name__ == "__main__":
    main()
