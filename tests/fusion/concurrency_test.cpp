// M5-07 concurrency matrix (AGENTS.md sync API boundary, design section 18):
// published SemanticSnapshots are immutable and readers keep their shared_ptr
// alive across later fusions (perception_session.hpp contract comment), while
// independent sessions run full analyze_change -> run_ocr -> fuse pipelines in
// parallel. Run under the TSAN preset: any report here is a finding.
//
// Reader discipline (design section 18): reader threads only dereference a
// shared_ptr copied BEFORE the thread starts (thread creation establishes the
// happens-before edge). They never call latest_snapshot() — copying that member
// while a fuse publishes races by contract — and never touch the session. All
// gtest assertions stay on the main thread; worker threads only fill plain
// observation structs.
//
// Known boundary (not faked here): CapabilityResultCache and FrameCache are
// explicitly not thread-safe, and PerceptionSession::create() offers no way to
// inject a caller-shared cache instance, so a cross-session shared-cache TSAN
// case is not expressible against the current API even though AGENTS.md reserves
// that pattern ("跨 session 共享缓存必须由调用方显式传入"). This matrix grows the day
// a cache-injection API lands; do not simulate it by sharing the sessions'
// internal caches behind their back.

#include <mirador/perception_session.hpp>

#include "fake_backends.hpp"

#include <mirador/evidence.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/status.hpp>
#include <mirador/transform.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mirador::CoordinateSpaceId;
using mirador::EvidenceSet;
using mirador::ExternalRegion;
using mirador::Frame;
using mirador::OcrRequest;
using mirador::PerceptionSession;
using mirador::PerceptionSessionOptions;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::Rotation;
using mirador::SemanticSnapshot;
using mirador::test::FakeOcrBackend;

constexpr const char* kSourceId = "concurrency-source";
constexpr int kReaderThreads = 4;
constexpr int kReaderIterations = 600;
constexpr int kFusionBumps = 20;
constexpr int kPipelineRounds = 20;

struct TestFrame {
    std::vector<std::byte> bytes;
    Frame frame;
};

TestFrame make_frame(int32_t width, int32_t height, uint64_t sequence, const char* source_id = kSourceId) {
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
    image.frame.source_id = source_id;
    image.frame.owner = std::make_shared<const std::vector<std::byte>>(image.bytes);
    return image;
}

PerceptionSession make_session(const char* source_id = kSourceId) {
    PerceptionSessionOptions options;
    options.source_id = source_id;
    return PerceptionSession::create(std::move(options)).take_value();
}

/// One accessibility-style evidence item; `x` slides the box between two
/// far-apart positions so every fuse allocates a fresh stable id and bumps the
/// snapshot generation (retained fraction 0/1 is below the DEC-010 ratio).
EvidenceSet single_box_evidence(float x) {
    EvidenceSet evidence;
    ExternalRegion region;
    region.bounds = RectF{x, x, 8.0F, 8.0F};
    region.text = "tile";
    region.role = "button";
    region.confidence = 1.0F;
    region.interactive = true;
    (void)evidence.add_external(region, CoordinateSpaceId::kOriented);
    return evidence;
}

// --- Snapshot readers vs later fusions --------------------------------------------------

/// Per-reader observation record; assertions run on the main thread only.
struct ReaderObservation {
    bool data_intact = true;
    bool generation_current = true;
};

void read_snapshot_forever(const SemanticSnapshot& expected, const std::shared_ptr<const SemanticSnapshot>& snapshot,
                           ReaderObservation* observation) {
    const uint64_t expected_id = expected.regions[0].stable_id;
    for (int i = 0; i < kReaderIterations; ++i) {
        // The full read surface of a published snapshot: regions, find_region,
        // is_generation_current. The pointee must never mutate under the
        // reader, no matter how many generations the session publishes.
        if (snapshot->regions.size() != expected.regions.size()) {
            observation->data_intact = false;
            return;
        }
        const mirador::VisualRegion* found = mirador::find_region(*snapshot, expected_id);
        if (found == nullptr || !(found->bounds == expected.regions[0].bounds) || found->text != "tile") {
            observation->data_intact = false;
            return;
        }
        if (!mirador::is_generation_current(*snapshot, snapshot->generation)) {
            observation->generation_current = false;
            return;
        }
    }
    if (!(*snapshot == expected)) {
        observation->data_intact = false;
    }
}

TEST(PerceptionSessionConcurrency, SnapshotReadersSurviveLaterFusions) {
    PerceptionSession session = make_session();
    const TestFrame frame = make_frame(100, 80, 1U);

    ASSERT_TRUE(session.analyze_change(frame.frame).ok());
    const auto first = session.fuse(frame.frame, single_box_evidence(0.0F));
    ASSERT_TRUE(first.ok());

    // Publish and share: the shared_ptr handed to the readers is copied before
    // the threads start (happens-before via std::thread construction).
    const std::shared_ptr<const SemanticSnapshot> published = session.latest_snapshot();
    ASSERT_NE(published, nullptr);
    // Immutable value snapshot of the publication: the reference the readers
    // must keep observing no matter how the session evolves.
    const SemanticSnapshot expected = *published;  // NOLINT(performance-unnecessary-copy-initialization)
    ASSERT_EQ(expected.generation, 1U);
    ASSERT_EQ(expected.regions.size(), 1U);

    std::vector<ReaderObservation> observations(static_cast<size_t>(kReaderThreads));
    std::vector<std::thread> readers;
    readers.reserve(static_cast<size_t>(kReaderThreads));
    for (int i = 0; i < kReaderThreads; ++i) {
        readers.emplace_back(read_snapshot_forever, std::cref(expected), std::cref(published),
                             &observations[static_cast<size_t>(i)]);
    }

    // Main thread keeps fusing while the readers hold the old publication.
    // Box positions alternate between two disjoint spots (never the published
    // position at 0, so even the first fuse allocates a fresh stable id): every
    // publication is a fresh stable id, so every fuse bumps the generation.
    for (int i = 0; i < kFusionBumps; ++i) {
        const auto fused = session.fuse(frame.frame, single_box_evidence((i % 2) == 0 ? 60.0F : 0.0F));
        if (!fused.ok()) {
            ADD_FAILURE() << "fuse failed while readers held an older snapshot: " << fused.status().message();
            break;
        }
    }

    for (std::thread& reader : readers) {
        reader.join();
    }

    for (const ReaderObservation& observation : observations) {
        EXPECT_TRUE(observation.data_intact) << "reader saw corrupted or replaced snapshot data";
        EXPECT_TRUE(observation.generation_current) << "reader saw an inconsistent generation";
    }
    // The held publication still observes exactly the first fuse.
    EXPECT_TRUE(*published == expected);
    // latest_snapshot() on the main thread only, after the readers joined.
    const std::shared_ptr<const SemanticSnapshot> latest = session.latest_snapshot();
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->generation, expected.generation + static_cast<uint64_t>(kFusionBumps));
    EXPECT_NE(latest.get(), published.get());
}

// --- Two sessions running full pipelines in parallel ------------------------------------

/// Everything one pipeline run observed; plain data, safe to fill off the main
/// thread and assert on afterwards.
struct PipelineOutcome {
    bool all_ok = true;
    std::string first_error;
    std::vector<uint64_t> first_region_ids;  // leading region's stable id per round
    SemanticSnapshot final_snapshot;         // value copy of the last publication
};

/// One session's deterministic pipeline: analyze_change -> run_ocr -> fuse per
/// round, external accessibility box merged with the OCR text box, plus a
/// far-apart extra box on odd rounds. Touches only thread-local objects: its
/// own session, backend, frame and evidence.
void run_pipeline(PipelineOutcome* outcome) {
    PerceptionSession session = make_session();
    FakeOcrBackend ocr;
    const TestFrame frame = make_frame(100, 80, 1U);
    for (int round = 0; round < kPipelineRounds; ++round) {
        const auto change = session.analyze_change(frame.frame);
        if (!change.ok()) {
            outcome->all_ok = false;
            outcome->first_error = change.status().message();
            return;
        }
        const auto texts = session.run_ocr(frame.frame, &ocr, OcrRequest{});
        if (!texts.ok() || texts.value().size() != 1U) {
            outcome->all_ok = false;
            outcome->first_error = texts.ok() ? "unexpected OCR region count" : texts.status().message();
            return;
        }
        EvidenceSet evidence;
        // Containment-gated merge with the OCR box ({25, 20, 50, 40} for this
        // frame), nudged deterministically per round so stable-id matching
        // works against real geometry churn.
        const float shift = static_cast<float>(round % 3) * 4.0F;
        ExternalRegion button;
        button.bounds = RectF{20.0F + shift, 15.0F + shift, 60.0F, 50.0F};
        button.text = "OK";
        button.role = "button";
        (void)evidence.add_external(button, CoordinateSpaceId::kOriented);
        (void)evidence.add_text(texts.value()[0], CoordinateSpaceId::kOriented);
        if ((round % 2) != 0) {
            ExternalRegion far_away;
            far_away.bounds = RectF{85.0F, 70.0F, 10.0F, 10.0F};
            (void)evidence.add_external(far_away, CoordinateSpaceId::kOriented);
        }
        const auto fused = session.fuse(frame.frame, evidence);
        if (!fused.ok() || fused.value().regions.empty()) {
            outcome->all_ok = false;
            outcome->first_error = fused.ok() ? "fusion produced no regions" : fused.status().message();
            return;
        }
        outcome->first_region_ids.push_back(fused.value().regions[0].stable_id);
        outcome->final_snapshot = fused.value();
    }
}

TEST(PerceptionSessionConcurrency, SessionsRunPipelinesInParallel) {
    // Reference run: the same pipeline single-threaded on its own session.
    // Session state is per-source and the pipelines are deterministic
    // (DEC-012), so the parallel runs must reproduce it exactly.
    PipelineOutcome reference;
    ASSERT_NO_FATAL_FAILURE(run_pipeline(&reference));
    ASSERT_TRUE(reference.all_ok) << reference.first_error;

    PipelineOutcome first;
    PipelineOutcome second;
    std::thread thread_a(run_pipeline, &first);
    std::thread thread_b(run_pipeline, &second);
    thread_a.join();
    thread_b.join();

    ASSERT_TRUE(first.all_ok) << first.first_error;
    ASSERT_TRUE(second.all_ok) << second.first_error;
    EXPECT_EQ(first.first_region_ids, reference.first_region_ids);
    EXPECT_EQ(second.first_region_ids, reference.first_region_ids);

    // Both sessions publish the reference structure: identical snapshot value
    // (operator== covers generation, coordinate space, regions, change report)
    // and stable ids unique within each snapshot (RULE-09).
    const auto check_publication = [&reference](const PipelineOutcome& outcome, const char* label) {
        EXPECT_TRUE(outcome.final_snapshot == reference.final_snapshot)
            << label << " final snapshot diverged from the single-threaded reference";
        const std::vector<mirador::VisualRegion>& regions = outcome.final_snapshot.regions;
        for (size_t i = 0; i < regions.size(); ++i) {
            for (size_t j = i + 1; j < regions.size(); ++j) {
                ASSERT_NE(regions[i].stable_id, regions[j].stable_id)
                    << label << ": stable ids must be unique within a snapshot";
            }
        }
    };
    check_publication(first, "parallel-a");
    check_publication(second, "parallel-b");
}

}  // namespace
