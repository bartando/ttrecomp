#include "native/tabletennis_native_scene_compositor.h"

#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_full_family.h"

#include <algorithm>
#include <utility>

#include <rex/logging.h>

namespace tabletennis::native {
namespace {

void Reject(NativeSceneCompositionPlan &plan,
            NativeSceneCompositionRejectReason reason) {
  plan.readiness.reject_reason = reason;
  plan.readiness.observer_composition_ready = false;
  plan.draws.clear();
}

bool CatalogDrawMatches(const SceneCatalogDrawOccurrence &catalog,
                        const VenueFullFamilyDrawSnapshot &venue) {
  const SceneCatalogDrawOccurrence &source = venue.source;
  const VenueDrawSnapshot &captured = venue.captured;
  return source.ordinal == catalog.ordinal &&
         captured.ordinal == catalog.ordinal &&
         source.player == catalog.player &&
         source.scope_valid == catalog.scope_valid &&
         source.scope.shader == catalog.scope.shader &&
         source.scope.model == catalog.scope.model &&
         source.scope.geometry_index == catalog.scope.geometry_index &&
         source.pass.pass_descriptor == catalog.pass.pass_descriptor &&
         source.pass.program_pair == catalog.pass.program_pair &&
         source.pass.vertex_shader == catalog.pass.vertex_shader &&
         source.pass.pixel_shader == catalog.pass.pixel_shader &&
         source.primitive_type == catalog.primitive_type &&
         source.submitted_index_count == catalog.submitted_index_count &&
         captured.mesh != nullptr &&
         captured.mesh->submitted_index_count == catalog.submitted_index_count;
}

bool CatalogDrawMatches(const SceneCatalogDrawOccurrence &catalog,
                        const CrowdDrawSnapshot &crowd) {
  return crowd.ordinal == catalog.ordinal &&
         crowd.shader == catalog.scope.shader &&
         crowd.model == catalog.scope.model &&
         crowd.geometry_index == catalog.scope.geometry_index &&
         crowd.pass_descriptor == catalog.pass.pass_descriptor &&
         crowd.program_pair == catalog.pass.program_pair &&
         crowd.vertex_shader == catalog.pass.vertex_shader &&
         crowd.pixel_shader == catalog.pass.pixel_shader &&
         crowd.primitive_type == catalog.primitive_type &&
         crowd.submitted_index_count == catalog.submitted_index_count;
}

bool CatalogDrawMatches(const SceneCatalogDrawOccurrence &catalog,
                        const Venue14DDrawSnapshot &venue) {
  if (!venue.valid() || venue.title == nullptr ||
      venue.title->vertices == nullptr || venue.title->indices == nullptr) {
    return false;
  }
  const Venue14DTitleDrawSnapshot &title = *venue.title;
  const Venue14DVertexPayload &vertices = *title.vertices;
  const Venue14DIndexPayload &indices = *title.indices;
  return title.ordinal == catalog.ordinal && catalog.player == 0 &&
         title.pass_descriptor == catalog.pass.pass_descriptor &&
         title.program_pair == catalog.pass.program_pair &&
         title.title_vertex_shader == catalog.pass.vertex_shader &&
         title.title_pixel_shader == catalog.pass.pixel_shader &&
         catalog.pass.shader_fingerprints_valid &&
         venue.backend.vertex_shader_hash ==
             Venue14DVertexShaderForLayout(vertices.stride, vertices.endian) &&
         venue.backend.pixel_shader_hash == catalog.pass.pixel_shader_hash &&
         title.owner_kind == static_cast<uint32_t>(catalog.owner.kind) &&
         title.owner == catalog.owner.owner &&
         title.owner_renderable == catalog.owner.renderable &&
         title.identity.primitive_type == catalog.primitive_type &&
         title.identity.submitted_index_count ==
             catalog.submitted_index_count &&
         title.identity.guest_index_base == indices.physical_address &&
         title.identity.guest_vertex_base == vertices.physical_address &&
         title.identity.guest_vertex_bytes == vertices.byte_count &&
         title.identity.guest_vertex_endian == vertices.endian &&
         catalog.mesh.valid &&
         vertices.source_virtual_alias == catalog.mesh.vertex_buffer_alias &&
         vertices.byte_count == catalog.mesh.vertex_buffer_bytes &&
         vertices.stride == catalog.mesh.vertex_stride &&
         vertices.endian == catalog.mesh.vertex_endian &&
         indices.source_virtual_alias == catalog.mesh.index_buffer_alias &&
         indices.submitted_index_count == catalog.submitted_index_count &&
         catalog.mesh.index_element_size == sizeof(uint16_t) &&
         !catalog.mesh.index_is_32_bit &&
         title.material.vertex_declaration ==
             catalog.state.vertex_declaration &&
         title.material.vertex_declaration == catalog.mesh.vertex_declaration &&
         title.material.texture_fetches[0] ==
             catalog.state.texture_fetches[0] &&
         title.material.texture_fetches[1] == catalog.state.texture_fetches[1];
}

bool CatalogDrawMatches(const SceneCatalogDrawOccurrence &catalog,
                        const PlayerSkinDrawSnapshot &player) {
  return player.ordinal == catalog.ordinal && player.player == catalog.player &&
         player.shader == catalog.scope.shader &&
         player.model == catalog.scope.model &&
         player.geometry_index == catalog.scope.geometry_index &&
         player.pass_descriptor == catalog.pass.pass_descriptor &&
         player.program_pair == catalog.pass.program_pair &&
         player.vertex_shader == catalog.pass.vertex_shader &&
         player.pixel_shader == catalog.pass.pixel_shader &&
         player.primitive_type == catalog.primitive_type &&
         player.submitted_index_count == catalog.submitted_index_count &&
         player.alternate_pass == catalog.scope.alternate_pass;
}

void LogCatalogMismatch(const SceneCatalogDrawOccurrence &catalog,
                        const VenueFullFamilyDrawSnapshot &venue) {
  const SceneCatalogDrawOccurrence &source = venue.source;
  const VenueDrawSnapshot &captured = venue.captured;
  REXLOG_INFO(
      "Table Tennis native composition mismatch: family=venue_ps328 "
      "ordinal[catalog={} source={} captured={}] "
      "player[catalog={:08X} source={:08X}] "
      "scope[catalog={:08X}/{:08X}/{} source={:08X}/{:08X}/{}] "
      "pass[catalog={:08X}/{:08X}/{:08X}/{:08X} "
      "source={:08X}/{:08X}/{:08X}/{:08X}] "
      "draw[catalog={}/{} source={}/{} captured_count={}]",
      catalog.ordinal, source.ordinal, captured.ordinal, catalog.player,
      source.player, catalog.scope.shader, catalog.scope.model,
      catalog.scope.geometry_index, source.scope.shader, source.scope.model,
      source.scope.geometry_index, catalog.pass.pass_descriptor,
      catalog.pass.program_pair, catalog.pass.vertex_shader,
      catalog.pass.pixel_shader, source.pass.pass_descriptor,
      source.pass.program_pair, source.pass.vertex_shader,
      source.pass.pixel_shader, catalog.primitive_type,
      catalog.submitted_index_count, source.primitive_type,
      source.submitted_index_count,
      captured.mesh != nullptr ? captured.mesh->submitted_index_count : 0);
}

void LogCatalogMismatch(const SceneCatalogDrawOccurrence &catalog,
                        const Venue14DDrawSnapshot &venue) {
  const Venue14DTitleDrawSnapshot *title = venue.title.get();
  const Venue14DVertexPayload *vertices =
      title != nullptr ? title->vertices.get() : nullptr;
  REXLOG_INFO(
      "Table Tennis native composition mismatch: family=venue_14d "
      "ordinal[catalog={} title={}] pass[catalog={:08X}/{:08X}/{:08X}/"
      "{:08X} title={:08X}/{:08X}/{:08X}/{:08X}] "
      "hash[pass={:016X}/{:016X}/{} "
      "bound_catalog={:016X}/{:016X}/{}/{} backend={:016X}/{:016X} "
      "expected_vs={:016X}] "
      "draw[catalog={}/{} title={}/{}] owner[catalog={}/{:08X}/{:08X} "
      "title={}/{:08X}/{:08X}] "
      "vertex[catalog={:08X}/{}/{} title={:08X}/{}/{}]",
      catalog.ordinal, title != nullptr ? title->ordinal : 0,
      catalog.pass.pass_descriptor, catalog.pass.program_pair,
      catalog.pass.vertex_shader, catalog.pass.pixel_shader,
      title != nullptr ? title->pass_descriptor : 0,
      title != nullptr ? title->program_pair : 0,
      title != nullptr ? title->title_vertex_shader : 0,
      title != nullptr ? title->title_pixel_shader : 0,
      catalog.pass.vertex_shader_hash, catalog.pass.pixel_shader_hash,
      catalog.pass.shader_fingerprints_valid,
      catalog.bound_shaders.vertex_shader_hash,
      catalog.bound_shaders.pixel_shader_hash,
      catalog.bound_shaders.vertex_shader_valid,
      catalog.bound_shaders.pixel_shader_valid,
      venue.backend.vertex_shader_hash, venue.backend.pixel_shader_hash,
      Venue14DVertexShaderForLayout(
          vertices != nullptr ? vertices->stride : 0,
          vertices != nullptr ? vertices->endian : 0),
      catalog.primitive_type, catalog.submitted_index_count,
      title != nullptr ? title->identity.primitive_type : 0,
      title != nullptr ? title->identity.submitted_index_count : 0,
      static_cast<uint32_t>(catalog.owner.kind), catalog.owner.owner,
      catalog.owner.renderable, title != nullptr ? title->owner_kind : 0,
      title != nullptr ? title->owner : 0,
      title != nullptr ? title->owner_renderable : 0,
      catalog.mesh.vertex_buffer_alias, catalog.mesh.vertex_buffer_bytes,
      catalog.mesh.vertex_endian,
      vertices != nullptr ? vertices->source_virtual_alias : 0,
      vertices != nullptr ? vertices->byte_count : 0,
      vertices != nullptr ? vertices->endian : 0);
}

void LogCatalogMismatch(const SceneCatalogDrawOccurrence &catalog,
                        const CrowdDrawSnapshot &crowd) {
  REXLOG_INFO(
      "Table Tennis native composition mismatch: family=crowd_c6 "
      "ordinal[catalog={} crowd={}] scope[catalog={:08X}/{:08X}/{} "
      "crowd={:08X}/{:08X}/{}] "
      "pass[catalog={:08X}/{:08X}/{:08X}/{:08X} "
      "crowd={:08X}/{:08X}/{:08X}/{:08X}] "
      "draw[catalog={}/{} crowd={}/{}]",
      catalog.ordinal, crowd.ordinal, catalog.scope.shader,
      catalog.scope.model, catalog.scope.geometry_index, crowd.shader,
      crowd.model, crowd.geometry_index, catalog.pass.pass_descriptor,
      catalog.pass.program_pair, catalog.pass.vertex_shader,
      catalog.pass.pixel_shader, crowd.pass_descriptor, crowd.program_pair,
      crowd.vertex_shader, crowd.pixel_shader, catalog.primitive_type,
      catalog.submitted_index_count, crowd.primitive_type,
      crowd.submitted_index_count);
}

void LogCatalogMismatch(const SceneCatalogDrawOccurrence &catalog,
                        const PlayerSkinDrawSnapshot &player) {
  REXLOG_INFO(
      "Table Tennis native composition mismatch: family=player_ca9 "
      "ordinal[catalog={} player={}] player[catalog={:08X} draw={:08X}] "
      "scope[catalog={:08X}/{:08X}/{} player={:08X}/{:08X}/{}] "
      "pass[catalog={:08X}/{:08X}/{:08X}/{:08X} "
      "player={:08X}/{:08X}/{:08X}/{:08X}] "
      "draw[catalog={}/{} player={}/{}] alternate[catalog={} player={}]",
      catalog.ordinal, player.ordinal, catalog.player, player.player,
      catalog.scope.shader, catalog.scope.model,
      catalog.scope.geometry_index, player.shader, player.model,
      player.geometry_index, catalog.pass.pass_descriptor,
      catalog.pass.program_pair, catalog.pass.vertex_shader,
      catalog.pass.pixel_shader, player.pass_descriptor, player.program_pair,
      player.vertex_shader, player.pixel_shader, catalog.primitive_type,
      catalog.submitted_index_count, player.primitive_type,
      player.submitted_index_count, catalog.scope.alternate_pass,
      player.alternate_pass);
}

template <typename Draw, typename Ordinal, typename Matcher>
bool AppendFamilyDraws(NativeSceneCompositionPlan &plan,
                       const std::vector<Draw> &draws,
                       NativeSceneDrawFamily family,
                       const SceneDrawCatalogFrame &catalog,
                       Ordinal &&ordinal_of, Matcher &&matcher) {
  for (size_t draw_index = 0; draw_index < draws.size(); ++draw_index) {
    const Draw &draw = draws[draw_index];
    const uint32_t ordinal = ordinal_of(draw);
    if (ordinal == 0 || ordinal > catalog.ordered_draw_count) {
      plan.readiness.failed_family = family;
      plan.readiness.failed_family_draw_index =
          static_cast<uint32_t>(draw_index);
      plan.readiness.failed_ordinal = ordinal;
      Reject(plan, NativeSceneCompositionRejectReason::kOrdinalOutsideCatalog);
      return false;
    }
    const SceneCatalogDrawOccurrence &catalog_draw =
        catalog.ordered_draws[ordinal - 1];
    if (catalog_draw.ordinal != ordinal || !matcher(catalog_draw, draw)) {
      LogCatalogMismatch(catalog_draw, draw);
      plan.readiness.failed_family = family;
      plan.readiness.failed_family_draw_index =
          static_cast<uint32_t>(draw_index);
      plan.readiness.failed_ordinal = ordinal;
      Reject(plan,
             NativeSceneCompositionRejectReason::kCatalogIdentityMismatch);
      return false;
    }
    plan.draws.push_back({
        .family = family,
        .ordinal = ordinal,
        .family_draw_index = static_cast<uint32_t>(draw_index),
    });
  }
  return true;
}

bool SameFrame(uint64_t title_sequence, uint64_t family_sequence) {
  return title_sequence != 0 && family_sequence == title_sequence;
}

bool CrowdBackendProofValid(const CrowdFrameSnapshot &frame) {
  constexpr uint32_t kCrowdRasterizerMode = 0x00018002;
  const CrowdBackendBlockContractTelemetry &contract =
      frame.backend_last_contract;
  return frame.backend_block_proof_observed && contract.block_uniform &&
         contract.rasterizer_mode_control_valid &&
         contract.rasterizer_mode_control == kCrowdRasterizerMode;
}

} // namespace

NativeSceneCompositionPlan BuildNativeSceneCompositionPlan(
    std::shared_ptr<const TableTennisFrameScene> scene) {
  NativeSceneCompositionPlan plan;
  plan.scene = std::move(scene);
  if (plan.scene == nullptr) {
    return plan;
  }

  NativeSceneCompositionReadiness &ready = plan.readiness;
  ready.title_sequence = plan.scene->title.sequence;
  ready.gameplay_active = plan.scene->title.gameplay_active;
  if (!ready.gameplay_active) {
    Reject(plan, NativeSceneCompositionRejectReason::kGameplayInactive);
    return plan;
  }

  if (plan.scene->catalog == nullptr) {
    Reject(plan, NativeSceneCompositionRejectReason::kMissingCatalog);
    return plan;
  }
  const SceneDrawCatalogFrame &catalog = *plan.scene->catalog;
  ready.catalog_draw_count = catalog.ordered_draw_count;
  if (!SameFrame(ready.title_sequence, catalog.sequence)) {
    Reject(plan, NativeSceneCompositionRejectReason::kCatalogFrameMismatch);
    return plan;
  }
  // Aggregate read failures may belong to unrelated unselected draws. Every
  // selected family payload and catalog identity is validated below, while a
  // dropped ordered occurrence still makes the frame unusable.
  if (catalog.ordered_draw_count == 0 ||
      catalog.dropped_ordered_draws != 0) {
    Reject(plan, NativeSceneCompositionRejectReason::kCatalogIncomplete);
    return plan;
  }
  ready.catalog_exact = true;

  if (plan.scene->venue_ps328 == nullptr || plan.scene->venue_14d == nullptr ||
      plan.scene->crowd_c6 == nullptr || plan.scene->player_ca9 == nullptr) {
    Reject(plan, NativeSceneCompositionRejectReason::kMissingFamilyFrame);
    return plan;
  }
  ready.venue_ps328_draw_count =
      static_cast<uint32_t>(plan.scene->venue_ps328->draws.size());
  ready.venue_14d_draw_count =
      static_cast<uint32_t>(plan.scene->venue_14d->draws.size());
  ready.crowd_c6_draw_count =
      static_cast<uint32_t>(plan.scene->crowd_c6->draws.size());
  ready.player_ca9_draw_count =
      static_cast<uint32_t>(plan.scene->player_ca9->draws.size());
  if (!SameFrame(ready.title_sequence, plan.scene->venue_ps328->sequence) ||
      !SameFrame(ready.title_sequence, plan.scene->venue_14d->sequence) ||
      !SameFrame(ready.title_sequence, plan.scene->crowd_c6->sequence) ||
      !SameFrame(ready.title_sequence, plan.scene->player_ca9->sequence)) {
    Reject(plan, NativeSceneCompositionRejectReason::kFamilyFrameMismatch);
    return plan;
  }
  ready.family_frames_exact = true;

  if (!plan.scene->venue_ps328->valid() || !plan.scene->venue_14d->valid() ||
      !plan.scene->crowd_c6->valid() ||
      !CrowdBackendProofValid(*plan.scene->crowd_c6) ||
      !plan.scene->player_ca9->valid()) {
    Reject(plan, NativeSceneCompositionRejectReason::kInvalidFamilyFrame);
    return plan;
  }
  ready.family_payloads_valid = true;

  plan.draws.reserve(plan.scene->venue_ps328->draws.size() +
                     plan.scene->venue_14d->draws.size() +
                     plan.scene->crowd_c6->draws.size() +
                     plan.scene->player_ca9->draws.size());
  if (!AppendFamilyDraws(
          plan, plan.scene->venue_ps328->draws,
          NativeSceneDrawFamily::kVenuePs328, catalog,
          [](const VenueFullFamilyDrawSnapshot &draw) {
            return draw.source.ordinal;
          },
          [](const SceneCatalogDrawOccurrence &catalog_draw,
             const VenueFullFamilyDrawSnapshot &draw) {
            return CatalogDrawMatches(catalog_draw, draw);
          }) ||
      !AppendFamilyDraws(
          plan, plan.scene->venue_14d->draws, NativeSceneDrawFamily::kVenue14D,
          catalog,
          [](const Venue14DDrawSnapshot &draw) {
            return draw.title != nullptr ? draw.title->ordinal : 0;
          },
          [](const SceneCatalogDrawOccurrence &catalog_draw,
             const Venue14DDrawSnapshot &draw) {
            return CatalogDrawMatches(catalog_draw, draw);
          }) ||
      !AppendFamilyDraws(
          plan, plan.scene->crowd_c6->draws, NativeSceneDrawFamily::kCrowdC6,
          catalog, [](const CrowdDrawSnapshot &draw) { return draw.ordinal; },
          [](const SceneCatalogDrawOccurrence &catalog_draw,
             const CrowdDrawSnapshot &draw) {
            return CatalogDrawMatches(catalog_draw, draw);
          }) ||
      !AppendFamilyDraws(
          plan, plan.scene->player_ca9->draws,
          NativeSceneDrawFamily::kPlayerCa9, catalog,
          [](const PlayerSkinDrawSnapshot &draw) { return draw.ordinal; },
          [](const SceneCatalogDrawOccurrence &catalog_draw,
             const PlayerSkinDrawSnapshot &draw) {
            return CatalogDrawMatches(catalog_draw, draw);
          })) {
    return plan;
  }

  std::ranges::sort(plan.draws, {}, &NativeSceneDrawRef::ordinal);
  const auto duplicate = std::adjacent_find(
      plan.draws.begin(), plan.draws.end(),
      [](const NativeSceneDrawRef &left, const NativeSceneDrawRef &right) {
        return left.ordinal == right.ordinal;
      });
  if (duplicate != plan.draws.end()) {
    Reject(plan, NativeSceneCompositionRejectReason::kDuplicateOrdinal);
    return plan;
  }

  ready.ordinal_identity_exact = true;
  ready.reject_reason = NativeSceneCompositionRejectReason::kNone;
  ready.observer_composition_ready = !plan.draws.empty();
  return plan;
}

const char *NativeSceneCompositionRejectReasonName(
    NativeSceneCompositionRejectReason reason) {
  switch (reason) {
  case NativeSceneCompositionRejectReason::kNone:
    return "none";
  case NativeSceneCompositionRejectReason::kMissingScene:
    return "missing_scene";
  case NativeSceneCompositionRejectReason::kGameplayInactive:
    return "gameplay_inactive";
  case NativeSceneCompositionRejectReason::kMissingCatalog:
    return "missing_catalog";
  case NativeSceneCompositionRejectReason::kCatalogFrameMismatch:
    return "catalog_frame_mismatch";
  case NativeSceneCompositionRejectReason::kCatalogIncomplete:
    return "catalog_incomplete";
  case NativeSceneCompositionRejectReason::kMissingFamilyFrame:
    return "missing_family_frame";
  case NativeSceneCompositionRejectReason::kFamilyFrameMismatch:
    return "family_frame_mismatch";
  case NativeSceneCompositionRejectReason::kInvalidFamilyFrame:
    return "invalid_family_frame";
  case NativeSceneCompositionRejectReason::kOrdinalOutsideCatalog:
    return "ordinal_outside_catalog";
  case NativeSceneCompositionRejectReason::kCatalogIdentityMismatch:
    return "catalog_identity_mismatch";
  case NativeSceneCompositionRejectReason::kDuplicateOrdinal:
    return "duplicate_ordinal";
  }
  return "unknown";
}

} // namespace tabletennis::native
