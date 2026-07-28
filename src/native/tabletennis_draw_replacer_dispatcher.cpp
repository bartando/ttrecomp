#include "native/tabletennis_draw_replacer_dispatcher.h"

#include "native/tabletennis_6ae_player_observer.h"
#include "native/tabletennis_crowd_diagnostic_suppression.h"
#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_crowd_replacement_prewarm.h"
#include "native/tabletennis_d47_player_observer.h"
#include "native/tabletennis_hud_swf_backend_observer.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_phase0_rectangle_replay.h"
#include "native/tabletennis_player_palette_write_observer.h"
#include "native/tabletennis_player_a406_observer.h"
#include "native/tabletennis_player_2ac_observer.h"
#include "native/tabletennis_player_replacement_candidates.h"
#include "native/tabletennis_venue_diagnostic_suppression.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_526a_observer.h"
#include "native/tabletennis_venue_9e_observer.h"
#include "native/tabletennis_venue_e33_observer.h"
#include "native/tabletennis_venue_observer_renderer.h"

#include <array>
#include <utility>

namespace tabletennis::native {
namespace {

// Priority is part of the replacement contract. A future player route belongs
// here only after its observer has independently proven draw identity, state,
// and output parity.
constexpr std::array<DrawReplacementRoute, 5> kRoutes = {{
    {
        .matcher = &MatchCrowdDiagnosticSuppression,
        .renderer = &RenderCrowdDiagnosticSuppression,
        .user_data = nullptr,
    },
    {
        .matcher = &MatchCrowdReplacementPrewarm,
        .renderer = &RenderCrowdReplacementPrewarm,
        .user_data = nullptr,
    },
    {
        .matcher = &MatchVenueDiagnosticSuppression,
        .renderer = &RenderVenueDiagnosticSuppression,
        .user_data = nullptr,
    },
    {
        .matcher = &MatchVenueReplacement,
        .renderer = &RenderVenueReplacement,
        .user_data = nullptr,
    },
    {
        .matcher = &MatchPlayerReplacementPrewarm,
        .renderer = &RenderPlayerReplacementPrewarm,
        .user_data = nullptr,
    },
}};

DrawReplacerDispatcher g_dispatcher(kRoutes);

} // namespace

DrawReplacerDispatcher::DrawReplacerDispatcher(
    std::span<const DrawReplacementRoute> routes)
    : routes_(routes) {}

bool DrawReplacerDispatcher::Match(
    const rex::graphics::NativeGuestDrawContext &context) {
  // Observer-only C6/BD4 crowd proof. The tap is hard-wired to return false
  // and never participates in route selection or guest draw suppression.
  (void)ObserveCrowdBackendProofDraw(context);

  // Observer-only D47 shader proof. This tap never selects a route and cannot
  // suppress the guest draw; replacement routing below is unchanged.
  ObservePlayerPaletteWriteBackendDraw(context);
  ObservePlayer2ACBackendDraw(context);
  ObservePlayer6AEBackendDraw(context);
  ObserveD47PlayerBackendDraw(context);
  ObservePlayerA406BackendDraw(context);
  ObserveVenue14DBackendDraw(context);
  ObserveVenue526ABackendDraw(context);
  ObserveVenue9EBackendDraw(context);
  ObserveVenueE33BackendDraw(context);
  ObserveNetBB903BackendDraw(context);
  ObserveHudSwfBackendDraw(context);

  // Observer-only CA9 player proof. Keep this before route selection: placing
  // it inside the lowest-priority player matcher allowed an earlier route to
  // hide the backend callback. Early probes are counted and ignored; only
  // exact late borrowed-scope callbacks enter the proof ledger.
  ObservePlayerReplacementBackendDraw(context);

  // Run the global MAIN ledger after the individual family observers. Seeing
  // frame N+1 closes N, so this ordering lets any family publish its delayed
  // proof for N before the coverage ledger records that closing sequence.
  ObserveMainCoverageBackendDraw(context);
  ObservePhase0RectangleDraw(context);

  // A backend may abandon a successful match if it cannot borrow the guest
  // render scope. Never let that stale route claim a later renderer call.
  pending_route_ = kNoRoute;
  for (size_t index = 0; index < routes_.size(); ++index) {
    const DrawReplacementRoute &route = routes_[index];
    if (route.matcher == nullptr || route.renderer == nullptr ||
        !route.matcher(context, route.user_data)) {
      continue;
    }
    pending_route_ = index;
    return true;
  }
  return false;
}

bool DrawReplacerDispatcher::Render(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (pending_route_ >= routes_.size()) {
    return false;
  }
  const DrawReplacementRoute route = routes_[pending_route_];
  pending_route_ = kNoRoute;
  return route.renderer(context, route.user_data);
}

void DrawReplacerDispatcher::Reset() { pending_route_ = kNoRoute; }

bool DrawReplacerDispatcher::has_pending_route() const {
  return pending_route_ < routes_.size();
}

bool MatchDispatchedDrawReplacement(
    const rex::graphics::NativeGuestDrawContext &context, void *) {
  return g_dispatcher.Match(context);
}

bool RenderDispatchedDrawReplacement(
    const rex::graphics::NativeGuestDrawContext &context, void *) {
  return g_dispatcher.Render(context);
}

void ResetDrawReplacerDispatcher() { g_dispatcher.Reset(); }

} // namespace tabletennis::native
