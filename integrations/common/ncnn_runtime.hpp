#ifndef MIRADOR_INTEGRATIONS_NCNN_RUNTIME_HPP
#define MIRADOR_INTEGRATIONS_NCNN_RUNTIME_HPP

#include <mirador/execution_context.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mirador::integrations {

/// Load options for one reference-backend model (DEC-015). Paths are supplied
/// explicitly by the caller: weights are never embedded, downloaded, cached or
/// logged here (RULE-10, project standards section 9.2.4).
struct NcnnRuntimeOptions {
    std::string param_path;  ///< ncnn .param (layer graph, text format)
    std::string bin_path;    ///< ncnn .bin (weight data)
    /// Intra-op threads ncnn may use inside a single forward call. The
    /// synchronous SPI boundary still completes before the backend returns
    /// (AGENTS.md concurrency rules allow runtime-internal parallelism).
    /// 1 keeps results bit-deterministic, which reference backends require
    /// for cache correctness (DEC-012 determinism contract).
    int num_threads = 1;
};

/// Planar CHW float tensor in ncnn Mat layout: `data` holds `channels` planes
/// of width*height floats, plane-major. This is the only tensor surface the
/// integrations layer shares with reference backends; ncnn types never appear
/// in any header (DEC-002 by analogy, DEC-015 storage boundary).
struct NcnnTensor {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<float> data;
};

/// One named input blob for `NcnnRuntime::run_multi` (M7-12): the two-model
/// NanoTrack layout feeds the template and search features into the head net
/// as separate blobs, so a single-blob run is not enough there.
struct NcnnNamedTensor {
    std::string blob;  ///< blob name as spelled in the .param file
    NcnnTensor tensor;
};

/// Minimal synchronous wrapper over one loaded ncnn network (M5-02, DEC-015).
/// It validates the "real runtime fits behind the Backend SPI" claim of
/// POST-05; OCR/Detector reference backends (M5-03/M5-04) build on it. The
/// wrapper owns the loaded network, never mutates inputs, and propagates
/// failure/cancel explicitly instead of silently swallowing them (RULE-08).
class NcnnRuntime {
public:
    /// Null runtime: calls on it fail with kBackendUnavailable. Lets backend
    /// objects hold a runtime by value and populate it in their own create()
    /// factories. Defined out of line: the PIMPL type is incomplete here.
    NcnnRuntime() noexcept;

    /// Loads param+bin from `options` paths; kBackendUnavailable when the
    /// files are missing or ncnn rejects them, with the ncnn error code in
    /// the status message.
    static Result<NcnnRuntime> create(const NcnnRuntimeOptions& options);

    NcnnRuntime(NcnnRuntime&& other) noexcept;
    NcnnRuntime& operator=(NcnnRuntime&& other) noexcept;
    ~NcnnRuntime();

    NcnnRuntime(const NcnnRuntime&) = delete;
    NcnnRuntime& operator=(const NcnnRuntime&) = delete;

    /// Runs one forward pass: planar CHW `input` in at `input_blob`, copied
    /// output tensor out from `output_blob`. Cancellation is polled before
    /// and after the forward call; a single forward is atomic and cannot be
    /// interrupted mid-flight, so callers bound model size and budget
    /// upstream (design section 20). Const so session objects can hold
    /// references to a backend's shared runtime (the tracker contract keeps
    /// the owning backend alive for every session's lifetime).
    Result<NcnnTensor> run(const std::string& input_blob, const NcnnTensor& input, const std::string& output_blob,
                           const ExecutionContext& context) const;

    /// M7-12 generalization of `run` for multi-blob models: every entry of
    /// `inputs` is set on its named blob, one atomic forward runs, and each
    /// name in `output_blobs` is extracted (in the given order) into copied
    /// planar tensors. Same contract as `run`: inputs are validated then never
    /// modified, cancellation is polled before and after the forward (never
    /// mid-flight), unknown blobs are kInvalidArgument, extract failures are
    /// kBackendFailure. `inputs`/`output_blobs` must be non-empty with unique
    /// names; duplicate output names return kInvalidArgument.
    Result<std::vector<NcnnTensor>> run_multi(const std::vector<NcnnNamedTensor>& inputs,
                                              const std::vector<std::string>& output_blobs,
                                              const ExecutionContext& context) const;

    [[nodiscard]] int num_threads() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit NcnnRuntime(std::unique_ptr<Impl> impl) noexcept;
};

/// Packs an ImageView into a planar CHW float tensor. Values are copied as
/// stored (0..255 range, no normalization, no layout reinterpretation);
/// reference backends apply their own normalization on top. Supported
/// formats: kGray8 (1 plane), kRgb8 (3 planes), kRgba8 (4 planes) — anything
/// else returns kUnsupportedFormat. Non-contiguous rows are honored via
/// row_stride_bytes; the packing loop polls `context` periodically.
Result<NcnnTensor> pack_image(const ImageView& image, const ExecutionContext& context);

}  // namespace mirador::integrations

#endif  // MIRADOR_INTEGRATIONS_NCNN_RUNTIME_HPP
