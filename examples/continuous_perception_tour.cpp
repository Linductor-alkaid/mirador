// M9-04 (DEC-026 sections 3/5): runtime-free example of the continuous-
// perception cycle loop. A caller drives ContinuousPerception with the
// reference latest-pending policy — one in-flight work set plus one latest
// pending frame, newer captures replacing the pending slot — over a tiny
// synthetic RGBA frame. The tour shows the near-zero-cost reuse path
// (static frames re-verified by exact content digests issue no work), the
// content-mismatch invalidation when a region's pixels change under a
// fingerprint-stable frame, whole-batch accept with duplicate rejection,
// and the source-reset ending old-generation tokens. No backend, no model,
// no threads, nothing persisted.

#include <mirador/continuous_perception.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>
#include <mirador/semantic_snapshot.hpp>
#include <mirador/transform.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using mirador::AttentionCondition;
using mirador::AttentionPolicy;
using mirador::ContinuousPerception;
using mirador::ContinuousPerceptionOptions;
using mirador::CoordinateSpaceId;
using mirador::CoverageState;
using mirador::ExecutionContext;
using mirador::Frame;
using mirador::PixelFormat;
using mirador::RectF;
using mirador::SemanticSnapshot;
using mirador::VisualRegion;
using mirador::WorkAcceptReport;
using mirador::WorkPlan;
using mirador::WorkRejectionReason;
using mirador::WorkToken;

constexpr int kWidth = 48;
constexpr int kHeight = 64;
constexpr size_t kPixelBytes = static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4U;

/// Fills the pixel buffer and binds a non-owning view of it into a Frame.
[[nodiscard]] Frame make_frame(std::vector<std::uint8_t>* pixels, std::uint64_t sequence) {
    pixels->assign(kPixelBytes, 235U);
    Frame frame;
    frame.image.data = reinterpret_cast<const std::byte*>(pixels->data());
    frame.image.width = kWidth;
    frame.image.height = kHeight;
    frame.image.row_stride_bytes = static_cast<int64_t>(kWidth) * 4;
    frame.image.format = PixelFormat::kRgba8;
    frame.sequence = sequence;
    return frame;
}

void paint(std::vector<std::uint8_t>* pixels, const RectF& region, std::uint8_t value) {
    const auto x0 = static_cast<int>(region.x);
    const auto y0 = static_cast<int>(region.y);
    const auto x1 = static_cast<int>(region.x + region.width);
    const auto y1 = static_cast<int>(region.y + region.height);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(kWidth) + static_cast<size_t>(x)) * 4U;
            (*pixels)[index] = value;
        }
    }
}

[[nodiscard]] SemanticSnapshot snapshot_with_region(std::uint64_t generation, const RectF& bounds,
                                                    std::uint64_t stable_id) {
    SemanticSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.coordinate_space = CoordinateSpaceId::kOriented;
    VisualRegion region;
    region.stable_id = stable_id;
    region.bounds = bounds;
    snapshot.regions.push_back(region);
    return snapshot;
}

[[nodiscard]] std::vector<WorkToken> tokens_of(const WorkPlan& plan) {
    std::vector<WorkToken> tokens;
    tokens.reserve(plan.work.size());
    for (const mirador::PlannedWorkBinding& binding : plan.work) {
        tokens.push_back(binding.token);
    }
    return tokens;
}

void print_plan(const char* label, const WorkPlan& plan) {
    std::printf("%s: work=%zu reuse=%zu invalidations=%zu replaced=%llu coverage=%s resync=%d\n", label,
                plan.work.size(), plan.reuse.size(), plan.invalidations.size(),
                static_cast<unsigned long long>(plan.replaced_inflight),
                plan.coverage == CoverageState::kComplete ? "complete" : "incomplete", plan.resync_required ? 1 : 0);
}

void print_accept(const char* label, const WorkAcceptReport& report) {
    std::printf("%s: committed=%d adopted=%llu\n", label, report.committed ? 1 : 0,
                static_cast<unsigned long long>(report.adopted_count));
}

}  // namespace

int main() {
    const ExecutionContext context{};
    ContinuousPerception coordinator =
        ContinuousPerception::create(ContinuousPerceptionOptions{.source_id = "tour-display"}).take_value();

    const RectF focus_roi{0.0F, 0.0F, 24.0F, 24.0F};
    AttentionPolicy policy;
    AttentionCondition focus;
    focus.roi = focus_roi;
    focus.roi_space = CoordinateSpaceId::kOriented;
    policy.conditions.push_back(focus);
    std::printf("policy engaged: %s (revision %llu)\n",
                coordinator.set_attention_policy(policy).ok() ? "ok" : "rejected",
                static_cast<unsigned long long>(coordinator.policy_revision()));

    // Initial fill cycle: the focus region is analyzed and accepted; with
    // bootstrap unauthorized, everything else stays uncovered.
    std::vector<std::uint8_t> fill_pixels;
    const Frame fill = make_frame(&fill_pixels, 1U);
    paint(&fill_pixels, focus_roi, 40U);
    const SemanticSnapshot snapshot = snapshot_with_region(7U, focus_roi, 900U);
    const WorkPlan fill_plan = coordinator.plan_cycle(snapshot, fill, {}, 0U, context).value();
    print_plan("fill", fill_plan);
    print_accept("fill", coordinator.accept_work(tokens_of(fill_plan), fill, context).value());

    // Static cycle: identical pixels — the exact re-digests verify the
    // coverage, the focus candidate is suppressed into reuse and no work is
    // issued (AC9-03 mechanism).
    std::vector<std::uint8_t> same_pixels;
    const Frame same = make_frame(&same_pixels, 2U);
    paint(&same_pixels, focus_roi, 40U);
    const WorkPlan static_plan = coordinator.plan_cycle(snapshot, same, {}, 0U, context).value();
    print_plan("static", static_plan);

    // Content drift under a fingerprint-stable frame: the exact re-digest
    // catches the single-region change, invalidates the entry and re-issues
    // work for it (DEC-026 section 3).
    std::vector<std::uint8_t> drifted_pixels;
    const Frame drifted = make_frame(&drifted_pixels, 3U);
    paint(&drifted_pixels, focus_roi, 41U);
    const WorkPlan drift_plan = coordinator.plan_cycle(snapshot, drifted, {}, 0U, context).value();
    print_plan("drift", drift_plan);
    for (const mirador::WorkInvalidation& invalidation : drift_plan.invalidations) {
        std::printf("  invalidation: reason=%d global=%d\n", static_cast<int>(invalidation.reason),
                    invalidation.global ? 1 : 0);
    }
    const std::vector<WorkToken> drifted_tokens = tokens_of(drift_plan);
    print_accept("drift", coordinator.accept_work(drifted_tokens, drifted, context).value());

    // Duplicate submission: the same tokens again are rejected whole-batch
    // with kDuplicateSubmission and the counter moves (DEC-026 section 5).
    const WorkAcceptReport duplicate = coordinator.accept_work(drifted_tokens, drifted, context).value();
    const std::optional<WorkRejectionReason> first_rejection =
        duplicate.item_rejections.empty() ? std::nullopt : duplicate.item_rejections.front();
    const int first_reason = first_rejection.has_value() ? static_cast<int>(*first_rejection) : -1;
    std::printf(
        "duplicate: committed=%d first-reason=%d count=%llu\n", duplicate.committed ? 1 : 0, first_reason,
        static_cast<unsigned long long>(coordinator.rejection_count(WorkRejectionReason::kDuplicateSubmission)));

    // Source rebuild: the reset ends the epoch and empties the coverage; the
    // next cycle reports resync_required once and re-issues the focus work
    // under the new epoch (DEC-026 section 8).
    static_cast<void>(coordinator.notify_source_reset());
    const WorkPlan after_reset = coordinator.plan_cycle(snapshot, drifted, {}, 0U, context).value();
    print_plan("after-reset", after_reset);
    return 0;
}
