#include "ctc_decode.hpp"

#include <mirador/result.hpp>
#include <mirador/status.hpp>

#include <cmath>
#include <cstdint>

namespace mirador::integrations {

Result<CtcDecodedLine> ctc_decode_greedy(const float* data, int32_t steps, int32_t classes,
                                         int32_t blank_index) noexcept {
    if (data == nullptr) {
        return Status{ErrorCode::kInvalidArgument, "ctc_decode_greedy: data is null"};
    }
    if (steps < 1 || classes < 2) {
        return Status{ErrorCode::kInvalidArgument, "ctc_decode_greedy: need steps >= 1 and classes >= 2"};
    }
    if (blank_index < 0 || blank_index >= classes) {
        return Status{ErrorCode::kInvalidArgument, "ctc_decode_greedy: blank_index out of range"};
    }

    CtcDecodedLine line;
    double confidence_sum = 0.0;
    int32_t previous_class = -1;
    for (int32_t t = 0; t < steps; ++t) {
        const float* row = data + static_cast<int64_t>(t) * classes;
        int32_t best = 0;
        float best_value = row[0];
        for (int32_t c = 1; c < classes; ++c) {
            if (row[c] > best_value) {
                best = c;
                best_value = row[c];
            }
        }
        // Softmax over the step for a bounded confidence; argmax is unchanged.
        const double max_value = best_value;
        double sum = 0.0;
        for (int32_t c = 0; c < classes; ++c) {
            sum += std::exp(static_cast<double>(row[c]) - max_value);
        }
        const double probability = 1.0 / sum;

        if (best != blank_index && best != previous_class) {
            line.class_indices.push_back(best);
            confidence_sum += probability;
        }
        previous_class = best;
    }
    line.confidence = line.class_indices.empty()
                          ? 0.0F
                          : static_cast<float>(confidence_sum / static_cast<double>(line.class_indices.size()));
    return line;
}

}  // namespace mirador::integrations
