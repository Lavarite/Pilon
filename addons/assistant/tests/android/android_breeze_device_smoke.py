#!/usr/bin/env python3
"""Run the native Breeze worker on the authorized USB phone, outside app data."""
import hashlib
import json
from pathlib import Path
import selectors
import shlex
import subprocess
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[4]
ADB = Path.home() / "Android/Sdk/platform-tools/adb"
PROOF = ROOT / "build/android/breeze/device-proof"
MODEL = ROOT / "build/android/breeze/models/breeze-tts-2-q8_0.gguf"
EXPECTED = "b17b89b1c21107c44e8d556ab0a856d7acd06bdd0c914e68e0e57f6e33f555a3"


def adb(*args, **kwargs):
    return subprocess.run([str(ADB), "-d", *map(str, args)], check=True, capture_output=True, **kwargs)


def main():
    PROOF.mkdir(parents=True, exist_ok=True)
    with MODEL.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == EXPECTED
    remote = "/data/local/tmp/pilon-breeze-probe-" + uuid.uuid4().hex
    facts = {"scope": "ARM64 production speech worker under isolated shell UID; app UI remains a separate acceptance gate",
        "threads": 4, "modelSha256": EXPECTED, "requests": []}
    process = None
    selector = selectors.DefaultSelector()
    pid = None
    peak_rss, minimum_available = 0, 1 << 60

    def save():
        (PROOF / "smoke.json").write_text(json.dumps(facts, indent=2) + "\n")

    def memory():
        nonlocal peak_rss, minimum_available
        status = adb("shell", f"cat /proc/{pid}/status 2>/dev/null; cat /proc/meminfo", text=True).stdout
        values = {}
        for line in status.splitlines():
            if line.startswith(("VmRSS:", "VmHWM:", "MemAvailable:")):
                name, value, _ = line.split()
                values[name[:-1]] = int(value)
        peak_rss = max(peak_rss, values.get("VmRSS", 0), values.get("VmHWM", 0))
        minimum_available = min(minimum_available, values.get("MemAvailable", minimum_available))
        return values

    def receive():
        deadline = time.monotonic() + 900
        while time.monotonic() < deadline:
            if selector.select(2):
                line = process.stdout.readline()
                assert line, f"Worker exited with {process.poll()}; see worker.log"
                return json.loads(line)
            state = memory()
            assert state.get("MemAvailable", 0) >= 650 * 1024, "Stopping isolated probe because available phone RAM is low"
        raise AssertionError("Native Breeze request exceeded 15 minutes")

    def request(name, value):
        started = time.monotonic()
        process.stdin.write(json.dumps(value) + "\n"); process.stdin.flush()
        result = receive()
        facts["requests"].append({"name": name, "seconds": time.monotonic() - started, "result": result, "memoryKiB": memory()})
        save()
        return result

    def sha(path):
        return adb("shell", "sha256sum", path, text=True).stdout.split()[0]

    def pull_audio(name):
        adb("pull", remote + "/" + name, PROOF / name)
        with wave.open(str(PROOF / name)) as stream:
            assert stream.getnchannels() == 1 and stream.getsampwidth() == 2 and stream.getframerate() == 24000
            assert stream.getnframes() > 0
            return stream.getnframes() / stream.getframerate()

    try:
        adb("shell", "mkdir", "-p", remote)
        runtime = ROOT / "builds/android/runtime/arm64-v8a"
        libraries = [runtime / name for name in ("libpilon_breeze.so", "libaudiocpp.so", "libpilon_ffmpeg.so")]
        libraries += [Path.home() / "Qt/6.12.0/android_arm64_v8a/lib/libQt6Core_arm64-v8a.so",
            Path.home() / "Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"]
        for file in libraries:
            rows = subprocess.check_output(["readelf", "-lW", file], text=True).splitlines()
            assert min(int(row.split()[-1], 16) for row in rows if row.strip().startswith("LOAD")) >= 16384
            adb("push", file, remote + "/" + file.name)
        started = time.monotonic()
        adb("push", MODEL, remote + "/model.gguf")
        assert sha(remote + "/model.gguf") == EXPECTED
        facts["modelPushAndVerifySeconds"] = time.monotonic() - started
        adb("shell", "chmod", "700", remote + "/libpilon_breeze.so", remote + "/libpilon_ffmpeg.so")
        prefix = f"env LD_LIBRARY_PATH={shlex.quote(remote)} {shlex.quote(remote + '/libpilon_breeze.so')}"
        checked = adb("shell", prefix + " --self-test " + shlex.quote(remote + "/libpilon_ffmpeg.so") + " " + shlex.quote(remote), text=True)
        facts["selfTest"] = json.loads(checked.stdout)
        command = "echo $$ > " + shlex.quote(remote + "/worker.pid") + "; exec " + prefix + " " + shlex.quote(remote + "/model.gguf") + " " + shlex.quote(remote + "/libpilon_ffmpeg.so") + " 4"
        log = (PROOF / "worker.log").open("w")
        started = time.monotonic()
        process = subprocess.Popen([str(ADB), "-d", "shell", "-T", "setsid sh -c " + shlex.quote(command)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)
        selector.register(process.stdout, selectors.EVENT_READ)
        for _ in range(100):
            result = subprocess.run([str(ADB), "-d", "shell", "cat", remote + "/worker.pid"], capture_output=True, text=True)
            if result.returncode == 0:
                pid = int(result.stdout.strip()); break
            time.sleep(0.05)
        assert pid
        assert receive() == {"ready": True}
        facts["loadSeconds"] = time.monotonic() - started
        output, reference = remote + "/preview.wav", remote + "/voice.wav"
        sample = {"path": reference, "text": "Hello, this is my voice."}
        result = request("new-voice", {"preview": True, "speed": 5,
            "instruction": "A calm adult man with a clear, natural voice.",
            "reference_text": sample["text"], "reference_output": reference, "output": output})
        assert result == {"output": output, "reference": sample}, result
        original = sha(reference)
        assert sha(output) == original
        seconds = pull_audio("voice.wav")
        facts["referenceSeconds"] = seconds
        for speed in (1, 10):
            name = f"preview-speed-{speed}.wav"
            result = request(f"speed-{speed}", {"preview": True, "speed": speed, "reference": sample, "output": remote + "/" + name})
            assert result["reference"] == sample and sha(reference) == original
            duration = pull_audio(name)
            assert abs(duration - seconds / (0.5 + speed / 10)) < 0.08
        result = request("saved-voice-reply", {"text": "The saved voice speaks this reply.", "speed": 5,
            "reference": sample, "output": remote + "/reply.wav"})
        assert result["reference"] == sample and sha(reference) == original
        facts["replySeconds"] = pull_audio("reply.wav")
        result = request("missing-reference", {"text": "Hello.", "reference": {"path": remote + "/missing.wav", "text": "Hello."}, "output": remote + "/missing-output.wav"})
        assert "error" in result
        result = request("error-recovery", {"preview": True, "speed": 5, "reference": sample, "output": remote + "/recovered.wav"})
        assert result["reference"] == sample and sha(remote + "/recovered.wav") == original
        adb("shell", "sh", "-c", shlex.quote("printf 'preserve existing output' > " + shlex.quote(remote + "/cancelled.wav")))
        before = sha(remote + "/cancelled.wav")
        process.stdin.write(json.dumps({"text": "Keep speaking until the user stops. " * 30, "speed": 5,
            "reference": sample, "output": remote + "/cancelled.wav"}) + "\n"); process.stdin.flush()
        partial = remote + "/cancelled.wav.render.wav.partial.wav"
        for _ in range(200):
            if subprocess.run([str(ADB), "-d", "shell", "test", "-f", partial]).returncode == 0: break
            time.sleep(0.05)
        else: raise AssertionError("Cancellation never reached native synthesis")
        adb("shell", f"kill -KILL -{pid}")
        process.wait(timeout=10)
        assert sha(remote + "/cancelled.wav") == before and sha(reference) == original
        pid = None
        facts["activeCancellationPreservedOutputAndReference"] = True
        facts["peakRssKiB"], facts["minimumAvailableKiB"] = peak_rss, minimum_available
    finally:
        if pid:
            subprocess.run([str(ADB), "-d", "shell", f"kill -KILL -{pid}"], capture_output=True)
        if process:
            if process.poll() is None: process.terminate()
            process.wait(timeout=10)
        selector.close()
        cleaned = adb("shell", "rm", "-rf", remote)
        facts["cleaned"] = cleaned.returncode == 0
        save()
    print(json.dumps(facts, indent=2))


if __name__ == "__main__":
    main()
