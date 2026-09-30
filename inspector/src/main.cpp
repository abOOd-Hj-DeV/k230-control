// k230-inspector: big-core (RT-Smart) side.
//
//   PacketSource (DATAFIFO on K230 | .k230rec replay on PC)
//     --> VideoDecoder (VDEC | FFmpeg) --> SyncEngine --> Analyzer (KPU | heuristic)
//     --> Policy --> VerdictSink (IPCMSG on K230 | stdout on PC)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>

#include "k230/cli.hpp"
#include "k230/inspector/pipeline.hpp"
#include "k230/ipc/channel.hpp"
#include "k230/ipc/datafifo_channel.hpp"
#include "k230/log.hpp"
#include "k230/recording.hpp"

namespace {

constexpr std::size_t kReplayQueueCapacity = 256;
constexpr std::size_t kReplayHighWater = kReplayQueueCapacity / 2;

std::atomic<bool> g_stop{false};
k230::inspector::InspectorPipeline* g_pipeline = nullptr;
void on_signal(int) {
  g_stop = true;
  if (g_pipeline) g_pipeline->request_stop();
}

class StdoutVerdictSink final : public k230::ipc::VerdictSink {
 public:
  bool push(k230::Verdict&& v) override {
    std::fputs(k230::to_json_line(v).c_str(), stdout);
    std::fflush(stdout);
    return true;
  }
  void close() override {}
};

// Feeds a recording into an in-process queue, optionally at the original
// pace (so the SyncEngine timeouts behave as they would live).
void replay_thread(const std::string& path, std::shared_ptr<k230::ipc::InProcessQueue<k230::MediaPacket>> queue,
                   bool realtime) {
  using namespace k230;
  RecordingReader reader(path);
  if (!reader.ok()) {
    K230_LOG_ERROR("replay") << reader.error();
    queue->close();
    return;
  }
  std::int64_t first_pts = -1;
  auto t0 = std::chrono::steady_clock::now();
  while (!g_stop) {
    auto pkt = reader.next();
    if (!pkt) break;
    if (realtime && pkt->pts_us >= 0) {
      if (first_pts < 0) first_pts = pkt->pts_us;
      const auto due = t0 + std::chrono::microseconds(pkt->pts_us - first_pts);
      std::this_thread::sleep_until(due);
    }
    // Offline replay must not lose packets to the lossy queue: throttle instead.
    while (!g_stop && queue->size() >= kReplayHighWater) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    queue->push(std::move(*pkt));
  }
  if (!reader.error().empty()) K230_LOG_WARN("replay") << reader.error();
  queue->close();
}

void usage() {
  std::puts(
      "k230-inspector [options]\n"
      "  --source datafifo|replay      packet source (default: replay if --replay given, else datafifo)\n"
      "  --replay FILE                 .k230rec produced by `k230-bridge --record`\n"
      "  --realtime                    pace the replay by PTS instead of as fast as possible\n"
      "  --verdicts ipcmsg|stdout      where verdicts go (default: stdout on PC)\n"
      "  --kmodel PATH                 use the KPU analyzer with this model (K230 only)\n"
      "  --dump-dir DIR                one PNG per second + one 8-second WAV, aligned by phone PTS\n"
      "  --no-audio                    ignore audio, release frames immediately\n"
      "  --warn X --block X --confirm N --cooldown-ms N   policy tuning\n"
      "  --audio-before-ms N --audio-after-ms N --max-wait-ms N  sync tuning\n"
      "  --verbose");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace k230;
  using namespace k230::inspector;
  Cli cli(argc, argv);
  if (cli.has("help")) {
    usage();
    return 0;
  }
  if (cli.has("verbose")) log::threshold() = log::Level::Debug;

  PipelineConfig cfg;
  cfg.sync.audio_enabled = !cli.has("no-audio");
  cfg.sync.audio_before_us = cli.get_int("audio-before-ms", 2500) * 1000;
  cfg.sync.audio_after_us = cli.get_int("audio-after-ms", 500) * 1000;
  cfg.sync.max_wait_us = cli.get_int("max-wait-ms", 1000) * 1000;
  cfg.policy.warn_threshold = static_cast<float>(cli.get_double("warn", 0.60));
  cfg.policy.block_threshold = static_cast<float>(cli.get_double("block", 0.85));
  cfg.policy.confirm_frames = static_cast<std::uint32_t>(cli.get_int("confirm", 3));
  cfg.policy.cooldown_us = cli.get_int("cooldown-ms", 5000) * 1000;
  cfg.dump_dir = cli.get("dump-dir", "");
  cfg.idle_timeout_ms = cli.get_int("idle-timeout-ms", 0);

  const std::string source_kind = cli.get("source", cli.has("replay") ? "replay" : "datafifo");
  const std::string verdict_kind = cli.get("verdicts", source_kind == "replay" ? "stdout" : "ipcmsg");

  std::shared_ptr<ipc::PacketSource> source;
  std::shared_ptr<ipc::VerdictSink> verdicts;
  std::thread replay;
  try {
    if (source_kind == "replay") {
      auto queue = std::make_shared<ipc::InProcessQueue<MediaPacket>>(kReplayQueueCapacity, ipc::keep_config_packets);
      source = queue;
      replay = std::thread(replay_thread, cli.get("replay", "capture.k230rec"), queue, cli.has("realtime"));
    } else {
      source = std::make_shared<ipc::DataFifoPacketSource>(ipc::DataFifoConfig{});
    }
    if (verdict_kind == "stdout") {
      verdicts = std::make_shared<StdoutVerdictSink>();
    } else {
      verdicts = std::make_shared<ipc::IpcMsgVerdictSink>(ipc::IpcMsgConfig{});
    }
  } catch (const std::exception& e) {
    K230_LOG_ERROR("inspector") << e.what();
    g_stop = true;
    if (replay.joinable()) replay.join();
    return 1;
  }

  std::unique_ptr<Analyzer> analyzer;
  if (cli.has("kmodel")) {
    KpuConfig kpu;
    kpu.kmodel_path = cli.get("kmodel");
    analyzer = make_kpu_analyzer(kpu);
  } else {
    analyzer = std::make_unique<HeuristicAnalyzer>();
  }

  InspectorPipeline pipeline(cfg, source, verdicts, make_default_video_decoder(), std::move(analyzer));
  g_pipeline = &pipeline;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  pipeline.set_observer([&](const SyncedSample& s, const Scores& scores, const Verdict& v) {
    if (v.action >= Action::Warn || log::threshold() <= log::Level::Debug) {
      K230_LOG_INFO("sample") << "pts=" << s.frame.pts_us << " " << s.frame.width << "x" << s.frame.height
                              << " audio=" << s.audio_filled_us / 1000 << "/" << (s.audio_end_us - s.audio_start_us) / 1000
                              << "ms" << (s.audio_complete ? "" : " (incomplete)") << " nudity=" << scores.nudity
                              << " rms=" << scores.audio_level << " -> " << to_string(v.action);
    }
  });

  pipeline.run();
  g_pipeline = nullptr;
  g_stop = true;
  if (replay.joinable()) replay.join();

  const auto& st = pipeline.stats();
  const auto& ss = pipeline.sync().stats();
  K230_LOG_INFO("inspector") << "sync: emitted=" << ss.emitted << " incomplete=" << ss.emitted_incomplete
                             << " backpressure=" << ss.emitted_backpressure << " last_av_lead_ms=" << ss.last_av_lead_us / 1000
                             << " | verdicts escalated=" << st.verdicts_escalated;
  return st.video_packets > 0 && st.frames_decoded == 0 ? 1 : 0;
}
