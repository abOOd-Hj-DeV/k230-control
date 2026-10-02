// k230-monitor: PC harness running the little-core bridge and the big-core
// inspector in one process.
//
//   phone --adb--> ScrcpySession --> InProcessQueue<MediaPacket> --> InspectorPipeline
//                                        (stands in for DATAFIFO)          |
//   companion app <-- VerdictDispatcher <-- InProcessQueue<Verdict> <------+
//                                        (stands in for IPCMSG)
//
// Everything above the queues is the exact code that will run on the K230;
// only the transport differs.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>

#include "k230/bridge/adb_controller.hpp"
#include "k230/bridge/scrcpy_session.hpp"
#include "k230/bridge/verdict_dispatcher.hpp"
#include "k230/cli.hpp"
#include "k230/inspector/pipeline.hpp"
#include "k230/inspector/nsfwjs_analyzer.hpp"
#include "k230/ipc/channel.hpp"
#include "k230/log.hpp"
#include "k230/recording.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// Forwards packets to the inspector queue and, optionally, to a recording.
class TeeSink final : public k230::ipc::PacketSink {
 public:
  TeeSink(std::shared_ptr<k230::ipc::PacketSink> primary, std::unique_ptr<k230::RecordingWriter> recording)
      : primary_(std::move(primary)), recording_(std::move(recording)) {}
  bool push(k230::MediaPacket&& p) override {
    if (recording_) recording_->write(p);
    return primary_->push(std::move(p));
  }
  void close() override { primary_->close(); }

 private:
  std::shared_ptr<k230::ipc::PacketSink> primary_;
  std::unique_ptr<k230::RecordingWriter> recording_;
};

void usage() {
  std::puts(
      "k230-monitor [options]   (PC: phone over USB, both cores emulated)\n"
      "  --adb PATH --serial S --server-jar PATH\n"
      "  --max-size N --max-fps N --bitrate N --video-codec h264|h265 --audio-codec raw|opus --no-audio\n"
      "  --record FILE                 also save the packet stream for `k230-inspector --replay`\n"
      "  --dump-dir DIR                PNGs up to 10 fps + continuous WAV segments, aligned by phone PTS\n"
      "  --nsfwjs-model PATH           use NSFWJS MobileNetV2 ONNX (PC, warning-only evaluation)\n"
      "  --onnx-threads N              CPU inference threads (default: 1)\n"
      "  --warn X --block X --confirm N --cooldown-ms N   policy tuning\n"
      "  --duration SEC --verbose");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace k230;
  Cli cli(argc, argv);
  if (cli.has("help")) {
    usage();
    return 0;
  }
  if (cli.has("verbose")) log::threshold() = log::Level::Debug;

  bridge::ScrcpyConfig scfg;
  scfg.server_jar = cli.get("server-jar", scfg.server_jar);
  scfg.serial = cli.get("serial", "");
  scfg.max_size = static_cast<std::uint32_t>(cli.get_int("max-size", scfg.max_size));
  scfg.max_fps = static_cast<std::uint32_t>(cli.get_int("max-fps", scfg.max_fps));
  scfg.video_bit_rate = static_cast<std::uint32_t>(cli.get_int("bitrate", scfg.video_bit_rate));
  scfg.video_codec = cli.get("video-codec", scfg.video_codec);
  scfg.audio_codec = cli.get("audio-codec", scfg.audio_codec);
  scfg.audio = !cli.has("no-audio");

  inspector::PipelineConfig pcfg;
  pcfg.sync.audio_enabled = scfg.audio;
  pcfg.policy.warn_threshold = static_cast<float>(cli.get_double("warn", 0.60));
  pcfg.policy.block_threshold = static_cast<float>(cli.get_double("block", 0.85));
  pcfg.policy.confirm_frames = static_cast<std::uint32_t>(cli.get_int("confirm", 3));
  pcfg.policy.cooldown_us = cli.get_int("cooldown-ms", 5000) * 1000;
  pcfg.dump_dir = cli.get("dump-dir", "");

  std::unique_ptr<inspector::Analyzer> analyzer;
  if (cli.has("nsfwjs-model")) {
    inspector::NsfwjsConfig nsfw;
    nsfw.model_path = cli.get("nsfwjs-model");
    nsfw.threads = static_cast<int>(cli.get_int("onnx-threads", 1));
    analyzer = inspector::make_nsfwjs_analyzer(nsfw);
    if (!analyzer || !analyzer->open()) return 1;
    pcfg.allow_analyzer_fallback = false;
    pcfg.policy.escalated_action = Action::Warn;
  } else {
    analyzer = std::make_unique<inspector::HeuristicAnalyzer>();
  }

  auto packets = std::make_shared<ipc::InProcessQueue<MediaPacket>>(128, ipc::keep_config_packets);
  auto verdicts = std::make_shared<ipc::InProcessQueue<Verdict>>(64);

  std::unique_ptr<RecordingWriter> recording;
  if (cli.has("record")) {
    recording = std::make_unique<RecordingWriter>(cli.get("record"));
    if (!recording->ok()) {
      K230_LOG_ERROR("monitor") << "cannot open recording file";
      return 1;
    }
  }
  auto sink = std::make_shared<TeeSink>(packets, std::move(recording));

  bridge::AdbController adb(cli.get("adb", "adb"));
  if (!adb.available()) {
    K230_LOG_ERROR("monitor") << "adb not found; install android platform-tools";
    return 1;
  }

  bridge::ScrcpySession session(scfg, adb, sink);
  if (!session.start()) return 1;

  inspector::InspectorPipeline pipeline(pcfg, packets, verdicts, inspector::make_default_video_decoder(),
                                        std::move(analyzer));
  pipeline.set_observer([](const inspector::SyncedSample& s, const inspector::Scores& sc, const Verdict& v) {
    if (v.action >= Action::Warn || log::threshold() <= log::Level::Debug) {
      K230_LOG_INFO("sample") << "pts=" << s.frame.pts_us << " audio=" << s.audio_filled_us / 1000 << "ms"
                              << (s.audio_complete ? "" : " (incomplete)") << " " << inspector::describe_scores(sc)
                              << " -> " << to_string(v.action);
    }
  });
  std::atomic<bool> inspector_done{false};
  bool inspector_ok = false;
  std::thread inspector_thread([&] {
    inspector_ok = pipeline.run();
    inspector_done = true;
  });

  bridge::VerdictDispatcher dispatcher(bridge::CompanionConfig{}, adb, session.device_serial(), verdicts);
  dispatcher.set_observer([](const Verdict& v) {
    if (v.action >= Action::Warn) std::fputs(to_json_line(v).c_str(), stdout);
  });
  dispatcher.start();

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const long duration = cli.get_int("duration", 0);
  const auto t0 = std::chrono::steady_clock::now();
  auto next_report = t0 + std::chrono::seconds(5);
  while (!g_stop && session.running() && !inspector_done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto now = std::chrono::steady_clock::now();
    if (duration > 0 && now - t0 >= std::chrono::seconds(duration)) break;
    if (now >= next_report) {
      next_report = now + std::chrono::seconds(5);
      const auto v = session.video_stats();
      const auto a = session.audio_stats();
      const auto& st = pipeline.stats();
      const auto& ss = pipeline.sync().stats();
      K230_LOG_INFO("monitor") << "in: video=" << v.packets << " audio=" << a.packets << " | decoded=" << st.frames_decoded
                               << " analyzed=" << st.samples_analyzed << " incomplete=" << ss.emitted_incomplete
                               << " av_lead=" << ss.last_av_lead_us / 1000 << "ms"
                               << " | queue=" << packets->size() << " drops=" << packets->drops()
                               << " gaps=" << st.video_gaps
                               << " | verdicts sent=" << dispatcher.sent() << " dropped=" << dispatcher.dropped();
    }
  }

  session.stop();
  packets->close();
  inspector_thread.join();
  dispatcher.stop();
  K230_LOG_INFO("monitor") << "stopped";
  return inspector_ok ? 0 : 1;
}
