import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

import numpy as np
import onnx
import onnxruntime as ort
import tensorflow as tf
import tensorflowjs as tfjs
import tf2onnx


REVISION = "836ff6b3e8bcbceb27f18aa43d5866f80cfe068c"
FILES = {
    "model.json": "11846416217e68bf1eb7b0e651bcfd305973566453c63275bbd16766ab089979",
    "group1-shard1of1": "8e7dddbb16acacc1bf1601b1b8a761e730ff934b7f2d7771312b2f000e5f5f13",
}
CLASSES = ["Drawing", "Hentai", "Neutral", "Porn", "Sexy"]


def download_source(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, expected_hash in FILES.items():
        path = directory / name
        if not path.exists():
            url = f"https://raw.githubusercontent.com/infinitered/nsfwjs/{REVISION}/models/mobilenet_v2/{name}"
            with urllib.request.urlopen(url, timeout=60) as response:
                path.write_bytes(response.read())
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
            raise ValueError(f"Source checksum mismatch: {path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, default=Path("output/nsfwjs-source"))
    parser.add_argument("--output", type=Path, default=Path("models/nsfwjs-mobilenet-v2.onnx"))
    parser.add_argument("--validation-dir", type=Path, default=Path("output/nsfwjs-validation"))
    args = parser.parse_args()
    download_source(args.source_dir)
    tf.config.threading.set_intra_op_parallelism_threads(1)
    tf.config.threading.set_inter_op_parallelism_threads(1)
    model = tfjs.converters.load_keras_model(str(args.source_dir / "model.json"))
    signature = [tf.TensorSpec((1, 224, 224, 3), tf.float32, name="image")]
    converted, _ = tf2onnx.convert.from_keras(model, input_signature=signature, opset=13)
    onnx.helper.set_model_props(converted, {
        "nsfwjs.classes": ",".join(CLASSES),
        "nsfwjs.source_revision": REVISION,
        "nsfwjs.input": "rgb_nhwc_float32_0_1_align_corners",
    })
    onnx.checker.check_model(converted)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(converted, str(args.output))

    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    session = ort.InferenceSession(str(args.output), options, providers=["CPUExecutionProvider"])
    args.validation_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(230)
    images = [np.full((224, 224, 3), value, dtype=np.uint8) for value in (0, 127, 255)]
    images.extend(rng.integers(0, 256, (224, 224, 3), dtype=np.uint8) for _ in range(13))
    gradient = np.linspace(0, 255, 224).astype(np.uint8)
    images.append(np.broadcast_to(gradient[None, :, None], (224, 224, 3)).copy())
    fixtures = []
    max_error = 0.0
    for index, image in enumerate(images):
        pixels = image.astype(np.float32)[None] / 255.0
        expected = model(pixels, training=False).numpy()
        actual = session.run(None, {"image": pixels})[0]
        np.testing.assert_allclose(actual, expected, atol=1e-5, rtol=1e-4)
        max_error = max(max_error, float(np.max(np.abs(actual - expected))))
        image.tofile(args.validation_dir / f"{index}.rgb")
        fixtures.append({"file": f"{index}.rgb", "onnx": actual[0].tolist()})
    report = {
        "source_revision": REVISION,
        "classes": CLASSES,
        "fixtures": fixtures,
        "keras_onnx_max_absolute_error": max_error,
        "onnx_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
        "note": "Synthetic RGB conversion checks; not a classifier accuracy evaluation.",
    }
    (args.validation_dir / "conversion.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "fixtures"}, indent=2))


if __name__ == "__main__":
    main()
