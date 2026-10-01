// K230 hardware video decoder (big core, RT-Smart) through the MPP VDEC API.
//
// Bring-up notes for the board (K230 SDK, src/big/mpp):
//   - headers:  mpi_vdec_api.h, mpi_vb_api.h, mpi_sys_api.h, k_vdec_comm.h
//   - the decoder needs a VB (video buffer) pool sized for the stream:
//       k_vb_config cfg{}; cfg.max_pool_cnt = 1;
//       cfg.comm_pool[0].blk_size = VDEC_ALIGN(w*h*3/2, 0x1000); cfg.comm_pool[0].blk_cnt = 6;
//       kd_mpi_vb_set_config(&cfg); kd_mpi_vb_init();
//   - channel setup:
//       k_vdec_chn_attr attr{}; attr.type = K_PT_H264 (or K_PT_H265);
//       attr.mode = K_VDEC_SEND_MODE_FRAME; attr.pic_width = w; attr.pic_height = h;
//       attr.stream_buf_size = w*h; attr.frame_buf_cnt = 6;
//       kd_mpi_vdec_create_chn(chn, &attr); kd_mpi_vdec_start_chn(chn);
//   - per access unit (SPS/PPS can be sent as their own stream too):
//       k_vdec_stream s{}; s.addr = phys; s.len = n; s.pts = pts_us; s.end_of_stream = K_FALSE;
//       kd_mpi_vdec_send_stream(chn, &s, -1);
//   - output:
//       k_video_frame_info f; kd_mpi_vdec_get_frame(chn, &f, 40);  // NV12, f.v_frame.pts
//       ... run KPU on f (no copy needed, the buffer is physically contiguous) ...
//       kd_mpi_vdec_release_frame(chn, &f);
//
// The stream buffer must be physically contiguous memory obtained from
// kd_mpi_sys_mmz_alloc(); packets coming out of DATAFIFO are copied into it.

#include "k230/inspector/decoder.hpp"
#include "k230/log.hpp"

namespace k230::inspector {

namespace {

class VdecVideoDecoder final : public VideoDecoder {
 public:
  const char* name() const override { return "k230-vdec"; }

  bool open(CodecId codec) override {
#ifdef K230_HAS_MPP
    // TODO(k230): kd_mpi_vb_* / kd_mpi_vdec_create_chn as sketched above.
    (void)codec;
    return false;
#else
    K230_LOG_ERROR("vdec") << "VDEC decoder for " << to_string(codec)
                           << " is only available when building for the K230 big core (K230_HAS_MPP)";
    return false;
#endif
  }

  bool decode(const MediaPacket&, std::vector<VideoFrame>&) override { return false; }
  void flush(std::vector<VideoFrame>&) override {}
};

}  // namespace

std::unique_ptr<VideoDecoder> make_vdec_video_decoder() { return std::make_unique<VdecVideoDecoder>(); }

}  // namespace k230::inspector
