#pragma once

#include "native/tabletennis_crowd_snapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct CrowdBackendBlockContractTelemetry {
  uint32_t backend = 0;
  uint32_t render_pass_key = 0;
  uint32_t surface_pitch = 0;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t color_control = 0;
  uint32_t blend_control_0 = 0;
  uint32_t rasterizer_mode_control = 0;
  uint32_t primitive_restart_index = 0;
  std::array<uint32_t, 4> color_attachment_formats{};
  uint32_t color_attachment_count = 0;
  uint32_t depth_attachment_format = 0;
  uint32_t stencil_attachment_format = 0;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;
  bool primitive_restart_enabled = false;
  bool rasterizer_mode_control_valid = false;
  bool draw_state_contract_valid = false;
  bool attachment_contract_valid = false;
  bool block_uniform = false;
};

struct CrowdFrameSnapshot {
  uint64_t sequence = 0;
  uint32_t render_scope_count = 0;
  uint32_t drawable_submit_count = 0;
  uint32_t owner_indexed_draw_count = 0;
  uint32_t mesh_valid_owner_draw_count = 0;
  uint32_t stride_36_owner_draw_count = 0;
  uint32_t secondary_stream_owner_draw_count = 0;
  uint32_t nonzero_index_owner_draw_count = 0;
  uint32_t first_owner_vertex_aggregate = 0;
  uint32_t first_owner_primary_stream = 0;
  uint32_t first_owner_secondary_stream = 0;
  uint32_t first_owner_vertex_alias = 0;
  uint32_t first_owner_vertex_stride = 0;
  uint32_t first_owner_index_alias = 0;
  uint32_t first_owner_index_element_size = 0;
  uint32_t first_owner_mesh_read_failures = 0;
  uint32_t first_owner_primitive_type = 0;
  uint32_t first_owner_submitted_index_count = 0;
  bool first_owner_mesh_valid = false;
  uint32_t candidate_draw_count = 0;
  uint32_t valid_draw_count = 0;
  uint32_t dropped_draw_count = 0;
  uint32_t submitted_index_count = 0;
  uint32_t unique_geometry_count = 0;
  uint32_t copy_failures = 0;
  uint32_t backend_c6_draw_count = 0;
  uint32_t backend_finalized_block_count = 0;
  uint32_t backend_matched_block_count = 0;
  uint32_t backend_pending_block_count = 0;
  uint32_t backend_unmatched_block_count = 0;
  uint32_t backend_repeated_tile_block_count = 0;
  uint32_t backend_fifo_disambiguated_block_count = 0;
  uint32_t backend_contract_mismatch_block_count = 0;
  uint32_t backend_overflow_block_count = 0;
  uint32_t backend_dropped_block_count = 0;
  uint32_t backend_captured_title_frame_count = 0;
  uint32_t backend_unique_render_pass_count = 0;
  uint32_t backend_last_block_draw_count = 0;
  uint32_t backend_last_tile_ordinal = 0;
  uint64_t backend_last_title_generation = 0;
  uint64_t backend_last_sequence_fingerprint = 0;
  CrowdBackendBlockContractTelemetry backend_last_contract{};
  bool backend_block_proof_observed = false;
  bool family_contract_verified = false;
  bool trace_parity_verified = false;
  std::vector<CrowdDrawSnapshot> draws;

  bool valid() const {
    return family_contract_verified && candidate_draw_count != 0 &&
           valid_draw_count == candidate_draw_count &&
           dropped_draw_count == 0 && copy_failures == 0 &&
           draws.size() == candidate_draw_count;
  }
};

// One draw from a title frame whose complete ordered C6 backend block has
// already matched. Candidates are armed only for a later ordered tile block,
// while its prefix continues to match the proven title sequence.
struct CrowdReplacementCandidate {
  uint64_t title_generation = 0;
  uint64_t backend_block_sequence = 0;
  uint32_t tile_ordinal = 0;
  uint32_t draw_index = 0;
  uint32_t render_pass_key = 0;
  uint32_t primitive_type = 0;
  uint32_t submitted_index_count = 0;
  uint32_t guest_index_base = 0;
  std::shared_ptr<const CrowdFrameSnapshot> frame;

  bool valid() const {
    return title_generation != 0 && backend_block_sequence != 0 &&
           tile_ordinal >= 2 &&
           frame != nullptr && frame->valid() &&
           frame->sequence == title_generation &&
           draw_index < frame->draws.size() &&
           primitive_type == frame->draws[draw_index].primitive_type &&
           submitted_index_count ==
               frame->draws[draw_index].submitted_index_count &&
           frame->draws[draw_index].indices != nullptr &&
           guest_index_base ==
               frame->draws[draw_index].indices->physical_address;
  }
};

bool CrowdObserverEnabled();

// Outer fxCrowdGfx::Render scope (sub_82385AB0). This validates the title
// owner and supplies high-level telemetry; draw admission uses the deeper
// drawable/model submit scope below.
void BeginCrowdRenderScope(uint8_t* guest_base, uint32_t crowd);
void EndCrowdRenderScope();

// sub_82385C88 receives the exact fxCrowdGfx owner plus one of the two
// drawable/model-record pairs stored on that owner. It synchronously walks
// instances and reaches sub_820EE910 -> sub_820EE6E8 -> DrawIndexedPrimitive.
// This is the semantic token used to admit crowd draws.
void BeginCrowdDrawableSubmitScope(uint8_t* guest_base, uint32_t crowd,
                                   uint32_t drawable, uint32_t model);
void EndCrowdDrawableSubmitScope();

void ObserveCrowdDraw(uint8_t* guest_base,
                      const SceneCatalogDrawOccurrence& draw);

// Dispatcher-side observer tap for contiguous translated C6 crowd blocks.
// Early matcher probes are ignored. Late non-C6 draws finalize a block for
// ordered comparison against the bounded title-frame FIFO. This always
// returns false, so it can never claim or suppress a guest draw.
bool ObserveCrowdBackendProofDraw(
    const rex::graphics::NativeGuestDrawContext& context);

// Consumes the ordered candidate armed by ObserveCrowdBackendProofDraw for
// this exact late backend callback. A failed identity check discards it.
std::shared_ptr<const CrowdReplacementCandidate>
ConsumeCrowdReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext& context);

void CrowdObserverFrameEnd();

std::shared_ptr<const CrowdFrameSnapshot> LatestCrowdFrameSnapshot();

// Exact immutable frame backing the currently proven backend block. This is
// intentionally separate from the newest capture, which may be several
// translation frames ahead of the borrowed callback.
std::shared_ptr<const CrowdFrameSnapshot>
LatestCrowdBackendProofFrameSnapshot();

}  // namespace tabletennis::native
