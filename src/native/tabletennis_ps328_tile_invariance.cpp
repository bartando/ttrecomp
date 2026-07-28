#include "native/tabletennis_ps328_tile_invariance.h"

#include "native/tabletennis_guarded_venue_capture_retirement.h"
#include "native/tabletennis_main_coverage_ledger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <ranges>
#include <vector>

#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

namespace tabletennis::native {
namespace {

using ReplayToken = rex::graphics::NativeGuestTranslatedReplayTokenContext;

constexpr uint64_t kPs328VertexShaderHash = 0x0E9982BE6B1E99A1ull;
constexpr uint64_t kPs328PixelShaderHash = 0x328FA02B07C392DCull;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumTokensPerFrame = 512;

struct TokenFrame {
  uint64_t sequence = 0;
  std::vector<ReplayToken> tokens;
};

std::mutex g_mutex;
std::deque<TokenFrame> g_frames;
std::shared_ptr<const Ps328TileInvarianceSnapshot> g_published;
std::shared_ptr<const Ps328FamilyAuthorizationSnapshot> g_family_published;
std::shared_ptr<const Ps328FamilyAuthorizationSnapshot>
    g_latest_valid_family_published;
std::atomic<bool> g_first_texture_mismatch_logged = false;

template <typename T> bool ExactEqual(const T &left, const T &right) {
  return left == right;
}

bool FloatBitsEqual(float left, float right) {
  uint32_t left_bits = 0;
  uint32_t right_bits = 0;
  std::memcpy(&left_bits, &left, sizeof(left_bits));
  std::memcpy(&right_bits, &right, sizeof(right_bits));
  return left_bits == right_bits;
}

template <size_t Count>
bool FloatArrayBitsEqual(const std::array<float, Count> &left,
                         const std::array<float, Count> &right) {
  for (size_t index = 0; index < Count; ++index) {
    if (!FloatBitsEqual(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

TileInvarianceClass CompareSystemConstants(const ReplayToken &first,
                                           const ReplayToken &other) {
  if (!first.constant_contents_valid || !other.constant_contents_valid ||
      first.system_constants.empty() ||
      first.system_constants.size() != other.system_constants.size() ||
      first.system_constants_mechanical_mask.size() !=
          first.system_constants.size() ||
      other.system_constants_mechanical_mask !=
          first.system_constants_mechanical_mask) {
    return TileInvarianceClass::kSemantic;
  }
  bool mechanical_difference = false;
  for (size_t index = 0; index < first.system_constants.size(); ++index) {
    if (first.system_constants[index] == other.system_constants[index]) {
      continue;
    }
    if (!first.system_constants_mechanical_mask[index]) {
      return TileInvarianceClass::kSemantic;
    }
    mechanical_difference = true;
  }
  return mechanical_difference ? TileInvarianceClass::kMechanicalTile
                               : TileInvarianceClass::kInvariant;
}

TileInvarianceClass Merge(TileInvarianceClass left, TileInvarianceClass right) {
  return static_cast<uint8_t>(left) >= static_cast<uint8_t>(right) ? left
                                                                   : right;
}

TileInvarianceClass
CompareFloatConstants(const std::array<uint64_t, 4> &first_usage,
                      const std::vector<uint32_t> &first_values,
                      const std::array<uint64_t, 4> &other_usage,
                      const std::vector<uint32_t> &other_values) {
  return first_usage == other_usage && first_values == other_values
             ? TileInvarianceClass::kInvariant
             : TileInvarianceClass::kSemantic;
}

TileInvarianceClass CompareResources(const ReplayToken &first,
                                     const ReplayToken &other,
                                     uint32_t &mismatch_mask) {
  mismatch_mask |=
      uint32_t(!first.resource_contents_valid || !other.resource_contents_valid)
      << 0;
  mismatch_mask |=
      uint32_t(first.pipeline_generation != other.pipeline_generation ||
               first.pipeline_layout_generation !=
                   other.pipeline_layout_generation)
      << 1;
  mismatch_mask |= uint32_t(first.vertex_shader_modification !=
                                other.vertex_shader_modification ||
                            first.pixel_shader_modification !=
                                other.pixel_shader_modification)
                   << 2;
  mismatch_mask |= uint32_t(first.descriptor_set_valid_mask !=
                            other.descriptor_set_valid_mask)
                   << 3;
  mismatch_mask |= uint32_t(first.texture_resources != other.texture_resources)
                   << 4;
  mismatch_mask |= uint32_t(first.sampler_resources != other.sampler_resources)
                   << 5;
  // Set 0 names the shared-memory / EDRAM resources and is persistent. Sets
  // 1-3 may be transient allocations; constants are compared by value and
  // shader-used textures/samplers by the exact evidence vectors above.
  constexpr uint32_t kSharedMemoryAndEdramSet = 0;
  constexpr uint32_t kSharedMemoryAndEdramSetBit = uint32_t(1)
                                                   << kSharedMemoryAndEdramSet;
  const auto &first_shared = first.descriptor_sets[kSharedMemoryAndEdramSet];
  const auto &other_shared = other.descriptor_sets[kSharedMemoryAndEdramSet];
  if (!(first.descriptor_set_valid_mask & kSharedMemoryAndEdramSetBit) ||
      first_shared.generation != other_shared.generation ||
      first_shared.valid != other_shared.valid) {
    mismatch_mask |= uint32_t(1) << 6;
  }
  return mismatch_mask == 0 ? TileInvarianceClass::kInvariant
                            : TileInvarianceClass::kSemantic;
}

void LogFirstTextureResourceMismatch(const ReplayToken &first,
                                     const ReplayToken &other,
                                     uint32_t family_offset,
                                     uint32_t other_tile) {
  if (g_first_texture_mismatch_logged.exchange(true,
                                               std::memory_order_acq_rel)) {
    return;
  }
  if (first.texture_resources.size() != other.texture_resources.size()) {
    REXLOG_INFO("  PS328 first texture mismatch: offset={} tile=1/{} "
                "binding_counts={}/{} observer_only=true",
                family_offset, other_tile, first.texture_resources.size(),
                other.texture_resources.size());
    return;
  }
  for (size_t binding_index = 0; binding_index < first.texture_resources.size();
       ++binding_index) {
    const auto &left = first.texture_resources[binding_index];
    const auto &right = other.texture_resources[binding_index];
    if (left == right) {
      continue;
    }
    uint32_t difference_mask = 0;
    difference_mask |=
        uint32_t(left.descriptor_binding != right.descriptor_binding ||
                 left.fetch_constant != right.fetch_constant ||
                 left.dimension != right.dimension ||
                 left.is_vertex_shader != right.is_vertex_shader ||
                 left.is_signed != right.is_signed)
        << 0;
    difference_mask |= uint32_t(left.fetch_words != right.fetch_words) << 1;
    difference_mask |=
        uint32_t(left.texture_key_hash != right.texture_key_hash ||
                 left.base_address != right.base_address ||
                 left.base_length != right.base_length ||
                 left.mip_address != right.mip_address ||
                 left.mip_length != right.mip_length ||
                 left.width != right.width || left.height != right.height ||
                 left.depth_or_array_size != right.depth_or_array_size ||
                 left.format != right.format ||
                 left.scaled_resolve != right.scaled_resolve)
        << 2;
    difference_mask |=
        uint32_t(left.image_view_generation != right.image_view_generation)
        << 3;
    difference_mask |=
        uint32_t(left.content_generation != right.content_generation) << 4;
    difference_mask |= uint32_t(left.resident != right.resident) << 5;
    REXLOG_INFO(
        "  PS328 first texture mismatch: offset={} tile=1/{} binding={} "
        "difference_mask={:02X} fetch={}/{} key={:016X}/{:016X} "
        "base={:08X}/{:08X} length={}/{} "
        "view_generation={}/{} content_generation={}/{} "
        "resident={}/{} observer_only=true",
        family_offset, other_tile, binding_index, difference_mask,
        left.fetch_constant, right.fetch_constant, left.texture_key_hash,
        right.texture_key_hash, left.base_address, right.base_address,
        left.base_length, right.base_length, left.image_view_generation,
        right.image_view_generation, left.content_generation,
        right.content_generation, left.resident, right.resident);
    return;
  }
}

TileInvarianceClass CompareViewportNdcScissor(const ReplayToken &first,
                                              const ReplayToken &other) {
  // Depth range is semantic. Position/extent, NDC and scissor are the only
  // candidates for a later mechanical untiled normalization.
  if (!FloatBitsEqual(first.viewport[4], other.viewport[4]) ||
      !FloatBitsEqual(first.viewport[5], other.viewport[5])) {
    return TileInvarianceClass::kSemantic;
  }
  bool mechanical = false;
  for (size_t index = 0; index < 4; ++index) {
    mechanical |= !FloatBitsEqual(first.viewport[index], other.viewport[index]);
  }
  mechanical |= !FloatArrayBitsEqual(first.ndc_scale, other.ndc_scale);
  mechanical |= !FloatArrayBitsEqual(first.ndc_offset, other.ndc_offset);
  mechanical |= first.scissor_offset != other.scissor_offset;
  mechanical |= first.scissor_extent != other.scissor_extent;
  return mechanical ? TileInvarianceClass::kMechanicalTile
                    : TileInvarianceClass::kInvariant;
}

TileInvarianceClass CompareIndexAndDynamicState(const ReplayToken &first,
                                                const ReplayToken &other) {
  const bool invariant =
      first.guest_primitive_type == other.guest_primitive_type &&
      first.host_primitive_type == other.host_primitive_type &&
      first.processed_index_buffer_type == other.processed_index_buffer_type &&
      first.host_index_format == other.host_index_format &&
      first.guest_vertex_or_index_count == other.guest_vertex_or_index_count &&
      first.host_vertex_or_index_count == other.host_vertex_or_index_count &&
      first.guest_index_base == other.guest_index_base &&
      first.indexed == other.indexed &&
      first.guest_index_base_valid == other.guest_index_base_valid &&
      FloatBitsEqual(first.depth_bias_constant_factor,
                     other.depth_bias_constant_factor) &&
      FloatBitsEqual(first.depth_bias_slope_factor,
                     other.depth_bias_slope_factor) &&
      FloatArrayBitsEqual(first.blend_constants, other.blend_constants) &&
      first.stencil_compare_mask_front == other.stencil_compare_mask_front &&
      first.stencil_compare_mask_back == other.stencil_compare_mask_back &&
      first.stencil_write_mask_front == other.stencil_write_mask_front &&
      first.stencil_write_mask_back == other.stencil_write_mask_back &&
      first.stencil_reference_front == other.stencil_reference_front &&
      first.stencil_reference_back == other.stencil_reference_back;
  return invariant ? TileInvarianceClass::kInvariant
                   : TileInvarianceClass::kSemantic;
}

TileInvarianceClass CompareAttachments(const ReplayToken &first,
                                       const ReplayToken &other) {
  const bool invariant =
      first.color_attachment_formats == other.color_attachment_formats &&
      first.color_attachment_count == other.color_attachment_count &&
      first.depth_attachment_format == other.depth_attachment_format &&
      first.stencil_attachment_format == other.stencil_attachment_format &&
      first.sample_count == other.sample_count &&
      first.sample_mask == other.sample_mask &&
      first.dynamic_rendering == other.dynamic_rendering;
  return invariant ? TileInvarianceClass::kInvariant
                   : TileInvarianceClass::kSemantic;
}

bool MatchesSelectedDraw(const ReplayToken &token,
                         const MainCoverageDrawSnapshot &draw) {
  return token.valid && token.constant_contents_valid &&
         token.backend_frame_sequence != 0 &&
         token.vertex_shader_hash == draw.vertex_shader_hash &&
         token.pixel_shader_hash == draw.pixel_shader_hash &&
         token.guest_primitive_type == draw.identity.primitive_type &&
         token.guest_vertex_or_index_count ==
             draw.identity.submitted_index_count &&
         token.guest_index_base_valid &&
         token.guest_index_base == draw.identity.physical_index_base;
}

bool IsPs328ShaderPair(const MainCoverageDrawSnapshot &draw) {
  return draw.vertex_shader_hash == kPs328VertexShaderHash &&
         draw.pixel_shader_hash == kPs328PixelShaderHash;
}

bool IsAuthoritativeVenuePs328Draw(const MainCoverageDrawSnapshot &draw) {
  return IsPs328ShaderPair(draw) &&
         draw.assignment == MainCoverageAssignmentFamily::kVenuePs328 &&
         draw.assignment_proof ==
             MainCoverageAssignmentProof::kTitlePayloadOnly;
}

bool IsAuthoritativeMainCoveragePs328Tail(const MainCoverageDrawSnapshot &draw,
                                          size_t family_offset,
                                          size_t family_size) {
  // Live MAIN-ledger evidence has one shader-pair draw after the complete
  // venue payload prefix. It is a title/backend-joined four-index draw, but
  // correctly has no venue assignment because it did not pass the venue
  // material selector. Do not generalize this to arbitrary unassigned draws.
  return IsPs328ShaderPair(draw) && family_size > 1 &&
         family_offset + 1 == family_size &&
         draw.assignment == MainCoverageAssignmentFamily::kUnassigned &&
         draw.assignment_proof == MainCoverageAssignmentProof::kUnassigned &&
         draw.identity.submitted_index_count == 4;
}

Ps328FamilyMemberAuthority
ClassifyPs328FamilyAuthority(const MainCoverageDrawSnapshot &draw,
                             size_t family_offset, size_t family_size) {
  if (IsAuthoritativeVenuePs328Draw(draw)) {
    return Ps328FamilyMemberAuthority::kVenueTitlePayload;
  }
  if (family_offset != 0 &&
      IsAuthoritativeMainCoveragePs328Tail(draw, family_offset, family_size)) {
    return Ps328FamilyMemberAuthority::kMainCoverageTrailingShaderPair;
  }
  return Ps328FamilyMemberAuthority::kMissing;
}

bool IsSemantic(TileInvarianceClass value) {
  return value == TileInvarianceClass::kSemantic ||
         value == TileInvarianceClass::kMissing;
}

bool IsMechanical(TileInvarianceClass value) {
  return value == TileInvarianceClass::kMechanicalTile;
}

uint32_t OrderedFloatBits(float value) {
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  return bits & 0x80000000u ? ~bits : bits | 0x80000000u;
}

bool FloatWithinUlps(float left, float right, uint32_t maximum_ulps = 2) {
  if (!std::isfinite(left) || !std::isfinite(right)) {
    return false;
  }
  const uint32_t left_ordered = OrderedFloatBits(left);
  const uint32_t right_ordered = OrderedFloatBits(right);
  const uint32_t distance = left_ordered > right_ordered
                                ? left_ordered - right_ordered
                                : right_ordered - left_ordered;
  return distance <= maximum_ulps;
}

struct TileAffine {
  std::array<float, 2> slope{};
  std::array<float, 2> intercept_twice{};
};

TileAffine CalculateTileAffine(const ReplayToken &token) {
  TileAffine affine;
  for (size_t axis = 0; axis < 2; ++axis) {
    const float viewport_origin = token.viewport[axis];
    const float viewport_extent = token.viewport[axis + 2];
    affine.slope[axis] = viewport_extent * token.ndc_scale[axis];
    affine.intercept_twice[axis] =
        viewport_origin * 2.0f +
        viewport_extent * (token.ndc_offset[axis] + 1.0f);
  }
  return affine;
}

Ps328NormalizedReplayState
ProveMechanicalNormalization(const std::array<ReplayToken, 3> &tokens,
                             Ps328NormalizationRejectReason &reason) {
  Ps328NormalizedReplayState normalized;
  reason = Ps328NormalizationRejectReason::kMissingInstances;
  for (const ReplayToken &token : tokens) {
    if (!token.valid || !token.constant_contents_valid) {
      return normalized;
    }
  }

  const ReplayToken &first = tokens[0];
  const size_t constants_size = first.system_constants.size();
  const uint32_t scale_offset = first.system_constants_ndc_scale_byte_offset;
  const uint32_t offset_offset = first.system_constants_ndc_offset_byte_offset;
  constexpr size_t kFloat3Size = sizeof(float) * 3;
  if (constants_size == 0 ||
      first.system_constants_mechanical_mask.size() != constants_size ||
      scale_offset + kFloat3Size > constants_size ||
      offset_offset + kFloat3Size > constants_size) {
    reason = Ps328NormalizationRejectReason::kInvalidConstants;
    return normalized;
  }
  for (size_t tile = 1; tile < tokens.size(); ++tile) {
    const ReplayToken &token = tokens[tile];
    if (token.system_constants.size() != constants_size ||
        token.system_constants_mechanical_mask !=
            first.system_constants_mechanical_mask ||
        token.system_constants_ndc_scale_byte_offset != scale_offset ||
        token.system_constants_ndc_offset_byte_offset != offset_offset) {
      reason = Ps328NormalizationRejectReason::kInvalidConstants;
      return normalized;
    }
    for (size_t byte = 0; byte < constants_size; ++byte) {
      if (first.system_constants[byte] == token.system_constants[byte]) {
        continue;
      }
      const bool ndc_xy =
          (byte >= scale_offset && byte < scale_offset + sizeof(float) * 2) ||
          (byte >= offset_offset && byte < offset_offset + sizeof(float) * 2);
      if (!ndc_xy) {
        reason = Ps328NormalizationRejectReason::kUnexpectedSystemConstantDelta;
        return normalized;
      }
    }
  }

  uint32_t cumulative_strip_height = 0;
  const TileAffine first_affine = CalculateTileAffine(first);
  for (size_t tile = 0; tile < tokens.size(); ++tile) {
    const ReplayToken &token = tokens[tile];
    if (!std::ranges::all_of(
            token.viewport, [](float value) { return std::isfinite(value); }) ||
        token.viewport[2] <= 0.0f || token.viewport[3] <= 0.0f) {
      reason = Ps328NormalizationRejectReason::kInvalidViewport;
      return normalized;
    }
    if (!FloatBitsEqual(token.viewport[4], first.viewport[4]) ||
        !FloatBitsEqual(token.viewport[5], first.viewport[5]) ||
        !FloatBitsEqual(token.ndc_scale[2], first.ndc_scale[2]) ||
        !FloatBitsEqual(token.ndc_offset[2], first.ndc_offset[2])) {
      reason = Ps328NormalizationRejectReason::kDepthTransformMismatch;
      return normalized;
    }
    if (token.scissor_offset[0] != 0 || token.scissor_offset[1] != 0 ||
        token.scissor_extent[0] != Ps328NormalizedReplayState::kOutputWidth ||
        token.scissor_extent[1] == 0) {
      reason = Ps328NormalizationRejectReason::kInvalidScissor;
      return normalized;
    }
    if (cumulative_strip_height > Ps328NormalizedReplayState::kOutputHeight ||
        token.scissor_extent[1] > Ps328NormalizedReplayState::kOutputHeight -
                                      cumulative_strip_height) {
      reason = Ps328NormalizationRejectReason::kScissorCoverageMismatch;
      return normalized;
    }
    if (!FloatWithinUlps(token.viewport[0], 0.0f) ||
        !FloatWithinUlps(token.viewport[1], 0.0f) ||
        !FloatWithinUlps(token.viewport[2],
                         float(Ps328NormalizedReplayState::kOutputWidth))) {
      reason = Ps328NormalizationRejectReason::kLocalStripShapeMismatch;
      return normalized;
    }
    const float expected_remaining_height = float(
        Ps328NormalizedReplayState::kOutputHeight - cumulative_strip_height);
    if (!FloatWithinUlps(token.viewport[3], expected_remaining_height)) {
      reason = Ps328NormalizationRejectReason::kRemainingExtentMismatch;
      return normalized;
    }
    const TileAffine affine = CalculateTileAffine(token);
    for (size_t axis = 0; axis < 2; ++axis) {
      if (!FloatWithinUlps(affine.slope[axis], first_affine.slope[axis])) {
        reason = Ps328NormalizationRejectReason::kAffineTransformMismatch;
        return normalized;
      }
    }
    if (!FloatWithinUlps(affine.intercept_twice[0],
                         first_affine.intercept_twice[0])) {
      reason = Ps328NormalizationRejectReason::kAffineTransformMismatch;
      return normalized;
    }
    const float derived_global_y =
        (first_affine.intercept_twice[1] - affine.intercept_twice[1]) * 0.5f;
    if (!FloatWithinUlps(derived_global_y, float(cumulative_strip_height))) {
      reason = Ps328NormalizationRejectReason::kDerivedTileOriginMismatch;
      return normalized;
    }
    cumulative_strip_height += token.scissor_extent[1];
  }
  if (cumulative_strip_height != Ps328NormalizedReplayState::kOutputHeight) {
    reason = Ps328NormalizationRejectReason::kScissorCoverageMismatch;
    return normalized;
  }

  normalized.output_width = Ps328NormalizedReplayState::kOutputWidth;
  normalized.output_height = Ps328NormalizedReplayState::kOutputHeight;
  normalized.viewport = first.viewport;
  normalized.scissor_offset = {0, 0};
  normalized.scissor_extent = {
      normalized.output_width,
      normalized.output_height,
  };
  normalized.ndc_scale = first.ndc_scale;
  normalized.ndc_offset = first.ndc_offset;
  if (!std::ranges::all_of(normalized.ndc_scale,
                           [](float value) { return std::isfinite(value); }) ||
      !std::ranges::all_of(normalized.ndc_offset,
                           [](float value) { return std::isfinite(value); })) {
    reason = Ps328NormalizationRejectReason::kInvalidCanonicalTransform;
    return {};
  }
  // Tile 1 is already the full 1280x720 viewport transform. Only its local
  // scissor is widened; no NDC or SystemConstants value is synthesized.
  normalized.system_constants = first.system_constants;
  normalized.valid = true;
  reason = Ps328NormalizationRejectReason::kNone;
  return normalized;
}

std::shared_ptr<Ps328TileInvarianceSnapshot>
BuildMemberProof(uint64_t sequence, const MainCoverageDrawSnapshot &draw,
                 const std::array<ReplayToken, 3> &matching) {
  auto result = std::make_shared<Ps328TileInvarianceSnapshot>();
  result->sequence = sequence;
  result->selected_ordinal = draw.original_ordinal;
  result->matching_instance_count = static_cast<uint32_t>(matching.size());
  result->exact_dynamic_selection = true;
  result->three_instances_exact = true;
  result->selected_token = std::make_shared<const ReplayToken>(matching[0]);

  result->system_constants = TileInvarianceClass::kInvariant;
  result->vertex_float_constants = TileInvarianceClass::kInvariant;
  result->pixel_float_constants = TileInvarianceClass::kInvariant;
  result->bool_loop_constants = TileInvarianceClass::kInvariant;
  result->fetch_constants = TileInvarianceClass::kInvariant;
  result->resources = TileInvarianceClass::kInvariant;
  result->viewport_ndc_scissor = TileInvarianceClass::kInvariant;
  result->index_and_dynamic_state = TileInvarianceClass::kInvariant;
  result->attachments = TileInvarianceClass::kInvariant;
  for (size_t tile = 1; tile < matching.size(); ++tile) {
    const ReplayToken &first = matching[0];
    const ReplayToken &other = matching[tile];
    result->system_constants =
        Merge(result->system_constants, CompareSystemConstants(first, other));
    result->vertex_float_constants =
        Merge(result->vertex_float_constants,
              CompareFloatConstants(first.vertex_float_constant_usage,
                                    first.vertex_float_constants,
                                    other.vertex_float_constant_usage,
                                    other.vertex_float_constants));
    result->pixel_float_constants =
        Merge(result->pixel_float_constants,
              CompareFloatConstants(first.pixel_float_constant_usage,
                                    first.pixel_float_constants,
                                    other.pixel_float_constant_usage,
                                    other.pixel_float_constants));
    result->bool_loop_constants =
        Merge(result->bool_loop_constants,
              ExactEqual(first.bool_loop_constants, other.bool_loop_constants)
                  ? TileInvarianceClass::kInvariant
                  : TileInvarianceClass::kSemantic);
    result->fetch_constants =
        Merge(result->fetch_constants,
              ExactEqual(first.fetch_constants, other.fetch_constants)
                  ? TileInvarianceClass::kInvariant
                  : TileInvarianceClass::kSemantic);
    result->resources =
        Merge(result->resources,
              CompareResources(first, other, result->resource_mismatch_mask));
    result->viewport_ndc_scissor = Merge(
        result->viewport_ndc_scissor, CompareViewportNdcScissor(first, other));
    result->index_and_dynamic_state =
        Merge(result->index_and_dynamic_state,
              CompareIndexAndDynamicState(first, other));
    result->attachments =
        Merge(result->attachments, CompareAttachments(first, other));
  }

  const std::array classifications = {
      result->system_constants,      result->vertex_float_constants,
      result->pixel_float_constants, result->bool_loop_constants,
      result->fetch_constants,       result->resources,
      result->viewport_ndc_scissor,  result->index_and_dynamic_state,
      result->attachments,
  };
  result->semantic_difference =
      std::ranges::any_of(classifications, IsSemantic);
  result->mechanical_normalization_required =
      std::ranges::any_of(classifications, IsMechanical);
  result->normalized_state =
      ProveMechanicalNormalization(matching, result->normalization_reason);
  result->one_copy_replay_ready =
      !result->semantic_difference &&
      result->resources == TileInvarianceClass::kInvariant &&
      result->normalized_state.valid &&
      result->normalization_reason == Ps328NormalizationRejectReason::kNone;
  return result;
}

std::shared_ptr<const ReplayToken>
NormalizeTokenAgainstCanonical(const ReplayToken &current,
                               const ReplayToken &prior_tile1,
                               const Ps328NormalizedReplayState &canonical) {
  const auto bit_exact = [](const auto &left, const auto &right) {
    static_assert(sizeof(left) == sizeof(right));
    return std::memcmp(&left, &right, sizeof(left)) == 0;
  };
  if (canonical.output_width != Ps328NormalizedReplayState::kOutputWidth ||
      canonical.output_height != Ps328NormalizedReplayState::kOutputHeight ||
      !bit_exact(current.viewport, canonical.viewport) ||
      !bit_exact(current.ndc_scale, canonical.ndc_scale) ||
      !bit_exact(current.ndc_offset, canonical.ndc_offset) ||
      current.scissor_offset != std::array<int32_t, 2>{0, 0} ||
      current.scissor_extent[0] != canonical.output_width ||
      current.scissor_extent[1] != prior_tile1.scissor_extent[1] ||
      current.scissor_extent[1] == 0 ||
      !FloatBitsEqual(current.viewport[4], prior_tile1.viewport[4]) ||
      !FloatBitsEqual(current.viewport[5], prior_tile1.viewport[5]) ||
      current.system_constants.empty() ||
      current.system_constants_mechanical_mask.size() !=
          current.system_constants.size()) {
    return nullptr;
  }
  const uint32_t scale_offset = current.system_constants_ndc_scale_byte_offset;
  const uint32_t offset_offset =
      current.system_constants_ndc_offset_byte_offset;
  constexpr size_t kFloat3Bytes = sizeof(float) * 3;
  if (scale_offset + kFloat3Bytes > current.system_constants.size() ||
      offset_offset + kFloat3Bytes > current.system_constants.size() ||
      (scale_offset < offset_offset + kFloat3Bytes &&
       offset_offset < scale_offset + kFloat3Bytes) ||
      std::memcmp(current.system_constants.data() + scale_offset,
                  current.ndc_scale.data(), kFloat3Bytes) != 0 ||
      std::memcmp(current.system_constants.data() + offset_offset,
                  current.ndc_offset.data(), kFloat3Bytes) != 0) {
    return nullptr;
  }
  auto normalized = std::make_shared<ReplayToken>(current);
  normalized->scissor_offset = canonical.scissor_offset;
  normalized->scissor_extent = canonical.scissor_extent;
  return normalized;
}

} // namespace

bool Ps328FamilyAuthorizedDraw::matches_identity(
    uint32_t ordinal, uint32_t candidate_primitive_type,
    uint32_t candidate_submitted_index_count,
    uint32_t candidate_physical_index_base) const {
  return original_ordinal == ordinal &&
         primitive_type == candidate_primitive_type &&
         submitted_index_count == candidate_submitted_index_count &&
         physical_index_base == candidate_physical_index_base;
}

bool Ps328FamilyAuthorizedDraw::valid() const {
  if (original_ordinal == 0 || primitive_type == 0 ||
      submitted_index_count == 0 || physical_index_base == 0 ||
      authority == Ps328FamilyMemberAuthority::kMissing ||
      !normalized_state.valid || tile1_token == nullptr ||
      !one_copy_replay_ready || resource_mismatch_mask != 0 ||
      normalization_reason != Ps328NormalizationRejectReason::kNone) {
    return false;
  }
  const ReplayToken &token = *tile1_token;
  return token.valid && token.constant_contents_valid &&
         token.resource_contents_valid &&
         token.vertex_shader_hash == kPs328VertexShaderHash &&
         token.pixel_shader_hash == kPs328PixelShaderHash &&
         token.guest_index_base_valid &&
         matches_identity(original_ordinal, token.guest_primitive_type,
                          token.guest_vertex_or_index_count,
                          token.guest_index_base) &&
         normalized_state.output_width ==
             Ps328NormalizedReplayState::kOutputWidth &&
         normalized_state.output_height ==
             Ps328NormalizedReplayState::kOutputHeight &&
         normalized_state.scissor_offset == std::array<int32_t, 2>{0, 0} &&
         normalized_state.scissor_extent ==
             std::array<uint32_t, 2>{
                 Ps328NormalizedReplayState::kOutputWidth,
                 Ps328NormalizedReplayState::kOutputHeight} &&
         FloatArrayBitsEqual(normalized_state.viewport, token.viewport) &&
         FloatArrayBitsEqual(normalized_state.ndc_scale, token.ndc_scale) &&
         FloatArrayBitsEqual(normalized_state.ndc_offset, token.ndc_offset) &&
         normalized_state.system_constants == token.system_constants;
}

bool Ps328FamilyAuthorizationSnapshot::valid() const {
  if (sequence == 0 || logical_draw_count == 0 ||
      uint64_t(observed_token_count) !=
          uint64_t(logical_draw_count) *
              MainCoverageFrameSnapshot::kRequiredTileBlockCount ||
      authorized_draw_count != logical_draw_count ||
      draws.size() != logical_draw_count ||
      reject_reason != Ps328FamilyAuthorizationRejectReason::kNone) {
    return false;
  }
  uint32_t previous_ordinal = 0;
  uint32_t previous_catalog_index = 0;
  for (size_t offset = 0; offset < draws.size(); ++offset) {
    const Ps328FamilyAuthorizedDraw &draw = draws[offset];
    if (!draw.valid() || draw.family_offset != offset ||
        draw.tile1_token->backend_frame_sequence != sequence ||
        (draw.authority ==
             Ps328FamilyMemberAuthority::kMainCoverageTrailingShaderPair &&
         (offset + 1 != draws.size() || offset == 0 ||
          draw.submitted_index_count != 4)) ||
        (offset + 1 != draws.size() &&
         draw.authority != Ps328FamilyMemberAuthority::kVenueTitlePayload) ||
        (offset != 0 && (draw.original_ordinal <= previous_ordinal ||
                         draw.catalog_draw_index <= previous_catalog_index))) {
      return false;
    }
    previous_ordinal = draw.original_ordinal;
    previous_catalog_index = draw.catalog_draw_index;
  }
  return true;
}

bool Ps328FamilyAuthorizationSnapshot::private_batch_ready() const {
  if (!valid()) {
    return false;
  }
  return std::ranges::all_of(draws, [](const Ps328FamilyAuthorizedDraw &draw) {
    return draw.tile1_token->resources_stable_for_deferred_replay;
  });
}

uint32_t Ps328FamilyAuthorizationSnapshot::private_batch_draw_count() const {
  return private_batch_ready() ? logical_draw_count : 0;
}

const Ps328FamilyAuthorizedDraw *
Ps328FamilyAuthorizationSnapshot::private_batch_draw(
    uint32_t family_offset) const {
  if (!private_batch_ready() || family_offset >= draws.size()) {
    return nullptr;
  }
  const Ps328FamilyAuthorizedDraw &draw = draws[family_offset];
  return draw.family_offset == family_offset ? &draw : nullptr;
}

bool FilterPs328TileInvarianceToken(uint64_t vertex_shader_hash,
                                    uint64_t pixel_shader_hash) {
  return vertex_shader_hash == kPs328VertexShaderHash &&
         pixel_shader_hash == kPs328PixelShaderHash;
}

void ObservePs328TileInvarianceToken(const ReplayToken &context) {
  if (!FilterPs328TileInvarianceToken(context.vertex_shader_hash,
                                      context.pixel_shader_hash) ||
      !context.valid || !context.constant_contents_valid ||
      context.backend_frame_sequence == 0) {
    return;
  }
  std::lock_guard lock(g_mutex);
  auto frame = std::ranges::find(g_frames, context.backend_frame_sequence,
                                 &TokenFrame::sequence);
  if (frame == g_frames.end()) {
    g_frames.push_back({.sequence = context.backend_frame_sequence});
    frame = std::prev(g_frames.end());
  }
  if (frame->tokens.size() < kMaximumTokensPerFrame) {
    frame->tokens.push_back(context);
  }
  while (g_frames.size() > kMaximumRetainedFrames) {
    g_frames.pop_front();
  }
}

void EvaluatePs328TileInvariance(const MainCoverageFrameSnapshot &coverage) {
  auto family = std::make_shared<Ps328FamilyAuthorizationSnapshot>();
  family->sequence = coverage.backend_frame_sequence;
  auto legacy = std::make_shared<Ps328TileInvarianceSnapshot>();
  legacy->sequence = coverage.backend_frame_sequence;
  if (coverage.backend_frame_sequence == 0) {
    std::lock_guard lock(g_mutex);
    g_published = std::move(legacy);
    g_family_published = std::move(family);
    return;
  }

  std::vector<const MainCoverageDrawSnapshot *> logical_draws;
  std::vector<ReplayToken> tokens;
  if (!coverage.valid()) {
    family->reject_reason =
        Ps328FamilyAuthorizationRejectReason::kInvalidCoverage;
  } else {
    for (const MainCoverageDrawSnapshot &draw : coverage.draws) {
      if (IsPs328ShaderPair(draw)) {
        logical_draws.push_back(&draw);
      }
    }
    family->logical_draw_count = static_cast<uint32_t>(logical_draws.size());
    if (logical_draws.empty()) {
      family->reject_reason =
          Ps328FamilyAuthorizationRejectReason::kNoLogicalDraws;
    } else {
      family->reject_reason = Ps328FamilyAuthorizationRejectReason::kNone;
    }
  }

  if (family->reject_reason ==
      Ps328FamilyAuthorizationRejectReason::kInvalidCoverage) {
    // Keep the first failure only.
  } else if (family->reject_reason ==
             Ps328FamilyAuthorizationRejectReason::kNoLogicalDraws) {
    // Keep the first failure only.
  } else {
    for (size_t offset = 0; offset < logical_draws.size(); ++offset) {
      const MainCoverageDrawSnapshot &draw = *logical_draws[offset];
      if (ClassifyPs328FamilyAuthority(draw, offset, logical_draws.size()) ==
          Ps328FamilyMemberAuthority::kMissing) {
        family->reject_reason = Ps328FamilyAuthorizationRejectReason::
            kMissingAuthoritativeAssignment;
        family->first_rejected_family_offset = static_cast<uint32_t>(offset);
        family->first_rejected_ordinal = draw.original_ordinal;
        break;
      }
      if (offset != 0 && logical_draws[offset - 1]->original_ordinal >=
                             draw.original_ordinal) {
        family->reject_reason =
            Ps328FamilyAuthorizationRejectReason::kAmbiguousOrder;
        family->first_rejected_family_offset = static_cast<uint32_t>(offset);
        family->first_rejected_ordinal = draw.original_ordinal;
        break;
      }
    }
  }

  if (family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kInvalidCoverage ||
      family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kNoLogicalDraws ||
      family->reject_reason == Ps328FamilyAuthorizationRejectReason::
                                   kMissingAuthoritativeAssignment ||
      family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kAmbiguousOrder) {
    // The structural checks above rejected the family.
  } else {
    bool token_frame_found = false;
    {
      std::lock_guard lock(g_mutex);
      const auto frame = std::ranges::find(
          g_frames, coverage.backend_frame_sequence, &TokenFrame::sequence);
      if (frame != g_frames.end()) {
        token_frame_found = true;
        tokens = frame->tokens;
      }
    }
    family->observed_token_count = static_cast<uint32_t>(tokens.size());
    if (!token_frame_found) {
      family->reject_reason =
          Ps328FamilyAuthorizationRejectReason::kMissingTokenFrame;
    } else if (tokens.size() !=
               logical_draws.size() *
                   MainCoverageFrameSnapshot::kRequiredTileBlockCount) {
      family->reject_reason =
          Ps328FamilyAuthorizationRejectReason::kTokenCountMismatch;
    }
  }

  if (family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kInvalidCoverage ||
      family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kNoLogicalDraws ||
      family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kMissingTokenFrame ||
      family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kTokenCountMismatch ||
      family->reject_reason == Ps328FamilyAuthorizationRejectReason::
                                   kMissingAuthoritativeAssignment ||
      family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kAmbiguousOrder) {
    // Nothing can be semantically evaluated.
  } else {
    family->reject_reason = Ps328FamilyAuthorizationRejectReason::kNone;
    family->draws.reserve(logical_draws.size());
    for (size_t offset = 0; offset < logical_draws.size(); ++offset) {
      const MainCoverageDrawSnapshot &draw = *logical_draws[offset];
      std::array<ReplayToken, 3> matching{};
      bool identity_exact = true;
      for (size_t tile = 0; tile < matching.size(); ++tile) {
        const ReplayToken &token = tokens[tile * logical_draws.size() + offset];
        identity_exact &= MatchesSelectedDraw(token, draw);
        matching[tile] = token;
      }
      if (!identity_exact) {
        family->reject_reason =
            Ps328FamilyAuthorizationRejectReason::kTokenIdentityMismatch;
        family->first_rejected_family_offset = static_cast<uint32_t>(offset);
        family->first_rejected_ordinal = draw.original_ordinal;
        break;
      }

      auto proof =
          BuildMemberProof(coverage.backend_frame_sequence, draw, matching);
      if (offset == 0) {
        legacy = proof;
      }
      Ps328FamilyAuthorizedDraw member;
      member.family_offset = static_cast<uint32_t>(offset);
      member.original_ordinal = draw.original_ordinal;
      member.catalog_draw_index = draw.catalog_draw_index;
      member.primitive_type = draw.identity.primitive_type;
      member.submitted_index_count = draw.identity.submitted_index_count;
      member.physical_index_base = draw.identity.physical_index_base;
      member.authority =
          ClassifyPs328FamilyAuthority(draw, offset, logical_draws.size());
      member.normalized_state = proof->normalized_state;
      member.tile1_token = proof->selected_token;
      member.resource_mismatch_mask = proof->resource_mismatch_mask;
      member.normalization_reason = proof->normalization_reason;
      member.one_copy_replay_ready = proof->one_copy_replay_ready;
      family->draws.push_back(std::move(member));
      if (!proof->one_copy_replay_ready) {
        if ((proof->resource_mismatch_mask & (uint32_t(1) << 4)) != 0) {
          for (size_t tile = 1; tile < matching.size(); ++tile) {
            if (matching[0].texture_resources !=
                matching[tile].texture_resources) {
              LogFirstTextureResourceMismatch(matching[0], matching[tile],
                                              static_cast<uint32_t>(offset),
                                              static_cast<uint32_t>(tile + 1));
              break;
            }
          }
        }
        family->reject_reason =
            Ps328FamilyAuthorizationRejectReason::kUnsafeMember;
        family->first_rejected_family_offset = static_cast<uint32_t>(offset);
        family->first_rejected_ordinal = draw.original_ordinal;
        break;
      }
      ++family->authorized_draw_count;
    }
  }

  // Frontend frames intentionally have no complete MAIN coverage. Reporting
  // every one of them produces thousands of identical lines before gameplay
  // and can perturb the timing of the automated path.
  if (family->reject_reason !=
          Ps328FamilyAuthorizationRejectReason::kInvalidCoverage ||
      family->logical_draw_count != 0) {
    REXLOG_INFO("Table Tennis PS328 family authorization: frame={} logical={} "
                "tokens={} authorized={} reject={} first_offset={} "
                "first_ordinal={} observer_only=true",
                family->sequence, family->logical_draw_count,
                family->observed_token_count, family->authorized_draw_count,
                Ps328FamilyAuthorizationRejectReasonName(family->reject_reason),
                family->first_rejected_family_offset,
                family->first_rejected_ordinal);
  }
  if (family->reject_reason ==
          Ps328FamilyAuthorizationRejectReason::kUnsafeMember &&
      !family->draws.empty()) {
    const Ps328FamilyAuthorizedDraw &rejected = family->draws.back();
    REXLOG_INFO(
        "  PS328 first rejection: offset={} ordinal={} "
        "resource_mismatch_mask={:02X} normalization={} "
        "one_copy_ready={}",
        rejected.family_offset, rejected.original_ordinal,
        rejected.resource_mismatch_mask,
        Ps328NormalizationRejectReasonName(rejected.normalization_reason),
        rejected.one_copy_replay_ready);
  }
  const bool retained_replay_safe_proof =
      family->valid() && family->private_batch_ready();
  {
    std::lock_guard lock(g_mutex);
    if (family->valid()) {
      g_latest_valid_family_published = family;
    }
    g_published = std::move(legacy);
    g_family_published = std::move(family);
  }
  // Retirement is a consequence of retaining a complete replay-safe proof,
  // not of enabling or reaching the private replay output callback. Keep the
  // backend token filter installed: current-frame guarded tokens are still
  // available to replay consumers, and the independent venue-9E family keeps
  // collecting in RexGlue.
  if (retained_replay_safe_proof) {
    TryRetirePs328TitleCapture();
  }
}

std::shared_ptr<const Ps328TileInvarianceSnapshot>
LatestPs328TileInvarianceSnapshot() {
  std::lock_guard lock(g_mutex);
  return g_published;
}

std::shared_ptr<const Ps328FamilyAuthorizationSnapshot>
LatestPs328FamilyAuthorizationSnapshot() {
  std::lock_guard lock(g_mutex);
  return g_family_published;
}

std::shared_ptr<const Ps328FamilyAuthorizationSnapshot>
LatestValidPs328FamilyAuthorizationSnapshot() {
  std::lock_guard lock(g_mutex);
  return g_latest_valid_family_published;
}

bool TryRetirePs328TitleCapture() {
  const auto authorization = LatestValidPs328FamilyAuthorizationSnapshot();
  if (authorization == nullptr || !authorization->private_batch_ready()) {
    return false;
  }
  return RetireGuardedVenueTitleCapture(
      GuardedVenueTitleCaptureFamily::kPs328,
      authorization->sequence);
}

bool Ps328TitleCaptureRetired() {
  return GuardedVenueTitleCaptureRetired(
      GuardedVenueTitleCaptureFamily::kPs328);
}

std::shared_ptr<const ReplayToken> FindGuardedPs328TokenForFrame(
    uint64_t backend_frame_sequence,
    const Ps328TileInvarianceSnapshot &prior_authorization) {
  if (backend_frame_sequence == 0 || !prior_authorization.valid() ||
      prior_authorization.semantic_difference ||
      prior_authorization.resources != TileInvarianceClass::kInvariant ||
      prior_authorization.sequence >= backend_frame_sequence ||
      prior_authorization.selected_token == nullptr) {
    return nullptr;
  }
  const ReplayToken &reference = *prior_authorization.selected_token;
  std::lock_guard lock(g_mutex);
  const auto frame = std::ranges::find(g_frames, backend_frame_sequence,
                                       &TokenFrame::sequence);
  if (frame == g_frames.end()) {
    return nullptr;
  }
  const auto candidate = std::ranges::find_if(
      frame->tokens,
      [&reference, backend_frame_sequence](const ReplayToken &token) {
        return token.valid && token.constant_contents_valid &&
               token.resource_contents_valid &&
               token.resources_stable_for_deferred_replay &&
               token.backend_frame_sequence == backend_frame_sequence &&
               token.vertex_shader_hash == reference.vertex_shader_hash &&
               token.pixel_shader_hash == reference.pixel_shader_hash &&
               token.vertex_shader_modification ==
                   reference.vertex_shader_modification &&
               token.pixel_shader_modification ==
                   reference.pixel_shader_modification &&
               token.guest_primitive_type == reference.guest_primitive_type &&
               token.host_primitive_type == reference.host_primitive_type &&
               token.processed_index_buffer_type ==
                   reference.processed_index_buffer_type &&
               token.host_index_format == reference.host_index_format &&
               token.guest_vertex_or_index_count ==
                   reference.guest_vertex_or_index_count &&
               token.host_vertex_or_index_count ==
                   reference.host_vertex_or_index_count &&
               token.indexed == reference.indexed &&
               token.guest_index_base_valid;
      });
  return candidate != frame->tokens.end()
             ? std::make_shared<const ReplayToken>(*candidate)
             : nullptr;
}

std::shared_ptr<const ReplayToken> PrepareNormalizedPs328TokenForFrame(
    uint64_t backend_frame_sequence,
    const Ps328TileInvarianceSnapshot &prior_authorization) {
  if (!prior_authorization.one_copy_replay_ready ||
      !prior_authorization.normalized_state.valid ||
      prior_authorization.selected_token == nullptr) {
    return nullptr;
  }
  const auto current = FindGuardedPs328TokenForFrame(backend_frame_sequence,
                                                     prior_authorization);
  if (current == nullptr) {
    return nullptr;
  }
  const ReplayToken &prior_tile1 = *prior_authorization.selected_token;
  const Ps328NormalizedReplayState &canonical =
      prior_authorization.normalized_state;
  return NormalizeTokenAgainstCanonical(*current, prior_tile1, canonical);
}

std::shared_ptr<const ReplayToken> NormalizeAuthorizedPs328FamilyToken(
    const ReplayToken &current,
    const Ps328FamilyAuthorizedDraw &prior_authorization,
    uint64_t backend_frame_sequence) {
  if (backend_frame_sequence == 0 ||
      current.backend_frame_sequence != backend_frame_sequence ||
      !current.valid || !current.constant_contents_valid ||
      !current.resource_contents_valid ||
      !current.resources_stable_for_deferred_replay ||
      !prior_authorization.valid() ||
      prior_authorization.tile1_token == nullptr ||
      current.vertex_shader_hash != kPs328VertexShaderHash ||
      current.pixel_shader_hash != kPs328PixelShaderHash ||
      current.vertex_shader_modification !=
          prior_authorization.tile1_token->vertex_shader_modification ||
      current.pixel_shader_modification !=
          prior_authorization.tile1_token->pixel_shader_modification) {
    return nullptr;
  }
  return NormalizeTokenAgainstCanonical(current,
                                        *prior_authorization.tile1_token,
                                        prior_authorization.normalized_state);
}

const char *TileInvarianceClassName(TileInvarianceClass classification) {
  switch (classification) {
  case TileInvarianceClass::kMissing:
    return "missing";
  case TileInvarianceClass::kInvariant:
    return "invariant";
  case TileInvarianceClass::kMechanicalTile:
    return "mechanical_tile";
  case TileInvarianceClass::kSemantic:
    return "semantic";
  }
  return "unknown";
}

const char *
Ps328NormalizationRejectReasonName(Ps328NormalizationRejectReason reason) {
  switch (reason) {
  case Ps328NormalizationRejectReason::kNone:
    return "none";
  case Ps328NormalizationRejectReason::kMissingInstances:
    return "missing_instances";
  case Ps328NormalizationRejectReason::kInvalidConstants:
    return "invalid_constants";
  case Ps328NormalizationRejectReason::kUnexpectedSystemConstantDelta:
    return "unexpected_system_constant_delta";
  case Ps328NormalizationRejectReason::kInvalidViewport:
    return "invalid_viewport";
  case Ps328NormalizationRejectReason::kDepthTransformMismatch:
    return "depth_transform_mismatch";
  case Ps328NormalizationRejectReason::kInvalidScissor:
    return "invalid_scissor";
  case Ps328NormalizationRejectReason::kScissorOutsideOutput:
    return "scissor_outside_output";
  case Ps328NormalizationRejectReason::kScissorOverlap:
    return "scissor_overlap";
  case Ps328NormalizationRejectReason::kScissorCoverageMismatch:
    return "scissor_coverage_mismatch";
  case Ps328NormalizationRejectReason::kLocalStripShapeMismatch:
    return "local_strip_shape_mismatch";
  case Ps328NormalizationRejectReason::kRemainingExtentMismatch:
    return "remaining_extent_mismatch";
  case Ps328NormalizationRejectReason::kDerivedTileOriginMismatch:
    return "derived_tile_origin_mismatch";
  case Ps328NormalizationRejectReason::kAffineTransformMismatch:
    return "affine_transform_mismatch";
  case Ps328NormalizationRejectReason::kInvalidCanonicalTransform:
    return "invalid_canonical_transform";
  }
  return "unknown";
}

const char *Ps328FamilyAuthorizationRejectReasonName(
    Ps328FamilyAuthorizationRejectReason reason) {
  switch (reason) {
  case Ps328FamilyAuthorizationRejectReason::kNone:
    return "none";
  case Ps328FamilyAuthorizationRejectReason::kInvalidCoverage:
    return "invalid_coverage";
  case Ps328FamilyAuthorizationRejectReason::kNoLogicalDraws:
    return "no_logical_draws";
  case Ps328FamilyAuthorizationRejectReason::kMissingTokenFrame:
    return "missing_token_frame";
  case Ps328FamilyAuthorizationRejectReason::kTokenCountMismatch:
    return "token_count_mismatch";
  case Ps328FamilyAuthorizationRejectReason::kMissingAuthoritativeAssignment:
    return "missing_authoritative_assignment";
  case Ps328FamilyAuthorizationRejectReason::kAmbiguousOrder:
    return "ambiguous_order";
  case Ps328FamilyAuthorizationRejectReason::kTokenIdentityMismatch:
    return "token_identity_mismatch";
  case Ps328FamilyAuthorizationRejectReason::kUnsafeMember:
    return "unsafe_member";
  }
  return "unknown";
}

void ResetPs328TileInvariance() {
  std::lock_guard lock(g_mutex);
  g_frames.clear();
  g_published.reset();
  g_family_published.reset();
  g_latest_valid_family_published.reset();
  ResetGuardedVenueTitleCaptureRetirement();
  g_first_texture_mismatch_logged.store(false, std::memory_order_release);
}

} // namespace tabletennis::native
