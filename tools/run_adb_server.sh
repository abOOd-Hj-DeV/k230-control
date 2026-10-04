#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${K230_SERVER_BINARY:-"$root/bin/k230-server"}
if [ -z "${K230_SERVER_BINARY:-}" ] && [ ! -x "$binary" ]; then
  binary="$root/build/apps/k230-server"
fi
adb=${ADB:-adb}
if [ -z "${ADB:-}" ] && [ -x "$root/bin/adb" ]; then
  adb="$root/bin/adb"
fi
export LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$binary" --adb "$adb" --server-jar "$root/assets/scrcpy-server" \
  --ui-model "$root/models/android-ui-yolov8n.onnx" \
  --nsfwjs-model "$root/models/nsfwjs-mobilenet-v2.onnx" \
  --protection-v2 "$@"
