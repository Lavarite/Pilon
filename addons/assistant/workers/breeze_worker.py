"""Private stdin/stdout worker for Pilon's local Breeze voice-design runtime."""
import contextlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import wave

os.umask(0o077)
protocol = sys.stdout


def emit(value):
    protocol.write(json.dumps(value) + "\n")
    protocol.flush()


def adjust_speed(output: Path, speed: int):
    if not 1 <= speed <= 10:
        raise ValueError("Voice speed must be between 1 and 10.")
    if speed == 5:
        return
    adjusted = output.with_suffix(".tempo.wav")
    try:
        # Keep voice pitch unchanged; 5 is normal, 1 is 0.6x and 10 is 1.5x.
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(output),
                        "-af", f"atempo={0.5 + speed / 10:.1f}", "-c:a", "pcm_s16le", str(adjusted)],
                       check=True, capture_output=True)
        adjusted.replace(output)
    finally:
        adjusted.unlink(missing_ok=True)


def speech_segments(text):
    # Fit each utterance in Breeze's audio context, without discarding the rest
    # of a long answer. Every segment is conditioned on the same saved speaker.
    while len(text) > 200:
        end = max(text.rfind(mark, 0, 200) for mark in (". ", "! ", "? ", "\n"))
        if end < 0:
            end = text.rfind(" ", 0, 200)
        end = end + 1 if end >= 0 else 200
        yield text[:end]
        text = text[end:]
    if text:
        yield text


def synthesize_reply(text, output, reference, synthesize):
    if len(text) <= 200:
        synthesize(text, "Speak clearly and naturally.", output, reference)
        return
    with tempfile.TemporaryDirectory(prefix="pilon-speech-", dir=output.parent) as folder:
        with wave.open(str(output), "wb") as combined:
            audio_format = None
            for index, segment in enumerate(speech_segments(text)):
                part = Path(folder) / f"{index}.wav"
                synthesize(segment, "Speak clearly and naturally.", part, reference)
                with wave.open(str(part), "rb") as audio:
                    current = (audio.getnchannels(), audio.getsampwidth(), audio.getframerate())
                    if audio_format is None:
                        audio_format = current
                        combined.setparams((*current, 0, "NONE", "not compressed"))
                    elif current != audio_format:
                        raise ValueError("Speech segments returned different audio formats.")
                    combined.writeframes(audio.readframes(audio.getnframes()))
                part.unlink()


def process_request(request, synthesize):
    speed = int(request.get("speed", 5))
    if not 1 <= speed <= 10:
        raise ValueError("Voice speed must be between 1 and 10.")
    output = Path(request["output"])
    reference = request.get("reference")
    if reference is not None:
        if not reference.get("text") or not Path(reference.get("path", "")).is_file():
            raise ValueError("The saved voice sample is missing. Preview and save a voice again.")
    else:
        reference = {"path": request["reference_output"], "text": request["reference_text"]}
        synthesize(reference["text"], request["instruction"][:2000], Path(reference["path"]), None)
    if request.get("preview", False):
        # Preview exactly the chosen sample, including when only speed changes.
        shutil.copyfile(reference["path"], output)
    else:
        synthesize_reply(request["text"], output, reference, synthesize)
    adjust_speed(output, speed)
    return {"output": str(output), "reference": reference}


def load_synthesizer(model_path, device_override=None):
    import soundfile as sf
    import torch
    from breeze_infer.runtime import load_runtime, resolve_device, set_all_seeds, update_generation_config_for_breeze
    from breeze_infer.templates import get_template, prepare_inputs

    device = device_override or resolve_device()
    tokenizer, model, audio_tokenizer = load_runtime(
        Path(model_path), device=device, attn_implementation="eager")
    if device == "cpu":
        model.float()
    update_generation_config_for_breeze(model)
    runtime = None
    if device.startswith("cuda"):
        from models.fast_streaming import FastBreezeStreamingRuntime, FastStreamingConfig
        runtime = FastBreezeStreamingRuntime(model, audio_tokenizer,
            FastStreamingConfig(max_new_tokens=1500, max_seq_len=2048, fast_all=False,
                                repetition_penalty=1.1), tokenizer=tokenizer)
    sample_rate = int(model.config.codec_config.sampling_rate)

    @torch.inference_mode()
    def synthesize(text, instruction, output, reference):
        set_all_seeds(42)
        utterance = {"id": "pilon", "text": text, "instruction": instruction, "speaker": "S0"}
        template = "tts_instruction"
        if reference is not None:
            utterance.update(ref_audio_path=reference["path"], ref_text=reference["text"])
            template = "ref_edit_tata"
        inputs = prepare_inputs(tokenizer, audio_tokenizer, model, [utterance],
            get_template(template), guidance_scale=4.0 if reference is None else 1.0,
            guidance_scale_ref=None, guidance_scale_ins=None)
        if runtime is None:
            # Standard generation mixes text prompt IDs with audio IDs. Its
            # generic repetition processor cannot index those in the audio logits.
            audio = model.generate(**inputs, output_audio=True, audio_tokenizer=audio_tokenizer,
                max_new_tokens=1500, repetition_penalty=1.0, return_dict_in_generate=False)
            if not audio or audio[0].numel() == 0:
                raise ValueError("Breeze returned no audio.")
            sf.write(output, audio[0].detach().float().cpu().numpy().reshape(-1),
                     sample_rate, subtype="PCM_16")
            return
        with sf.SoundFile(output, mode="w", samplerate=sample_rate,
                          channels=1, subtype="PCM_16") as audio:
            for chunk in runtime.iter_audio_chunks(inputs, request_id="pilon", seed=42):
                audio.write(chunk.audio)

    return synthesize


def serve(model_path, lines, emit_result):
    import gc
    import torch
    synthesize = None
    device = None
    # Retry exactly once on CPU, including an OOM during initial model loading.
    try:
        synthesize = load_synthesizer(model_path)
    except torch.cuda.OutOfMemoryError:
        device = "cpu"
    if device == "cpu":
        gc.collect()
        torch.cuda.empty_cache()
        synthesize = load_synthesizer(model_path, "cpu")
    emit_result({"ready": True})
    for line in lines:
        retry = False
        try:
            request = json.loads(line)
            emit_result(process_request(request, synthesize))
        except torch.cuda.OutOfMemoryError as error:
            if device == "cpu":
                emit_result({"error": str(error)})
            else:
                retry = True
        except Exception as error:
            emit_result({"error": str(error)})
        if retry:
            # Outside the except block: release traceback references to CUDA tensors.
            synthesize = None
            gc.collect()
            torch.cuda.empty_cache()
            device = "cpu"
            emit_result({"status": "Speech is switching to CPU because GPU memory is full."})
            try:
                synthesize = load_synthesizer(model_path, "cpu")
                emit_result(process_request(request, synthesize))
            except Exception as error:
                emit_result({"error": str(error)})
                return


def main():
    sys.path.insert(0, sys.argv[2])
    serve(sys.argv[1], sys.stdin, emit)


if __name__ == "__main__":
    # Third-party progress logs never enter the protocol.
    with contextlib.redirect_stdout(sys.stderr):
        try:
            main()
        except Exception as error:
            emit({"error": str(error)})
            raise
