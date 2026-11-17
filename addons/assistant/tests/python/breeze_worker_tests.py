"""Verify saved speech speed without loading the TTS model."""
import array
import importlib.util
import math
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
import wave
from unittest.mock import Mock, MagicMock, patch

spec = importlib.util.spec_from_file_location("breeze_worker", Path(__file__).resolve().parents[4] / "addons/assistant/workers/breeze_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class RuntimeTests(unittest.TestCase):
    def test_cuda_oom_retries_same_request_once_on_cpu(self):
        class Oom(Exception): pass
        cuda = SimpleNamespace(OutOfMemoryError=Oom, empty_cache=Mock())
        for load_fails in (False, True):
            with self.subTest(load_fails=load_fails):
                gpu, cpu = Mock(), Mock()
                load = Mock(side_effect=[Oom("CUDA out of memory") if load_fails else gpu, cpu])
                process = Mock(side_effect=[{"output": "first.wav"}, {"output": "second.wav"}]
                               if load_fails else [Oom("CUDA out of memory"), {"output": "first.wav"}, {"output": "second.wav"}])
                results = []
                with patch.dict(sys.modules, {"torch": SimpleNamespace(cuda=cuda)}), \
                     patch.object(worker, "load_synthesizer", load), patch.object(worker, "process_request", process):
                    worker.serve("model", ['{"text":"first"}', '{"text":"second"}'], results.append)
                self.assertEqual(load.call_count, 2)
                self.assertEqual(load.call_args.args, ("model", "cpu"))
                self.assertEqual([r["output"] for r in results if "output" in r], ["first.wav", "second.wav"])
                self.assertFalse(any("error" in r for r in results))
                if not load_fails:
                    self.assertEqual(process.call_args_list[0].args[0], process.call_args_list[1].args[0])
                self.assertIs(process.call_args.args[1], cpu)

    def test_cpu_and_cuda_generate_with_the_saved_voice(self):
        for device in ("cpu", "cuda:0"):
            with self.subTest(device=device):
                model = Mock()
                model.config.codec_config.sampling_rate = 24000
                sample = Mock()
                sample.numel.return_value = 24000
                model.generate.return_value = [sample]
                tokenizer, audio_tokenizer = Mock(), Mock()
                runtime_api = SimpleNamespace(load_runtime=Mock(return_value=(tokenizer, model, audio_tokenizer)),
                    resolve_device=lambda: device, set_all_seeds=Mock(), update_generation_config_for_breeze=Mock())
                templates = SimpleNamespace(get_template=Mock(side_effect=lambda name: name),
                    prepare_inputs=Mock(return_value={"input_ids": "prepared"}))
                fast = MagicMock()
                fast.FastBreezeStreamingRuntime.return_value.iter_audio_chunks.return_value = [SimpleNamespace(audio="chunk")]
                sf = MagicMock()
                modules = {"torch": SimpleNamespace(inference_mode=lambda: lambda function: function),
                    "soundfile": sf, "breeze_infer.runtime": runtime_api, "breeze_infer.templates": templates,
                    "models.fast_streaming": fast}
                with patch.dict(sys.modules, modules):
                    synthesize = worker.load_synthesizer("model")
                    reference = {"path": "/saved/voice.wav", "text": "Saved voice."}
                    synthesize("Hello.", "Speak clearly.", Path("reply.wav"), reference)
                    utterance = templates.prepare_inputs.call_args.args[3][0]
                    self.assertEqual(utterance["ref_audio_path"], reference["path"])
                    self.assertEqual(utterance["ref_text"], reference["text"])
                    templates.get_template.assert_called_with("ref_edit_tata")
                    if device == "cpu":
                        model.float.assert_called_once()
                        fast.FastBreezeStreamingRuntime.assert_not_called()
                        model.generate.assert_called_once_with(input_ids="prepared", output_audio=True,
                            audio_tokenizer=audio_tokenizer, max_new_tokens=1500,
                            repetition_penalty=1.0, return_dict_in_generate=False)
                        sf.write.assert_called_once()
                        self.assertEqual(sf.write.call_args.args[2], 24000)
                        self.assertEqual(sf.write.call_args.kwargs["subtype"], "PCM_16")
                        model.generate.return_value = []
                        with self.assertRaisesRegex(ValueError, "no audio"):
                            synthesize("Hello.", "Speak clearly.", Path("empty.wav"), reference)
                    else:
                        model.generate.assert_not_called()
                        model.float.assert_not_called()
                        fast.FastBreezeStreamingRuntime.assert_called_once()
                        sf.SoundFile.return_value.__enter__.return_value.write.assert_called_once_with("chunk")


class SpeechSpeedTests(unittest.TestCase):
    def test_speed_changes_duration_without_changing_pitch(self):
        sample_rate = 16000
        samples = array.array("h", (int(12000 * math.sin(2 * math.pi * 440 * i / sample_rate))
                                    for i in range(sample_rate * 2)))
        with tempfile.TemporaryDirectory() as folder:
            for speed, rate in [(1, 0.6), (5, 1.0), (10, 1.5)]:
                with self.subTest(speed=speed):
                    path = Path(folder) / f"speed-{speed}.wav"
                    with wave.open(str(path), "wb") as audio:
                        audio.setparams((1, 2, sample_rate, 0, "NONE", "not compressed"))
                        audio.writeframes(samples.tobytes())
                    original = path.read_bytes()
                    worker.adjust_speed(path, speed)
                    if speed == 5:
                        self.assertEqual(path.read_bytes(), original)
                    with wave.open(str(path), "rb") as audio:
                        self.assertEqual(audio.getframerate(), sample_rate)
                        duration = audio.getnframes() / sample_rate
                        result = array.array("h", audio.readframes(audio.getnframes()))
                    self.assertAlmostEqual(duration, 2 / rate, delta=0.06)
                    crossings = sum(a < 0 <= b for a, b in zip(result, result[1:]))
                    self.assertAlmostEqual(crossings / duration, 440, delta=5)
                    self.assertFalse(path.with_suffix(".tempo.wav").exists())

    def test_invalid_speed_leaves_audio_untouched(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "speech.wav"
            path.write_bytes(b"unchanged")
            for speed in (0, 11):
                with self.assertRaises(ValueError):
                    worker.adjust_speed(path, speed)
                self.assertEqual(path.read_bytes(), b"unchanged")


class VoiceIdentityTests(unittest.TestCase):
    def test_long_reply_keeps_all_text_audio_and_saved_voice(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            reference = {"path": str(root / "voice.wav"), "text": "Saved voice."}
            Path(reference["path"]).write_bytes(b"reference")
            text = "A complete sentence with evidence. " * 200 + "The final conclusion."
            calls = []

            def synthesize(segment, instruction, output, speaker):
                calls.append(segment)
                self.assertEqual(speaker, reference)
                with wave.open(str(output), "wb") as audio:
                    audio.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                    audio.writeframes(b"\1\0" * 160)

            output = root / "reply.wav"
            worker.process_request({"text": text, "reference": reference, "output": str(output)}, synthesize)
            self.assertEqual("".join(calls), text)
            with wave.open(str(output), "rb") as audio:
                self.assertEqual(audio.getnframes(), 160 * len(calls))
            self.assertEqual(Path(reference["path"]).read_bytes(), b"reference")
            self.assertFalse(list(root.glob("pilon-speech-*")))

    def test_unbroken_text_is_not_dropped(self):
        text = "文字" * 2000
        self.assertEqual("".join(worker.speech_segments(text)), text)

    def test_preview_and_replies_reuse_unmodified_reference(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            calls = []

            def synthesize(text, instruction, output, reference):
                calls.append((text, instruction, reference))
                with wave.open(str(output), "wb") as audio:
                    audio.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                    audio.writeframes(b"\1\0" * 16000)

            request = {"text": "Preview sentence.", "instruction": "British accent.", "speed": 10,
                       "preview": True, "output": str(root / "preview.wav"),
                       "reference_output": str(root / "voice.wav"), "reference_text": "Preview sentence."}
            first = worker.process_request(request, synthesize)
            reference = first["reference"]
            original = Path(reference["path"]).read_bytes()
            self.assertEqual(len(calls), 1)
            self.assertIsNone(calls[0][2])
            self.assertEqual(calls[0][1], "British accent.")
            request.update(reference=reference, speed=1, output=str(root / "slow.wav"))
            worker.process_request(request, synthesize)
            self.assertEqual(len(calls), 1)
            request["preview"] = False
            for index, text in enumerate(("First reply.", "Completely different words.")):
                request.update(text=text, output=str(root / f"reply-{index}.wav"))
                result = worker.process_request(request, synthesize)
                self.assertEqual(calls[-1], (text, "Speak clearly and naturally.", reference))
                self.assertEqual(result["reference"], reference)
                self.assertEqual(Path(reference["path"]).read_bytes(), original)

    def test_missing_reference_does_not_redesign_voice(self):
        synthesize = Mock()
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError, "saved voice sample is missing"):
                worker.process_request({"output": str(Path(folder) / "reply.wav"),
                    "reference": {"path": str(Path(folder) / "missing.wav"), "text": "Sample."}}, synthesize)
        synthesize.assert_not_called()


if __name__ == "__main__":
    unittest.main()
