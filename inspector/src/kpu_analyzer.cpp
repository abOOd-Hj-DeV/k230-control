// KPU-backed analyzer for the K230 big core.
//
// Bring-up notes:
//   1. Convert the model with nncase (PC):
//        python tools/compile_kmodel.py model.onnx --target k230 --input 224x224 --calib images/
//      (quantised uint8 kmodel; see nncase docs "k230 compile" for the exact options)
//   2. Load with the nncase runtime shipped in the K230 SDK:
//        nncase::runtime::interpreter interp;
//        interp.load_model({data, size});
//        auto in = interp.input_tensor(0); auto out = interp.output_tensor(0);
//   3. Pre-processing on the big core: crop/scale the NV12 VDEC frame to the
//      model input with the 2D hardware scaler (VO/GSD or kd_mpi_vicap resize
//      is not applicable here; use `kd_mpi_vgs_*` if available in the SDK
//      version, otherwise a simple nearest-neighbour on the Y/UV planes) and
//      convert to the model's expected layout (RGB planar, uint8).
//   4. Post-processing: sigmoid/softmax to Scores.
//
// Until the runtime is wired in, this class fails open() with a clear message
// so the pipeline falls back to HeuristicAnalyzer.

#include "k230/inspector/analyzer.hpp"
#include "k230/log.hpp"

namespace k230::inspector {

namespace {

class KpuAnalyzer final : public Analyzer {
 public:
  explicit KpuAnalyzer(KpuConfig config) : config_(std::move(config)) {}

  bool open() override {
#ifdef K230_HAS_NNCASE
    // TODO(k230): load config_.kmodel_path with nncase::runtime::interpreter.
    return false;
#else
    K230_LOG_ERROR("kpu") << "KPU analyzer (" << config_.kmodel_path
                          << ") requires the nncase runtime from the K230 SDK (K230_HAS_NNCASE)";
    return false;
#endif
  }

  Scores analyze(const SyncedSample&) override { return {}; }
  const char* name() const override { return "k230-kpu"; }

 private:
  KpuConfig config_;
};

}  // namespace

std::unique_ptr<Analyzer> make_kpu_analyzer(const KpuConfig& config) {
  return std::make_unique<KpuAnalyzer>(config);
}

}  // namespace k230::inspector
