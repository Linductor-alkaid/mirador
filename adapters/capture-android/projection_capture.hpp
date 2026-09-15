#ifndef MIRADOR_ADAPTERS_CAPTURE_ANDROID_PROJECTION_CAPTURE_HPP
#define MIRADOR_ADAPTERS_CAPTURE_ANDROID_PROJECTION_CAPTURE_HPP

#ifndef __ANDROID__
#error "projection_capture requires an Android NDK build (media/NdkImageReader.h)"
#endif

#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/result.hpp>
#include <mirador/transform.hpp>

#include <cstdint>
#include <memory>

// Opaque NDK handle; the NDK headers stay out of this adapter header
// (RULE-02). The tag matches android/native_window.h's `struct ANativeWindow`.
struct ANativeWindow;

namespace mirador::adapters {

struct ProjectionCaptureOptions {
    /// Capture buffer dimensions; the MediaProjection VirtualDisplay renders
    /// the (resized) display content into this buffer.
    int32_t buffer_width = 0;
    int32_t buffer_height = 0;
    /// Max in-flight AImages held by the reader (producer back-pressure).
    int32_t max_images = 4;
};

/// Synchronous MediaProjection capture adapter (M5-05, Android only). The
/// Java side owns the MediaProjection permission flow, creates the
/// VirtualDisplay on `window()` and stops the projection; this adapter reads
/// the resulting frames through an `AImageReader` (YUV_420_888) and converts
/// them into plain RGB8 `Frame`s (view + owner, design section 6). Pixels
/// are processed in memory only and never written to disk (RULE-10).
///
/// Independent adaptation layer (AGENTS.md): Mirador core never touches the
/// NDK. Not thread-safe: one instance per capture site; callers own
/// scheduling (AGENTS.md concurrency rules).
class ProjectionCapture {
public:
    ProjectionCapture() noexcept = default;
    ProjectionCapture(ProjectionCapture&& other) noexcept = default;
    ProjectionCapture& operator=(ProjectionCapture&& other) noexcept = default;
    /// Releases the underlying AImageReader (defined in the .cpp: the
    /// deleter needs the complete NDK type).
    ~ProjectionCapture();
    ProjectionCapture(const ProjectionCapture&) = delete;
    ProjectionCapture& operator=(const ProjectionCapture&) = delete;

    /// Creates the underlying AImageReader. Errors: kInvalidArgument for
    /// non-positive dimensions or max_images; kBackendUnavailable when the
    /// reader cannot be created. Never throws.
    [[nodiscard]] static Result<ProjectionCapture> create(const ProjectionCaptureOptions& options);

    /// Surface sink for the caller's VirtualDisplay; owned by the reader.
    /// The pointer stays valid for the adapter's lifetime.
    [[nodiscard]] ANativeWindow* window() const noexcept;

    /// Captures the latest buffered frame into an owned RGB8 frame. Polls
    /// `context` around the (atomic) acquire and on the conversion loop.
    /// kBackendFailure when no buffer is available or the image layout is
    /// unreadable; kCancelled/kTimeout per the context.
    [[nodiscard]] Result<Frame> capture(const ExecutionContext& context) const;

    /// Display-space mapping of the captured view (DEC-016): the
    /// kOriented -> kDisplay scale restoring physical display coordinates
    /// for a VirtualDisplay that resized the display into the capture
    /// buffer. Pass the real display size read on the Java side
    /// (DisplayMetrics); core never queries platform metrics (RULE-01).
    /// Errors: kInvalidArgument for non-positive display sizes.
    [[nodiscard]] Result<Transform2D> display_transform(int32_t display_width, int32_t display_height) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mirador::adapters

#endif  // MIRADOR_ADAPTERS_CAPTURE_ANDROID_PROJECTION_CAPTURE_HPP
