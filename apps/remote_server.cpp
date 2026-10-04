#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "k230/cli.hpp"
#include "k230/companion_v2.hpp"
#include "k230/inspector/nsfwjs_analyzer.hpp"
#include "k230/inspector/protection_session.hpp"
#include "k230/inspector/ui_detector.hpp"

namespace {
using k230::companion::Json;
constexpr std::size_t kMaxFrame = 8 * 1024 * 1024;
constexpr std::uint64_t kMaxPixels = 8 * 1024 * 1024;
std::atomic<unsigned> clients{0};
std::mutex inference_mutex;

void require(bool valid) { if (!valid) throw std::runtime_error("invalid_remote_record"); }
std::uint64_t be(const std::uint8_t* bytes, unsigned count) {
  std::uint64_t result = 0;
  for (unsigned i = 0; i < count; ++i) result = (result << 8) | bytes[i];
  return result;
}
void read_exact(SSL* ssl, void* data, std::size_t size) {
  auto* cursor = static_cast<std::uint8_t*>(data);
  while (size) {
    int count = SSL_read(ssl, cursor, static_cast<int>(size));
    require(count > 0); cursor += count; size -= count;
  }
}
void write_exact(SSL* ssl, const void* data, std::size_t size) {
  const auto* cursor = static_cast<const std::uint8_t*>(data);
  while (size) {
    int count = SSL_write(ssl, cursor, static_cast<int>(size));
    require(count > 0); cursor += count; size -= count;
  }
}
struct Record { std::uint8_t type; std::vector<std::uint8_t> bytes; };
Record read_record(SSL* ssl) {
  std::array<std::uint8_t, 5> header{};
  read_exact(ssl, header.data(), header.size());
  const auto length = be(header.data() + 1, 4);
  const auto maximum = header[0] == 2 ? kMaxFrame : k230::companion::kMaxLine;
  require(length > 0 && length <= maximum && header[0] <= 2);
  Record record{header[0], std::vector<std::uint8_t>(length)};
  read_exact(ssl, record.bytes.data(), record.bytes.size());
  return record;
}
class ControlWriter final : public k230::ipc::ControlSink {
 public:
  explicit ControlWriter(SSL* ssl) : ssl_(ssl) {}
  bool push(Json&& value) override {
    try {
      const auto text = k230::companion::line(value);
      const auto length = static_cast<std::uint32_t>(text.size());
      const std::array<std::uint8_t, 5> header{1, static_cast<std::uint8_t>(length >> 24),
        static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 8),
        static_cast<std::uint8_t>(length)};
      write_exact(ssl_, header.data(), header.size()); write_exact(ssl_, text.data(), text.size());
      return true;
    } catch (const std::exception&) { return false; }
  }
  void close() override {}
 private:
  SSL* ssl_;
};
k230::inspector::VideoFrame decode_png(const Record& record) {
  require(record.bytes.size() > 84);
  const auto* p = record.bytes.data();
  const auto pts = be(p, 8), width = be(p + 8, 4), height = be(p + 12, 4);
  require(pts > 0 && pts <= static_cast<std::uint64_t>(INT64_MAX) && width > 0 && height > 0 &&
    width <= 4096 && height <= 4096 && width * height <= kMaxPixels);
  const auto* png = p + 60;
  const std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  require(std::equal(signature.begin(), signature.end(), png) &&
    be(png + 8, 4) == 13 && std::memcmp(png + 12, "IHDR", 4) == 0 &&
    be(png + 16, 4) == width && be(png + 20, 4) == height);
  const auto context_deleter = [](AVCodecContext* c) { avcodec_free_context(&c); };
  std::unique_ptr<AVCodecContext, decltype(context_deleter)> context(
    avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_PNG)), context_deleter);
  require(context != nullptr); context->max_pixels = kMaxPixels; context->thread_count = 1;
  require(avcodec_open2(context.get(), avcodec_find_decoder(AV_CODEC_ID_PNG), nullptr) >= 0);
  const auto packet_deleter = [](AVPacket* p) { av_packet_free(&p); };
  const auto frame_deleter = [](AVFrame* f) { av_frame_free(&f); };
  std::unique_ptr<AVPacket, decltype(packet_deleter)> packet(av_packet_alloc(), packet_deleter);
  std::unique_ptr<AVFrame, decltype(frame_deleter)> image(av_frame_alloc(), frame_deleter);
  require(packet != nullptr && image != nullptr);
  require(av_new_packet(packet.get(), static_cast<int>(record.bytes.size() - 60)) >= 0);
  std::memcpy(packet->data, png, record.bytes.size() - 60);
  require(avcodec_send_packet(context.get(), packet.get()) >= 0 &&
    avcodec_receive_frame(context.get(), image.get()) >= 0);
  require(image->width == static_cast<int>(width) && image->height == static_cast<int>(height));
  k230::inspector::VideoFrame frame;
  frame.width = width; frame.height = height; frame.pts_us = pts; frame.key_frame = true;
  frame.data.resize(width * height + 2 * ((width + 1) / 2) * ((height + 1) / 2));
  std::uint8_t* planes[4]{}; int strides[4]{};
  require(av_image_fill_arrays(planes, strides, frame.data.data(), AV_PIX_FMT_YUV420P, width, height, 1) >= 0);
  std::unique_ptr<SwsContext, decltype(&sws_freeContext)> conversion(sws_getContext(width, height,
    static_cast<AVPixelFormat>(image->format), width, height, AV_PIX_FMT_YUV420P,
    SWS_BILINEAR, nullptr, nullptr, nullptr), sws_freeContext);
  require(conversion != nullptr);
  const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
  require(sws_setColorspaceDetails(conversion.get(), coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16) >= 0);
  require(sws_scale(conversion.get(), image->data, image->linesize, 0, height, planes, strides) == static_cast<int>(height));
  return frame;
}
void session(int fd, SSL_CTX* tls, const std::string& token,
             k230::inspector::RegionDetector& detector, k230::inspector::RegionAnalyzer& analyzer) {
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(tls), SSL_free);
  try {
    require(ssl != nullptr && SSL_set_fd(ssl.get(), fd) == 1 && SSL_accept(ssl.get()) == 1);
    auto auth = read_record(ssl.get());
    require(auth.type == 0 && auth.bytes.size() == token.size() &&
      CRYPTO_memcmp(auth.bytes.data(), token.data(), token.size()) == 0);
    auto downstream = std::make_shared<ControlWriter>(ssl.get());
    k230::inspector::ProtectionSession protection(downstream, false,
      [] { return std::chrono::steady_clock::now(); }, "android-mediaprojection-display");
    Json state;
    std::int64_t last_pts = -1;
    auto last_frame = std::chrono::steady_clock::time_point{};
    while (true) {
      auto record = read_record(ssl.get());
      if (record.type == 1) {
        require(record.bytes.back() == '\n');
        const auto message = k230::companion::parse(std::string(record.bytes.begin(), record.bytes.end() - 1));
        protection.receive(message);
        if (message.at("type") == "state") {
          state = message;
          const std::array<std::uint8_t, 6> heartbeat{3, 0, 0, 0, 1, 1};
          write_exact(ssl.get(), heartbeat.data(), heartbeat.size());
        }
      } else {
        require(record.type == 2 && record.bytes.size() > 60);
        const auto consumed = [&] {
          const std::array<std::uint8_t, 6> receipt{5, 0, 0, 0, 1, 1};
          write_exact(ssl.get(), receipt.data(), receipt.size());
        };
        const auto raw_pts = be(record.bytes.data(), 8);
        require(raw_pts <= static_cast<std::uint64_t>(INT64_MAX));
        const auto pts = static_cast<std::int64_t>(raw_pts);
        const auto now = std::chrono::steady_clock::now();
        require(pts > last_pts && (last_pts < 0 || now - last_frame >= std::chrono::milliseconds(100)));
        last_pts = pts; last_frame = now;
        protection.observe_capture_pts(pts);
        if (state.is_null() || state.at("screen").is_null()) { consumed(); continue; }
        const auto& screen = state.at("screen");
        const auto epoch = be(record.bytes.data() + 16, 8);
        const std::string screen_token(record.bytes.begin() + 24, record.bytes.begin() + 60);
        if (screen_token != screen.at("screen_token") ||
            std::to_string(epoch) != screen.at("content_epoch") ||
            be(record.bytes.data() + 8, 4) != screen.at("width").get<unsigned>() ||
            be(record.bytes.data() + 12, 4) != screen.at("height").get<unsigned>()) { consumed(); continue; }
        auto frame = decode_png(record);
        k230::inspector::AnalysisBatch batch;
        batch.pts_us = pts; batch.width = frame.width; batch.height = frame.height;
        {
          std::lock_guard<std::mutex> lock(inference_mutex);
          const auto regions = detector.detect(frame);
          batch.complete = regions.complete && !regions.regions.empty();
          for (std::size_t i = 0; i < regions.regions.size(); ++i) {
            const auto& region = regions.regions[i];
            const auto scores = analyzer.analyze_region(frame, region);
            require(scores.nsfwjs.has_value() && scores.analysis_complete);
            const auto& nsfw = *scores.nsfwjs;
            batch.regions.push_back({{static_cast<int>(region.x), static_cast<int>(region.y),
              static_cast<int>(region.width), static_cast<int>(region.height)}, regions.kinds.at(i),
              {nsfw.porn, nsfw.hentai, nsfw.sexy}, true, false});
          }
        }
        const std::array<std::uint8_t, 6> analyzed{4, 0, 0, 0, 1, static_cast<std::uint8_t>(batch.complete ? 1 : 0)};
        protection.analyze(std::move(batch));
        write_exact(ssl.get(), analyzed.data(), analyzed.size());
        consumed();
      }
    }
  } catch (const std::exception&) {
    std::cerr << "Remote session closed; execution is unconfirmed after disconnect.\n";
  }
  close(fd); --clients;
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  k230::Cli cli(argc, argv);
  if (cli.has("help")) {
    std::cout << "k230-server --cert FILE --key FILE [--listen 127.0.0.1] [--port 8443]\n"
      "  --ui-model FILE --nsfwjs-model FILE\nK230_REMOTE_TOKEN must contain 32-512 bytes.\n";
    return 0;
  }
  try {
    const char* value = std::getenv("K230_REMOTE_TOKEN");
    require(value != nullptr);
    const std::string token(value);
    require(token.size() >= 32 && token.size() <= 512 && cli.has("cert") && cli.has("key"));
    k230::inspector::UiDetectorConfig ui; ui.model_path = cli.get("ui-model", ui.model_path);
    k230::inspector::NsfwjsConfig nsfw; nsfw.model_path = cli.get("nsfwjs-model", nsfw.model_path);
    auto detector = k230::inspector::make_ui_detector(ui);
    auto analyzer = k230::inspector::make_nsfwjs_region_analyzer(nsfw);
    require(detector && analyzer && detector->open() && analyzer->open());
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> tls(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    require(tls != nullptr && SSL_CTX_set_min_proto_version(tls.get(), TLS1_2_VERSION) == 1);
    require(SSL_CTX_use_certificate_chain_file(tls.get(), cli.get("cert").c_str()) == 1 &&
      SSL_CTX_use_PrivateKey_file(tls.get(), cli.get("key").c_str(), SSL_FILETYPE_PEM) == 1 &&
      SSL_CTX_check_private_key(tls.get()) == 1);
    const auto port = cli.get_int("port", 8443); require(port > 0 && port <= 65535);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    require(inet_pton(AF_INET, cli.get("listen", "127.0.0.1").c_str(), &address.sin_addr) == 1);
    const int listener = socket(AF_INET, SOCK_STREAM, 0); require(listener >= 0);
    int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 4) == 0);
    std::cout << "TLS analyzer listening on port " << port << "; media remains in memory.\n" << std::flush;
    while (true) {
      const int fd = accept(listener, nullptr, nullptr);
      if (fd < 0) continue;
      if (clients.fetch_add(1) >= 4) { --clients; close(fd); continue; }
      timeval timeout{5, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      std::thread(session, fd, tls.get(), std::cref(token), std::ref(*detector), std::ref(*analyzer)).detach();
    }
  } catch (const std::exception&) {
    std::cerr << "Server startup failed. Check model paths, TLS files, token and bind address.\n";
    return 1;
  }
}
