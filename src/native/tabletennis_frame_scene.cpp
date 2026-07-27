#include "native/tabletennis_frame_scene.h"

#include "native/tabletennis_6ae_player_observer.h"
#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_d47_player_observer.h"
#include "native/tabletennis_hud_swf_capture.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_native_scene_transaction.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_player_palette_write_observer.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_player_replacement_candidates.h"
#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_venue_full_family.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_e33_observer.h"

#include <algorithm>
#include <deque>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_frame_scene_capture, false, "Table Tennis",
    "Publish one immutable observer-only gameplay scene from the title's "
    "ordered real draw stream. This never suppresses guest rendering.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    tabletennis_native_frame_scene_log_interval, 120, "Table Tennis",
    "Published frame interval between native scene readiness reports.")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint32_t kExpectedVenuePs328DrawCount = 72;
constexpr uint32_t kExpectedVenue14DDrawCount = 47;
constexpr uint32_t kExpectedVenueE33DrawCount = 23;
constexpr uint32_t kExpectedCrowdC6DrawCount = 156;
constexpr uint32_t kExpectedPlayerCa9DrawCount = 20;
constexpr uint32_t kExpectedPlayerD47DrawCount = 23;
constexpr uint32_t kExpectedPlayer6AEDrawCount = 14;
constexpr uint32_t kRequiredConsecutiveCompleteFrames = 3;
constexpr size_t kMaximumPendingFrameScenes = 16;

std::mutex g_frame_scene_mutex;
std::shared_ptr<const TableTennisFrameScene> g_published_scene;
std::deque<TableTennisFrameScene> g_pending_scenes;
uint64_t g_generation = 0;
uint32_t g_consecutive_complete_frames = 0;
bool g_logged_first_gameplay_frame = false;

uint32_t DrawCount(
    const std::shared_ptr<const VenueFullFamilyFrame> &frame) {
  return frame == nullptr ? 0 : static_cast<uint32_t>(frame->draws.size());
}

uint32_t DrawCount(const std::shared_ptr<const CrowdFrameSnapshot> &frame) {
  return frame == nullptr ? 0 : static_cast<uint32_t>(frame->draws.size());
}

uint32_t DrawCount(
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
  return frame == nullptr ? 0 : static_cast<uint32_t>(frame->draws.size());
}

bool CurrentCatalog(const SceneDrawCatalogFrame &catalog,
                    uint64_t title_sequence) {
  return title_sequence != 0 && catalog.sequence == title_sequence &&
         catalog.ordered_draw_count != 0 &&
         catalog.dropped_ordered_draws == 0 &&
         catalog.guest_read_failures == 0;
}

template <typename Frame>
bool SameTitleFrame(const std::shared_ptr<const Frame> &frame,
                    uint64_t title_sequence) {
  return frame != nullptr && title_sequence != 0 &&
         frame->sequence == title_sequence;
}

TableTennisFrameScene *PendingSceneForSequence(uint64_t sequence) {
  if (sequence == 0) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      g_pending_scenes, [sequence](const TableTennisFrameScene &scene) {
        return scene.title.sequence == sequence;
      });
  return found == g_pending_scenes.end() ? nullptr : &*found;
}

template <typename Frame>
void AttachPendingFrame(
    const std::shared_ptr<const Frame> &frame,
    std::shared_ptr<const Frame> TableTennisFrameScene::*member) {
  if (frame == nullptr) {
    return;
  }
  TableTennisFrameScene *const pending =
      PendingSceneForSequence(frame->sequence);
  if (pending != nullptr) {
    pending->*member = frame;
  }
}

bool CompositionComponentsJoined(const TableTennisFrameScene &scene) {
  const uint64_t sequence = scene.title.sequence;
  return sequence != 0 && scene.title.gameplay_active &&
         scene.catalog != nullptr && scene.catalog->sequence == sequence &&
         SameTitleFrame(scene.venue_ps328, sequence) &&
         SameTitleFrame(scene.venue_14d, sequence) &&
         SameTitleFrame(scene.crowd_c6, sequence) &&
         SameTitleFrame(scene.player_ca9, sequence);
}

void LogReadiness(const TableTennisFrameScene &scene) {
  const NativeSceneReadiness &ready = scene.readiness;
  const D47PlayerObserverTelemetry d47 =
      LatestD47PlayerObserverTelemetry();
  const Venue14DObserverTelemetry venue_14d =
      LatestVenue14DObserverTelemetry();
  const VenueE33ObserverTelemetry venue_e33 =
      LatestVenueE33ObserverTelemetry();
  const Player6AEObserverTelemetry player_6ae =
      LatestPlayer6AEObserverTelemetry();
  const NetBB903ObserverTelemetry net_bb903 =
      LatestNetBB903ObserverTelemetry();
  const PlayerPaletteWriteObserverFrame palette =
      LatestPlayerPaletteWriteObserverFrame();
  const uint32_t palette_destination_0 =
      palette.write_count > 0 ? palette.writes[0].physical_destination : 0;
  const uint32_t palette_destination_1 =
      palette.write_count > 1 ? palette.writes[1].physical_destination : 0;
  const uint32_t palette_player_0 =
      palette.write_count > 0 ? palette.writes[0].player : 0;
  const uint32_t palette_player_1 =
      palette.write_count > 1 ? palette.writes[1].player : 0;
  REXLOG_INFO(
      "Table Tennis native frame scene: generation={} title_frame={} "
      "gameplay={} reference_main={}/{} missing={} raw_families[ps328={} "
      "14d={} e33={} c6={} ca9={} d47={} 6ae={}] "
      "catalog[frame={} draws={} dropped={} "
      "read_failures={}] "
      "d47[candidates={} admitted={} backend={} blocks={} pending={} "
      "queued={} finalized={} valid={} expired={} mismatches={}] "
      "14d[seen={} candidates={} valid_title={} reads={} copies={} "
      "textures={} backend_seen={} backend_frame={} backend_hash={} backend_pair={} "
      "backend={} analyzed={} unjoined={} blocks={} "
      "pending={} queued={} finalized={} valid={} expired={} mismatches={}] "
      "6ae[candidates={} admitted={} backend={} blocks={} pending={} "
      "queued={} finalized={} valid={} rejected={} expired={} "
      "mismatches={}] "
      "net_diag[seen={} candidates={} valid_title={} missing={}/{} "
      "backend_frame={} joined={} unjoined={} content={}/{} "
      "eligibility={}/{} eligibility_parts[ps={} shape={} eligible={}] "
      "analyzed={} blocks={} pending={} queued={} "
      "finalized={} complete={} expired={} mismatches={}] "
      "palette[writes={}/{} rejected={} d47={} "
      "halves={:08X}@{:08X},{:08X}@{:08X}] "
      "owners[table={} ball={} paddle={}] "
      "hud[batches={} textures={} capture_valid={} replay_complete={}] "
      "net_bb903[draws={} observed={} serve={}] "
      "frame_exact={} stale_components={} stable={} takeover_ready={} "
      "observer_only=true",
      scene.generation, scene.title.sequence, ready.gameplay_active,
      ready.reference_covered_main_draw_count,
      NativeSceneReadiness::kReferenceMainDrawCount,
      ready.reference_missing_main_draw_count,
      ready.venue_ps328_draw_count, ready.venue_14d_draw_count,
      ready.venue_e33_draw_count,
      ready.crowd_c6_draw_count,
      ready.player_ca9_draw_count, ready.player_d47_draw_count,
      ready.player_6ae_draw_count,
      scene.catalog == nullptr ? 0 : scene.catalog->sequence,
      ready.catalog_draw_count, ready.catalog_dropped_draw_count,
      ready.catalog_guest_read_failures, d47.title_candidates,
      d47.admitted_draws, d47.backend_events,
      d47.backend_tile_blocks_matched, d47.pending_frames,
      d47.queued_backend_events, d47.finalized_frames, d47.valid_frames,
      d47.expired_frames, d47.backend_sequence_mismatches,
      venue_14d.title_draws_observed, venue_14d.title_candidates,
      venue_14d.valid_title_snapshots,
      venue_14d.title_guest_read_failures,
      venue_14d.title_payload_copy_failures,
      venue_14d.title_texture_capture_failures,
      venue_14d.backend_draws_observed,
      venue_14d.latest_backend_frame_sequence,
      venue_14d.backend_pixel_hash_matches,
      venue_14d.backend_shader_pair_matches,
      venue_14d.backend_events,
      venue_14d.backend_frames_analyzed,
      venue_14d.backend_events_without_title_frame,
      venue_14d.backend_tile_blocks_matched, venue_14d.pending_frames,
      venue_14d.queued_backend_events, venue_14d.finalized_frames,
      venue_14d.valid_frames, venue_14d.expired_frames,
      venue_14d.backend_sequence_mismatches,
      player_6ae.title_candidates, player_6ae.admitted_draws,
      player_6ae.backend_events,
      player_6ae.backend_tile_blocks_matched,
      player_6ae.pending_frames, player_6ae.queued_backend_events,
      player_6ae.finalized_frames, player_6ae.valid_frames,
      player_6ae.rejected_frames, player_6ae.expired_frames,
      player_6ae.backend_sequence_mismatches,
      net_bb903.title_draws_observed, net_bb903.title_candidates,
      net_bb903.valid_title_draws, net_bb903.title_missing_mesh,
      net_bb903.title_missing_textures,
      net_bb903.latest_backend_frame_sequence,
      net_bb903.backend_events_joined,
      net_bb903.backend_events_without_title_frame,
      net_bb903.backend_content_hash_matches,
      net_bb903.backend_content_contract_matches,
      net_bb903.backend_eligibility_content_hash_matches,
      net_bb903.backend_eligibility_indexed_matches,
      net_bb903.backend_eligibility_pixel_hash_matches,
      net_bb903.backend_eligibility_content_shape_matches,
      net_bb903.backend_eligibility_eligible_matches,
      net_bb903.backend_frames_analyzed,
      net_bb903.backend_tile_blocks_matched, net_bb903.pending_frames,
      net_bb903.queued_backend_events, net_bb903.finalized_frames,
      net_bb903.observer_complete_frames, net_bb903.expired_frames,
      net_bb903.backend_sequence_mismatches,
      palette.valid_write_count, palette.write_attempt_count,
      palette.rejected_write_count, palette.d47_contract_draw_count,
      palette_player_0, palette_destination_0, palette_player_1,
      palette_destination_1,
      ready.table_owner_observed,
      ready.ball_owner_observed, ready.paddle_owner_observed,
      ready.hud_batch_count,
      ready.hud_texture_bind_count, ready.hud_capture_valid,
      ready.hud_complete,
      ready.net_bb903_draw_count, ready.net_bb903_observer_complete,
      ready.net_bb903_ready_to_serve,
      ready.exact_frame_components, ready.stale_component_count,
      ready.consecutive_complete_frames, ready.takeover_ready);
  REXLOG_INFO(
      "Table Tennis E33 frame diagnostic: title_seen={} candidates={} "
      "capture_attempts={} valid_title={} payloads={} textures={} "
      "materials={} renderer[mips={} shapes={} raster={} mode={:08X}] "
      "generations={} backend_seen={} hash={} pair={} contract={} "
      "events={} stale={} blocks={} proofs={} learned={}/{} finalized={} "
      "valid={} rejected={} expired={} mismatches={} pending={} queued={} "
      "observer_only=true",
      venue_e33.title_draws_observed, venue_e33.title_candidates,
      venue_e33.title_capture_attempts, venue_e33.valid_title_snapshots,
      venue_e33.title_payload_copy_failures,
      venue_e33.title_texture_capture_failures,
      venue_e33.title_material_validation_failures,
      venue_e33.title_renderer_full_mip_textures,
      venue_e33.title_renderer_texture_shape_matches,
      venue_e33.backend_rasterizer_contract_matches,
      venue_e33.latest_rasterizer_mode_control,
      venue_e33.capture_generation_mismatches,
      venue_e33.backend_draws_observed,
      venue_e33.backend_pixel_hash_matches,
      venue_e33.backend_shader_pair_matches,
      venue_e33.backend_contract_matches, venue_e33.backend_events,
      venue_e33.backend_events_stale,
      venue_e33.backend_tile_blocks_matched, venue_e33.proof_frames,
      venue_e33.learned_generation,
      venue_e33.learned_unique_program_count,
      venue_e33.finalized_frames, venue_e33.valid_frames,
      venue_e33.capture_frames_rejected, venue_e33.expired_frames,
      venue_e33.backend_sequence_mismatches, venue_e33.pending_frames,
      venue_e33.queued_backend_events);
  REXLOG_INFO(
      "Table Tennis BB903 title gate: shape={} material={} owner={} "
      "scope={} pass={} mesh={} state={} transform={} physical_index={} "
      "candidates={} valid={} observer_only=true",
      net_bb903.title_shape_matches, net_bb903.title_material_matches,
      net_bb903.title_owner_matches, net_bb903.title_scope_matches,
      net_bb903.title_pass_matches, net_bb903.title_mesh_matches,
      net_bb903.title_state_matches, net_bb903.title_transform_matches,
      net_bb903.title_physical_index_matches, net_bb903.title_candidates,
      net_bb903.valid_title_draws);
  REXLOG_INFO(
      "Table Tennis E33 title gate: base={} topology={} stride={} endian={} "
      "index_layout={} bounds={} identity={} declaration={} candidates={} "
      "observer_only=true",
      venue_e33.title_base_structure_matches,
      venue_e33.title_topology_matches,
      venue_e33.title_stride_matches, venue_e33.title_endian_matches,
      venue_e33.title_index_layout_matches,
      venue_e33.title_buffer_bounds_matches,
      venue_e33.title_identity_matches,
      venue_e33.title_declaration_matches, venue_e33.title_candidates);
}

} // namespace

bool NativeFrameSceneCaptureEnabled() {
  // The MAIN coverage ledger and private native-scene transaction consume the
  // same immutable family publications. Either one must arm the complete
  // capture graph even if the older master scene cvar is off.
  return REXCVAR_GET(tabletennis_native_frame_scene_capture) ||
         MainCoverageLedgerEnabled() ||
         NativeSceneTransactionObserverEnabled();
}

void NativeFrameSceneFrameEnd() {
  if (!NativeFrameSceneCaptureEnabled()) {
    std::lock_guard lock(g_frame_scene_mutex);
    g_published_scene.reset();
    g_pending_scenes.clear();
    g_consecutive_complete_frames = 0;
    g_logged_first_gameplay_frame = false;
    return;
  }

  const CapturedFrame current_title = LatestCapturedFrame();
  const std::shared_ptr<const SceneDrawCatalogFrame> latest_catalog =
      LatestSceneDrawCatalogFrameSnapshot();
  const std::shared_ptr<const VenueFullFamilyFrame> latest_venue_ps328 =
      LatestVenueFullFamilyFrame();
  const std::shared_ptr<const Venue14DFrameSnapshot> latest_venue_14d =
      LatestVenue14DFrameSnapshot();
  const std::shared_ptr<const VenueE33FrameSnapshot> latest_venue_e33 =
      LatestVenueE33FrameSnapshot();
  // Use the immutable title frame retained by the independently matched
  // translated-backend block, not merely the newest title-side capture.
  const std::shared_ptr<const CrowdFrameSnapshot> latest_crowd_c6 =
      LatestCrowdBackendProofFrameSnapshot();
  const std::shared_ptr<const PlayerSkinFrameSnapshot> latest_player_ca9 =
      LatestPlayerSkinFrameSnapshot();
  const std::shared_ptr<const D47PlayerFrameSnapshot> latest_player_d47 =
      LatestD47PlayerFrameSnapshot();
  const std::shared_ptr<const Player6AEFrameSnapshot> latest_player_6ae =
      LatestPlayer6AEFrameSnapshot();
  const std::shared_ptr<const HudSwfFrameSnapshot> latest_hud =
      LatestHudSwfFrameSnapshot();
  const std::shared_ptr<const NetBB903FrameSnapshot> latest_net =
      LatestNetBB903FrameSnapshot();
  const SceneOwnerObserverFrame latest_owners =
      LatestSceneOwnerObserverFrame();

  TableTennisFrameScene scene;
  {
    std::lock_guard lock(g_frame_scene_mutex);
    TableTennisFrameScene *current =
        PendingSceneForSequence(current_title.sequence);
    if (current == nullptr) {
      g_pending_scenes.emplace_back();
      current = &g_pending_scenes.back();
    }
    current->title = current_title;

    if (latest_catalog != nullptr) {
      TableTennisFrameScene *const pending =
          PendingSceneForSequence(latest_catalog->sequence);
      if (pending != nullptr) {
        pending->catalog = latest_catalog;
      }
    }
    AttachPendingFrame(latest_venue_ps328,
                       &TableTennisFrameScene::venue_ps328);
    AttachPendingFrame(latest_venue_14d,
                       &TableTennisFrameScene::venue_14d);
    AttachPendingFrame(latest_venue_e33,
                       &TableTennisFrameScene::venue_e33);
    AttachPendingFrame(latest_crowd_c6,
                       &TableTennisFrameScene::crowd_c6);
    AttachPendingFrame(latest_player_ca9,
                       &TableTennisFrameScene::player_ca9);
    AttachPendingFrame(latest_player_d47,
                       &TableTennisFrameScene::player_d47);
    AttachPendingFrame(latest_player_6ae,
                       &TableTennisFrameScene::player_6ae);
    AttachPendingFrame(latest_hud, &TableTennisFrameScene::hud_swf);
    AttachPendingFrame(latest_net, &TableTennisFrameScene::net_bb903);
    if (TableTennisFrameScene *const pending =
            PendingSceneForSequence(latest_owners.sequence);
        pending != nullptr) {
      pending->owners = latest_owners;
    }

    while (g_pending_scenes.size() > kMaximumPendingFrameScenes) {
      g_pending_scenes.pop_front();
    }

    const auto joined = std::find_if(
        g_pending_scenes.rbegin(), g_pending_scenes.rend(),
        [](const TableTennisFrameScene &candidate) {
          return CompositionComponentsJoined(candidate);
        });
    scene = joined != g_pending_scenes.rend() ? *joined
                                              : g_pending_scenes.back();
  }
  scene.generation = ++g_generation;
  scene.readiness = {};

  NativeSceneReadiness &ready = scene.readiness;
  ready.gameplay_active = scene.title.gameplay_active;
  if (scene.catalog != nullptr) {
    ready.catalog_current =
        CurrentCatalog(*scene.catalog, scene.title.sequence);
    ready.catalog_draw_count = scene.catalog->ordered_draw_count;
    ready.catalog_dropped_draw_count =
        scene.catalog->dropped_ordered_draws;
    ready.catalog_guest_read_failures =
        scene.catalog->guest_read_failures;
  }

  const bool venue_ps328_current =
      SameTitleFrame(scene.venue_ps328, scene.title.sequence);
  const bool venue_14d_current =
      SameTitleFrame(scene.venue_14d, scene.title.sequence);
  const bool venue_e33_current =
      SameTitleFrame(scene.venue_e33, scene.title.sequence);
  const bool crowd_c6_current =
      SameTitleFrame(scene.crowd_c6, scene.title.sequence);
  const bool player_ca9_current =
      SameTitleFrame(scene.player_ca9, scene.title.sequence);
  const bool player_d47_current =
      SameTitleFrame(scene.player_d47, scene.title.sequence);
  const bool player_6ae_current =
      SameTitleFrame(scene.player_6ae, scene.title.sequence);
  const bool hud_current =
      SameTitleFrame(scene.hud_swf, scene.title.sequence);
  const bool net_bb903_current =
      SameTitleFrame(scene.net_bb903, scene.title.sequence);
  const bool owners_current =
      scene.title.sequence != 0 &&
      scene.owners.sequence == scene.title.sequence;
  ready.stale_component_count =
      static_cast<uint32_t>(scene.catalog != nullptr &&
                            scene.catalog->sequence != scene.title.sequence) +
      static_cast<uint32_t>(scene.venue_ps328 != nullptr &&
                            !venue_ps328_current) +
      static_cast<uint32_t>(scene.venue_14d != nullptr &&
                            !venue_14d_current) +
      static_cast<uint32_t>(scene.venue_e33 != nullptr &&
                            !venue_e33_current) +
      static_cast<uint32_t>(scene.crowd_c6 != nullptr &&
                            !crowd_c6_current) +
      static_cast<uint32_t>(scene.player_ca9 != nullptr &&
                            !player_ca9_current) +
      static_cast<uint32_t>(scene.player_d47 != nullptr &&
                            !player_d47_current) +
      static_cast<uint32_t>(scene.player_6ae != nullptr &&
                            !player_6ae_current) +
      static_cast<uint32_t>(scene.hud_swf != nullptr && !hud_current) +
      static_cast<uint32_t>(scene.net_bb903 != nullptr &&
                            !net_bb903_current) +
      static_cast<uint32_t>(scene.owners.sequence != 0 && !owners_current);
  ready.exact_frame_components =
      ready.catalog_current && owners_current && hud_current &&
      ready.stale_component_count == 0;

  ready.venue_ps328_draw_count = DrawCount(scene.venue_ps328);
  ready.venue_14d_draw_count =
      scene.venue_14d == nullptr
          ? 0
          : static_cast<uint32_t>(scene.venue_14d->draws.size());
  ready.venue_e33_draw_count =
      scene.venue_e33 == nullptr
          ? 0
          : static_cast<uint32_t>(scene.venue_e33->draws.size());
  ready.crowd_c6_draw_count = DrawCount(scene.crowd_c6);
  ready.player_ca9_draw_count = DrawCount(scene.player_ca9);
  ready.player_d47_draw_count =
      scene.player_d47 == nullptr
          ? 0
          : static_cast<uint32_t>(scene.player_d47->draws.size());
  ready.player_6ae_draw_count =
      scene.player_6ae == nullptr
          ? 0
          : static_cast<uint32_t>(scene.player_6ae->draws.size());
  if (scene.hud_swf != nullptr) {
    ready.hud_batch_count =
        static_cast<uint32_t>(scene.hud_swf->batches.size());
    ready.hud_texture_bind_count =
        static_cast<uint32_t>(scene.hud_swf->texture_binds.size());
    ready.hud_capture_valid = hud_current && scene.hud_swf->valid();
  }
  if (scene.net_bb903 != nullptr) {
    ready.net_bb903_draw_count =
        NetBB903FrameSnapshot::kContentDrawCount;
    ready.net_bb903_observer_complete =
        scene.net_bb903->observer_complete();
    ready.net_bb903_ready_to_serve =
        net_bb903_current && scene.net_bb903->ready_to_serve();
  }
  ready.venue_ps328_complete =
      venue_ps328_current && scene.venue_ps328->valid() &&
      ready.venue_ps328_draw_count == kExpectedVenuePs328DrawCount;
  ready.venue_14d_complete =
      venue_14d_current && scene.venue_14d->valid() &&
      ready.venue_14d_draw_count == kExpectedVenue14DDrawCount;
  ready.venue_e33_complete =
      venue_e33_current && scene.venue_e33->valid() &&
      ready.venue_e33_draw_count == kExpectedVenueE33DrawCount;
  ready.crowd_c6_complete =
      crowd_c6_current && scene.crowd_c6->valid() &&
      ready.crowd_c6_draw_count == kExpectedCrowdC6DrawCount;
  const PlayerReplacementCandidateTelemetry ca9_backend =
      LatestPlayerReplacementCandidateTelemetry();
  ready.player_ca9_complete =
      player_ca9_current && scene.player_ca9->valid() &&
      ready.player_ca9_draw_count == kExpectedPlayerCa9DrawCount &&
      ca9_backend.backend_frames_verified != 0;
  ready.player_d47_complete =
      player_d47_current && scene.player_d47->valid() &&
      ready.player_d47_draw_count == kExpectedPlayerD47DrawCount;
  ready.player_6ae_complete =
      player_6ae_current && scene.player_6ae->valid() &&
      ready.player_6ae_draw_count == kExpectedPlayer6AEDrawCount;
  const bool owner_frame_valid =
      owners_current &&
      scene.owners.guest_read_failure_count == 0 &&
      scene.owners.dropped_identity_count == 0;
  ready.table_owner_observed =
      owner_frame_valid && scene.owners.table_render_scope_count != 0;
  ready.ball_owner_observed =
      owner_frame_valid &&
      (scene.owners.ball_render_scope_count != 0 ||
       scene.owners.ball_transform_count != 0);
  ready.paddle_owner_observed =
      owner_frame_valid && scene.owners.paddle_render_scope_count != 0;

  // Only non-overlapping, fully validated shader families contribute their
  // census MAIN coverage. Raw title vectors include auxiliary phases and are
  // telemetry only.
  ready.reference_covered_main_draw_count =
      (ready.venue_ps328_complete ? kExpectedVenuePs328DrawCount : 0) +
      (ready.venue_14d_complete ? kExpectedVenue14DDrawCount : 0) +
      (ready.venue_e33_complete ? kExpectedVenueE33DrawCount : 0) +
      (ready.crowd_c6_complete ? kExpectedCrowdC6DrawCount : 0) +
      (ready.player_ca9_complete ? kExpectedPlayerCa9DrawCount : 0) +
      (ready.player_d47_complete ? kExpectedPlayerD47DrawCount : 0) +
      (ready.player_6ae_complete ? kExpectedPlayer6AEDrawCount : 0);
  ready.reference_missing_main_draw_count =
      NativeSceneReadiness::kReferenceMainDrawCount -
      std::min(ready.reference_covered_main_draw_count,
               NativeSceneReadiness::kReferenceMainDrawCount);

  // Deliberately remains false until the remaining material families and the
  // 2D replay list have exact immutable captures. Partial native frames are
  // evidence, never output.
  ready.main_complete =
      ready.exact_frame_components &&
      ready.reference_missing_main_draw_count == 0;
  // The immutable draw list is now observed, but texture payload/state replay
  // is not implemented yet. Observation must not unlock takeover by itself.
  ready.hud_complete = false;
  const bool complete =
      ready.gameplay_active && ready.main_complete && ready.hud_complete;
  g_consecutive_complete_frames =
      complete ? g_consecutive_complete_frames + 1 : 0;
  ready.consecutive_complete_frames = g_consecutive_complete_frames;
  ready.takeover_ready =
      complete &&
      g_consecutive_complete_frames >= kRequiredConsecutiveCompleteFrames;

  const uint32_t log_interval =
      REXCVAR_GET(tabletennis_native_frame_scene_log_interval);
  const bool first_gameplay =
      ready.gameplay_active && !g_logged_first_gameplay_frame;
  if (first_gameplay || scene.generation % log_interval == 0) {
    LogReadiness(scene);
  }
  g_logged_first_gameplay_frame |= ready.gameplay_active;

  std::lock_guard lock(g_frame_scene_mutex);
  g_published_scene =
      std::make_shared<const TableTennisFrameScene>(std::move(scene));
}

std::shared_ptr<const TableTennisFrameScene> LatestNativeFrameScene() {
  std::lock_guard lock(g_frame_scene_mutex);
  return g_published_scene;
}

} // namespace tabletennis::native
