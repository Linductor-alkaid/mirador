#ifndef MIRADOR_TRACKER_BACKEND_HPP
#define MIRADOR_TRACKER_BACKEND_HPP

#include <mirador/backend_info.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/result.hpp>

#include <memory>
#include <string>

namespace mirador {

/// Experimental (M7, DEC-019/DEC-020): fields and signatures may change within
/// M7 until the contract freeze is approved (the DEC-017/DEC-018 promotion
/// pattern, draft DEC-022); the contract is not covered by the compatibility
/// promise until then.

// Stateful tracker Backend SPI (DEC-020, field and error semantics frozen at
// M7-11). Unlike OcrBackend/DetectorBackend (DEC-012: stateless single-shot,
// "same input same result" to support capability-result caching), a deep
// tracker keeps template/filter state across calls. The contract below freezes
// how statefulness fits the existing SPI rules; every decision is normative
// for implementations and for the fusion-side orchestration (M7-13).
//
// 1. Handle-based three-stage lifecycle (DEC-020 decision 1): one backend
//    implementation serves many tracked targets, one session per target.
//      info() -> initialize(prepared_image, request, context)
//            -> Result<std::unique_ptr<TrackerSession>>   (stage 1)
//      update(prepared_image, request, context) x N       (stage 2)
//      ~TrackerSession()                                  (stage 3: discard)
//    There are no other state transitions on this SPI. Re-initialization
//    means building a NEW session; an existing session is never restarted or
//    re-targeted in place. The caller must keep the owning TrackerBackend
//    alive for the whole lifetime of every session it created (the handle
//    never extends the backend's lifetime); destroying the backend before its
//    sessions is the caller's lifetime error. Sessions of one backend are
//    independent: one session's calls, failure or teardown never observe or
//    mutate another session's state.
//
// 2. State ownership and lifetime (DEC-020 decision 2): all tracking state
//    (templates, filters, motion models) belongs to the backend session;
//    destroying the TrackerSession is the explicit discard. The fusion side
//    holds the handle alongside its TargetTrack — as pool-side parallel
//    single-slot storage (the frozen M7-05 kStructureBaselineOverheadBytes /
//    M7-06 kStateSlotOverheadBytes / M7-08 kRedetectSlotOverheadBytes
//    pattern; the M7-01-frozen TargetTrack layout stays untouched). At most
//    one handle slot exists per track; a session rebuild replaces the handle
//    in place (destroy old, store new). The slot's byte overhead is accounted
//    in ObjectTracker::byte_size() and bounded by pool_budget_bytes
//    (RULE-06); an insertion that cannot fit fails explicitly with
//    kBudgetExceeded; the handle — and with it the session state — is
//    destroyed on track termination, pool eviction and reset. (The slot
//    overhead constant itself lands with the M7-13 injection wiring; THIS
//    header freezes the ownership decision.)
//
// 3. Failure is visible, never stale (DEC-020 decision 2): implementations
//    never fabricate a result and never return a cached/stale answer for a
//    failed frame. Errors map to the frozen RULE-08 vocabulary:
//      - kCancelled / kTimeout (from `context`): the session stays fully
//        usable — retry the same handle on a later frame.
//      - kInvalidArgument / kUnsupportedFormat: caller error; the session is
//        untouched and stays usable.
//      - kBackendFailure (DEC-020's "kBackendFailed" semantics — the frozen
//        ErrorCode enumeration spells it kBackendFailure, status.hpp): the
//        call failed or the session detected state corruption. The session
//        must be treated as unusable: destroy it; recovery is a fresh
//        initialize (typically on the current frame's confirmed bounds). The
//        fusion side degrades the corresponding track to kUncertain through
//        the existing frozen primitives and may rebuild the session (the
//        M7-13 wiring; RISK-2026-18 hook — no new ObjectTracker API).
//    All-or-nothing update: a call either fully succeeds (session state
//    advanced AND result returned) or leaves the session state exactly as it
//    was before the call. Implementations buffer the per-frame result and
//    commit it to the session state only on success — this is what makes the
//    error paths above stateless to reason about.
//
// 4. Synchronous boundary, cancellation ordering (RULE-03, no exemption;
//    DEC-020 decision 3): initialize/update are synchronous calls taking
//    `const ExecutionContext&`. Implementations must observe cancellation and
//    deadline and report kCancelled/kTimeout explicitly (never silently
//    swallow them), polling at regular intervals during long work. Backends
//    never create threads, thread pools or timers; a model runtime's internal
//    asynchronous facilities must complete before the synchronous call
//    returns (AGENTS.md sync boundary). Whether one backend instance may be
//    called concurrently from several threads (several handles in flight) is
//    declared solely by BackendInfo::thread_safe; calls on ONE handle are
//    strictly serial — concurrent update calls on the same session are the
//    caller's error.
//    FROZEN ORDERING: structural validation takes precedence over
//    cancellation — a malformed request reports kInvalidArgument even when
//    the context is already cancelled. This is the M7-05 verify_track /
//    M7-06 commit_track_evidence pool precedent (object_tracker.hpp), the
//    deliberate contrast to the M7-02 adopt_track cancel-first entry: the
//    fusion side consumes this SPI next to those pool entries, and a
//    malformed request is a caller programming error that must surface
//    regardless of cancellation. Cancellation and deadline are checked after
//    validation at entry, then polled during work.
//
// 5. Cache exemption (DEC-020 decision 4): tracking session state never
//    enters the capability result cache. RULE-07's key invariant ("same
//    input same result") does NOT hold for update — its result depends on
//    the frame sequence — so cache layers must not intercept, hit or reuse
//    initialize/update calls (see the DEC-012 scope note in that decision).
//    The only cacheable products are deterministic initialize pre-processing
//    artifacts (e.g. prepared template tensors), keyed per RULE-07 over
//    every input the product derives from — backend name, implementation
//    version, model id/revision, a digest of the request parameters AND a
//    content digest of the prepared image (the artifact is derived from it;
//    a template cache keyed without the image digest serves stale templates,
//    the same reason CapabilityKeyFields carries image_fingerprint,
//    capability_cache.hpp, design section 12).
//
// 6. Coordinates and input (DEC-012, same contract): all bounds — the
//    request priors and the returned results — live in `prepared_image`
//    pixel space ([0, w) x [0, h), as presented; `ImageView::rotation` is
//    caller-side metadata the backend never interprets). The caller recovers
//    results to its output space through its own Transform2D chain (RULE-05,
//    with the standard round-trip tolerance rules, DOD-03). The prepared
//    format must be one of info().accepted_formats (callers pick the target
//    format from that declaration; an undeclared format is rejected with
//    kUnsupportedFormat, never silently converted). Implementations never
//    modify the input pixels and never take ownership of them.
//
// 7. Identity (DEC-020 decision 6): BackendInfo is reused verbatim
//    (name/implementation_version/model_id/model_revision, accepted_formats,
//    thread_safe). It feeds diagnostics, benchmark attribution and
//    supply-chain registration; info() reports the currently loaded model
//    state and never mutates anything. Model/runtime ownership follows
//    DEC-012: the implementation owns model loading and inference; no
//    runtime type ever crosses this boundary (DEC-002).
//
// 8. Determinism framing: per-call "same input same result" explicitly does
//    NOT hold for update (the DEC-012 scope note). Per-SEQUENCE determinism
//    does, and is required: replaying the identical sequence of update calls
//    over identical frames on one session produces bit-identical results,
//    and two sessions built from identical initialize inputs and fed the
//    identical frame sequence produce identical result sequences (no
//    wall-clock, no randomness, no hidden global state — RULE-03).
//
// 9. Privacy (RULE-10/DOD-06): sessions live in memory only — no
//    filesystem writes, no network access, and no template content, pixel
//    content or frame dumps in logs; diagnostics are ids, counts and short
//    Status messages. Destruction discards everything.

/// Initialization request of one tracking session (M7-11 field freeze,
/// DEC-020 decision 1). The Mirador pipeline consumes none of these fields —
/// unlike OcrRequest/DetectionRequest there is no roi/output_space/cache
/// layering here: the caller prepares the view (crop to the search
/// neighborhood, format per accepted_formats) and owns coordinate recovery,
/// so every field below is backend-consumed.
struct TrackerInitRequest {
    /// Target bounds to lock onto, in prepared-image pixel space: finite,
    /// positive area, fully inside `prepared_image`. The backend builds its
    /// initial template from this region.
    RectF initial_bounds;
    /// Opaque implementation-private parameters (the DEC-012 backend_params
    /// trade: string, not type-safe, so implementations evolve without a
    /// public ABI break). Part of the RULE-07 parameter digest when an
    /// implementation caches initialize pre-processing artifacts.
    std::string backend_params;
};

/// Per-frame update request of one tracking session (M7-11 field freeze).
struct TrackerUpdateRequest {
    /// The caller's position prior for this frame, in prepared-image pixel
    /// space: finite, positive area, fully inside `prepared_image`. Typically
    /// the tracked object's current fusion-side estimate (e.g. the track
    /// bounds, motion-compensated) mapped into this prepared view. The
    /// backend searches around this prior; the session's internal state
    /// contributes the appearance model only — the position estimate comes
    /// from the caller on every call (design section 6.2: update is called
    /// with the track's current bounds). The backend never fabricates a
    /// position prior of its own from wall-clock or call counting.
    RectF prior_bounds;
    /// Opaque implementation-private parameters, as in TrackerInitRequest.
    std::string backend_params;
};

/// Deterministic outcome of one update call (M7-11 field freeze): the
/// tracked target's new bounds and the backend's appearance confidence.
/// Consumers treat both as untrusted backend output (DEC-021): implementations
/// report finite bounds with positive area inside the prepared image and a
/// confidence in [0, 1]; the pipeline validates at its evidence-adoption
/// points and rejects instead of clamping.
struct TrackerUpdateResult {
    /// Tracked target bounds in prepared-image pixel space (DEC-012
    /// coordinate contract; the caller recovers them to its output space).
    RectF bounds;
    /// Backend appearance confidence in [0, 1]; 1 means maximal confidence.
    /// Not an EvidenceGrade — mapping it onto track evidence is the fusion
    /// side's frozen decision domain (M7-13 combination rules).
    float confidence = 0.0F;

    /// Component equality (test convenience, DetectionRegion precedent).
    [[nodiscard]] friend bool operator==(const TrackerUpdateResult& lhs, const TrackerUpdateResult& rhs) noexcept {
        return lhs.bounds == rhs.bounds && lhs.confidence == rhs.confidence;
    }
};

/// One tracking session: the handle that owns all backend-side state for
/// exactly one tracked target (DEC-020 decision 1). Obtained from
/// TrackerBackend::initialize; destroyed by the caller to discard the state
/// (stage 3). Move-only through its unique_ptr; never copied, never
/// reopened. Abstract on purpose: implementations live behind this SPI, in
/// caller-provided backends (adapters/, integrations/ or test fakes) —
/// mirador-core ships only this interface, so the core link closure is
/// unchanged (DEC-020 decision 6).
class TrackerSession {
public:
    virtual ~TrackerSession() = default;
    TrackerSession() = default;
    TrackerSession(const TrackerSession&) = delete;
    TrackerSession& operator=(const TrackerSession&) = delete;

    /// Advances the session by one frame and returns the tracked target's
    /// new bounds and confidence. Synchronous (contract block, decision 3/4):
    /// validates, then honors `context` (kCancelled/kTimeout reported
    /// explicitly, polled during work), then executes; the session state
    /// commits only on success (all-or-nothing, decision 3).
    ///
    /// Errors: kInvalidArgument for an invalid `prepared_image` (the
    /// validate(const ImageView&) rule set) or a `request.prior_bounds` that
    /// is non-finite, empty or not fully inside the prepared image;
    /// kUnsupportedFormat when the prepared format is not declared in
    /// info().accepted_formats; kBackendFailure on execution failure or
    /// detected session-state corruption (destroy this session; rebuild via
    /// a fresh initialize); kCancelled/kTimeout from `context` (the session
    /// stays usable). On any error the session state is exactly as before
    /// the call. Never throws.
    [[nodiscard]] virtual Result<TrackerUpdateResult> update(const ImageView& prepared_image,
                                                             const TrackerUpdateRequest& request,
                                                             const ExecutionContext& context) = 0;
};

/// Synchronous stateful tracker Backend SPI (DEC-020; design section 2 row D
/// and section 6.2 deep-evidence channel). The contract block at the top of
/// this header is normative; the reference consumer is the fusion-side deep
/// channel wiring (M7-13), the reference implementation the NanoTrack
/// deep-tracker backend (M7-12, integrations/, DEC-015) — both behind this
/// interface.
class TrackerBackend {
public:
    virtual ~TrackerBackend() = default;

    /// Capability and identity query (contract block, decision 7). Same
    /// BackendInfo type and validate() gating as the DEC-012 SPIs.
    [[nodiscard]] virtual BackendInfo info() const = 0;

    /// Initializes one tracking session: locks onto `request.initial_bounds`
    /// in `prepared_image` and builds the backend-side template state
    /// (DEC-020 decision 1, stage 1). One backend implementation may hold
    /// many sessions concurrently (thread-safety per BackendInfo::thread_safe;
    /// calls on one session are strictly serial). Returns the session
    /// handle; destroying it discards the state (contract block, decision 2).
    ///
    /// Errors: kInvalidArgument for an invalid `prepared_image` or a
    /// `request.initial_bounds` that is non-finite, empty or not fully inside
    /// the prepared image; kUnsupportedFormat for an undeclared prepared
    /// format; kBackendFailure when the implementation fails to build the
    /// session (model not loaded, runtime failure, template construction
    /// failed) — no handle exists in that case; kCancelled/kTimeout from
    /// `context`. Validation takes precedence over cancellation (frozen
    /// ordering, contract block decision 4). Never throws.
    [[nodiscard]] virtual Result<std::unique_ptr<TrackerSession>> initialize(const ImageView& prepared_image,
                                                                             const TrackerInitRequest& request,
                                                                             const ExecutionContext& context) = 0;
};

}  // namespace mirador

#endif  // MIRADOR_TRACKER_BACKEND_HPP
