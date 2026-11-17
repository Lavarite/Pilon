#!/usr/bin/env python3
"""Offline PP-OCRv5 text detection and recognition, with measured word boxes."""
import contextlib
import json
import os
from pathlib import Path
import sys


class OpenVINOSession:
    """Run an OCR stage on a detected Intel device, retaining its CPU fallback."""
    def __init__(self, core, path, devices, fallback, cache):
        self.core, self.path, self.devices = core, path, list(devices)
        self.fallback, self.cache = fallback, cache
        self.request, self.shape = None, None

    def __call__(self, inputs):
        while self.devices:
            device = self.devices[0]
            try:
                if self.request is None:
                    model = self.core.read_model(self.path)
                    # Detection tiles have a fixed raster size. The NPU needs
                    # static shapes; recognition batches remain dynamic on GPU.
                    if device.startswith("NPU"):
                        model.reshape(list(inputs.shape))
                        self.shape = inputs.shape
                    config = {"PERFORMANCE_HINT": "LATENCY", "CACHE_DIR": str(self.cache)}
                    if device == "CPU":
                        config["INFERENCE_NUM_THREADS"] = 2
                    self.request = self.core.compile_model(model, device, config).create_infer_request()
                    print(f"Handwriting {self.path.name}: OpenVINO {device}", file=sys.stderr, flush=True)
                if self.shape is not None and self.shape != inputs.shape:
                    raise ValueError("NPU input shape changed")
                self.request.infer([inputs])
                return self.request.get_output_tensor().data.copy()
            except Exception as error:
                print(f"Handwriting {device} unavailable: {error}", file=sys.stderr, flush=True)
                self.devices.pop(0)
                self.request, self.shape = None, None
        return self.fallback(inputs)


def create_engine(root):
    from rapidocr import RapidOCR, OCRVersion, ModelType, LangRec
    import onnxruntime as ort

    device = os.environ.get("PILON_NOTES_OCR_DEVICE", "auto").lower()
    if device not in ("auto", "cpu", "cuda", "gpu", "npu"):
        raise ValueError("PILON_NOTES_OCR_DEVICE must be auto, cpu, cuda, gpu or npu")
    cuda = device in ("auto", "cuda") and "CUDAExecutionProvider" in ort.get_available_providers()
    if cuda and hasattr(ort, "preload_dlls"):
        try:
            ort.preload_dlls(directory="")
        except (OSError, RuntimeError) as error:
            print(f"Handwriting CUDA unavailable: {error}", file=sys.stderr, flush=True)
            cuda = False
    params = {
        "Det.model_path": str(root / "ch_PP-OCRv5_det_server.onnx"),
        "Det.ocr_version": OCRVersion.PPOCRV5, "Det.model_type": ModelType.SERVER,
        "Rec.model_path": str(root / "en_PP-OCRv5_rec_mobile.onnx"),
        "Rec.ocr_version": OCRVersion.PPOCRV5, "Rec.model_type": ModelType.MOBILE,
        "Rec.lang_type": LangRec.EN,
        "Cls.model_path": str(root / "ch_ppocr_mobile_v2.0_cls_mobile.onnx"),
        "Global.use_cls": False, "Global.log_level": "error",
        "Global.max_side_len": 768,
        "EngineConfig.onnxruntime.intra_op_num_threads": 2,
        "EngineConfig.onnxruntime.inter_op_num_threads": 1,
        "EngineConfig.onnxruntime.use_cuda": cuda,
        "EngineConfig.onnxruntime.cuda_ep_cfg.cudnn_conv_algo_search": "HEURISTIC",
    }
    try:
        engine = RapidOCR(params=params)
    except Exception:
        if not cuda:
            raise
        params["EngineConfig.onnxruntime.use_cuda"] = False
        engine = RapidOCR(params=params)

    core = None
    available = []
    if device in ("auto", "gpu", "npu"):
        try:
            from openvino import Core
            core = Core()
            available = core.available_devices
        except (ImportError, RuntimeError):
            pass
    cache = root.parent.parent / "runtime" / "handwriting" / "openvino-cache"
    for stage, name in ((engine.text_det, "ch_PP-OCRv5_det_server.onnx"),
                        (engine.text_rec, "en_PP-OCRv5_rec_mobile.onnx")):
        providers = stage.session.session.get_providers()
        if "CUDAExecutionProvider" in providers:
            print(f"Handwriting {name}: CUDA", file=sys.stderr, flush=True)
            continue
        devices = []
        if device in ("auto", "npu") and stage is engine.text_det:
            devices += [d for d in available if d.startswith("NPU")]
        devices += [d for d in available if d.startswith("GPU")]
        if "CPU" in available:
            devices.append("CPU")
        if devices:
            cache.mkdir(parents=True, exist_ok=True)
            stage.session = OpenVINOSession(core, root / name, devices, stage.session, cache)
        else:
            print(f"Handwriting {name}: CPU", file=sys.stderr, flush=True)
    return engine


def recognize(engine, tile):
    result = engine(tile["path"], use_cls=False, return_word_box=True,
                    return_single_char_box=True)
    lines = []
    if result.boxes is None:
        return lines

    def bounds(box):
        import numpy as np
        box = np.asarray(box)
        scale = tile["scale"]
        left, top = box.min(axis=0)
        right, bottom = box.max(axis=0)
        return dict(x=float(left) / scale + tile["x"], y=float(top) / scale + tile["y"],
                    width=float(right-left) / scale, height=float(bottom-top) / scale)

    def utf16_length(text):
        return len(text.encode("utf-16-le")) // 2

    for i, (box, text, score) in enumerate(zip(result.boxes, result.txts, result.scores)):
        region = bounds(box)
        # Overlap supplies context, but only one tile owns each detection.
        cx, cy = region["x"] + region["width"] / 2, region["y"] + region["height"] / 2
        if not (tile["coreX"] <= cx < tile["coreX"] + tile["coreSize"]
                and tile["coreY"] <= cy < tile["coreY"] + tile["coreSize"]):
            continue
        words, offset = [], 0
        word_line = result.word_results[i] if result.word_results and i < len(result.word_results) else []
        for token, confidence, word_box in word_line:
            start = text.find(token, offset)
            if start < 0:
                continue
            words.append(dict(bounds(word_box), start=utf16_length(text[:start]),
                              length=utf16_length(token)))
            offset = start + len(token)
        lines.append(dict(region, text=text, words=words, score=float(score)))
    return lines


def main():
    # Third-party logs must never enter the JSON protocol on stdout.
    with contextlib.redirect_stdout(sys.stderr):
        root = Path(sys.argv[1])
        engine = create_engine(root)
    for request in sys.stdin:
        try:
            tiles = []
            for tile in json.loads(request)["lines"]:
                with contextlib.redirect_stdout(sys.stderr):
                    lines = recognize(engine, tile)
                tiles.append(lines)
                print(json.dumps({"progress": len(tiles)}), flush=True)
            print(json.dumps({"tiles": tiles}), flush=True)
        except Exception as error:
            print(json.dumps({"error": str(error)}), flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(json.dumps({"error": str(error)}), flush=True)
        sys.exit(1)
