#include "native/tabletennis_player_2ac_renderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_2ac_renderer_observer, false, "Table Tennis",
    "Decode and preflight the real immutable 2AC player composite payloads. "
    "Observer-only; records no GPU commands and never suppresses a guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr float kMaximumPositionMagnitude = 1000000.0f;
constexpr std::array<uint32_t, 4> kWeightByteOrder = {2, 1, 0, 3};
constexpr std::array<uint32_t, 4> kSkinned36IndexByteOrder = {3, 2, 0, 1};
constexpr std::array<uint32_t, 4> kSkinned44IndexByteOrder = {2, 1, 0, 3};

std::mutex g_plan_mutex;
std::shared_ptr<const Player2ACCompositeRenderPlan> g_published_plan;
uint64_t g_last_built_sequence = 0;
Player2ACCompositeBlocker g_last_logged_blocker =
    Player2ACCompositeBlocker::kNone;
bool g_announced_valid_plan = false;

uint32_t LoadBeU32(const uint8_t *source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const uint8_t *source) {
  return std::bit_cast<float>(LoadBeU32(source));
}

float Signed10(uint32_t packed, uint32_t shift) {
  const uint32_t field = (packed >> shift) & 0x3FFu;
  return static_cast<float>(static_cast<int32_t>(field << 22u) >> 22);
}

std::array<float, 3> PackedSigned101010(uint32_t packed) {
  return {
      Signed10(packed, 0),
      Signed10(packed, 10),
      Signed10(packed, 20),
  };
}

bool FinitePosition(const std::array<float, 3> &position) {
  return std::ranges::all_of(position, [](float value) {
    return std::isfinite(value) && std::abs(value) < kMaximumPositionMagnitude;
  });
}

bool FiniteDirection(const std::array<float, 3> &direction) {
  return std::ranges::all_of(direction,
                             [](float value) { return std::isfinite(value); });
}

void FingerprintWord(uint64_t &fingerprint, uint32_t value) {
  for (uint32_t shift = 0; shift < 32; shift += 8) {
    fingerprint =
        (fingerprint ^ static_cast<uint8_t>(value >> shift)) * kFnvPrime;
  }
}

void FingerprintFloat(uint64_t &fingerprint, float value) {
  FingerprintWord(fingerprint, std::bit_cast<uint32_t>(value));
}

struct DecodedVertex {
  std::array<float, 3> position{};
  std::array<float, 3> normal{};
  std::array<float, 4> weights{};
  std::array<uint8_t, 4> bone_indices{};
  bool skinned = false;
};

bool DecodeVertex(const Player2ACVertexPayload &payload,
                  Player2ACTitleKind kind, uint32_t vertex_index,
                  DecodedVertex &decoded) {
  if (!payload.valid() || vertex_index >= payload.vertex_count) {
    return false;
  }
  const uint8_t *const source =
      payload.raw_bytes.data() +
      static_cast<size_t>(vertex_index) * payload.fetch.stride;
  decoded.position = {
      LoadBeF32(source + 0),
      LoadBeF32(source + 4),
      LoadBeF32(source + 8),
  };

  switch (kind) {
  case Player2ACTitleKind::kSingleStream32:
    if (payload.fetch.stride != 32) {
      return false;
    }
    decoded.normal = PackedSigned101010(LoadBeU32(source + 12));
    break;
  case Player2ACTitleKind::kSingleStream96:
    if (payload.fetch.stride != 96) {
      return false;
    }
    // The traced vfetch reads float4 at offset 4 (16 bytes) and consumes xyz.
    decoded.normal = {
        LoadBeF32(source + 16),
        LoadBeF32(source + 20),
        LoadBeF32(source + 24),
    };
    break;
  case Player2ACTitleKind::kSkinned36:
  case Player2ACTitleKind::kSkinned44: {
    const uint32_t expected_stride =
        kind == Player2ACTitleKind::kSkinned36 ? 36 : 44;
    if (payload.fetch.stride != expected_stride) {
      return false;
    }
    const uint32_t packed_weights = LoadBeU32(source + 12);
    const uint32_t packed_indices = LoadBeU32(source + 16);
    const auto &index_byte_order = kind == Player2ACTitleKind::kSkinned36
                                       ? kSkinned36IndexByteOrder
                                       : kSkinned44IndexByteOrder;
    float weight_sum = 0.0f;
    for (size_t influence = 0; influence < decoded.weights.size();
         ++influence) {
      const uint32_t weight_shift = kWeightByteOrder[influence] * 8;
      const uint32_t index_shift = index_byte_order[influence] * 8;
      decoded.weights[influence] =
          static_cast<float>((packed_weights >> weight_shift) & 0xFFu) / 255.0f;
      decoded.bone_indices[influence] =
          static_cast<uint8_t>((packed_indices >> index_shift) & 0xFFu);
      weight_sum += decoded.weights[influence];
    }
    // Both skinned shaders fetch the packed normal from byte offset 20.
    decoded.normal = PackedSigned101010(LoadBeU32(source + 20));
    decoded.skinned =
        std::isfinite(weight_sum) && std::abs(weight_sum - 1.0f) < 0.02f;
    if (!decoded.skinned) {
      return false;
    }
    break;
  }
  default:
    return false;
  }
  return FinitePosition(decoded.position) && FiniteDirection(decoded.normal);
}

bool ValidatePaletteReferences(const DecodedVertex &vertex,
                               uint32_t palette_record_count,
                               const Player2ACDrawConstants &constants,
                               uint32_t &maximum_record) {
  if (!vertex.skinned || palette_record_count == 0) {
    return !vertex.skinned;
  }
  // Xenos FMT_8_8_8_8 supplies normalized index components. The skinned
  // motion programs evaluate both palette halves: one address is
  // (byte/255)*c254.y, the other is (byte/255+c20.w)*c254.y. Trace values
  // c254.y=255.00195 and c20.w=183/255 intentionally land near integer
  // records despite floating-point addressing.
  const float record_scale = constants.vertex_constants_254_255[1];
  const float second_half_offset = constants.vertex_constants_0_20[83];
  if (!std::isfinite(record_scale) || !std::isfinite(second_half_offset) ||
      record_scale <= 0.0f || second_half_offset < 0.0f) {
    return false;
  }
  bool has_weight = false;
  for (size_t influence = 0; influence < vertex.weights.size(); ++influence) {
    if (vertex.weights[influence] <= 0.0f) {
      continue;
    }
    has_weight = true;
    const float normalized_bone =
        static_cast<float>(vertex.bone_indices[influence]) / 255.0f;
    const float first_address = normalized_bone * record_scale;
    const float second_address =
        (normalized_bone + second_half_offset) * record_scale;
    const float rounded_first = std::round(first_address);
    const float rounded_second = std::round(second_address);
    if (std::abs(first_address - rounded_first) > 0.02f ||
        std::abs(second_address - rounded_second) > 0.02f ||
        rounded_first < 0.0f || rounded_second < 0.0f) {
      return false;
    }
    const uint64_t first_record = static_cast<uint64_t>(rounded_first);
    const uint64_t second_record = static_cast<uint64_t>(rounded_second);
    if (first_record >= palette_record_count ||
        second_record >= palette_record_count) {
      return false;
    }
    maximum_record =
        std::max(maximum_record, static_cast<uint32_t>(second_record));
  }
  return has_weight;
}

void ExtendBounds(Player2ACDecodedDrawGeometry &geometry,
                  const std::array<float, 3> &position) {
  for (size_t axis = 0; axis < position.size(); ++axis) {
    geometry.bounds_min[axis] =
        std::min(geometry.bounds_min[axis], position[axis]);
    geometry.bounds_max[axis] =
        std::max(geometry.bounds_max[axis], position[axis]);
  }
}

bool DecodeDraw(const Player2ACDrawProof &proof,
                Player2ACDecodedDrawGeometry &geometry) {
  if (!proof.valid() || proof.title.payload == nullptr ||
      !proof.title.payload->valid()) {
    return false;
  }
  const Player2ACDrawPayload &payload = *proof.title.payload;
  const Player2ACVertexPayload &vertices = *payload.vertices;
  const Player2ACIndexPayload &indices = *payload.indices;
  geometry.kind = proof.title.kind;
  geometry.ordinal = proof.title.ordinal;
  geometry.primitive_type = proof.title.identity.primitive_type;
  geometry.rasterizer_mode_control = proof.backend.rasterizer_mode_control;
  geometry.vertex_stride = vertices.fetch.stride;
  geometry.source_vertex_count = vertices.vertex_count;
  geometry.submitted_index_count = indices.submitted_index_count;
  geometry.skinned = payload.palette_required;
  geometry.palette_record_count =
      payload.palette == nullptr ? 0 : payload.palette->record_count;
  geometry.bounds_min.fill(std::numeric_limits<float>::max());
  geometry.bounds_max.fill(std::numeric_limits<float>::lowest());

  // The captured vf95 range can be much larger than one mesh's indexed
  // subset. Sort the real submitted indices instead of zeroing a bitmap for
  // the whole source stream on every draw.
  std::vector<uint16_t> referenced_indices = indices.indices;
  std::ranges::sort(referenced_indices);
  const auto unique_end = std::ranges::unique(referenced_indices).begin();
  referenced_indices.erase(unique_end, referenced_indices.end());
  uint64_t fingerprint = kFnvOffsetBasis;
  for (const uint16_t vertex_index : referenced_indices) {
    DecodedVertex vertex;
    if (!DecodeVertex(vertices, proof.title.kind, vertex_index, vertex) ||
        !ValidatePaletteReferences(
            vertex, geometry.palette_record_count, payload.constants,
            geometry.maximum_referenced_palette_record)) {
      return false;
    }
    ExtendBounds(geometry, vertex.position);
    FingerprintWord(fingerprint, vertex_index);
    for (const float value : vertex.position) {
      FingerprintFloat(fingerprint, value);
    }
    for (const float value : vertex.normal) {
      FingerprintFloat(fingerprint, value);
    }
    if (vertex.skinned) {
      for (const float value : vertex.weights) {
        FingerprintFloat(fingerprint, value);
      }
      for (const uint8_t bone : vertex.bone_indices) {
        fingerprint = (fingerprint ^ bone) * kFnvPrime;
      }
    }
    ++geometry.referenced_vertex_count;
  }
  geometry.decoded_fingerprint = fingerprint;
  geometry.valid = geometry.referenced_vertex_count != 0 &&
                   geometry.decoded_fingerprint != 0;
  return geometry.valid;
}

size_t KindIndex(Player2ACTitleKind kind) {
  switch (kind) {
  case Player2ACTitleKind::kSingleStream32:
    return 0;
  case Player2ACTitleKind::kSingleStream96:
    return 1;
  case Player2ACTitleKind::kSkinned36:
    return 2;
  case Player2ACTitleKind::kSkinned44:
    return 3;
  default:
    return 4;
  }
}

} // namespace

Player2ACCompositeRenderPlan BuildPlayer2ACCompositeRenderPlan(
    std::shared_ptr<const Player2ACFrameSnapshot> frame) {
  Player2ACCompositeRenderPlan plan;
  plan.frame = std::move(frame);
  if (plan.frame == nullptr) {
    return plan;
  }
  plan.readiness.sequence = plan.frame->sequence;
  if (!plan.frame->valid()) {
    plan.readiness.blocker = Player2ACCompositeBlocker::kInvalidFrame;
    return plan;
  }
  plan.readiness.snapshot_exact = true;
  plan.readiness.draw_count = static_cast<uint32_t>(plan.frame->draws.size());
  plan.readiness.total_index_count = plan.frame->total_index_count;
  plan.draws.reserve(plan.frame->draws.size());

  uint32_t previous_ordinal = 0;
  for (const Player2ACDrawProof &proof : plan.frame->draws) {
    if (!proof.valid() || proof.title.payload == nullptr ||
        !proof.title.payload->valid()) {
      plan.readiness.blocker = Player2ACCompositeBlocker::kInvalidPayload;
      return plan;
    }
    if (!proof.title.payload->constants.valid) {
      plan.readiness.blocker = Player2ACCompositeBlocker::kInvalidPayload;
      return plan;
    }
    const size_t kind_index = KindIndex(proof.title.kind);
    if (kind_index >= plan.readiness.draws_by_kind.size()) {
      plan.readiness.blocker = Player2ACCompositeBlocker::kGeometryDecodeFailed;
      return plan;
    }
    ++plan.readiness.draws_by_kind[kind_index];
    if (!plan.draws.empty() && proof.title.ordinal <= previous_ordinal) {
      plan.readiness.blocker = Player2ACCompositeBlocker::kGeometryDecodeFailed;
      return plan;
    }
    previous_ordinal = proof.title.ordinal;

    Player2ACDecodedDrawGeometry geometry;
    if (!DecodeDraw(proof, geometry)) {
      plan.readiness.blocker = Player2ACCompositeBlocker::kGeometryDecodeFailed;
      return plan;
    }
    plan.readiness.decoded_vertex_reference_count +=
        geometry.referenced_vertex_count;
    plan.draws.push_back(std::move(geometry));
  }

  plan.readiness.payloads_exact = true;
  plan.readiness.constants_captured = true;
  plan.readiness.geometry_decoded = true;
  plan.readiness.order_preserved =
      plan.draws.size() == plan.frame->draws.size();
  // Do not turn a real-data proof into a fake renderer. The common pixel port
  // is available, but every draw still needs its exact bound VS port and the
  // resolved MAIN -> COMP handoff before command recording can be enabled.
  plan.readiness.blocker =
      Player2ACCompositeBlocker::kMissingVertexProgramPorts;
  return plan;
}

bool Player2ACCompositeObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_player_2ac_renderer_observer);
}

void Player2ACCompositeObserverFrameEnd() {
  if (!Player2ACCompositeObserverEnabled()) {
    std::lock_guard lock(g_plan_mutex);
    g_published_plan.reset();
    g_last_built_sequence = 0;
    g_last_logged_blocker = Player2ACCompositeBlocker::kNone;
    g_announced_valid_plan = false;
    return;
  }

  const std::shared_ptr<const Player2ACFrameSnapshot> frame =
      LatestPlayer2ACFrameSnapshot();
  if (frame == nullptr) {
    return;
  }
  {
    std::lock_guard lock(g_plan_mutex);
    if (frame->sequence == g_last_built_sequence) {
      return;
    }
  }

  auto plan = std::make_shared<Player2ACCompositeRenderPlan>(
      BuildPlayer2ACCompositeRenderPlan(frame));
  {
    std::lock_guard lock(g_plan_mutex);
    g_last_built_sequence = frame->sequence;
    g_published_plan = plan;
    const bool first_valid_plan =
        plan->observer_valid() && !g_announced_valid_plan;
    if (plan->readiness.blocker != g_last_logged_blocker || first_valid_plan) {
      g_last_logged_blocker = plan->readiness.blocker;
      g_announced_valid_plan |= plan->observer_valid();
      REXLOG_INFO(
          "Table Tennis 2AC renderer observer: frame={} draws={} indices={} "
          "decoded_refs={} kinds[32={},96={},36={},44={}] payloads={} "
          "constants={} geometry={} order={} pixel_port={} blocker={} "
          "gpu_recording_ready={} observer_only=true guest_suppressed=false",
          plan->readiness.sequence, plan->readiness.draw_count,
          plan->readiness.total_index_count,
          plan->readiness.decoded_vertex_reference_count,
          plan->readiness.draws_by_kind[0], plan->readiness.draws_by_kind[1],
          plan->readiness.draws_by_kind[2], plan->readiness.draws_by_kind[3],
          plan->readiness.payloads_exact, plan->readiness.constants_captured,
          plan->readiness.geometry_decoded, plan->readiness.order_preserved,
          plan->readiness.pixel_program_port_available,
          Player2ACCompositeBlockerName(plan->readiness.blocker),
          plan->readiness.gpu_recording_ready);
    }
  }
}

std::shared_ptr<const Player2ACCompositeRenderPlan>
LatestPlayer2ACCompositeRenderPlan() {
  std::lock_guard lock(g_plan_mutex);
  return g_published_plan;
}

const char *Player2ACCompositeBlockerName(Player2ACCompositeBlocker blocker) {
  switch (blocker) {
  case Player2ACCompositeBlocker::kNone:
    return "none";
  case Player2ACCompositeBlocker::kMissingFrame:
    return "missing frame";
  case Player2ACCompositeBlocker::kInvalidFrame:
    return "invalid frame";
  case Player2ACCompositeBlocker::kInvalidPayload:
    return "invalid payload";
  case Player2ACCompositeBlocker::kGeometryDecodeFailed:
    return "geometry decode failed";
  case Player2ACCompositeBlocker::kMissingVertexProgramPorts:
    return "missing vertex program ports";
  case Player2ACCompositeBlocker::kMissingResolvedSceneInput:
    return "missing resolved scene input";
  case Player2ACCompositeBlocker::kMissingCompositeTransaction:
    return "missing composite transaction";
  }
  return "unknown";
}

} // namespace tabletennis::native
