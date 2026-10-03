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
  --protection-v2
```

`--verified-pts-clock` is an optional diagnostic assertion, never an execution gate.
Initial `bind.capture_pts_us=null` negotiates metadata only. While unbound, the
decoder observer sends untouched source PTS in repeated bind records, at most
one every 500ms, eight non-null probes and three seconds per session. The native
peer replies `bound.status=pending` with null error until its clock verifier accepts.
All samples must strictly increase, be no more than 50ms in the future or 750ms
old against native `System.nanoTime()/1000` at receipt. Require at least three
samples spanning at least 1s in both source and receive clocks, with span difference
at most 100ms. Reject violations permanently for that session as `clock_unverified`;
do not silently reset the window, rebase PTS or relax bounds on a slow device.
Bind metadata/session/stream must remain unchanged throughout negotiation.
Missing/short evidence stays unbound; an accepted response without enough local
probes also cannot enable C++. `CaptureClockVerifier` is the C++ normative reference
for the independently implemented native gate, not evidence from a real device.
The C++ clock
estimate uses monotonic elapsed time from the latest phone state, never host
wall time or substituted frame PTS.

The combined monitor requires both real models, one ONNX thread per inference
worker, native clock-probe acceptance, valid native age/profile, capability, accessibility,
ready Keystore, paired membership
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
  missing ACKs/disconnects alone never remove conservative masks. Same-episode execution
  is deduplicated for repetition.
  Native zero-state/reconnect clear masks, not successful episodes in that same
  boot's rolling window; a new phone boot resets the clock-domain history.
  Count distinct successful cover/shield episodes in the strict rolling 60s phone
  monotonic window, excluding the current event's revisions. PTS is used only
  for evidence spans. Younger=2/older=3 cap at shield. Profile changes reset counters.
  Requested stages cannot decrease within an event, even after explicit failure.
- Releases are native verified navigation/new-content/guardian grants, correlated
  to event/revision/previous token. C++ never sends a release because its classifier
  sees the shield. Disconnect drops pending commands without replaying them and
  retains conservative protection knowledge. Screen/app/content changes block
  promotion of the old event until verified release. Reusing a token with changed
  screen identity disables binding rather than keeping a stale execution gate.
  No decisions are possible between hello/bind and the first accepted state.
  That first authoritative state replaces prior-session execution knowledge:
  stage=0 clears the old event/masks; positive stage restores event, native masks,
  `action_revision` and `target_screen_token`. The latter is the overlay's original
  target, not necessarily the current screen token. Both recovery fields are null
  iff inactive; active revisions are positive decimal strings and target tokens UUIDs.
  A restored cover revision N can promote an independently proven Porn region to
  HOME at N+1; a current-screen token change still blocks old-target promotion.
  Within the same session, two verified native stage=0 snapshots at least 100ms
  apart can reconcile absent protection, including a lost release packet; both
  must follow all old commands' PTS+750ms TTL+1500ms HOME bound.
  Still-live commands, stale snapshots and positive native state prevent
  reconciliation; expired missing ACKs remain counted unknown, never success.
  No classifier result clears native overlays. Evidence and local
  repetition knowledge reset on this recovery. No automatic retry is performed;
  an uncertain HOME is never resent as a new execution. Simultaneous cover crops
  share one complete decision. An oversized stage-1 batch is rejected atomically;
  no chunks or truncated proofs can apply only a subset of that batch. Naturally
  separate complete decisions from later newly analyzed frames may add covers
  under the same event with increasing revisions and native atomic union. The
  eight-crop event budget remains. Queue rejection creates no unsent claim.
  ACK timestamps and actual display rectangles must correlate to the requested
  proof before successful execution is counted. Receipt alone is not execution.

### Capture clock source and evidence limits

Protection pins server version 4.0 and verifies the actual local jar before model
startup and again immediately before ADB push, using existing PC libavutil SHA256.
Expected digest (the official [v4.0 release asset](https://api.github.com/repos/Genymobile/scrcpy/releases/tags/v4.0)
`scrcpy-server-v4.0`) is
`84924bd564a1eb6089c872c7521f968058977f91f5ff02514a8c74aff3210f3a`.
The bundled jar's META-INF/version-control-info.textproto identifies upstream revision
`048c747cc10b3a7fe2d313d8ad2ed41d26c90ae7`. Missing, non-regular, oversized,
modified or wrong-version servers cannot enable protection. Diagnostic capture
retains custom server support. This check is PC-only and does not claim K230 SDK
source verification. It assumes trusted local host/ADB shell/root transport;
already-privileged peers can tamper after verification and are outside this proof.

[scrcpy 4.0 Streamer](https://github.com/Genymobile/scrcpy/blob/v4.0/server/src/main/java/com/genymobile/scrcpy/device/Streamer.java)
forwards MediaCodec BufferInfo.presentationTimeUs unchanged; C++ demux and FFmpeg
decode preserve it. No host receipt timestamp is substituted. Android's
[SystemClock documentation](https://developer.android.com/reference/android/os/SystemClock)
places System.nanoTime in the uptime/monotonic clock family, excluding deep sleep;
elapsedRealtimeNanos includes sleep and is not the promised clock. AOSP
[Surface queueBuffer](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/gui/Surface.cpp)
uses SYSTEM_TIME_MONOTONIC for automatic timestamps, but MediaCodec/surface producer
behavior is still device-dependent. Suspend, codec restart and backlog need device
regressions. The probes establish numerical clock compatibility, not cryptographic
source provenance: falsified/rebased samples that equal the phone clock cannot be
distinguished numerically. Trusted raw source plumbing and independent native
validation remain required. No supported-device clock acceptance is claimed here.

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
