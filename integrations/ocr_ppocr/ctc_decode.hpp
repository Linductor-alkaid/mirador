#ifndef MIRADOR_INTEGRATIONS_PPOCR_CTC_DECODE_HPP
#define MIRADOR_INTEGRATIONS_PPOCR_CTC_DECODE_HPP

#include <mirador/result.hpp>

#include <cstdint>
#include <vector>

namespace mirador::integrations {

/// Greedy CTC decode of one recognition output (PP-OCR rec reference, M5-03).
struct CtcDecodedLine {
    /// Decoded class indices, one per emitted character, in time order.
    /// Values are class indices into the full output vocabulary (0 .. classes-1);
    /// mapping to UTF-8 text is the caller's charset concern.
    std::vector<int32_t> class_indices;
    /// Mean softmax probability of the emitted steps; 0.0 when nothing was
    /// emitted. Softmax is computed here so logits and probabilities give the
    /// same confidence scale.
    float confidence = 0.0F;
};

/// Greedy CTC decoding over a [steps x classes] row-major score matrix:
/// per step take the argmax (first maximum on ties, deterministic), collapse
/// consecutive repeats, drop the blank class. `data[t * classes + c]`.
///
/// Errors: kInvalidArgument when steps < 1, classes < 2 or blank_index is
/// outside [0, classes). Never throws.
[[nodiscard]] Result<CtcDecodedLine> ctc_decode_greedy(const float* data, int32_t steps, int32_t classes,
                                                       int32_t blank_index) noexcept;

}  // namespace mirador::integrations

#endif  // MIRADOR_INTEGRATIONS_PPOCR_CTC_DECODE_HPP
