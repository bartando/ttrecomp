#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestTranslatedReplayTokenContext;
}

namespace tabletennis::native {

struct MainCoverageFrameSnapshot;

enum class TileInvarianceClass : uint8_t {
  kMissing = 0,
  kInvariant,
  kMechanicalTile,
  kSemantic,
};

enum class Ps328NormalizationRejectReason : uint8_t {
  kNone = 0,
  kMissingInstances,
  kInvalidConstants,
  kUnexpectedSystemConstantDelta,
  kInvalidViewport,
  kDepthTransformMismatch,
  kInvalidScissor,
  kScissorOutsideOutput,
  kScissorOverlap,
  kScissorCoverageMismatch,
  kLocalStripShapeMismatch,
  kRemainingExtentMismatch,
  kDerivedTileOriginMismatch,
  kAffineTransformMismatch,
  kInvalidCanonicalTransform,
};

struct Ps328NormalizedReplayState {
  static constexpr uint32_t kOutputWidth = 1280;
  static constexpr uint32_t kOutputHeight = 720;

  uint32_t output_width = 0;
  uint32_t output_height = 0;
  std::array<float, 6> viewport{};
  std::array<int32_t, 2> scissor_offset{};
  std::array<uint32_t, 2> scissor_extent{};
  std::array<float, 3> ndc_scale{};
  std::array<float, 3> ndc_offset{};
  std::vector<uint8_t> system_constants;
  bool valid = false;
};

struct Ps328TileInvarianceSnapshot {
  uint64_t sequence = 0;
  uint32_t selected_ordinal = 0;
  uint32_t matching_instance_count = 0;
  TileInvarianceClass system_constants = TileInvarianceClass::kMissing;
  TileInvarianceClass vertex_float_constants = TileInvarianceClass::kMissing;
  TileInvarianceClass pixel_float_constants = TileInvarianceClass::kMissing;
  TileInvarianceClass bool_loop_constants = TileInvarianceClass::kMissing;
  TileInvarianceClass fetch_constants = TileInvarianceClass::kMissing;
  TileInvarianceClass resources = TileInvarianceClass::kMissing;
  TileInvarianceClass viewport_ndc_scissor = TileInvarianceClass::kMissing;
  TileInvarianceClass index_and_dynamic_state = TileInvarianceClass::kMissing;
  TileInvarianceClass attachments = TileInvarianceClass::kMissing;
  // Bit diagnostic for fail-closed resource comparison: 0 evidence,
  // 1 pipeline/layout, 2 shader modifications, 3 set presence, 4 textures,
  // 5 samplers, 6 persistent shared-memory/EDRAM set.
  uint32_t resource_mismatch_mask = 0;
  bool exact_dynamic_selection = false;
  bool three_instances_exact = false;
  bool semantic_difference = false;
  bool mechanical_normalization_required = false;
  Ps328NormalizationRejectReason normalization_reason =
      Ps328NormalizationRejectReason::kMissingInstances;
  Ps328NormalizedReplayState normalized_state;
  // Mechanical differences are deliberately not treated as replay-safe until
  // a separate output-sized untiled normalization proves them.
  bool one_copy_replay_ready = false;
  // First exact tile instance for the dynamically selected logical draw.
  // Opaque backend resources remain frame-scoped; consumers must still check
  // the token frame and resource-stability flag before replay.
  std::shared_ptr<const rex::graphics::NativeGuestTranslatedReplayTokenContext>
      selected_token;

  bool valid() const {
    return sequence != 0 && selected_ordinal != 0 && exact_dynamic_selection &&
           three_instances_exact;
  }
};

enum class Ps328FamilyAuthorizationRejectReason : uint8_t {
  kNone = 0,
  kInvalidCoverage,
  kNoLogicalDraws,
  kMissingTokenFrame,
  kTokenCountMismatch,
  kMissingAuthoritativeAssignment,
  kAmbiguousOrder,
  kTokenIdentityMismatch,
  kUnsafeMember,
};

// Proven source of the title-side identity used to authorize one logical
// shader-pair member. The coverage-tail case is deliberately narrow: it is
// only valid as the final four-index member after a non-empty venue prefix.
enum class Ps328FamilyMemberAuthority : uint8_t {
  kMissing = 0,
  kVenueTitlePayload,
  kMainCoverageTrailingShaderPair,
};

// One logical PS328 draw in exact title/backend callback order. This is
// observer evidence only: consumers must require the enclosing family
// snapshot to be valid before using any member as prior-frame authorization.
struct Ps328FamilyAuthorizedDraw {
  uint32_t family_offset = 0;
  uint32_t original_ordinal = 0;
  uint32_t catalog_draw_index = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t physical_index_base = 0;
  Ps328FamilyMemberAuthority authority = Ps328FamilyMemberAuthority::kMissing;
  Ps328NormalizedReplayState normalized_state;
  std::shared_ptr<const rex::graphics::NativeGuestTranslatedReplayTokenContext>
      tile1_token;
  uint32_t resource_mismatch_mask = 0;
  Ps328NormalizationRejectReason normalization_reason =
      Ps328NormalizationRejectReason::kMissingInstances;
  bool one_copy_replay_ready = false;

  // Verifies the member's copied identity against its tile-1 backend token
  // and canonical full-output normalization. Frame identity is checked by the
  // enclosing snapshot.
  bool valid() const;
  bool matches_identity(uint32_t ordinal, uint32_t candidate_primitive_type,
                        uint32_t candidate_submitted_index_count,
                        uint32_t candidate_physical_index_base) const;
};

struct Ps328FamilyAuthorizationSnapshot {
  uint64_t sequence = 0;
  uint32_t logical_draw_count = 0;
  uint32_t observed_token_count = 0;
  uint32_t authorized_draw_count = 0;
  Ps328FamilyAuthorizationRejectReason reject_reason =
      Ps328FamilyAuthorizationRejectReason::kInvalidCoverage;
  uint32_t first_rejected_family_offset = 0;
  uint32_t first_rejected_ordinal = 0;
  std::vector<Ps328FamilyAuthorizedDraw> draws;

  // Revalidates the complete ordered vector instead of trusting producer
  // counters: exact 3N evidence, contiguous family offsets, increasing title
  // ordinal/catalog order, member identity, and same-sequence tile-1 tokens.
  bool valid() const;
  // Private batch consumption additionally requires every tile-1 token's
  // guarded backend resources to remain stable for deferred replay.
  bool private_batch_ready() const;
  // Fail-closed indexed access for a private batch coordinator. No member of
  // a partial, stale, reordered, or resource-unstable family is exposed.
  uint32_t private_batch_draw_count() const;
  const Ps328FamilyAuthorizedDraw *
  private_batch_draw(uint32_t family_offset) const;
};

bool FilterPs328TileInvarianceToken(uint64_t vertex_shader_hash,
                                    uint64_t pixel_shader_hash);
void ObservePs328TileInvarianceToken(
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &context);
void EvaluatePs328TileInvariance(const MainCoverageFrameSnapshot &coverage);
std::shared_ptr<const Ps328TileInvarianceSnapshot>
LatestPs328TileInvarianceSnapshot();
std::shared_ptr<const Ps328FamilyAuthorizationSnapshot>
LatestPs328FamilyAuthorizationSnapshot();
// Retains the newest snapshot that passed the complete family contract even
// when later frames publish rejected diagnostic snapshots. Consumers must
// still prove and use current-frame tokens; retained tokens are evidence only.
std::shared_ptr<const Ps328FamilyAuthorizationSnapshot>
LatestValidPs328FamilyAuthorizationSnapshot();
// One-way family-local retirement gate, armed automatically when evaluation
// retains a complete replay-safe PS328 authorization (independent of private
// replay enablement). It stops new PS328 title/artifact proof construction
// without disabling MAIN coverage, other family learners, or current backend
// token capture/query/replay.
bool TryRetirePs328TitleCapture();
bool Ps328TitleCaptureRetired();
// Returns the first guarded PS328 token in exactly backend_frame_sequence
// whose local strip/pipeline shape matches a prior finalized proof. No stale
// proof token is returned or replayed.
std::shared_ptr<const rex::graphics::NativeGuestTranslatedReplayTokenContext>
FindGuardedPs328TokenForFrame(
    uint64_t backend_frame_sequence,
    const Ps328TileInvarianceSnapshot &prior_authorization);
std::shared_ptr<const rex::graphics::NativeGuestTranslatedReplayTokenContext>
PrepareNormalizedPs328TokenForFrame(
    uint64_t backend_frame_sequence,
    const Ps328TileInvarianceSnapshot &prior_authorization);
std::shared_ptr<const rex::graphics::NativeGuestTranslatedReplayTokenContext>
NormalizeAuthorizedPs328FamilyToken(
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &current,
    const Ps328FamilyAuthorizedDraw &prior_authorization,
    uint64_t backend_frame_sequence);
const char *TileInvarianceClassName(TileInvarianceClass classification);
const char *
Ps328NormalizationRejectReasonName(Ps328NormalizationRejectReason reason);
const char *Ps328FamilyAuthorizationRejectReasonName(
    Ps328FamilyAuthorizationRejectReason reason);
void ResetPs328TileInvariance();

} // namespace tabletennis::native
