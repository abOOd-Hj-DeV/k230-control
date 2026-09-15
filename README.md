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
| `pc` (default)| bridge + inspector + `k230-monitor` + tests | FFmpeg  | heuristic | in-process queues |
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
tests/      GoogleTest (38 tests incl. an end-to-end scrcpy-bytes → verdict run)
assets/     scrcpy-server 4.0 (pushed to the phone)
```

## Build (PC)

```bash
sudo apt install cmake ninja-build pkg-config libavcodec-dev libavutil-dev libswscale-dev libgtest-dev
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Cross builds only need `-DK230_TARGET=little|big` plus a toolchain file
(`-DCMAKE_TOOLCHAIN_FILE=...`); FFmpeg and GoogleTest are not required there.

## Run without a phone

```bash
./build/tools/k230-make-test-recording demo.k230rec 10      # 10 s, alternating safe / skin-tone screens
./build/inspector/k230-inspector --replay demo.k230rec --realtime --verbose \
    --dump-dir /tmp/dump --dump-every 10                    # PPM + WAV of synced samples
```

Verdicts are printed as JSON lines (the same format the companion app receives):

```json
{"seq":23,"pts_us":14200000,"action":"block","category":"nudity","confidence":1.000}
```

## Run with a phone (PC as stand-in for the K230)

1. Enable USB debugging, connect the phone, accept the RSA prompt.
2. `adb devices` must show `device` (not `unauthorized`).
3. `./build/apps/k230-monitor --record capture.k230rec --verbose`

`k230-monitor` runs the bridge and the inspector in one process, joined by the
same queues that DATAFIFO/IPCMSG replace on the board. `--record` saves the
compressed stream so it can be replayed later with `k230-inspector --replay`.
The bridge can also run alone: `./build/bridge/k230-bridge --record capture.k230rec`.

## Key design points

* **True PTS.** Video and audio packets keep the PTS written by the phone.
  `SyncEngine` aligns a frame with the audio window
  `[pts - 500 ms, pts + 200 ms]` on that clock; wall time is used only to
  give up waiting (`max_wait_us`) or under backpressure. Incomplete windows
  are zero-padded and flagged `audio_complete=false`, never silently dropped.
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

The bridge does `adb forward tcp:27185 localabstract:k230_companion`,
connects, expects one greeting byte `'K'`, then streams the JSON lines above.
Actions: `log` (never sent), `warn`, `block`, `delete`. The dispatcher drops
verdicts while the app is unreachable and reconnects; it never blocks the
media path.

## Status

- [x] Phase 0 — C++17 restructure, correct demuxing, PTS sync, tests (PC)
- [ ] Phase 1 — little core: buildroot with `android-tools`, `k230-bridge`, USB host validation
- [ ] Phase 2 — big core: DATAFIFO/IPCMSG, VDEC, `k230-inspector` on RT-Smart
- [ ] Phase 3 — KPU model (nncase → `.kmodel`), real classifier replaces heuristic
- [ ] Phase 4 — Android companion app
