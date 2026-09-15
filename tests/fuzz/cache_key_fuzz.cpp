// M5-07 fuzz entry point (design section 23, SCOPE-08): adversarial inputs into
// the capability cache key serialization (RULE-07, DEC-012). Fields are derived
// byte-deterministically and cover arbitrary bytes inside strings (including
// NUL) plus out-of-range enum scalars: the digest must stay a pure function of
// the field values for every input the types can hold.
//
// The pure digest functions are the fuzz surface here; CapabilityResultCache
// itself has no fuzzer-drivable data flow (its interface takes already-digested
// keys), so it is exercised by tests/cache instead.
//
// Invariants asserted on every input (a break aborts, a libFuzzer finding):
//   (a) digests are deterministic: the same fields produce an equal digest on
//       a repeated call (key digest, OCR and detection param digests);
//   (b) the digest does not degenerate to a constant: across 32 deterministic
//       single-field perturbations at least one must change the key digest
//       (a strict "every perturbation changes it" assertion would encode a
//       probabilistic hash property, which is deliberately not hardened here).

#include <mirador/capability_cache.hpp>

#include <mirador/cache_policy.hpp>
#include <mirador/detector_backend.hpp>
#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/transform.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct ByteCursor {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

uint8_t next_byte(ByteCursor& cursor) noexcept {
    const uint8_t value = cursor.data[cursor.pos];
    cursor.pos = (cursor.pos + 1) % cursor.size;
    return value;
}

uint32_t next_u32(ByteCursor& cursor) noexcept {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(next_byte(cursor)) << (8 * i);
    }
    return value;
}

uint64_t next_u64(ByteCursor& cursor) noexcept {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(next_byte(cursor)) << (8 * i);
    }
    return value;
}

int32_t next_i32(ByteCursor& cursor) noexcept {
    return static_cast<int32_t>(next_u32(cursor));
}

float next_f32(ByteCursor& cursor) noexcept {
    const uint32_t bits = next_u32(cursor);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits), "32-bit IEEE 754 float expected");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// Arbitrary-length (up to 31 bytes) string with any byte contents, NUL
/// included: the serialization must be length-prefixed and byte-exact.
std::string next_string(ByteCursor& cursor) noexcept {
    const size_t length = next_byte(cursor) % 32U;
    std::string value;
    value.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        value.push_back(static_cast<char>(next_byte(cursor)));
    }
    return value;
}

[[noreturn]] void fuzz_fail(const char* what, long detail) noexcept {
    std::fprintf(stderr, "FUZZ INVARIANT BROKEN: %s (%ld)\n", what, detail);
    std::fflush(stderr);
    std::abort();
}

void check(bool condition, const char* what, long detail = 0) noexcept {
    if (!condition) {
        fuzz_fail(what, detail);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) noexcept {
    if (size < 16) {
        return 0;
    }
    ByteCursor cursor{data, size, 0};

    mirador::CapabilityKeyFields fields;
    fields.image_fingerprint = next_u64(cursor);
    fields.source_id = next_string(cursor);
    fields.roi.x = next_i32(cursor);
    fields.roi.y = next_i32(cursor);
    fields.roi.width = next_i32(cursor);
    fields.roi.height = next_i32(cursor);
    fields.preprocessing_version = next_u32(cursor);
    fields.kind = static_cast<mirador::CapabilityKind>(next_byte(cursor));  // out-of-range values allowed
    fields.output_space = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    fields.backend_name = next_string(cursor);
    fields.implementation_version = next_string(cursor);
    fields.model_id = next_string(cursor);
    fields.model_revision = next_string(cursor);
    fields.request_params_digest = next_u64(cursor);

    // (a) Determinism of the key digest.
    const mirador::CacheKeyDigest digest = mirador::capability_cache_digest(fields);
    check(mirador::capability_cache_digest(fields) == digest, "capability_cache_digest must be deterministic", 0);

    // (a) Determinism of the request-parameter digests (bit-exact floats and
    // out-of-range enums included).
    mirador::OcrRequest ocr_request;
    ocr_request.roi = mirador::RectF{next_f32(cursor), next_f32(cursor), next_f32(cursor), next_f32(cursor)};
    ocr_request.roi_space = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    ocr_request.language_hint = next_string(cursor);
    ocr_request.min_confidence = next_f32(cursor);
    ocr_request.max_side = next_i32(cursor);
    ocr_request.backend_params = next_string(cursor);
    ocr_request.output_space = static_cast<mirador::CoordinateSpaceId>(next_u32(cursor));
    ocr_request.cache_policy = static_cast<mirador::CachePolicy>(next_byte(cursor));
    const uint64_t ocr_digest = mirador::ocr_request_params_digest(ocr_request);
    check(mirador::ocr_request_params_digest(ocr_request) == ocr_digest,
          "ocr_request_params_digest must be deterministic", 0);

    mirador::DetectionRequest detection_request;
    detection_request.roi = ocr_request.roi;
    detection_request.roi_space = ocr_request.roi_space;
    detection_request.min_confidence = ocr_request.min_confidence;
    detection_request.max_side = ocr_request.max_side;
    detection_request.backend_params = ocr_request.backend_params;
    detection_request.output_space = ocr_request.output_space;
    detection_request.cache_policy = ocr_request.cache_policy;
    const uint64_t detection_digest = mirador::detection_request_params_digest(detection_request);
    check(mirador::detection_request_params_digest(detection_request) == detection_digest,
          "detection_request_params_digest must be deterministic", 0);

    // (b) Anti-degeneration: 32 deterministic single-field perturbations, at
    // least one must move the digest. Field selection rides on the input so
    // coverage steering can reach every field.
    unsigned changed = 0;
    for (unsigned perturbation = 0; perturbation < 32; ++perturbation) {
        mirador::CapabilityKeyFields perturbed = fields;
        const unsigned selector = (next_byte(cursor) + perturbation) % 11U;
        switch (selector) {
            case 0:
                perturbed.image_fingerprint ^= (uint64_t{1} << (perturbation % 64U));
                break;
            case 1:
                if (perturbed.source_id.empty()) {
                    perturbed.source_id.push_back('\x01');
                } else {
                    perturbed.source_id[perturbation % perturbed.source_id.size()] =
                        static_cast<char>(~perturbed.source_id[perturbation % perturbed.source_id.size()]);
                }
                break;
            case 2:
                perturbed.roi.x ^= static_cast<int32_t>(perturbation + 1U);  // xor: overflow-free by design
                break;
            case 3:
                perturbed.roi.y ^= static_cast<int32_t>(perturbation + 1U);
                break;
            case 4:
                perturbed.roi.width ^= static_cast<int32_t>(perturbation) + 1;
                break;
            case 5:
                perturbed.roi.height ^= static_cast<int32_t>(perturbation) + 1;
                break;
            case 6:
                perturbed.preprocessing_version += perturbation + 1U;
                break;
            case 7:
                perturbed.kind =
                    static_cast<mirador::CapabilityKind>(perturbed.kind == mirador::CapabilityKind::kOcr
                                                             ? static_cast<uint8_t>(mirador::CapabilityKind::kDetection)
                                                             : static_cast<uint8_t>(mirador::CapabilityKind::kOcr));
                break;
            case 8:
                perturbed.output_space = static_cast<mirador::CoordinateSpaceId>(
                    static_cast<uint32_t>(perturbed.output_space) + perturbation + 1U);
                break;
            case 9:
                if (perturbed.backend_name.empty()) {
                    perturbed.backend_name.push_back('\x02');
                } else {
                    perturbed.backend_name[0] = static_cast<char>(~perturbed.backend_name[0]);
                }
                break;
            default:
                perturbed.request_params_digest ^= (uint64_t{1} << (perturbation % 64U));
                break;
        }
        if (!(mirador::capability_cache_digest(perturbed) == digest)) {
            ++changed;
        }
    }
    check(changed >= 1U, "digest degenerated: no perturbation changed it", static_cast<long>(changed));

    return 0;
}
