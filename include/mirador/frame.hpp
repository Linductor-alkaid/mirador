#ifndef MIRADOR_FRAME_HPP
#define MIRADOR_FRAME_HPP

#include <mirador/image_view.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace mirador {

/// One capture instant: an image view plus frame context. The view is non-owning;
/// `owner` keeps the backing memory (shared buffer, decoder output, platform object)
/// alive while views derived from this frame are in use. Copies of a Frame share the
/// same owner; once the last owner reference is released, all views into it dangle.
/// `owner` may be null when the caller guarantees the buffer outlives every consumer.
struct Frame {
    ImageView image;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp;
    std::string source_id;
    std::shared_ptr<const void> owner;
};

}  // namespace mirador

#endif  // MIRADOR_FRAME_HPP
