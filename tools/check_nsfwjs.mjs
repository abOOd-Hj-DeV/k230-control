import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const [sourceDir, validationDir, dependencyDir] = process.argv.slice(2);
if (!sourceDir || !validationDir || !dependencyDir) {
  throw new Error("Usage: node tools/check_nsfwjs.mjs SOURCE_DIR VALIDATION_DIR TFJS_DEPENDENCY_DIR");
}
const require = createRequire(path.resolve(dependencyDir, "package.json"));
const tf = require("@tensorflow/tfjs");
await tf.setBackend("cpu");
await tf.ready();
const json = JSON.parse(fs.readFileSync(path.join(sourceDir, "model.json"), "utf8"));
const manifest = json.weightsManifest;
const bytes = Buffer.concat(manifest.flatMap(group =>
  group.paths.map(file => fs.readFileSync(path.join(sourceDir, file)))));
const model = await tf.loadLayersModel({
  load: async () => ({
    modelTopology: json.modelTopology,
    weightSpecs: manifest.flatMap(group => group.weights),
    weightData: bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  }),
});
const reportPath = path.join(validationDir, "conversion.json");
const report = JSON.parse(fs.readFileSync(reportPath, "utf8"));
let maxError = 0;
for (const fixture of report.fixtures) {
  const rgb = fs.readFileSync(path.join(validationDir, fixture.file));
  const probabilities = tf.tidy(() => {
    const input = tf.tensor3d(new Uint8Array(rgb), [224, 224, 3], "int32")
      .toFloat().div(255).reshape([1, 224, 224, 3]);
    return Array.from(model.predict(input).dataSync());
  });
  probabilities.forEach((score, index) => {
    const error = Math.abs(score - fixture.onnx[index]);
    if (error > 1e-4 + 1e-4 * Math.abs(score)) {
      throw new Error(`Conversion mismatch: ${fixture.file}, class ${index}, ${error}`);
    }
    maxError = Math.max(maxError, error);
  });
}
report.tfjs_onnx_max_absolute_error = maxError;
fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + "\n");
model.dispose();
console.log(JSON.stringify({ fixtures: report.fixtures.length, tfjs_onnx_max_absolute_error: maxError }));
