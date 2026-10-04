# Temporary Android → C++ server mode

This opt-in path runs on Linux x86-64, independently of ADB/scrcpy. Existing
USB operation remains available. The companion Android changes are required.

## Build and run

Install the development packages documented in the main README plus
`libssl-dev`. Use the existing ONNX SDK and committed model files:

```sh
cmake -S . -B build-server -DK230_TARGET=pc -DK230_ENABLE_ONNX=ON \
  -DONNXRUNTIME_ROOT="$PWD/output/onnxruntime"
cmake --build build-server -j4
```

Use a certificate for your server hostname, or create a temporary certificate
with a matching DNS/IP Subject Alternative Name. Keep the private key and
connection token outside the repository. Example for a test host:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 7 \
  -subj '/CN=server.example.com' -addext 'subjectAltName=DNS:server.example.com' \
  -keyout server.key -out server.crt
chmod 600 server.key
export K230_REMOTE_TOKEN="$(openssl rand -hex 32)"
openssl x509 -in server.crt -outform DER | openssl dgst -sha256
build-server/apps/k230-server --listen 0.0.0.0 --port 8443 \
  --cert server.crt --key server.key \
  --ui-model models/android-ui-yolov8n.onnx \
  --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx
```

Transfer the connection token securely to the child phone with the guardian
present. Enter `tls://server.example.com:8443`, that token, and the certificate
SHA-256 fingerprint in the child app's server card. A publicly trusted
certificate needs no manually supplied fingerprint. Complete genuine pairing,
the age profile and accessibility permissions first; these gates are retained.
Accept the disclosure and Android's full-display sharing consent, then open
another application. Mentor's own secure UI is not captured for analysis.

Use a firewall to restrict this temporary listener to the intended devices or
private network. The default bind address is loopback. This is not an HTTP
website, and no ADB port is exposed.

## Wire and execution

TLS 1.2 or newer protects a binary stream. Each record is `u8 type + u32be
length + payload`. Authentication is the first record (`type=0`), checked
against `K230_REMOTE_TOKEN` in constant time. Companion JSON is `type=1`,
bounded at 16 KiB. PNG records are `type=2`, bounded at 8 MiB:

```
u64be capture_pts_us; u32be width; u32be height; u64be content_epoch;
36 ASCII bytes screen_token; PNG bytes
```

The server returns transport heartbeat records (`type=3`, payload byte `1`)
for native state updates. Capture uses the unmodified ImageReader timestamp,
at up to four frames per second. Screen identity, exact full-display geometry,
age evidence, the 750 ms decision expiry, local clock regression, journal and
execution ACK checks remain active. MediaProjection binding additionally
requires locally observed capture timestamps. Expired decisions are rejected;
network latency is not hidden by increasing expiry or weakening validation.

YOLO and NSFWJS run on the server. The Android accessibility runtime executes
cover/shield/HOME and creates the encrypted guardian incident locally. The
server does not receive guardian decryption keys. Images are decoded and
analyzed in memory; no recorder or frame dump is enabled. It does not promise
OS memory forensics protection or eliminate provider-controlled swap/core
dumps: disable swap and core dumps on the deployment host if required.

## Limits of this initial version

- Screen images are visible to the analyzer server after TLS decryption.
- Audio streaming and the eight-second audio test remain on the USB path.
- New analysis stops without a network connection. Existing local shield
  retention is preserved; there is no on-phone model fallback in this mode.
- Rotation or a capture size mismatch stops sharing and requires fresh consent.
- FLAG_SECURE content cannot be inspected. A blank/no-region frame is not a
  claim of safety. An ordinary Android app cannot force-stop other apps.
- Four concurrent connections share serialized model inference. This is a
  temporary single-instance service, not a multi-tenant production platform.
- Per-device accounts, automatic reconnect, production Firebase provisioning,
  server deployment and real-phone enforcement are separate work.

## Short verification

`tools/check_remote_server.py` uses a real TLS connection and the actual
models to check authentication, native binding/state framing and benign PNG
ingestion. It does not substitute for an Android device capture/enforcement
test or demonstrate classifier accuracy.
