#include "native/tabletennis_venue_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_venue_full_family.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_venue_observer, false, "Table Tennis",
    "Overlay the first trace-verified real static-venue shader family. "
    "Observer-only; guest draws remain enabled.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    tabletennis_native_venue_replace_draws, 0, "Table Tennis",
    "Replace this many draws from the end of the trace-verified 7-draw venue "
    "prefix in-order. Earlier draws require a previously verified full frame.")
    .range(0, 7)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

// The live title objects corresponding to GPU pair
// 0E9982BE6B1E99A1 / 328FA02B07C392DC. Program identity alone is treated as a
// candidate: the ordered trace prefix below is the verification gate.
constexpr uint32_t kTargetPass = 0x4021DE08;
constexpr uint32_t kTargetProgram = 0x40220580;
constexpr uint32_t kTargetVertexShader = 0x4021C430;
constexpr uint32_t kTargetPixelShader = 0x401FE790;
constexpr uint32_t kTargetVertexStride = 40;
constexpr uint32_t kMaximumPayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumLaterProgramCandidates = 256;
constexpr uint8_t kVertexEndian8In32 = 2;
constexpr uint32_t kColorOffset = 16;
constexpr uint32_t kTexcoord0Offset = 20;
constexpr uint32_t kTexcoord1Offset = 28;

// The first seven occurrences match trace commands 320-326 exactly. The
// venue can submit additional geometry afterward, so the verifier checks a
// stable prefix rather than assuming one arena's full draw count.
constexpr std::array<uint32_t, 7> kTracePrefix = {
    64, 51, 72, 22, 226, 94, 694};

struct MeshKey {
  uint32_t vertex_alias = 0;
  uint32_t index_alias = 0;
  uint32_t vertex_bytes = 0;
  uint32_t index_count = 0;
  uint32_t primitive_type = 0;
};

struct CachedMesh {
  MeshKey key{};
  std::shared_ptr<const VenueMeshSnapshot> snapshot;
};

std::mutex g_venue_mutex;
VenueFrameSnapshot g_building_frame;
std::shared_ptr<const VenueFrameSnapshot> g_published_frame;
std::array<std::shared_ptr<const VenueReplacementCandidate>,
           kTracePrefix.size()>
    g_latest_replacement_candidates;
std::vector<CachedMesh> g_mesh_cache;
uint64_t g_frame_sequence = 0;
uint64_t g_replacement_generation = 0;
bool g_logged_verified_family = false;
bool g_logged_later_program_candidates = false;

uint16_t LoadBeU16(const uint8_t* source) {
  uint16_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const uint8_t* source) {
  uint32_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::bit_cast<float>(std::byteswap(value));
}

uint64_t Fingerprint(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (uint8_t value : bytes) {
    hash = (hash ^ value) * 1099511628211ull;
  }
  return hash;
}

bool SameKey(const MeshKey& left, const MeshKey& right) {
  return left.vertex_alias == right.vertex_alias &&
         left.index_alias == right.index_alias &&
         left.vertex_bytes == right.vertex_bytes &&
         left.index_count == right.index_count &&
         left.primitive_type == right.primitive_type;
}

bool IsTargetCandidate(const SceneCatalogDrawOccurrence& draw) {
  return draw.pass.valid && draw.pass.pass_descriptor == kTargetPass &&
         draw.pass.program_pair == kTargetProgram &&
         draw.pass.vertex_shader == kTargetVertexShader &&
         draw.pass.pixel_shader == kTargetPixelShader &&
         draw.mesh.valid && draw.mesh.vertex_stride == kTargetVertexStride &&
         draw.state.valid && draw.world_view_projection_valid &&
         draw.submitted_index_count != 0;
}

bool IsTargetProgramCandidate(
    const SceneCatalogDrawOccurrence& draw) {
  return draw.pass.program_pair == kTargetProgram;
}

VenueProgramCandidateDiagnostic CaptureProgramCandidateDiagnostic(
    const SceneCatalogDrawOccurrence& draw) {
  return {
      .ordinal = draw.ordinal,
      .player = draw.player,
      .pass_descriptor = draw.pass.pass_descriptor,
      .program_pair = draw.pass.program_pair,
      .vertex_shader = draw.pass.vertex_shader,
      .pixel_shader = draw.pass.pixel_shader,
      .scope_shader = draw.scope.shader,
      .scope_model = draw.scope.model,
      .scope_geometry_index = draw.scope.geometry_index,
      .scope_lod = draw.scope.lod,
      .vertex_aggregate = draw.mesh.vertex_aggregate,
      .vertex_declaration = draw.state.vertex_declaration,
      .vertex_buffer_alias = draw.mesh.vertex_buffer_alias,
      .index_buffer_alias = draw.mesh.index_buffer_alias,
      .vertex_stride = draw.mesh.vertex_stride,
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .world_hash = draw.world_hash,
      .world_view_projection_hash =
          draw.world_view_projection_hash,
      .scope_valid = draw.scope_valid,
      .alternate_pass = draw.scope.alternate_pass,
  };
}

VenueMaterialSnapshot CaptureMaterial(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw) {
  VenueMaterialSnapshot material;
  material.vertex_declaration = draw.state.vertex_declaration;
  material.vertex_declaration_identity =
      ProbeVertexDeclaration(guest_base, material.vertex_declaration);
  material.texture_fetches = draw.state.texture_fetches;
  material.vertex_constants_0_6 =
      draw.state.vertex_constants_0_6;
  material.vertex_constants_12_15 =
      draw.state.vertex_constants_12_15;
  material.pixel_constant_20 = draw.state.pixel_constant_20;
  material.pixel_constant_46 = draw.state.pixel_constant_46;
  for (uint32_t slot = 0; slot < material.textures.size(); ++slot) {
    material.textures[slot] = CaptureTextureSnapshot(
        guest_base, kTargetPixelShader, slot,
        material.texture_fetches[slot]);
  }
  material.wvp_verified =
      std::equal(material.vertex_constants_12_15.begin(),
                 material.vertex_constants_12_15.end(),
                 draw.world_view_projection.begin());
  const bool texture_fetches_valid =
      std::all_of(material.texture_fetches.begin(),
                  material.texture_fetches.end(),
                  [](const auto& fetch) {
                    return (fetch[0] & 0x3u) == 2 &&
                           fetch[1] != 0;
                  });
  const bool constants_finite =
      std::all_of(material.vertex_constants_0_6.begin(),
                  material.vertex_constants_0_6.end(),
                  [](float value) { return std::isfinite(value); }) &&
      std::all_of(material.vertex_constants_12_15.begin(),
                  material.vertex_constants_12_15.end(),
                  [](float value) { return std::isfinite(value); }) &&
      std::all_of(material.pixel_constant_20.begin(),
                  material.pixel_constant_20.end(),
                  [](float value) { return std::isfinite(value); }) &&
      std::all_of(material.pixel_constant_46.begin(),
                  material.pixel_constant_46.end(),
                  [](float value) { return std::isfinite(value); });
  material.valid = material.vertex_declaration != 0 &&
                   material.vertex_declaration_identity.valid &&
                   texture_fetches_valid && constants_finite &&
                   material.wvp_verified &&
                   std::all_of(
                       material.textures.begin(), material.textures.end(),
                       [](const auto& texture) {
                         return texture != nullptr && texture->valid();
                       });
  return material;
}

std::shared_ptr<const VenueMeshSnapshot> CaptureMesh(
    uint8_t* base, const SceneCatalogDrawOccurrence& draw) {
  const SceneCatalogMeshIdentity& mesh = draw.mesh;
  if (base == nullptr || mesh.vertex_buffer_alias == 0 ||
      mesh.index_buffer_alias == 0 || mesh.vertex_buffer_bytes == 0 ||
      mesh.index_buffer_bytes == 0 ||
      mesh.vertex_buffer_bytes > kMaximumPayloadBytes ||
      mesh.index_buffer_bytes > kMaximumPayloadBytes ||
      mesh.vertex_buffer_bytes % kTargetVertexStride != 0 ||
      mesh.vertex_endian != kVertexEndian8In32 || mesh.index_is_32_bit ||
      mesh.index_element_size != sizeof(uint16_t) ||
      draw.submitted_index_count >
          mesh.index_buffer_bytes / sizeof(uint16_t)) {
    return nullptr;
  }

  const uint32_t vertex_count =
      mesh.vertex_buffer_bytes / kTargetVertexStride;
  if (vertex_count == 0) {
    return nullptr;
  }

  std::vector<uint8_t> vertex_bytes(mesh.vertex_buffer_bytes);
  std::vector<uint8_t> index_bytes(
      static_cast<size_t>(draw.submitted_index_count) * sizeof(uint16_t));
  if (!GuestTryCopy(vertex_bytes.data(),
                    REX_RAW_ADDR(mesh.vertex_buffer_alias),
                    vertex_bytes.size()) ||
      !GuestTryCopy(index_bytes.data(),
                    REX_RAW_ADDR(mesh.index_buffer_alias),
                    index_bytes.size())) {
    return nullptr;
  }

  auto snapshot = std::make_shared<VenueMeshSnapshot>();
  snapshot->raw_vertex_bytes = vertex_bytes;
  snapshot->raw_index_bytes = index_bytes;
  snapshot->positions.resize(vertex_count);
  snapshot->texcoords0.resize(vertex_count);
  snapshot->texcoords1.resize(vertex_count);
  snapshot->colors.resize(vertex_count);
  snapshot->indices.resize(draw.submitted_index_count);
  for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
    const uint8_t* source =
        vertex_bytes.data() +
        static_cast<size_t>(vertex) * kTargetVertexStride;
    auto& position = snapshot->positions[vertex];
    for (size_t axis = 0; axis < position.size(); ++axis) {
      position[axis] = LoadBeF32(source + axis * sizeof(float));
      if (!std::isfinite(position[axis]) ||
          std::abs(position[axis]) > 1000000.0f) {
        return nullptr;
      }
    }

    for (size_t axis = 0; axis < snapshot->texcoords0[vertex].size();
         ++axis) {
      const float texcoord0 =
          LoadBeF32(source + kTexcoord0Offset + axis * sizeof(float));
      const float texcoord1 =
          LoadBeF32(source + kTexcoord1Offset + axis * sizeof(float));
      if (!std::isfinite(texcoord0) || !std::isfinite(texcoord1) ||
          std::abs(texcoord0) > 1000000.0f ||
          std::abs(texcoord1) > 1000000.0f) {
        return nullptr;
      }
      snapshot->texcoords0[vertex][axis] = texcoord0;
      snapshot->texcoords1[vertex][axis] = texcoord1;
    }

    // vf0 uses 8-in-32 endian and the shader fetch writes .zyxw, matching
    // the proven table/net declaration.
    const uint8_t* color = source + kColorOffset;
    snapshot->colors[vertex] = {
        color[1] / 255.0f,
        color[2] / 255.0f,
        color[3] / 255.0f,
        color[0] / 255.0f,
    };
  }
  uint32_t minimum_index = std::numeric_limits<uint32_t>::max();
  uint32_t maximum_index = 0;
  for (uint32_t index = 0; index < draw.submitted_index_count; ++index) {
    const uint16_t decoded =
        LoadBeU16(index_bytes.data() + static_cast<size_t>(index) * 2);
    if (decoded >= vertex_count) {
      return nullptr;
    }
    snapshot->indices[index] = decoded;
    minimum_index = std::min<uint32_t>(minimum_index, decoded);
    maximum_index = std::max<uint32_t>(maximum_index, decoded);
  }

  snapshot->primitive_type = draw.primitive_type;
  snapshot->source_vertex_alias = mesh.vertex_buffer_alias;
  snapshot->source_index_alias = mesh.index_buffer_alias;
  snapshot->source_index_physical_address =
      GuestPhysicalAddressForVirtualAlias(mesh.index_buffer_alias);
  snapshot->source_vertex_stride = mesh.vertex_stride;
  snapshot->submitted_index_count = draw.submitted_index_count;
  snapshot->minimum_index = minimum_index;
  snapshot->maximum_index = maximum_index;
  snapshot->vertex_fingerprint = Fingerprint(vertex_bytes);
  snapshot->index_fingerprint = Fingerprint(index_bytes);
  return snapshot;
}

std::shared_ptr<const VenueMeshSnapshot> CaptureMeshCached(
    uint8_t* base, const SceneCatalogDrawOccurrence& draw) {
  const MeshKey key = {
      .vertex_alias = draw.mesh.vertex_buffer_alias,
      .index_alias = draw.mesh.index_buffer_alias,
      .vertex_bytes = draw.mesh.vertex_buffer_bytes,
      .index_count = draw.submitted_index_count,
      .primitive_type = draw.primitive_type,
  };

  {
    std::lock_guard lock(g_venue_mutex);
    const auto found =
        std::find_if(g_mesh_cache.begin(), g_mesh_cache.end(),
                     [&](const CachedMesh& cached) {
                       return SameKey(cached.key, key);
                     });
    if (found != g_mesh_cache.end()) {
      return found->snapshot;
    }
  }

  std::shared_ptr<const VenueMeshSnapshot> snapshot =
      CaptureMesh(base, draw);
  if (snapshot == nullptr) {
    return nullptr;
  }

  std::lock_guard lock(g_venue_mutex);
  const auto found =
      std::find_if(g_mesh_cache.begin(), g_mesh_cache.end(),
                   [&](const CachedMesh& cached) {
                     return SameKey(cached.key, key);
                   });
  if (found == g_mesh_cache.end()) {
    g_mesh_cache.push_back({key, snapshot});
  } else {
    snapshot = found->snapshot;
  }
  return snapshot;
}

bool HasTracePrefix(const VenueFrameSnapshot& frame) {
  if (frame.draws.size() < kTracePrefix.size()) {
    return false;
  }
  for (size_t index = 0; index < kTracePrefix.size(); ++index) {
    if (frame.draws[index].mesh == nullptr ||
        frame.draws[index].mesh->submitted_index_count !=
            kTracePrefix[index]) {
      return false;
    }
  }
  return true;
}

bool HasValidCapturedDraws(const VenueFrameSnapshot& frame) {
  if (frame.copy_failures != 0) {
    return false;
  }
  return std::all_of(
      frame.draws.begin(), frame.draws.end(),
      [](const VenueDrawSnapshot& draw) {
        return draw.mesh != nullptr && draw.mesh->valid() &&
               draw.material.valid;
      });
}

bool SameReplacementResources(const VenueDrawSnapshot& current,
                              const VenueDrawSnapshot& verified) {
  return current.mesh == verified.mesh &&
         current.material.textures[0] ==
             verified.material.textures[0] &&
         current.material.textures[1] ==
             verified.material.textures[1];
}

bool VenueFamilyCaptureRequested() {
  return REXCVAR_GET(tabletennis_native_venue_observer) ||
         REXCVAR_GET(tabletennis_native_venue_replace_draws) > 0;
}

}  // namespace

void ObserveVenueFamilyDraw(uint8_t* guest_base,
                            const SceneCatalogDrawOccurrence& draw) {
  const bool verified_capture_requested =
      VenueFamilyCaptureRequested();
  const bool full_family_requested =
      VenueFullFamilyObserverEnabled();
  if (!verified_capture_requested && !full_family_requested) {
    return;
  }

  const bool target_candidate = IsTargetCandidate(draw);
  VenueDrawSnapshot captured;
  bool capture_attempted = false;
  auto ensure_captured = [&]() -> const VenueDrawSnapshot& {
    if (!capture_attempted) {
      capture_attempted = true;
      captured.mesh = CaptureMeshCached(guest_base, draw);
      if (captured.mesh != nullptr) {
        captured.material = CaptureMaterial(guest_base, draw);
      }
      captured.world_view_projection =
          draw.world_view_projection;
      captured.ordinal = draw.ordinal;
    }
    return captured;
  };

  // This ledger is independent of the prefix verifier below. Every exact
  // title submission is appended, so repeated adjacent counts remain distinct.
  if (full_family_requested && target_candidate) {
    ObserveVenueFullFamilyDraw(draw, ensure_captured());
  }

  if (!verified_capture_requested) {
    return;
  }

  // The GPU trace contains a longer target shader sequence, but owner-pass
  // telemetry proves engine submissions after this prefix do not map 1:1 to
  // it. Preserve their ordered identities for diagnosis without copying
  // payloads or admitting them to the verified observer/replacement path.
  {
    std::lock_guard lock(g_venue_mutex);
    if (g_building_frame.draws.size() == kTracePrefix.size() &&
        HasTracePrefix(g_building_frame) &&
        HasValidCapturedDraws(g_building_frame)) {
      if (IsTargetProgramCandidate(draw)) {
        if (g_building_frame.later_program_candidates.size() <
            kMaximumLaterProgramCandidates) {
          g_building_frame.later_program_candidates.push_back(
              CaptureProgramCandidateDiagnostic(draw));
        } else {
          ++g_building_frame.dropped_later_program_candidates;
        }
      }
      return;
    }
  }

  if (!target_candidate) {
    return;
  }

  // Program 40220580 owns more than one material variant. Only admit the
  // ordered prefix proven against the host GPU trace; later occurrences stay
  // guest-only until their exact shader/material identity is captured.
  {
    std::lock_guard lock(g_venue_mutex);
    size_t prefix_index = g_building_frame.draws.size();
    if (prefix_index == kTracePrefix.size()) {
      return;
    }
    if (draw.submitted_index_count != kTracePrefix[prefix_index]) {
      if (draw.submitted_index_count != kTracePrefix.front()) {
        return;
      }
      g_building_frame = {};
      prefix_index = 0;
    }
  }

  const VenueDrawSnapshot& shared_capture = ensure_captured();
  if (shared_capture.mesh == nullptr) {
    std::lock_guard lock(g_venue_mutex);
    ++g_building_frame.copy_failures;
    return;
  }

  std::lock_guard lock(g_venue_mutex);
  const size_t prefix_index = g_building_frame.draws.size();
  if (prefix_index == kTracePrefix.size() ||
      draw.submitted_index_count != kTracePrefix[prefix_index]) {
    return;
  }
  g_building_frame.program_pair = draw.pass.program_pair;
  g_building_frame.pass_descriptor = draw.pass.pass_descriptor;
  g_building_frame.submitted_index_count += draw.submitted_index_count;
  g_building_frame.draws.push_back({
      .mesh = shared_capture.mesh,
      .material = shared_capture.material,
      .world_view_projection = draw.world_view_projection,
      .ordinal = draw.ordinal,
  });
  const size_t captured_index = g_building_frame.draws.size() - 1;
  const bool current_prefix_fully_verified =
      captured_index + 1 == kTracePrefix.size() &&
      HasTracePrefix(g_building_frame) &&
      HasValidCapturedDraws(g_building_frame);
  const bool verified_by_previous_frame =
      g_published_frame != nullptr && g_published_frame->valid() &&
      captured_index < g_published_frame->draws.size() &&
      SameReplacementResources(
          g_building_frame.draws[captured_index],
          g_published_frame->draws[captured_index]);
  if (current_prefix_fully_verified || verified_by_previous_frame) {
    g_latest_replacement_candidates[captured_index] =
        std::make_shared<const VenueReplacementCandidate>(
            VenueReplacementCandidate{
                .generation = ++g_replacement_generation,
                .prefix_index = static_cast<uint32_t>(captured_index),
                .draw = g_building_frame.draws[captured_index],
            });
  }
}

void VenueFamilyFrameEnd() {
  const bool enabled = VenueFamilyCaptureRequested();
  std::lock_guard lock(g_venue_mutex);
  ++g_frame_sequence;
  if (!enabled) {
    g_building_frame = {};
    g_published_frame.reset();
    g_latest_replacement_candidates = {};
    g_logged_later_program_candidates = false;
    return;
  }

  g_building_frame.sequence = g_frame_sequence;
  g_building_frame.trace_prefix_verified = HasTracePrefix(g_building_frame);
  if (!g_building_frame.draws.empty()) {
    g_published_frame =
        std::make_shared<const VenueFrameSnapshot>(
            std::move(g_building_frame));
    if (g_published_frame->valid() && !g_logged_verified_family) {
      g_logged_verified_family = true;
      REXLOG_INFO(
          "Table Tennis venue observer: verified real shader family "
          "pass={:08X} program={:08X} draws={} submitted_indices={} "
          "trace_prefix=64,51,72,22,226,94,694",
          g_published_frame->pass_descriptor,
          g_published_frame->program_pair,
          g_published_frame->draws.size(),
          g_published_frame->submitted_index_count);
      const VenueMaterialSnapshot& material =
          g_published_frame->draws.front().material;
      REXLOG_INFO(
          "  venue_state declaration={:08X} tf0={:08X},{:08X} "
          "tf1={:08X},{:08X} ps_c20_z={:.6f} ps_c46_x={:.6f} "
          "wvp_verified={}",
          material.vertex_declaration,
          material.texture_fetches[0][0],
          material.texture_fetches[0][1],
          material.texture_fetches[1][0],
          material.texture_fetches[1][1],
          material.pixel_constant_20[2],
          material.pixel_constant_46[0],
          material.wvp_verified);
    }
    if (g_published_frame->valid() &&
        !g_published_frame->later_program_candidates.empty() &&
        !g_logged_later_program_candidates) {
      g_logged_later_program_candidates = true;
      REXLOG_INFO(
          "Table Tennis venue diagnostics: captured {} ordered "
          "target-program engine submissions after verified draw 6 "
          "(dropped={}); identity-only, never served",
          g_published_frame->later_program_candidates.size(),
          g_published_frame->dropped_later_program_candidates);
      for (size_t index = 0;
           index <
           g_published_frame->later_program_candidates.size();
           ++index) {
        const VenueProgramCandidateDiagnostic& candidate =
            g_published_frame->later_program_candidates[index];
        REXLOG_INFO(
            "  later[{}] ordinal={} count={} prim={:X} "
            "pass={:08X} program={:08X} vs={:08X} ps={:08X} "
            "player={:08X} scope={}:shader={:08X},model={:08X},"
            "geom={},lod={},alt={} mesh={:08X} decl={:08X} "
            "stride={} vb={:08X} ib={:08X} world={:016X} "
            "wvp={:016X}",
            index, candidate.ordinal,
            candidate.submitted_index_count,
            candidate.primitive_type,
            candidate.pass_descriptor, candidate.program_pair,
            candidate.vertex_shader, candidate.pixel_shader,
            candidate.player, candidate.scope_valid,
            candidate.scope_shader, candidate.scope_model,
            candidate.scope_geometry_index, candidate.scope_lod,
            candidate.alternate_pass, candidate.vertex_aggregate,
            candidate.vertex_declaration, candidate.vertex_stride,
            candidate.vertex_buffer_alias,
            candidate.index_buffer_alias, candidate.world_hash,
            candidate.world_view_projection_hash);
      }
    }
  } else {
    g_published_frame.reset();
  }
  g_building_frame = {};
}

bool VenueFamilyObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_venue_observer);
}

bool VenueFamilyCaptureEnabled() {
  return VenueFamilyCaptureRequested();
}

uint32_t VenueFamilyReplacementDrawCount() {
  return static_cast<uint32_t>(
      std::max(REXCVAR_GET(tabletennis_native_venue_replace_draws), 0));
}

bool HasVenueFrameSnapshot() {
  std::lock_guard lock(g_venue_mutex);
  return g_published_frame != nullptr && g_published_frame->valid();
}

std::shared_ptr<const VenueFrameSnapshot> LatestVenueFrameSnapshot() {
  std::lock_guard lock(g_venue_mutex);
  return g_published_frame;
}

std::shared_ptr<const VenueReplacementCandidate>
LatestVenueReplacementCandidate(uint32_t prefix_index) {
  std::lock_guard lock(g_venue_mutex);
  return prefix_index < g_latest_replacement_candidates.size()
             ? g_latest_replacement_candidates[prefix_index]
             : nullptr;
}

}  // namespace tabletennis::native
