# Parental protection v2 — C++ integration

Contract: `mentor-parental-v2.0`; age policy: `age-10-15-v1`.
Base: `devin/1791040873-android-ui-detection`, commit
`8c0ef9bd3a35dd0371bfc5df29772a6d8cca149a`, not main.

## Supported path and defaults

The implemented live path is the **PC ONNX monitor**, retaining K230's split
bridge/inspector architecture, ADB, scrcpy display/audio capture and existing
diagnostics. Enforcement is an explicit opt-in; ordinary ONNX analysis stays
warning-only. No MediaProjection, new model weights, screenshots or media uploads
are introduced. HOME is not force-stop.

```sh
python3 tools/setup_onnxruntime.py
cmake -S . -B build-v2-onnx -G Ninja -DK230_TARGET=pc \
  -DK230_ENABLE_ONNX=ON -DONNXRUNTIME_ROOT="$PWD/output/onnxruntime"
ninja -C build-v2-onnx
./build-v2-onnx/apps/k230-monitor --server-jar /path/to/scrcpy-server-v4.0 \
  --ui-model models/android-ui-yolov8n.onnx \
  --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx \
  --protection-v2 --verified-pts-clock
```

`--verified-pts-clock` is an operator assertion, **not an automatic clock proof**.
Before enabling it on each device/scrcpy combination, compare original scrcpy
PTS to Android `System.nanoTime()/1000`, including restart and suspend; require
the documented future/stale bounds. An unverified device must remain diagnostic.
Native Android `bound` acceptance is required as a second gate. The C++ clock
estimate uses monotonic elapsed time from the latest phone state, never host
wall time or substituted frame PTS.

The combined monitor requires both real models, one ONNX thread per inference
worker, verified clock opt-in, valid native age/profile, capability, accessibility
and verified screen. Legacy threshold overrides/layout input are rejected in
this mode. The old verdict consumer logs locally only and does not compete with
v2 for the companion socket. Standalone bridge/inspector reject `--protection-v2`:
cross-core control adapters are not implemented. Host little/big builds check
compilation only; SDK DATAFIFO/IPCMSG, KPU/VDEC and hardware behavior remain
unproven. There is no heuristic enforcement fallback.

## Evidence and episode lifecycle

- Each batch evaluates all eligible Image/BackgroundImage/Video regions before
  choosing the maximum stage. Stage 1 retains all independently proven rectangles;
  stages 2/3 transmit one decisive proof. Sexy never contributes to risk.
- Age 10–12 uses E thresholds .60/.80/.90; age 13–15 uses .70/.85/.95.
  Invalid or rolled-back profiles do not replace the last valid profile.
- Explicit cover/shield: at least three observations spanning 400ms. Exit:
  at least five spanning 1s, each E at exit threshold and Porn >= .60.
  Hentai-dominant is H>P and P<.60, requires H>.60 and five observations
  spanning 1s, and can never reach HOME. Chains retain at most 32 real samples.
- Visual tracks use class, one-to-one IoU>=.70, center movement<=5% diagonal,
  side changes<=10%, deterministic tie breaks and ambiguity resets. Missing,
  invalid, masked, discontinuous and changed-identity observations cannot
  inherit proof. Duplicate/decreasing PTS cannot add evidence.
- Policy cadence selection is at most 10Hz; capture and audio continue separately.
  Scheduled pending-frame eviction/decoder gaps invalidate evidence. A failing
  independent crop prevents Safe but does not erase another valid positive crop.
  Empty/partial analysis is never Safe. Legacy diagnostic Safe semantics remain
  unchanged; v2 Safe is a narrowly scoped batch result, never a release command.
- To avoid covering a currently qualifying Porn chain before HOME is provable,
  lower actions may be held until that track's exit chain first observation +2s.
  The deadline never slides; a broken chain or identity starts a fresh deadline.
  Five observations spaced at the maximum 500ms gap can complete the proof;
  at 10Hz HOME remains eligible at 1s. Even a competing Hentai shield cannot
  preempt an active exit candidate. At the
  deadline, the strongest **currently proven** action wins, not an invented
  fifth observation. A risk dip releases the hold immediately. Gaps invalidate
  the chain; stale/failed evidence cannot be executed merely because a timer ran.
- Submitted masks are conservative until ACK/state confirmation. Covered pixels
  never advance evidence, Safe does not remove protection, and no revealing
  overlay cycle is used to escalate. A successful correlated execution ACK is
  required before counting an executed episode. Failed/unknown ACKs are degraded,
  not success. An explicit failed/rejected ACK with executed_stage=0 removes
  only that revision's optimistic mask/stage and resets evidence for fresh proof.
  Earlier executed/unknown revisions and authoritative native protection remain;
  missing ACKs/disconnects never remove conservative masks. Same-episode execution
  is deduplicated for repetition; count only
  distinct successful cover/shield episodes in the strict rolling 60s window,
  with younger=2/older=3 cap at shield. Profile changes reset counters.
- Releases are native verified navigation/new-content/guardian grants, correlated
  to event/revision/previous token. C++ never sends a release because its classifier
  sees the shield. Disconnect drops pending commands without replaying them and
  retains conservative protection knowledge. No automatic retry is performed;
  an uncertain HOME is never resent as a new execution. All simultaneous cover
  crops share one decision/revision/ACK. If the complete set exceeds the bounded
  line or crop budget, reject the whole decision; never send partial chunks.

## Wire, queues and ownership

The companion greets `K` followed by v2 hello+LF. K-only legacy companions,
missing greetings and unsupported hellos fail bounded negotiation; there is no
v1 action fallback. Each connect/greeting/read assembly/send is bounded to 500ms;
idle at a line boundary is allowed. Dispatch runs off the media thread.

`common/src/companion_v2.cpp` is the normative structural and semantic validator.
It rejects duplicate keys, unknown fields, invalid UTF-8/JSON, BOM/CR/NUL/LF in
payloads, non-integer dimensions/ages, decimal-string overflow, invalid probability
sums, unsupported transforms and mismatched proof/identity. Maximum wire line
including LF is 16384 bytes, depth 10, object/array members 32, tokens 4096.

`protocol/v2/companion.schema.json` documents the exact seven message shapes;
cross-field time/probability/geometry/evidence predicates additionally require the
native validator. `fixtures/` contains seven synthetic complete wire records and
40 executable age-policy traces; `manifest.json` hashes their exact bytes. Regenerate
with `python3 tools/generate_protection_fixtures.py`; verify without changes with
`python3 tools/generate_protection_fixtures.py --check`. C++ owns these assets;
final integration must reconcile byte parity with the Mentor equivalents, not
silently assert it has already been tested.

The in-process control queue has 32 non-evicting, size-validated entries.
The optional binary control envelope is `J`, version=2, kind (decision=1,
state=2, ack=3, bind=4, bound=5, released=6, hello=7), flags=0, uint32 big-endian
payload length, followed by JSON without LF. It is separate from unchanged
24-byte legacy Verdict IPC. No SDK control adapter is claimed.

Coordinates are half-open frame pixels, transformed with floor/ceil corners.
Live v2 supports unmirrored default scrcpy display 0, rotation_cw=0 and a full
display viewport. Already-rotated scrcpy frames must match native display rotation;
aspect tolerance is 1.5%. Custom crops/insets/mirror/rotation are rejected, not
guessed. Evidence must be after screen valid_from, end at command PTS, maintain
<=500ms gaps, and predate applicable overlay masks. Commands expire at PTS+750ms;
future allowance is 50ms, verified screen sampling freshness is 500ms. Revalidate
these gates before both enqueue and socket send; at most ten decisions/second.

## Security and release limits

The native service must verify shell/root peer UID before greeting and independently
validate actions, masks, signatures, authenticated pair membership and pinned
signing/encryption keys. Incident construction, strict Keystore-protected HPKE,
signature verification and ciphertext-only outbox/cloud are Mentor-owned. C++
sends allowlisted metadata only, not media or sensitive view text. No mandatory
Play Integrity/App Check rollout gate is added; attestation may be optional additional
protection, and existing configured controls must not be weakened.

Local protection does not depend on cloud health, but still requires USB/K230,
native accessibility permission and functioning models/IPC. Real Android execution,
device clock provenance, cross-repository schema parity and actual robot-versus-adult
classifier accuracy remain **unverified** until integration/device/labeled tests.
Compiling or loading model files does not establish classification accuracy.

## Verification

```sh
cmake -S . -B build-v2 -G Ninja -DK230_TARGET=pc
ninja -C build-v2
ctest --test-dir build-v2 --output-on-failure
ctest --test-dir build-v2-onnx --output-on-failure
cmake -S . -B build-v2-little -G Ninja -DK230_TARGET=little -DK230_BUILD_TESTS=OFF
ninja -C build-v2-little
cmake -S . -B build-v2-big -G Ninja -DK230_TARGET=big -DK230_BUILD_TESTS=OFF
ninja -C build-v2-big
python3 tools/generate_protection_fixtures.py --check
git diff --check
```

Tests include real loopback hello/bind/state/decision/ACK, K-only bounded shutdown,
HOME as the first action, competing Hentai/Porn crops, timing/route/reset/repetition
edges, parser bounds, coordinate/expiry/mask checks, conservative reconnect and
diagnostic regressions. Synthetic probabilities are not media accuracy fixtures.

nlohmann/json is vendored at 3.11.3 (MIT, January 2024) and verified by CMake SHA256
`9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`.
Existing ONNX/model hashes and dependencies are retained.
