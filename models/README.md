# NSFWJS MobileNetV2

`nsfwjs-mobilenet-v2.onnx` is derived from the MobileNetV2 Layers model shipped
with [NSFWJS v4.2.1](https://github.com/infinitered/nsfwjs/tree/836ff6b3e8bcbceb27f18aa43d5866f80cfe068c/models/mobilenet_v2).
Upstream repository license: MIT, copied in `NSFWJS-LICENSE`.

Source commit: `836ff6b3e8bcbceb27f18aa43d5866f80cfe068c`.

SHA-256:

| Artifact | SHA-256 |
|---|---|
| TFJS `model.json` | `11846416217e68bf1eb7b0e651bcfd305973566453c63275bbd16766ab089979` |
| TFJS `group1-shard1of1` | `8e7dddbb16acacc1bf1601b1b8a761e730ff934b7f2d7771312b2f000e5f5f13` |
| Exported ONNX | `63b0624e6ade8cf8e80e7533798572973c7c98603912f4961dfa91d44dadbfa5` |

## Contract

- Input: `image`, FP32 RGB, NHWC `[1,224,224,3]`, values `[0,1]`.
- Resize: stretch the whole frame with bilinear interpolation, `alignCorners=true`,
  matching NSFWJS; no crop, letterbox, channel reversal or mean/std normalization.
- Decoded I420/NV12 is converted using the same BT.601 limited-range integer
  conversion as this project's PNG snapshots, including clamping to 8-bit RGB
  **before** normalization and resize.
- Output: FP32 Softmax `[1,5]`, ordered
  `Drawing, Hentai, Neutral, Porn, Sexy`.
- ONNX opset 13; inference uses ONNX Runtime CPU 1.22.1.
- The shipped TFJS weight file already uses uint8 *storage* quantization.
  The official TFJS loader dequantizes it; export keeps FP32 computation and
  adds no further quantization. ONNX is approximately 10.3 MB.

`Scores::nsfwjs` retains all five outputs; `nudity = Porn + Hentai` supplies
the existing temporal policy. The analyzer rotates through the full frame and
up to eight overlapping strips so content embedded in a tall phone interface
is not reduced to a small part of the model input. Temporal confirmation is
tracked independently for each region. `Sexy` is diagnostic during this evaluation.
Only one inference is run per incoming frame. At 10 FPS with nine regions,
each region is revisited every 0.9 seconds; three positive observations may
take up to 2.7 seconds after content appears. Near-square frames and frames
whose short dimension is below 224 pixels use only the full-frame input.
NSFWJS mode caps escalations at `Warn`. Violence and profanity remain zero;
audio RMS is diagnostic. None of these scores is a calibrated guarantee.

The analyzer validates tensor shapes/types, label order/preprocessing metadata,
and finite normalized probabilities. Model/inference failures stop evaluation
with a nonzero status rather than emitting safe results or falling back.

## Reproduce the conversion (optional)

Regular users only need the committed ONNX file and C++ runtime. To re-export:

```bash
python3.11 -m venv output/nsfwjs-export-venv
output/nsfwjs-export-venv/bin/pip install -r tools/nsfwjs-export-requirements.txt
TF_CPP_MIN_LOG_LEVEL=2 CUDA_VISIBLE_DEVICES='' \
  output/nsfwjs-export-venv/bin/python tools/export_nsfwjs.py
npm install --prefix output/nsfwjs-reference @tensorflow/tfjs@4.22.0
node tools/check_nsfwjs.mjs output/nsfwjs-source output/nsfwjs-validation output/nsfwjs-reference
```

Use Python 3.10/3.11. The exporter downloads source artifacts from the pinned
commit and verifies both checksums, loads them using the official
`tensorflowjs.converters.load_keras_model`, exports with `tf2onnx`, checks the
ONNX graph, and compares Keras/ONNX output on 17 deterministic benign RGB
fixtures. The Node check loads the **original TFJS topology and weights** with
TensorFlow.js CPU and compares the same five outputs to ONNX.

Verified here:

- Maximum Keras ↔ ONNX absolute difference: `1.55e-6`.
- Maximum original TFJS ↔ ONNX absolute difference: `1.32e-6`.
- C++ tests independently cover RGB normalization, corner-aligned bilinear
  resize, odd-size I420/NV12, invalid frames/models, real-model reference
  scores, PTS and temporal confirmation, and inference errors.
- A 12-second synthetic H.264+PCM replay retained all 120 analyzed frames,
  120 PNGs and continuous WAV output, with no video gaps.

These fixtures verify conversion and integration, **not detection accuracy**.
Gameplay, sports, swimming and cartoons still need representative evaluation
for false alerts. No phone recording is bundled. Conversion to nncase/KPU,
quantization accuracy, memory and inference timing on K230 are unverified.
