import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("handwriting_worker", Path(__file__).resolve().parents[4] / "addons/notes/workers/handwriting_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class HandwritingTests(unittest.TestCase):
    def session(self, devices):
        core = Mock()
        request = Mock()
        request.get_output_tensor.return_value.data = [1, 2, 3]
        core.compile_model.return_value.create_infer_request.return_value = request
        fallback = Mock(return_value=[9])
        session = worker.OpenVINOSession(core, Path("det.onnx"), devices, fallback, Path("cache"))
        return session, core, request, fallback

    def test_npu_uses_static_input_and_reuses_compiled_model(self):
        session, core, request, fallback = self.session(["NPU", "GPU"])
        inputs = SimpleNamespace(shape=(1, 3, 736, 736))
        self.assertEqual(session(inputs), [1, 2, 3])
        self.assertEqual(session(inputs), [1, 2, 3])
        core.read_model.return_value.reshape.assert_called_once_with([1, 3, 736, 736])
        core.compile_model.assert_called_once()
        self.assertEqual(request.infer.call_count, 2)
        fallback.assert_not_called()

    def test_missing_npu_driver_falls_back_to_gpu_once(self):
        session, core, request, fallback = self.session(["NPU", "GPU"])
        compiled = core.compile_model.return_value
        core.compile_model.side_effect = [RuntimeError("driver unavailable"), compiled]
        inputs = SimpleNamespace(shape=(1, 3, 736, 736))
        self.assertEqual(session(inputs), [1, 2, 3])
        self.assertEqual(session(inputs), [1, 2, 3])
        self.assertEqual([c.args[1] for c in core.compile_model.call_args_list], ["NPU", "GPU"])
        fallback.assert_not_called()

    def test_runtime_device_failure_falls_back_to_cpu(self):
        session, core, request, fallback = self.session(["GPU"])
        request.infer.side_effect = RuntimeError("device lost")
        inputs = SimpleNamespace(shape=(1, 3, 736, 736))
        self.assertEqual(session(inputs), [9])
        self.assertEqual(session(inputs), [9])
        request.infer.assert_called_once()
        self.assertEqual(fallback.call_count, 2)

    def test_changed_npu_shape_uses_dynamic_gpu(self):
        session, core, request, fallback = self.session(["NPU", "GPU"])
        session(SimpleNamespace(shape=(1, 3, 736, 736)))
        session(SimpleNamespace(shape=(1, 3, 736, 512)))
        self.assertEqual([c.args[1] for c in core.compile_model.call_args_list], ["NPU", "GPU"])
        self.assertIsNone(session.shape)

    def test_cuda_profile_enables_available_provider_and_preloads_libraries(self):
        enum = SimpleNamespace(PPOCRV5=5, SERVER="server", MOBILE="mobile", EN="en")
        ort = SimpleNamespace(get_available_providers=lambda: ["CUDAExecutionProvider", "CPUExecutionProvider"], preload_dlls=Mock())
        session = SimpleNamespace(session=SimpleNamespace(get_providers=lambda: ["CUDAExecutionProvider", "CPUExecutionProvider"]))
        engine = SimpleNamespace(text_det=SimpleNamespace(session=session), text_rec=SimpleNamespace(session=session))
        rapidocr = SimpleNamespace(RapidOCR=Mock(return_value=engine), OCRVersion=enum, ModelType=enum, LangRec=enum)
        with patch.dict("sys.modules", rapidocr=rapidocr, onnxruntime=ort), patch.dict("os.environ", PILON_NOTES_OCR_DEVICE="cuda"):
            self.assertIs(worker.create_engine(Path("models")), engine)
        self.assertTrue(rapidocr.RapidOCR.call_args.kwargs["params"]["EngineConfig.onnxruntime.use_cuda"])
        ort.preload_dlls.assert_called_once_with(directory="")


if __name__ == "__main__":
    unittest.main()
