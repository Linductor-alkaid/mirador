#ifndef MIRADOR_OPENCV_ADAPTER_HPP
#define MIRADOR_OPENCV_ADAPTER_HPP

#include <mirador/image_buffer.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <opencv2/core.hpp>

namespace mirador {

/// Optional adapter between OpenCV images and the Mirador input model (design
/// section 24 M1). Lives entirely in adapters/: no mirador-core or mirador-image
/// header or source knows about OpenCV, and no adapter type leaks into the
/// public input model. Mirador treats OpenCV as an integrator-provided build
/// dependency (never vendored, never distributed); the surface used here is
/// stable across OpenCV 4.x. Tested with OpenCV 4.6.0 (Ubuntu 24.04).

/// Maps a `cv::Mat` onto a non-owning ImageView in rotation k0. The view
/// borrows the Mat's memory: keep the Mat alive and un-resized for as long as
/// the view (and everything derived from it, such as Frame owner semantics in
/// later milestones) is used. Works for non-continuous Mats (for example a ROI
/// of a larger Mat): `row_stride_bytes` carries the Mat step. Channel semantics
/// follow OpenCV conventions, not RGB order: CV_8UC1 -> kGray8, CV_8UC3 ->
/// kBgr8, CV_8UC4 -> kBgra8. Orientation metadata is not part of a Mat; callers
/// that know the capture rotation set it on a copy or record it separately.
///
/// Errors: kInvalidArgument for an empty Mat, kUnsupportedFormat for any depth
/// or channel count other than CV_8U with 1, 3 or 4 channels, kInvalidArgument
/// when dimensions are outside [1, kMaxImageDimension] or the step is too small
/// for the mapped format. Never throws.
[[nodiscard]] Result<ImageView> wrap_mat(const cv::Mat& mat) noexcept;

/// Maps a Mat holding an NV12 image onto a non-owning ImageView. OpenCV has no
/// NV12 type: the conventional representation is a CV_8UC1 Mat with
/// `height + ceil(height / 2)` rows, luma on top and interleaved UV below in
/// the same stride. `width`/`height` carry the semantic image size the Mat
/// layout cannot express. The chroma plane starts at `data + step * height`
/// with the same row stride (DEC-007), so the step must be at least
/// `width + width % 2` even when the luma alone would fit in less.
///
/// Errors: kInvalidArgument for an empty Mat, too few rows, an insufficient
/// step, or dimensions outside [1, kMaxImageDimension]; kUnsupportedFormat when
/// the Mat is not CV_8UC1. Never throws.
[[nodiscard]] Result<ImageView> wrap_nv12_mat(const cv::Mat& mat, int32_t width, int32_t height) noexcept;

/// Deep-copies a `cv::Mat` into a newly allocated packed ImageBuffer of the
/// same mapped format (see `wrap_mat` for the format mapping). The allocation
/// is checked against `max_bytes` before it happens (RULE-06). Bounded by the
/// same errors as `wrap_mat` plus kBudgetExceeded when the copy does not fit
/// the budget or the allocation fails. Never throws.
[[nodiscard]] Result<ImageBuffer> export_mat(const cv::Mat& mat, int64_t max_bytes) noexcept;

/// Deep-copies an NV12 Mat (conventional `height + ceil(height / 2)`-row
/// CV_8UC1 layout) into a packed ImageBuffer with DEC-007 plane layout: tight
/// luma rows followed by chroma rows of `width + width % 2` bytes. Bounded by
/// the same errors as `wrap_nv12_mat` plus kBudgetExceeded. Never throws.
[[nodiscard]] Result<ImageBuffer> export_nv12_mat(const cv::Mat& mat, int32_t width, int32_t height,
                                                  int64_t max_bytes) noexcept;

}  // namespace mirador

#endif  // MIRADOR_OPENCV_ADAPTER_HPP
