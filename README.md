# k230-control

Parental-control pipeline built around a **01Studio CanMV K230 (2 GB)** sitting
between an Android phone and its owner:

```
┌──────────── Android phone ────────────┐
│ scrcpy-server  (H.264 + PCM, real PTS) │◄── USB / adb ─────┐
│ Companion app  (executes verdicts)     │◄── adb socket ────┤
└────────────────────────────────────────┘                   │
                                                             ▼
┌──────────── K230 little core (Linux) ── bridge/ ─────────────────────────┐
│ AdbController → ScrcpySession → ScrcpyDemuxer ──► compressed MediaPackets │
│ VerdictDispatcher ◄── verdicts                                            │
└─────────────┬───────────────────────────────────────────────▲─────────────┘
              │ DATAFIFO (media)                    IPCMSG    │ (verdicts)
┌─────────────▼───────────────────────────────────────────────┴─────────────┐
│ K230 big core (RT-Smart) ── inspector/                                    │
│ VDEC (hw decode) → SyncEngine (phone PTS) → Analyzer (KPU) → Policy      │
└───────────────────────────────────────────────────────────────────────────┘
```

Everything is C++17. The same sources build for three targets:

| `K230_TARGET` | builds                                   | decoder | analyzer  | transport         |
|---------------|------------------------------------------|---------|-----------|-------------------|
| `pc` (default)| bridge + inspector + `k230-monitor` + tests | FFmpeg  | heuristic / NSFWJS ONNX | in-process queues |
| `little`      | `k230-bridge` only                       | –       | –         | DATAFIFO / IPCMSG |
| `big`         | `k230-inspector` only                    | VDEC    | KPU       | DATAFIFO / IPCMSG |

The K230 backends (`DataFifo*`, `IpcMsg*`, `VdecDecoder`, `KpuAnalyzer`) are
compile-time stubs until they are wired to the SDK headers (see
[docs/K230_BRINGUP.ar.md](docs/K230_BRINGUP.ar.md)). Everything above them —
protocol, demuxing, PTS sync, policy — is final and covered by tests.

## Layout

```
common/     protocol + IPC shared by both cores
  scrcpy_demuxer   incremental parser for scrcpy 4.0 sockets (12-byte headers, session/config/key flags)
  media_packet     compressed packet with the phone's PTS preserved
  ipc/wire         24-byte DATAFIFO/IPCMSG framing; ipc/channel = bounded in-process queue (PC)
  recording        .k230rec capture format for offline replay
bridge/     little core: adb, scrcpy server lifecycle, socket readers, verdict → companion app
inspector/  big core: decoders, PcmRing, SyncEngine, Analyzer, Policy, pipeline
apps/       k230-monitor: both halves in one PC process (phone over USB)
tools/      k230-make-test-recording: synthetic H.264+PCM capture, no phone needed
tests/      GoogleTest, including scrcpy-bytes → verdict and NSFWJS inference checks
models/     pinned NSFWJS MobileNetV2 ONNX weights and upstream MIT license
assets/     scrcpy-server 4.0 (pushed to the phone)
```

## Build (PC)

```bash
sudo apt install cmake ninja-build pkg-config libavcodec-dev libavutil-dev libswscale-dev libgtest-dev
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

The bridge uses POSIX sockets/processes (it targets the little core's Linux),
so on Windows build inside **WSL2** (Ubuntu): run the commands above from the
WSL shell (`cd /mnt/c/...`). A phone can be passed into WSL2 with
[usbipd-win](https://github.com/dorssel/usbipd-win); the replay/test tools
need no phone. Do not compile single files with `g++ file.cpp` — CMake wires
the include paths and libraries.

Cross builds only need `-DK230_TARGET=little|big` plus a toolchain file
(`-DCMAKE_TOOLCHAIN_FILE=...`); FFmpeg and GoogleTest are not required there.

## تجربة NSFWJS على PC / WSL

### كشف مناطق الوسائط بصرياً باستخدام Android UI YOLOv8 Nano

للمسار الجديد، أضف `--ui-model models/android-ui-yolov8n.onnx` إلى أمر
التشغيل مع `--nsfwjs-model`. النموذج يحدد `Image` و`BackgroundImage` من
**الإطار نفسه**، ثم يحلل عاملان NSFWJS القصوص المؤهلة. هذا المسار لا يحتاج
Mentor أو تشغيل Accessibility، ولا ينتظر اكتمال نافذة الصوت. التحذير فقط
مفعّل كما في تجربة NSFWJS؛ يظل تسجيل الصوت والفيديو وتوقيت الهاتف كما هو.

بعد تجهيز ONNX Runtime والبناء بالأوامر أدناه، شغّل داخل WSL:

```bash
mkdir -p output
./build/apps/k230-monitor --adb /usr/bin/adb --max-fps 10 \
  --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx \
  --ui-model models/android-ui-yolov8n.onnx \
  --ui-min-side 64 --ui-min-area 0.01 --ui-confidence 0.25 \
  --record output/ui-phone.k230rec --dump-dir output/ui-phone \
  --duration 30 --verbose 2>&1 | tee -i output/ui-phone.log
```

يُستبعد القص قبل تشغيل NSFWJS إذا كان **عرضه أو ارتفاعه أقل من 64 بكسل**،
أو مساحته أقل من **1% من مساحة الإطار المفكوك**. يمكن تعديل الحدين عبر
`--ui-min-side` و`--ui-min-area`؛ المثال يستخدم القيم الافتراضية.
القياسات بعد إزالة letterbox وقص المستطيل إلى حدود الإطار، وليست على
صورة إدخال YOLO. يعالج المسار حتى ثماني مناطق في الإطار، مع NMS على أعلى
300 مرشح. يمكن ضبط العدد عبر `--ui-max-regions` وحد التداخل عبر `--ui-iou`.
عند انعدام المناطق المؤهلة، لا يُشغّل NSFWJS ولا تُستخدم الشاشة
الكاملة أو الشرائح البديلة، وتكون النتيجة `Unknown` بدلاً من `Safe`.
السجل يعرض `ignored_small` و`skipped` وإحداثيات القص الفائز `crop=x,y,WxH`.

لإعادة تحليل تسجيل دون خدمة التخطيط:

```bash
./build/inspector/k230-inspector --replay output/ui-phone.k230rec --realtime \
  --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx \
  --ui-model models/android-ui-yolov8n.onnx --verbose
```

الأوزان المحوّلة موجودة في المستودع ولا يحتاج التشغيل إلى Python أو Ultralytics.
راجع [مصدر الأوزان والتحويل والترخيص](models/ANDROID_UI_MODEL.md).
الاختبارات على PC لا تثبت دقة الكشف في كل تطبيق، أو سرعة وتوافق K230/KPU.
لا تجمع `--ui-model` مع `--layout` أو `--layout-replay`. الخيار القديم
`--nsfwjs-regions` لا يضيف شرائح في مسار UI.

### إعداد ONNX Runtime والتجربة الأصلية

الأوزان المحوّلة موجودة في المستودع؛ لا تحتاج إلى TensorFlow أو JavaScript
لتشغيل المصنّف. نفّذ من جذر مشروع C++ داخل Linux أو WSL (Python 3.10–3.12):

```bash
python3 tools/setup_onnxruntime.py
cmake -S . -B build -G Ninja -DK230_TARGET=pc \
  -DK230_ENABLE_ONNX=ON -DONNXRUNTIME_ROOT="$PWD/output/onnxruntime"
cmake --build build
ctest --test-dir build --output-on-failure
```

إذا كان `python3` لديك أحدث من 3.12، استخدم Python 3.12 مع `uv`:

```bash
curl -LsSf https://astral.sh/uv/0.12.10/install.sh | sh
"$HOME/.local/bin/uv" run --no-project --python 3.12 \
  tools/setup_onnxruntime.py --output output/onnxruntime-py312
cmake -S . -B build -G Ninja -DK230_TARGET=pc -DK230_ENABLE_ONNX=ON \
  -DONNXRUNTIME_ROOT="$PWD/output/onnxruntime-py312"
cmake --build build
```

السكربت يثبت ONNX Runtime 1.22.1 في بيئة معزولة ويجهز المكتبة وترويسات C++.
يحتاج الإنترنت للتثبيت فقط؛ تحليل الصور والصوت محلي. يمكن استخدام SDK رسمي
موجود لديك بتحديد `ONNXRUNTIME_ROOT` بدلاً من السكربت.

**التجربة المباشرة على الهاتف:**

```bash
adb devices
./build/apps/k230-monitor --max-fps 10 \
  --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx --onnx-threads 1 \
  --nsfwjs-regions 9 \
  --record output/nsfwjs-phone.k230rec --dump-dir output/nsfwjs-phone \
  --verbose 2>&1 | tee -i output/nsfwjs-phone.log
```

**إعادة تحليل تسجيل محفوظ:**

```bash
./build/inspector/k230-inspector --replay output/nsfwjs-phone.k230rec \
  --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx --verbose \
  2>&1 | tee -i output/nsfwjs-replay.log
```

إذا كانت لديك PNG/WAV فقط، أعد الالتقاط مع `--record` للحصول على `.k230rec`.
ولتجربة التشغيل دون هاتف:

```bash
./build/tools/k230-make-test-recording output/nsfwjs-demo.k230rec 12 10
./build/inspector/k230-inspector --replay output/nsfwjs-demo.k230rec \
  --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx --verbose
```

يعرض كل سطر `sample` توقيت الهاتف `pts` ودرجات الفئات الخمس:
`Drawing`, `Hentai`, `Neutral`, `Porn`, `Sexy`، وزمن التحليل `analysis_ms`
شاملاً تجهيز الصورة. احتفظ بسجل تجربة اللعب لقياس الإنذارات الكاذبة.
`--onnx-threads` يحدد عدد خيوط الاستدلال؛ نبدأ بخيط واحد ثم نقيس.
`--nsfwjs-regions` يدوّر التحليل بين الشاشة كاملة ومناطق متداخلة منها من دون
زيادة عدد مرات تشغيل النموذج لكل إطار. تتبع السياسة تأكيد كل منطقة على حدة.
عند 10 FPS وتسع مناطق، تعود المنطقة نفسها كل 0.9 ثانية؛ تأكيد ثلاث قراءات
خطرة قد يحتاج حتى 2.7 ثانية بعد ظهور المحتوى. استخدم `--nsfwjs-regions 1`
للمقارنة مع تحليل الشاشة كاملة فقط.
راقب أيضاً `drops` و`video_gaps` أثناء الالتقاط إذا لم يواكب الكمبيوتر 10 FPS.

السياسة التجريبية تستخدم `nudity = Porn + Hentai` مع التأكيد المتتابع ومدة
تهدئة التنبيهات الحاليين. درجة `Sexy` مستقلة في السجل ولا تدخل في التصعيد
حالياً، حتى نقيّم صور الرياضة والسباحة. عند اختيار NSFWJS تُرسل تنبيهات
`warn` فقط، حتى لو تجاوزت الدرجة حد `--block`؛ تقييم الحظر يأتي لاحقاً.
`log`/`safe` في بروتوكول القرار يعني عدم تجاوز هذه السياسة، ولا يضمن ملاءمة
المشهد للأطفال. المصنّف لا يقيس الدماء أو سلامة النص، ولا يحلل الصوت؛
المزامنة والتسجيل الصوتي وقياس RMS يستمران.

يجب أن يظهر `analyzer=nsfwjs-mobilenet-v2-onnx`. غياب الأوزان أو عدم توافقها
ينهي التشغيل بخطأ، ولا يستبدل المصنّف بالمحلل التجريبي. بدون
`--nsfwjs-model` يبقى المحلل التجريبي الحالي. تفاصيل مصدر الأوزان والتحويل
ومقارنة النتائج في [models/README.md](models/README.md).

هذا تشغيل CPU على PC؛ تحويل nncase إلى `.kmodel` وقياس K230 لم يُنفّذا بعد.

## Run without a phone

```bash
./build/tools/k230-make-test-recording demo.k230rec 12 10  # 12 s at 10 fps
./build/inspector/k230-inspector --replay demo.k230rec --verbose --dump-dir /tmp/dump
# /tmp/dump: frame_<100ms slot>_pts_<phone PTS>.png
# and audio_pts_<start PTS>_end_<end PTS>.wav (48 kHz stereo)
```

Verdicts are printed as JSON lines (the same format the companion app receives):

```json
{"seq":23,"pts_us":14200000,"action":"block","category":"nudity","confidence":1.000}
```

## Run with a phone (PC as stand-in for the K230)

1. Enable USB debugging, connect the phone, accept the RSA prompt.
2. `adb devices` must show `device` (not `unauthorized`).
3. `./build/apps/k230-monitor --record capture.k230rec --dump-dir output/capture --verbose`

`k230-monitor` runs the bridge and the inspector in one process, joined by the
same queues that DATAFIFO/IPCMSG replace on the board. `--record` saves the
compressed stream so it can be replayed later with `k230-inspector --replay`.
The bridge can also run alone: `./build/bridge/k230-bridge --record capture.k230rec`.
The default server command requests `max_fps=10`, `audio_source=playback` and
`audio_dup=true`, retaining playback on the phone while forwarding audio.
Duplication requires **Android 13+**; applications may opt out of playback
capture. The C++ pipeline keeps `audio_codec=raw` (PCM) to avoid lossy encoding
and decoding. There is no fallback to the `output` source that mutes the phone.

Capture runs until stopped (Ctrl+C), or until an explicit `--duration SEC`.
With `--dump-dir`, PNGs are selected in 100 ms slots using phone PTS throughout
the session. `max_fps` is a ceiling: a static screen or a slower encoder may
produce fewer frames. Missing slots are not filled with duplicate images.
Audio is streamed continuously and exported in consecutive one-second WAV
segments, with a shorter final segment at shutdown, using a bounded two-second
PCM history. All names include phone PTS; the first WAV starts at the first
decoded video PTS. Missing audio is zero-filled and reported as incomplete.
Without `--dump-dir`, no PNG/WAV files are written; synchronized analysis and
optional `.k230rec` recording continue for the full session.

## Key design points

* **True PTS.** Video and audio packets keep the PTS written by the phone.
  `SyncEngine` aligns a frame with the audio window
  `[pts - 2500 ms, pts + 500 ms]` (3 s, tunable) on that clock; wall time is used only to
  give up waiting (`max_wait_us`) or under backpressure. Incomplete windows
  are zero-padded and flagged `audio_complete=false`, never silently dropped.
  The optional dump uses the same PTS clock for continuous WAV segments and
  PNGs selected every 100 ms (not by packet count), without an eight-second cap.
  Raw PCM is the default scrcpy audio codec so no lossy audio decoder is needed.
* **Compressed across cores.** The little core never decodes. H.264 at
  max-size 800 / 10 fps / 2 Mbit/s plus 48 kHz stereo PCM (1.5 Mbit/s) is
  ≈ 3.5 Mbit/s over DATAFIFO, 50–100× less than raw frames.
* **Config packets are sacred.** The bounded queue evicts the oldest media
  packet when full but never SPS/PPS (`keep_config_packets`), so a decoder
  restart always has what it needs.
* **Policy with hysteresis.** `Policy` needs `confirm_frames` consecutive
  frames above a threshold before `Warn`/`Block`, then silences the category
  for `cooldown_us`. One noisy frame never reaches the phone.
* **No `adb kill-server`.** The bridge never restarts the host adb daemon.

## Companion app contract (to be implemented)

### Mentor accessibility layout (implemented)

Install [Mentor](https://github.com/abOOd-Hj-DeV/mentor-app) and manually enable its
accessibility service. This is a local layout channel; it does not capture pixels
or implement the verdict-command channel described below.

```bash
./build/apps/k230-monitor --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx \
  --onnx-threads 1 --max-fps 10 --layout \
  --layout-record output/layout.bin --record output/phone.k230rec --verbose
./build/tools/k230-layout-dump --replay output/layout.bin
./build/inspector/k230-inspector --replay output/phone.k230rec \
  --layout-replay output/layout.bin --nsfwjs-model models/nsfwjs-mobilenet-v2.onnx --realtime
```

`--layout` forwards `tcp:27186` to `localabstract:k230_layout`. A bounded receiver
validates length, version, flags, timestamps, sequence, dimensions and node bounds.
The wire contract is documented in Mentor; `tests/fixtures/layout-kotlin-v1.bin`
was exported by its actual Kotlin encoder. `layout-android-api35.bin` is the first
complete message received from its running service on the Android 35 emulator.
No accessibility text is transmitted.

Decoded frames immediately enter two independent ONNX workers, each with one
inference thread. Image rectangles from the same frame are scaled from current
screen coordinates (already rotated), then prepared directly to RGB 224×224.
Up to eight candidate regions plus a full-frame check are evaluated. Missing or
partial layouts add overlapping fallback regions. The highest region risk feeds
one policy observation per PTS; safe regions cannot cancel a dangerous region.
Only the latest pending frame is kept. Vision does not wait for the audio window.
Layout age is limited to 250 ms and invalidation barriers prevent reuse across
scroll/window changes. Accessibility cannot prove pixel coverage, so these results
remain `analysis_complete=false` and cannot declare the screen Safe.

Logs include layout session/sequence, source region, transform/preparation/inference
time, frame completion time, layout misses, worker failures, dropped frames,
video gaps and skipped frames. Timing on the development VM is not a phone or
K230 performance guarantee. Device PTS alignment, rotation, multiwindow behavior,
WebView coverage and warning latency still require real-device validation.

The bridge does `adb forward tcp:27185 localabstract:k230_companion`,
connects, expects one greeting byte `'K'`, then streams the JSON lines above.
Actions: `log` (never sent), `warn`, `block`, `delete`. The dispatcher drops
verdicts while the app is unreachable and reconnects; it never blocks the
media path.

## Status

- [x] Phase 0 — C++17 restructure, correct demuxing, PTS sync, tests (PC)
- [x] PC evaluation — NSFWJS MobileNetV2 through ONNX Runtime, five-class diagnostics
- [ ] Phase 1 — little core: buildroot with `android-tools`, `k230-bridge`, USB host validation
- [ ] Phase 2 — big core: DATAFIFO/IPCMSG, VDEC, `k230-inspector` on RT-Smart
- [ ] Phase 3 — KPU model (nncase → `.kmodel`), real classifier replaces heuristic
- [ ] Phase 4 — Android companion app
