#!/usr/bin/env python3
"""Exercise the native Breeze JSON protocol with the pinned, real GGUF model."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time
import wave


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audio(path):
    with wave.open(str(path)) as stream:
        assert stream.getnchannels() == 1 and stream.getsampwidth() == 2
        frames = stream.getnframes()
        assert frames > 0 and stream.getframerate() == 24000
        return {"seconds": frames / stream.getframerate(), "sha256": digest(path)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("worker", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("ffmpeg", type=Path)
    parser.add_argument("proof", type=Path)
    args = parser.parse_args()
    args.proof.mkdir(parents=True, exist_ok=True)
    facts = []

    def record(value):
        facts.append(value)
        (args.proof / "smoke.json").write_text(json.dumps(facts, indent=2) + "\n")
        print(json.dumps(value), flush=True)

    subprocess.run([args.worker, "--self-test", args.ffmpeg, args.proof], check=True)
    log = (args.proof / "worker.log").open("ab")
    process = subprocess.Popen([args.worker, args.model, args.ffmpeg, "8"], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=log, text=True, start_new_session=True, bufsize=1)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)

    def receive():
        assert selector.select(1800), "Breeze did not answer within 30 minutes"
        line = process.stdout.readline()
        assert line, f"Breeze exited with {process.poll()}; see worker.log"
        return json.loads(line)

    def request(value):
        process.stdin.write(json.dumps(value) + "\n")
        process.stdin.flush()
        result = receive()
        if "error" in result:
            raise AssertionError(result)
        return result

    try:
        started = time.monotonic()
        assert receive() == {"ready": True}
        record({"loadSeconds": time.monotonic() - started})
        process.stdin.write("{bad json}\n"); process.stdin.flush()
        assert "error" in receive()
        record({"invalidJsonRecovered": True})
        reference = args.proof / "voice.wav"
        preview = args.proof / "preview.wav"
        started = time.monotonic()
        result = request({"text": "Hello, this is my voice.", "preview": True, "speed": 5,
            "instruction": "A calm adult man with a clear, natural voice.",
            "reference_text": "Hello, this is my voice.", "reference_output": str(reference),
            "output": str(preview)})
        expected = {"path": str(reference), "text": "Hello, this is my voice."}
        assert result == {"output": str(preview), "reference": expected}
        assert digest(reference) == digest(preview)
        sample = audio(reference)
        record({"newVoiceSeconds": time.monotonic() - started, "reference": sample, "exactPreview": True})
        for speed in (1, 10):
            output = args.proof / f"preview-speed-{speed}.wav"
            result = request({"preview": True, "speed": speed, "reference": expected, "output": str(output)})
            assert result["reference"] == expected and digest(reference) == sample["sha256"]
            adjusted = audio(output)
            assert abs(adjusted["seconds"] - sample["seconds"] / (0.5 + speed / 10)) < 0.08
            record({"speed": speed, **adjusted, "referenceUnchanged": True})
        output = args.proof / "reply.wav"
        started = time.monotonic()
        result = request({"text": "The saved voice speaks this reply.", "speed": 5,
            "reference": expected, "output": str(output)})
        assert result["reference"] == expected and digest(reference) == sample["sha256"]
        record({"replySeconds": time.monotonic() - started, "audio": audio(output), "referenceUnchanged": True})
        cancelled = args.proof / "cancelled.wav"
        cancelled.write_bytes(b"existing speech output")
        previous = digest(cancelled)
        process.stdin.write(json.dumps({"text": "Keep speaking until the user stops. " * 30,
            "speed": 5, "reference": expected, "output": str(cancelled)}) + "\n")
        process.stdin.flush()
        deadline = time.monotonic() + 30
        while not Path(str(cancelled) + ".render.wav.partial.wav").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert Path(str(cancelled) + ".render.wav.partial.wav").exists(), "The cancelled request never entered synthesis"
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=10)
        assert digest(cancelled) == previous and digest(reference) == sample["sha256"]
        record({"nativeActiveCancellation": True, "existingOutputPreserved": True, "referenceUnchanged": True})
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=10)
        selector.close()
        log.close()
        for name in ("cancelled.wav.render.wav.partial.wav", "cancelled.wav.render.wav", "cancelled.wav.tempo.wav"):
            (args.proof / name).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
