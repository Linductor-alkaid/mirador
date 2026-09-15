// M5-07 privacy negative tests (DOD-06, RULE-10): the core pipeline processes
// images in memory only — no files written, no evidence text leaked through
// Status messages or diagnostic traces, no persistence APIs on the caches.
// These are behavioral negatives: they hold marker text flowing through the
// full pipeline and assert it never surfaces on any side channel.

#include <mirador/perception_session.hpp>

#include "fake_backends.hpp"

#include <mirador/evidence.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/fusion.hpp>
#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using mirador::CoordinateSpaceId;
using mirador::ErrorCode;
using mirador::EvidenceSet;
using mirador::ExecutionContext;
using mirador::ExternalRegion;
using mirador::Frame;
using mirador::FusionOptions;
using mirador::FusionTrace;
using mirador::OcrRequest;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::Status;
using mirador::TextRegion;
using mirador::Transform2D;
using mirador::test::FakeOcrBackend;

// Distinct markers per evidence source so a leak can be attributed.
constexpr const char* kOcrMarker = "SECRET-OCR-TEXT";
constexpr const char* kAccessibilityMarker = "SECRET-A11Y-LABEL";

constexpr const char* kSourceId = "privacy-source";

struct TestFrame {
    std::vector<std::byte> bytes;
    Frame frame;
};

TestFrame make_frame(int32_t width, int32_t height, uint64_t sequence) {
    TestFrame image;
    const int64_t stride = static_cast<int64_t>(width) * 4;  // rgba8
    image.bytes.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), std::byte{9});
    image.frame.image.data = image.bytes.data();
    image.frame.image.width = width;
    image.frame.image.height = height;
    image.frame.image.row_stride_bytes = stride;
    image.frame.image.format = PixelFormat::kRgba8;
    image.frame.image.rotation = Rotation::k0;
    image.frame.sequence = sequence;
    image.frame.source_id = kSourceId;
    image.frame.owner = std::make_shared<const std::vector<std::byte>>(image.bytes);
    return image;
}

PerceptionSession make_session() {
    PerceptionSessionOptions options;
    options.source_id = kSourceId;
    return PerceptionSession::create(std::move(options)).take_value();
}

TextRegion marker_ocr_region() {
    TextRegion region;
    region.bounds = RectF{25.0F, 20.0F, 50.0F, 40.0F};
    region.utf8_text = kOcrMarker;
    region.confidence = 0.9F;
    return region;
}

ExternalRegion marker_accessibility_region() {
    ExternalRegion region;
    region.bounds = RectF{20.0F, 15.0F, 60.0F, 50.0F};  // containment-merges with the OCR box
    region.text = kAccessibilityMarker;
    region.role = "button";
    region.confidence = 1.0F;
    region.interactive = true;
    return region;
}

/// Evidence carrying both markers through a successful fusion.
EvidenceSet marker_evidence() {
    EvidenceSet evidence;
    (void)evidence.add_external(marker_accessibility_region(), CoordinateSpaceId::kOriented);
    (void)evidence.add_text(marker_ocr_region(), CoordinateSpaceId::kOriented);
    return evidence;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Collects every string the fusion diagnostics could ever carry. The walk is
/// exhaustive for the current FusionTrace definition: RegionTrace,
/// ConfidenceContribution and AssociationObservation expose only numeric
/// fields (ids, masks, rules, doubles), pinned by the static assertions below,
/// so the trace is string-free by construction — this helper documents that
/// with member-by-member access and returns what a string-carrying trace would
/// have leaked.
std::vector<std::string> collect_trace_strings(const FusionTrace& trace) {
    std::vector<std::string> leaked;
    for (const mirador::RegionTrace& region : trace.regions) {
        // evidence_ids: uint64; source_mask: uint32 — nothing to leak.
        (void)region.evidence_ids;
        (void)region.source_mask;
        for (const mirador::ConfidenceContribution& contribution : region.confidence_contributions) {
            (void)contribution.evidence_id;
            (void)contribution.weight;
            (void)contribution.confidence;
        }
        for (const mirador::AssociationObservation& observation : region.associations) {
            (void)observation.first_evidence_id;
            (void)observation.second_evidence_id;
            (void)observation.rule;
            (void)observation.iou;
            (void)observation.containment;
            (void)observation.center_distance;
        }
    }
    (void)trace.input_evidence_count;
    return leaked;
}

// The structural claim behind collect_trace_strings, pinned at compile time:
// every FusionTrace member type is arithmetic or a container of arithmetic.
using ConfidenceContributions = decltype(mirador::RegionTrace::confidence_contributions)::value_type;
using AssociationObservations = decltype(mirador::RegionTrace::associations)::value_type;
static_assert(std::is_arithmetic_v<decltype(mirador::RegionTrace::source_mask)>);
static_assert(std::is_same_v<decltype(mirador::RegionTrace::evidence_ids)::value_type, uint64_t>);
static_assert(std::is_same_v<decltype(ConfidenceContributions::evidence_id), uint64_t>);
static_assert(std::is_arithmetic_v<decltype(ConfidenceContributions::weight)>);
static_assert(std::is_arithmetic_v<decltype(ConfidenceContributions::confidence)>);
static_assert(std::is_same_v<decltype(AssociationObservations::first_evidence_id), uint64_t>);
static_assert(std::is_same_v<decltype(AssociationObservations::second_evidence_id), uint64_t>);
static_assert(std::is_arithmetic_v<decltype(AssociationObservations::iou)>);
static_assert(std::is_arithmetic_v<decltype(AssociationObservations::containment)>);
static_assert(std::is_arithmetic_v<decltype(AssociationObservations::center_distance)>);
static_assert(std::is_arithmetic_v<decltype(FusionTrace::input_evidence_count)>);

/// Directory name -> type snapshot (non-recursive, error-code overloads: the
/// test must never throw or create anything itself).
std::map<std::string, std::filesystem::file_type> snapshot_directory(const std::filesystem::path& directory) {
    std::map<std::string, std::filesystem::file_type> entries;
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        ADD_FAILURE() << "cannot list " << directory << ": " << ec.message();
        return entries;
    }
    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code status_ec;
        entries[it->path().filename().string()] = it->status(status_ec).type();
        it.increment(ec);
        if (ec) {
            ADD_FAILURE() << "cannot iterate " << directory << ": " << ec.message();
            break;
        }
    }
    return entries;
}

// --- DOD-06: default is in-memory only --------------------------------------------------

TEST(Privacy, PipelineWritesNoFiles) {
    namespace fs = std::filesystem;
    const fs::path cwd = fs::current_path();
    const fs::path temp = fs::temp_directory_path();
    const auto cwd_before = snapshot_directory(cwd);
    const auto temp_before = snapshot_directory(temp);
    ASSERT_FALSE(cwd_before.empty());
    ASSERT_FALSE(temp_before.empty());

    // Full pipeline, several rounds, caches hot: run_ocr stores through the
    // capability cache (kReadWrite), analyze_change stores signatures in the
    // frame cache, plus one explicit caller cache write.
    PerceptionSession session = make_session();
    FakeOcrBackend ocr;
    const TestFrame frame = make_frame(100, 80, 1U);
    for (int round = 0; round < 5; ++round) {
        ASSERT_TRUE(session.analyze_change(frame.frame).ok());
        const auto texts = session.run_ocr(frame.frame, &ocr, OcrRequest{});
        ASSERT_TRUE(texts.ok());
        const auto fused = session.fuse(frame.frame, marker_evidence());
        ASSERT_TRUE(fused.ok());
    }
    const std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};
    ASSERT_TRUE(session.frame_cache().insert(0xDEADBEEFU, payload).ok());

    const auto cwd_after = snapshot_directory(cwd);
    const auto temp_after = snapshot_directory(temp);

    // No new entries anywhere. Disappearances are not the pipeline's doing
    // either, but only additions can be attributed to the test window.
    for (const auto& [name, type] : cwd_after) {
        (void)type;
        EXPECT_NE(cwd_before.find(name), cwd_before.end()) << "unexpected new file in cwd: " << name;
    }
    for (const auto& [name, type] : temp_after) {
        (void)type;
        EXPECT_NE(temp_before.find(name), temp_before.end()) << "unexpected new file in temp: " << name;
    }
}

// --- DOD-06: no evidence text on Status / trace side channels ---------------------------

/// A Status message from any path must stay free of both markers.
void expect_no_marker(const std::string& message, const char* path) {
    EXPECT_FALSE(contains(message, kOcrMarker)) << path << " leaked the OCR marker";
    EXPECT_FALSE(contains(message, kAccessibilityMarker)) << path << " leaked the accessibility marker";
}

template <typename T>
void expect_status(const mirador::Result<T>& result, mirador::ErrorCode expected, const char* path) {
    ASSERT_EQ(result.status().code(), expected) << path;
    expect_no_marker(result.status().message(), path);
}

/// Every session-level fuse failure path (bad options, cancellation, timeout,
/// budget, singular display transform); each Status message must stay free of
/// the markers.
void check_fuse_failure_paths(PerceptionSession& session, const TestFrame& frame) {
    FusionOptions bad_space;
    bad_space.target_space = CoordinateSpaceId::kModelInput;
    expect_status(session.fuse(frame.frame, marker_evidence(), bad_space), ErrorCode::kInvalidArgument,
                  "kInvalidArgument message");

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    expect_status(session.fuse(frame.frame, marker_evidence(), {}, cancelled), ErrorCode::kCancelled,
                  "kCancelled message");

    ExecutionContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    expect_status(session.fuse(frame.frame, marker_evidence(), {}, expired), ErrorCode::kTimeout, "kTimeout message");

    // Budget path: two disjoint marker-carrying clusters against a one-region
    // budget.
    EvidenceSet disjoint;
    (void)disjoint.add_external(marker_accessibility_region(), CoordinateSpaceId::kOriented);
    ExternalRegion far_marker;
    far_marker.bounds = RectF{85.0F, 70.0F, 10.0F, 10.0F};
    far_marker.text = kAccessibilityMarker;
    (void)disjoint.add_external(far_marker, CoordinateSpaceId::kOriented);
    FusionOptions tight;
    tight.max_regions = 1;
    expect_status(session.fuse(frame.frame, disjoint, tight), ErrorCode::kBudgetExceeded, "kBudgetExceeded message");

    // Coordinate-transform path: a kDisplay-space evidence item forces the
    // display transform to be inverted (DEC-016); a singular matrix must
    // surface as kCoordinateTransform.
    EvidenceSet display_evidence;
    (void)display_evidence.add_external(marker_accessibility_region(), CoordinateSpaceId::kDisplay);
    FusionOptions display;
    display.target_space = CoordinateSpaceId::kOriented;
    Transform2D singular;
    singular.matrix = {1.0, 2.0, 3.0, 2.0, 4.0, 6.0, 0.0, 0.0, 1.0};  // det = 0
    singular.from = CoordinateSpaceId::kOriented;
    singular.to = CoordinateSpaceId::kDisplay;
    display.display_transform = singular;
    const auto coordinate = session.fuse(frame.frame, display_evidence, display);
    ASSERT_EQ(coordinate.status().code(), ErrorCode::kCoordinateTransform);
    expect_status(coordinate, ErrorCode::kCoordinateTransform, "kCoordinateTransform message");
}

/// run_ocr failure paths: backend-reported failure and cancellation. The
/// capability cache is dropped first — an unchanged frame/request would be
/// served from the cache and never reach the backend or the context poll.
void check_ocr_failure_paths(PerceptionSession& session, const TestFrame& frame, FakeOcrBackend& ocr) {
    // Backend-reported failure (generic message, no marker inside — the
    // backend owns its message, the pipeline must not append evidence text).
    ocr.next_regions.clear();
    session.result_cache().clear();
    ocr.next_status = Status(ErrorCode::kBackendFailure, "fake-ocr failed");
    const auto backend_failure = session.run_ocr(frame.frame, &ocr, OcrRequest{});
    ASSERT_EQ(backend_failure.status().code(), ErrorCode::kBackendFailure);
    expect_no_marker(backend_failure.status().message(), "kBackendFailure message");
    ocr.next_status = Status::success();

    ExecutionContext cancelled;
    cancelled.is_cancelled = [] { return true; };
    const auto cancelled_ocr = session.run_ocr(frame.frame, &ocr, OcrRequest{}, cancelled);
    ASSERT_EQ(cancelled_ocr.status().code(), ErrorCode::kCancelled);
    expect_no_marker(cancelled_ocr.status().message(), "cancelled run_ocr message");
}

/// OCR success with marker text: the backend marker text reaches the caller
/// unmodified, proving the marker really flows through the pipeline.
void check_ocr_success(PerceptionSession& session, FakeOcrBackend& ocr, const TestFrame& frame) {
    ocr.next_regions = {marker_ocr_region()};
    const auto texts = session.run_ocr(frame.frame, &ocr, OcrRequest{});
    ASSERT_TRUE(texts.ok());
    ASSERT_EQ(texts.value().size(), 1U);
    EXPECT_TRUE(contains(texts.value()[0].utf8_text, kOcrMarker));
}

/// Fusion success: the markers surface only as snapshot text (the in-contract
/// data body), and the session publishes.
void check_snapshot_delivers_markers(PerceptionSession& session, const TestFrame& frame) {
    const auto fused = session.fuse(frame.frame, marker_evidence());
    ASSERT_TRUE(fused.ok());
    const SemanticSnapshot& snapshot = fused.value();
    ASSERT_FALSE(snapshot.regions.empty());
    const std::string& region_text = snapshot.regions[0].text;
    EXPECT_TRUE(contains(region_text, kOcrMarker));
    EXPECT_TRUE(contains(region_text, kAccessibilityMarker))
        << "sanity check failed: markers must reach the snapshot text (the data body)";
    EXPECT_NE(session.latest_snapshot(), nullptr);
}

/// FusionTrace (RegionTrace/AssociationObservation/ConfidenceContribution) is
/// diagnostic-only output: the structural walk must collect zero strings.
void check_trace_has_no_strings(const TestFrame& frame) {
    const auto engine_output = mirador::fuse_evidence(marker_evidence(), frame.frame);
    ASSERT_TRUE(engine_output.ok());
    EXPECT_TRUE(collect_trace_strings(engine_output.value().trace).empty())
        << "fusion trace must not carry string payloads";
}

TEST(Privacy, StatusAndTraceDoNotLeakEvidenceText) {
    PerceptionSession session = make_session();
    FakeOcrBackend ocr;
    const TestFrame frame = make_frame(100, 80, 1U);

    check_ocr_success(session, ocr, frame);
    check_snapshot_delivers_markers(session, frame);
    check_trace_has_no_strings(frame);
    check_fuse_failure_paths(session, frame);
    check_ocr_failure_paths(session, frame, ocr);
}

}  // namespace
