#pragma once

#include "native/tabletennis_native_capture.h"
#include "native/tabletennis_scene_owner_observer.h"

#include <cstdint>
#include <memory>

namespace tabletennis::native {

struct CrowdFrameSnapshot;
struct D47PlayerFrameSnapshot;
struct HudSwfFrameSnapshot;
struct NetBB903FrameSnapshot;
struct Player6AEFrameSnapshot;
struct PlayerSkinFrameSnapshot;
struct SceneDrawCatalogFrame;
struct Venue14DFrameSnapshot;
struct VenueE33FrameSnapshot;
struct VenueFullFamilyFrame;

// Reference capture coverage for the gameplay MAIN pass. 441 is the verified
// logical draw count in the census trace, before the Xenos backend replays it
// across three EDRAM tiles. Raw title submission counts vary with camera and
// auxiliary phases and must never be compared directly with this number.
struct NativeSceneReadiness {
  static constexpr uint32_t kReferenceMainDrawCount = 441;

  uint32_t venue_ps328_draw_count = 0;
  uint32_t venue_14d_draw_count = 0;
  uint32_t venue_e33_draw_count = 0;
  uint32_t crowd_c6_draw_count = 0;
  uint32_t player_ca9_draw_count = 0;
  uint32_t player_d47_draw_count = 0;
  uint32_t player_6ae_draw_count = 0;
  uint32_t reference_covered_main_draw_count = 0;
  uint32_t reference_missing_main_draw_count = kReferenceMainDrawCount;
  uint32_t catalog_draw_count = 0;
  uint32_t catalog_dropped_draw_count = 0;
  uint32_t catalog_guest_read_failures = 0;
  uint32_t hud_batch_count = 0;
  uint32_t hud_texture_bind_count = 0;
  uint32_t net_bb903_draw_count = 0;
  uint32_t stale_component_count = 0;
  uint32_t consecutive_complete_frames = 0;
  bool gameplay_active = false;
  bool exact_frame_components = false;
  bool catalog_current = false;
  bool venue_ps328_complete = false;
  bool venue_14d_complete = false;
  bool venue_e33_complete = false;
  bool crowd_c6_complete = false;
  bool player_ca9_complete = false;
  bool player_d47_complete = false;
  bool player_6ae_complete = false;
  bool table_owner_observed = false;
  bool ball_owner_observed = false;
  bool paddle_owner_observed = false;
  bool main_complete = false;
  bool hud_capture_valid = false;
  bool net_bb903_observer_complete = false;
  bool net_bb903_ready_to_serve = false;
  bool hud_complete = false;
  bool takeover_ready = false;
};

// One immutable title-frame publication. Resource-owning family snapshots are
// retained by shared ownership so the render thread never follows live guest
// pointers after the swap boundary.
struct TableTennisFrameScene {
  uint64_t generation = 0;
  CapturedFrame title{};
  std::shared_ptr<const SceneDrawCatalogFrame> catalog;
  std::shared_ptr<const VenueFullFamilyFrame> venue_ps328;
  std::shared_ptr<const Venue14DFrameSnapshot> venue_14d;
  std::shared_ptr<const VenueE33FrameSnapshot> venue_e33;
  std::shared_ptr<const CrowdFrameSnapshot> crowd_c6;
  std::shared_ptr<const PlayerSkinFrameSnapshot> player_ca9;
  std::shared_ptr<const D47PlayerFrameSnapshot> player_d47;
  std::shared_ptr<const Player6AEFrameSnapshot> player_6ae;
  std::shared_ptr<const HudSwfFrameSnapshot> hud_swf;
  std::shared_ptr<const NetBB903FrameSnapshot> net_bb903;
  SceneOwnerObserverFrame owners{};
  NativeSceneReadiness readiness{};
};

// Observer-only master switch. This makes the generic ordered catalog publish
// every frame, but never suppresses or replaces a guest draw.
bool NativeFrameSceneCaptureEnabled();

// Full diagnostic capture arms every discovered family, HUD and owner
// observer. The private four-family transaction deliberately does not: it
// captures only the components it consumes.
bool NativeFrameSceneFullCaptureEnabled();

// Called after every family publisher and the ordered catalog at title Swap.
void NativeFrameSceneFrameEnd();

std::shared_ptr<const TableTennisFrameScene> LatestNativeFrameScene();

} // namespace tabletennis::native
