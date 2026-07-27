#pragma once

#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_texture_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

// Immutable host-endian geometry copied while the streaming-owned guest
// buffers are live.
struct VenueMeshSnapshot {
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 2>> texcoords0;
  std::vector<std::array<float, 2>> texcoords1;
  std::vector<std::array<float, 4>> colors;
  std::vector<uint16_t> indices;
  uint32_t primitive_type = 0;
  uint32_t source_vertex_alias = 0;
  uint32_t source_index_alias = 0;
  uint32_t source_vertex_stride = 0;
  uint32_t submitted_index_count = 0;
  uint64_t vertex_fingerprint = 0;
  uint64_t index_fingerprint = 0;

  bool valid() const {
    return !positions.empty() &&
           texcoords0.size() == positions.size() &&
           texcoords1.size() == positions.size() &&
           colors.size() == positions.size() && !indices.empty() &&
           submitted_index_count == indices.size();
  }
};

struct VenueMaterialSnapshot {
  uint32_t vertex_declaration = 0;
  std::array<std::array<uint32_t, 6>, 2> texture_fetches{};
  std::array<float, 28> vertex_constants_0_6{};
  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> pixel_constant_20{};
  std::array<float, 4> pixel_constant_46{};
  std::array<std::shared_ptr<const TextureSnapshot>, 2> textures{};
  bool wvp_verified = false;
  bool valid = false;
};

struct VenueDrawSnapshot {
  std::shared_ptr<const VenueMeshSnapshot> mesh;
  VenueMaterialSnapshot material{};
  std::array<float, 16> world_view_projection{};
  uint32_t ordinal = 0;
};

// Ordered identity-only record for target-program engine submissions after
// the verified seven-draw prefix. These are diagnostics for discovering how
// engine draws expand into GPU draws; they are never observer or replacement
// input.
struct VenueProgramCandidateDiagnostic {
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t scope_shader = 0;
  uint32_t scope_model = 0;
  uint32_t scope_geometry_index = 0;
  uint32_t scope_lod = 0;
  uint32_t vertex_aggregate = 0;
  uint32_t vertex_declaration = 0;
  uint32_t vertex_buffer_alias = 0;
  uint32_t index_buffer_alias = 0;
  uint32_t vertex_stride = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint64_t world_hash = 0;
  uint64_t world_view_projection_hash = 0;
  bool scope_valid = false;
  bool alternate_pass = false;
};

// Current-frame serving token for one of the already verified first seven
// draws. The diagnostic ledger below can never produce one of these.
struct VenueReplacementCandidate {
  uint64_t generation = 0;
  uint32_t prefix_index = 0;
  VenueDrawSnapshot draw{};

  bool valid() const {
    return generation != 0 && prefix_index < 7 &&
           draw.mesh != nullptr && draw.mesh->valid() &&
           draw.material.valid;
  }
};

struct VenueFrameSnapshot {
  uint64_t sequence = 0;
  uint32_t program_pair = 0;
  uint32_t pass_descriptor = 0;
  uint32_t submitted_index_count = 0;
  uint32_t copy_failures = 0;
  uint32_t dropped_later_program_candidates = 0;
  bool trace_prefix_verified = false;
  std::vector<VenueDrawSnapshot> draws;
  std::vector<VenueProgramCandidateDiagnostic> later_program_candidates;

  bool valid() const {
    if (!trace_prefix_verified || copy_failures != 0 || draws.empty()) {
      return false;
    }
    for (const VenueDrawSnapshot& draw : draws) {
      if (draw.mesh == nullptr || !draw.mesh->valid() ||
          !draw.material.valid) {
        return false;
      }
    }
    return true;
  }
};

// Capture for the first real static-venue shader family. Capture itself never
// suppresses a draw; the renderer may selectively serve only the verified
// ordered prefix.
void ObserveVenueFamilyDraw(uint8_t* guest_base,
                            const SceneCatalogDrawOccurrence& draw);
void VenueFamilyFrameEnd();

bool VenueFamilyObserverEnabled();
bool VenueFamilyCaptureEnabled();
uint32_t VenueFamilyReplacementDrawCount();
bool HasVenueFrameSnapshot();
std::shared_ptr<const VenueFrameSnapshot> LatestVenueFrameSnapshot();
std::shared_ptr<const VenueReplacementCandidate>
LatestVenueReplacementCandidate(uint32_t prefix_index);

}  // namespace tabletennis::native
