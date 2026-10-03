# Android UI YOLOv8 Nano

- Source: https://huggingface.co/yasirfaizahmed/android_ui_detection_yolov8
- Revision: `f692e68a3d7c92d51b5e94bba2fc13196a280e18`
- Checkpoint SHA-256: `3f39b0d64832801072ac099ba370afe113aea32a360d4de8e24960b017b6d782`
- Export: Ultralytics 8.4.18, PyTorch 2.5.1 CPU, ONNX 1.17.0, opset 13.
- FP32 input `[1,3,640,640]`; output `[1,25,8400]`: decoded `cx,cy,w,h` followed by 21 class probabilities, without an objectness column or embedded NMS.
- Preprocessing: decoded YUV → RGB bytes; bilinear half-pixel resize preserving aspect ratio; centered 114 padding; NCHW float32 `[0,1]`.
- Media classes: `0:BackgroundImage` and `9:Image`. There is no separate Video class; detection of video player regions must be evaluated on representative recordings.
- Runtime: ONNX Runtime C++, PC/WSL only. No Python, PyTorch or Ultralytics is needed for execution. K230 nncase/KPU conversion and quantization have not been verified.

The model card declares Apache-2.0. The Ultralytics exporter writes an AGPL-3.0 license notice into the ONNX metadata; the export preserves that notice. Check the upstream model and Ultralytics terms for your distribution. This repository does not relicense their artifacts.

## Recreate the ONNX artifact

Use Python 3.12 in an isolated environment. For CPU-only PyTorch:

```bash
python3.12 -m venv output/ui-export-venv
output/ui-export-venv/bin/pip install torch==2.5.1 --index-url https://download.pytorch.org/whl/cpu
output/ui-export-venv/bin/pip install -r tools/ui-export-requirements.txt
output/ui-export-venv/bin/python tools/export_ui_detector.py
```

The script verifies the checkpoint checksum and full class order before export, adds the runtime contract metadata, runs the ONNX checker, and compares raw PyTorch and ONNX predictions on black and random tensors. These checks verify conversion, not real-world detection accuracy.

The input is static and square. Evaluation with Ultralytics' automatic rectangular input can produce different boxes; validate the actual exported model with the C++ preprocessing before generalizing results.
