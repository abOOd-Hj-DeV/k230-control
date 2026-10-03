import argparse
import hashlib
from pathlib import Path
import urllib.request

import numpy as np
import onnx
import onnxruntime as ort
import torch
from ultralytics import YOLO


REVISION = "f692e68a3d7c92d51b5e94bba2fc13196a280e18"
SHA256 = "3f39b0d64832801072ac099ba370afe113aea32a360d4de8e24960b017b6d782"
CLASSES = [
    "BackgroundImage", "Bottom_Navigation", "Card", "CheckBox", "Checkbox",
    "CheckedTextView", "Drawer", "EditText", "Icon", "Image", "Map", "Modal",
    "Multi_Tab", "PageIndicator", "Remember", "Spinner", "Switch", "Text",
    "TextButton", "Toolbar", "UpperTaskBar",
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, default=Path("output/ui-source"))
    parser.add_argument("--output", type=Path, default=Path("models/android-ui-yolov8n.onnx"))
    args = parser.parse_args()
    args.source_dir.mkdir(parents=True, exist_ok=True)
    checkpoint = args.source_dir / "best.pt"
    if not checkpoint.exists():
        url = (
            "https://huggingface.co/yasirfaizahmed/android_ui_detection_yolov8/resolve/"
            f"{REVISION}/best.pt"
        )
        with urllib.request.urlopen(url, timeout=60) as response:
            checkpoint.write_bytes(response.read())
    if hashlib.sha256(checkpoint.read_bytes()).hexdigest() != SHA256:
        raise ValueError("UI checkpoint checksum mismatch")
    torch.set_num_threads(1)
    model = YOLO(str(checkpoint))
    if [model.names[i] for i in range(len(model.names))] != CLASSES:
        raise ValueError("UI class order mismatch")
    exported = model.export(format="onnx", imgsz=640, opset=13, simplify=False,
                            dynamic=False, nms=False, half=False, device="cpu")
    graph = onnx.load(exported)
    metadata = {prop.key: prop.value for prop in graph.metadata_props}
    metadata.update({
        "k230.ui.media_classes": "0:BackgroundImage,9:Image",
        "k230.ui.input": "rgb_nchw_float32_0_1_letterbox_114_half_pixel",
        "k230.ui.source_revision": REVISION,
        "k230.ui.source_sha256": SHA256,
    })
    onnx.helper.set_model_props(graph, metadata)
    onnx.checker.check_model(graph)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(graph, str(args.output))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    session = ort.InferenceSession(str(args.output), options, providers=["CPUExecutionProvider"])
    if session.get_inputs()[0].shape != [1, 3, 640, 640] or session.get_outputs()[0].shape != [1, 25, 8400]:
        raise ValueError("UI tensor shape mismatch")
    rng = np.random.default_rng(230)
    maximum_error = 0.0
    for pixels in (np.zeros((1, 3, 640, 640), dtype=np.float32),
                   rng.random((1, 3, 640, 640), dtype=np.float32)):
        with torch.inference_mode():
            expected = model.model(torch.from_numpy(pixels))[0].numpy()
        actual = session.run(None, {session.get_inputs()[0].name: pixels})[0]
        np.testing.assert_allclose(actual[:, :4], expected[:, :4], atol=0.005, rtol=0.00001)
        np.testing.assert_allclose(actual[:, 4:], expected[:, 4:], atol=0.00001, rtol=0.0001)
        maximum_error = max(maximum_error, float(np.max(np.abs(actual - expected))))
    print(f"PyTorch/ONNX maximum absolute error: {maximum_error}")
    print(f"ONNX SHA-256: {hashlib.sha256(args.output.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
