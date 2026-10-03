#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "k230/inspector/nsfwjs_analyzer.hpp"
#include "k230/inspector/age_policy.hpp"
#include "k230/inspector/policy.hpp"
#include "k230/inspector/ui_detector.hpp"
#include "k230/ipc/channel.hpp"
#include "k230/layout.hpp"

namespace k230::inspector {

std::optional<ImageRegion> layout_region(const LayoutSnapshot& layout, const LayoutNode& node,
                                         const VideoFrame& frame);

struct VisionRegions {
  std::vector<ImageRegion> regions;
  std::vector<std::uint32_t> ids;
  std::vector<std::string> kinds;
  std::uint64_t session = 0;
  std::uint64_t identity = 0, sequence = 0;
  bool complete = false;
};
VisionRegions vision_regions(const VideoFrame& frame, const std::optional<LayoutSnapshot>& layout);

class FastVision {
 public:
  using Factory = std::function<std::unique_ptr<RegionAnalyzer>()>;
  using Observer = std::function<void(const VideoFrame&, const Scores&, const Verdict&)>;
  struct Stats {
    std::atomic<std::uint64_t> submitted{0}, dropped{0}, analyzed{0}, layout_misses{0}, failures{0}, warnings{0};
    std::atomic<std::uint64_t> skipped{0}, ignored_small{0};
  };
  FastVision(Factory factory, std::shared_ptr<LayoutCache> layouts, PolicyConfig policy,
             std::shared_ptr<ipc::VerdictSink> verdicts);
  FastVision(Factory factory, std::unique_ptr<RegionDetector> detector, PolicyConfig policy,
             std::shared_ptr<ipc::VerdictSink> verdicts);
  ~FastVision();
  bool start();
  void submit(VideoFrame frame);
  void finish();
  void set_observer(Observer observer) { observer_ = std::move(observer); }
  void set_batch_observer(std::function<void(AnalysisBatch)> observer) { batch_observer_ = std::move(observer); }
  void discontinuity() { discontinuity_.store(true); }
  const Stats& stats() const { return stats_; }

 private:
  struct FrameWork {
    std::shared_ptr<VideoFrame> frame;
    VisionRegions selection;
    std::vector<std::optional<Scores>> scores;
    std::size_t finished = 0;
    bool failed = false, discontinuity = false;
    std::chrono::steady_clock::time_point arrival;
  };
  struct Job { std::shared_ptr<FrameWork> work; std::size_t index; };
  void coordinate();
  void worker(std::size_t index);
  Factory factory_;
  std::shared_ptr<LayoutCache> layouts_;
  std::unique_ptr<RegionDetector> detector_;
  Policy policy_;
  std::shared_ptr<ipc::VerdictSink> verdicts_;
  Observer observer_;
  std::function<void(AnalysisBatch)> batch_observer_;
  std::atomic<bool> discontinuity_{false};
  std::int64_t selected_pts_ = -1;
  Stats stats_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Job> jobs_;
  std::shared_ptr<VideoFrame> pending_;
  std::chrono::steady_clock::time_point pending_arrival_;
  std::int64_t last_pts_us_ = -1;
  bool closing_ = false, stop_workers_ = false;
  std::vector<std::unique_ptr<RegionAnalyzer>> analyzers_;
  std::vector<std::thread> workers_;
  std::thread coordinator_;
};
}  // namespace k230::inspector
