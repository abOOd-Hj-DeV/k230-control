#include "k230/inspector/fast_vision.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

#include "k230/log.hpp"

namespace k230::inspector {
std::optional<ImageRegion> layout_region(const LayoutSnapshot& s, const LayoutNode& n,
                                         const VideoFrame& frame) {
  if (s.display_id != 0 || s.width == 0 || s.height == 0 || s.rotation > 3 ||
      frame.width == 0 || frame.height == 0 || n.left >= n.right || n.top >= n.bottom ||
      n.right > s.width || n.bottom > s.height) return std::nullopt;
  const double sx = static_cast<double>(frame.width) / s.width;
  const double sy = static_cast<double>(frame.height) / s.height;
  if (std::abs(sx - sy) > std::max(sx, sy) * 0.015) return std::nullopt;
  const auto left = static_cast<std::uint32_t>(std::floor(n.left * sx));
  const auto top = static_cast<std::uint32_t>(std::floor(n.top * sy));
  const auto right = std::min(frame.width, static_cast<std::uint32_t>(std::ceil(n.right * sx)));
  const auto bottom = std::min(frame.height, static_cast<std::uint32_t>(std::ceil(n.bottom * sy)));
  if (right <= left || bottom <= top) return std::nullopt;
  return ImageRegion{left, top, right - left, bottom - top};
}

VisionRegions vision_regions(const VideoFrame& frame, const std::optional<LayoutSnapshot>& layout) {
  VisionRegions out;
  out.identity = static_cast<std::uint64_t>(frame.width) << 32 | frame.height;
  if (layout && !(layout->flags & kLayoutInvalidate)) {
    out.sequence = layout->sequence;
    out.session = layout->session;
    out.identity ^= layout->session ^ (static_cast<std::uint64_t>(layout->window_id) << 8) ^ layout->rotation;
    std::vector<LayoutNode> candidates;
    for (const auto& node : layout->nodes) {
      if (node.kind != 0) candidates.push_back(node);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
      return (a.kind == 1 || a.kind == 2) > (b.kind == 1 || b.kind == 2);
    });
    for (const auto& node : candidates) {
      auto region = layout_region(*layout, node, frame);
      if (!region) continue;
      if (region->width < 16 || region->height < 16) continue;
      const auto duplicate = std::find_if(out.regions.begin(), out.regions.end(), [&](const auto& r) {
        return r.x == region->x && r.y == region->y && r.width == region->width && r.height == region->height;
      });
      if (duplicate == out.regions.end()) {
        out.regions.push_back(*region);
        out.ids.push_back(node.id);
      }
      if (out.regions.size() == 8) break;
    }
    if (!out.regions.empty()) {
      const auto fallback = nsfwjs_regions(frame, (layout->flags & kLayoutPartial) ? 9 : 1);
      for (const auto& region : fallback) {
        out.regions.push_back(region);
        out.ids.push_back(0);
      }
      out.complete = false;
      return out;
    }
  }
  out.regions = nsfwjs_regions(frame, 9);
  out.ids.resize(out.regions.size());
  out.sequence = 0;
  out.complete = false;
  return out;
}

FastVision::FastVision(Factory factory, std::shared_ptr<LayoutCache> layouts, PolicyConfig policy,
                       std::shared_ptr<ipc::VerdictSink> verdicts)
    : factory_(std::move(factory)), layouts_(std::move(layouts)), policy_(policy), verdicts_(std::move(verdicts)) {}
FastVision::~FastVision() { finish(); }

bool FastVision::start() {
  for (int i = 0; i < 2; ++i) {
    auto analyzer = factory_();
    if (!analyzer || !analyzer->open()) return false;
    analyzers_.push_back(std::move(analyzer));
  }
  for (std::size_t i = 0; i < analyzers_.size(); ++i) workers_.emplace_back(&FastVision::worker, this, i);
  coordinator_ = std::thread(&FastVision::coordinate, this);
  return true;
}

void FastVision::submit(VideoFrame frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_ || frame.pts_us < 0 || frame.pts_us <= last_pts_us_) return;
  last_pts_us_ = frame.pts_us;
  ++stats_.submitted;
  if (pending_) ++stats_.dropped;
  pending_ = std::make_shared<VideoFrame>(std::move(frame));
  pending_arrival_ = std::chrono::steady_clock::now();
  changed_.notify_all();
}

void FastVision::finish() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
    changed_.notify_all();
  }
  if (coordinator_.joinable()) coordinator_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_workers_ = true;
    changed_.notify_all();
  }
  for (auto& thread : workers_) {
    if (thread.joinable()) thread.join();
  }
}

void FastVision::worker(std::size_t index) {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [&] { return stop_workers_ || !jobs_.empty(); });
      if (stop_workers_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    std::optional<Scores> scores;
    try {
      scores = analyzers_[index]->analyze_region(*job.work->frame, job.work->selection.regions[job.index]);
    } catch (const std::exception& e) {
      ++stats_.failures;
      K230_LOG_ERROR("vision") << "inference failed: " << e.what();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job.work->scores[job.index] = scores;
      job.work->failed |= !scores;
      ++job.work->finished;
      changed_.notify_all();
    }
  }
}

void FastVision::coordinate() {
  for (;;) {
    auto work = std::make_shared<FrameWork>();
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [&] { return closing_ || pending_; });
      if (!pending_) return;
      work->frame = std::move(pending_);
      work->arrival = pending_arrival_;
    }
    const auto transform_start = std::chrono::steady_clock::now();
    auto layout = layouts_->match(work->frame->pts_us);
    work->selection = vision_regions(*work->frame, layout);
    const auto transform_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - transform_start).count();
    if (work->selection.sequence == 0) ++stats_.layout_misses;
    work->scores.resize(work->selection.regions.size());
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (std::size_t i = 0; i < work->scores.size(); ++i) jobs_.push_back({work, i});
      changed_.notify_all();
      changed_.wait(lock, [&] { return work->finished == work->scores.size(); });
    }
    Scores aggregate;
    for (std::size_t i = 0; i < work->scores.size(); ++i) {
      if (work->scores[i] && (!aggregate.nsfwjs || work->scores[i]->nudity > aggregate.nudity)) {
        aggregate = *work->scores[i];
        aggregate.analysis_region = static_cast<std::uint32_t>(i);
      }
    }
    aggregate.analysis_regions = static_cast<std::uint32_t>(work->scores.size());
    aggregate.analysis_layout = work->selection.identity;
    aggregate.layout_sequence = work->selection.sequence;
    aggregate.layout_session = work->selection.session;
    aggregate.source_region = work->selection.ids[aggregate.analysis_region];
    aggregate.transform_ms = transform_ms;
    aggregate.analysis_complete = work->selection.complete && !work->failed;
    aggregate.frame_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - work->arrival).count();
    aggregate.analysis_region = 0;
    auto verdict = policy_.evaluate(work->frame->pts_us, aggregate);
    ++stats_.analyzed;
    if (verdict.action >= Action::Warn) ++stats_.warnings;
    if (observer_) observer_(*work->frame, aggregate, verdict);
    verdicts_->push(std::move(verdict));
  }
}
}  // namespace k230::inspector
