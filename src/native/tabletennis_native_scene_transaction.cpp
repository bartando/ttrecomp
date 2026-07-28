#include "native/tabletennis_native_scene_transaction.h"

#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_crowd_observer_renderer.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_native_scene_compositor.h"
#include "native/tabletennis_native_scene_targets.h"
#include "native/tabletennis_player_observer_renderer.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_14d_renderer.h"
#include "native/tabletennis_venue_full_family.h"
#include "native/tabletennis_venue_observer_renderer.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_scene_transaction_observer, false, "Table Tennis",
    "Record all currently proven native scene families, in original title "
    "order, into a private discarded target. Never serves native output.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(
    tabletennis_native_scene_transaction_log_interval, 120, "Table Tennis",
    "Native private scene transaction observer log interval in title frames.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

enum class TransactionResult : uint8_t {
  kSucceeded,
  kPlanRejected,
  kTargetPreparationFailed,
  kFamilyPreparationFailed,
  kPassBeginFailed,
  kFamilyRecordFailed,
  kPassAbortFailed,
};

struct TransactionTelemetry {
  uint64_t sequence = 0;
  uint64_t catalog_sequence = 0;
  uint64_t venue_ps328_sequence = 0;
  uint64_t venue_14d_sequence = 0;
  uint64_t crowd_c6_sequence = 0;
  uint64_t player_ca9_sequence = 0;
  uint32_t planned_draws = 0;
  uint32_t recorded_draws = 0;
  uint32_t failed_family_draw_index = 0;
  NativeSceneCompositionRejectReason plan_reject =
      NativeSceneCompositionRejectReason::kMissingScene;
  NativeSceneDrawFamily failed_family = NativeSceneDrawFamily::kVenuePs328;
  uint32_t failed_ordinal = 0;
  const char *detail = "none";
  TransactionResult result = TransactionResult::kPlanRejected;
};

TransactionTelemetry g_latest;
uint64_t g_last_logged_sequence = 0;
TransactionResult g_last_logged_result = TransactionResult::kPlanRejected;
bool g_has_logged_result = false;

const char *TransactionResultName(TransactionResult result) {
  switch (result) {
  case TransactionResult::kSucceeded:
    return "succeeded";
  case TransactionResult::kPlanRejected:
    return "plan_rejected";
  case TransactionResult::kTargetPreparationFailed:
    return "target_preparation_failed";
  case TransactionResult::kFamilyPreparationFailed:
    return "family_preparation_failed";
  case TransactionResult::kPassBeginFailed:
    return "pass_begin_failed";
  case TransactionResult::kFamilyRecordFailed:
    return "family_record_failed";
  case TransactionResult::kPassAbortFailed:
    return "pass_abort_failed";
  }
  return "unknown";
}

const char *NativeSceneDrawFamilyName(NativeSceneDrawFamily family) {
  switch (family) {
  case NativeSceneDrawFamily::kVenuePs328:
    return "venue_ps328";
  case NativeSceneDrawFamily::kVenue14D:
    return "venue_14d";
  case NativeSceneDrawFamily::kCrowdC6:
    return "crowd_c6";
  case NativeSceneDrawFamily::kPlayerCa9:
    return "player_ca9";
  }
  return "unknown";
}

bool ShouldLog(const TransactionTelemetry &telemetry) {
  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_scene_transaction_log_interval);
  const bool changed =
      !g_has_logged_result || telemetry.result != g_last_logged_result;
  const bool interval_elapsed =
      interval != 0 &&
      (g_last_logged_sequence == 0 ||
       telemetry.sequence >= g_last_logged_sequence + interval);
  if (!changed && !interval_elapsed) {
    return false;
  }
  g_has_logged_result = true;
  g_last_logged_result = telemetry.result;
  g_last_logged_sequence = telemetry.sequence;
  return true;
}

void PublishAndMaybeLog(TransactionTelemetry telemetry) {
  g_latest = telemetry;
  if (!ShouldLog(telemetry)) {
    return;
  }
  REXLOG_INFO(
      "Table Tennis native scene transaction observer: frame={} result={} "
      "planned={} recorded={} plan_reject={} failed_family={} "
      "failed_draw_index={} failed_ordinal={} detail={} "
      "component_frames[catalog={} ps328={} "
      "14d={} c6={} ca9={}] private_target=true resolved=false "
      "guest_suppressed=false",
      telemetry.sequence, TransactionResultName(telemetry.result),
      telemetry.planned_draws, telemetry.recorded_draws,
      NativeSceneCompositionRejectReasonName(telemetry.plan_reject),
      telemetry.failed_ordinal != 0
          ? NativeSceneDrawFamilyName(telemetry.failed_family)
          : "none",
      telemetry.failed_family_draw_index, telemetry.failed_ordinal,
      telemetry.detail,
      telemetry.catalog_sequence, telemetry.venue_ps328_sequence,
      telemetry.venue_14d_sequence, telemetry.crowd_c6_sequence,
      telemetry.player_ca9_sequence);
}

bool PrepareFamilies(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const NativeSceneCompositionPlan &plan, const char *&detail_out) {
  if (!PrepareVenueFullFamilyNativeScene(
          context, targets, plan.scene->venue_ps328)) {
    detail_out = "venue_ps328_prepare";
    return false;
  }
  if (!PrepareVenue14DNativeScene(context, targets,
                                 plan.scene->venue_14d)) {
    detail_out = "venue_14d_prepare";
    return false;
  }
  if (!PrepareCrowdNativeScene(context, targets, plan.scene->crowd_c6)) {
    detail_out = "crowd_c6_prepare";
    return false;
  }
  if (!PreparePlayerNativeScene(context, targets, plan.scene->player_ca9)) {
    detail_out = "player_ca9_prepare";
    return false;
  }
  return true;
}

bool RecordDraw(const rex::graphics::NativeGuestOutputRenderContext &context,
                const NativeScenePassTargets &targets,
                const NativeSceneCompositionPlan &plan,
                const NativeSceneDrawRef &draw, const char *&detail_out) {
  switch (draw.family) {
  case NativeSceneDrawFamily::kVenuePs328: {
    const VenueNativeSceneRecordResult result =
        RecordPreparedVenueNativeSceneDraw(
            context, targets, plan.scene->venue_ps328, draw);
    detail_out = VenueNativeSceneRecordResultName(result);
    return result == VenueNativeSceneRecordResult::kRecorded;
  }
  case NativeSceneDrawFamily::kVenue14D: {
    const Venue14DNativeSceneRecordResult result =
        RecordPreparedVenue14DNativeSceneDraw(
            context, targets, plan.scene->venue_14d, draw);
    detail_out = Venue14DNativeSceneRecordResultName(result);
    return result == Venue14DNativeSceneRecordResult::kRecorded;
  }
  case NativeSceneDrawFamily::kCrowdC6: {
    const CrowdNativeSceneRecordResult result =
        RecordPreparedCrowdNativeSceneDraw(
            context, targets, plan.scene->crowd_c6, draw);
    detail_out = CrowdNativeSceneRecordResultName(result);
    return result == CrowdNativeSceneRecordResult::kRecorded;
  }
  case NativeSceneDrawFamily::kPlayerCa9: {
    const PlayerNativeSceneRecordResult result =
        RecordPreparedPlayerNativeSceneDraw(
            context, targets, plan.scene->player_ca9, draw);
    detail_out = PlayerNativeSceneRecordResultName(result);
    return result == PlayerNativeSceneRecordResult::kRecorded;
  }
  }
  detail_out = "unknown_family";
  return false;
}

} // namespace

bool NativeSceneTransactionObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_scene_transaction_observer);
}

void ObserveNativeSceneTransaction(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (!NativeSceneTransactionObserverEnabled()) {
    return;
  }

  NativeSceneCompositionPlan plan =
      BuildNativeSceneCompositionPlan(LatestNativeFrameScene());
  TransactionTelemetry telemetry;
  telemetry.sequence = plan.readiness.title_sequence;
  telemetry.planned_draws = static_cast<uint32_t>(plan.draws.size());
  telemetry.plan_reject = plan.readiness.reject_reason;
  telemetry.failed_family = plan.readiness.failed_family;
  telemetry.failed_family_draw_index =
      plan.readiness.failed_family_draw_index;
  telemetry.failed_ordinal = plan.readiness.failed_ordinal;
  if (plan.scene != nullptr) {
    telemetry.catalog_sequence =
        plan.scene->catalog != nullptr ? plan.scene->catalog->sequence : 0;
    telemetry.venue_ps328_sequence =
        plan.scene->venue_ps328 != nullptr
            ? plan.scene->venue_ps328->sequence
            : 0;
    telemetry.venue_14d_sequence =
        plan.scene->venue_14d != nullptr ? plan.scene->venue_14d->sequence : 0;
    telemetry.crowd_c6_sequence =
        plan.scene->crowd_c6 != nullptr ? plan.scene->crowd_c6->sequence : 0;
    telemetry.player_ca9_sequence =
        plan.scene->player_ca9 != nullptr ? plan.scene->player_ca9->sequence
                                          : 0;
  }
  if (!plan.valid()) {
    telemetry.result = TransactionResult::kPlanRejected;
    telemetry.detail = "composition_plan";
    PublishAndMaybeLog(telemetry);
    return;
  }

  NativeScenePassTargets targets;
  const NativeSceneRenderTargetsResult target_result =
      PrepareNativeSceneRenderTargets(context, targets);
  if (target_result != NativeSceneRenderTargetsResult::kSucceeded) {
    telemetry.result = TransactionResult::kTargetPreparationFailed;
    telemetry.detail = NativeSceneRenderTargetsResultName(target_result);
    PublishAndMaybeLog(telemetry);
    return;
  }

  const char *detail = "none";
  if (!PrepareFamilies(context, targets, plan, detail)) {
    telemetry.result = TransactionResult::kFamilyPreparationFailed;
    telemetry.detail = detail;
    PublishAndMaybeLog(telemetry);
    return;
  }

  constexpr NativeScenePassClearValues kDiagnosticClear = {
      .color = {0.0f, 0.0f, 0.0f, 1.0f},
      .depth = 1.0f,
  };
  const NativeSceneRenderTargetsResult begin_result =
      BeginNativeSceneRenderPass(context, targets, kDiagnosticClear);
  if (begin_result != NativeSceneRenderTargetsResult::kSucceeded) {
    telemetry.result = TransactionResult::kPassBeginFailed;
    telemetry.detail = NativeSceneRenderTargetsResultName(begin_result);
    PublishAndMaybeLog(telemetry);
    return;
  }

  for (const NativeSceneDrawRef &draw : plan.draws) {
    telemetry.failed_family = draw.family;
    telemetry.failed_family_draw_index = draw.family_draw_index;
    telemetry.failed_ordinal = draw.ordinal;
    if (!RecordDraw(context, targets, plan, draw, detail)) {
      telemetry.result = TransactionResult::kFamilyRecordFailed;
      telemetry.detail = detail;
      const NativeSceneRenderTargetsResult abort_result =
          AbortNativeSceneRenderPass(context);
      if (abort_result != NativeSceneRenderTargetsResult::kSucceeded) {
        telemetry.result = TransactionResult::kPassAbortFailed;
        telemetry.detail = NativeSceneRenderTargetsResultName(abort_result);
      }
      PublishAndMaybeLog(telemetry);
      return;
    }
    ++telemetry.recorded_draws;
  }

  const NativeSceneRenderTargetsResult abort_result =
      AbortNativeSceneRenderPass(context);
  if (abort_result != NativeSceneRenderTargetsResult::kSucceeded) {
    telemetry.result = TransactionResult::kPassAbortFailed;
    telemetry.detail = NativeSceneRenderTargetsResultName(abort_result);
    PublishAndMaybeLog(telemetry);
    return;
  }
  telemetry.result = TransactionResult::kSucceeded;
  telemetry.plan_reject = NativeSceneCompositionRejectReason::kNone;
  telemetry.failed_family_draw_index = 0;
  telemetry.failed_ordinal = 0;
  telemetry.detail = "all_draws_recorded_then_discarded";
  PublishAndMaybeLog(telemetry);
}

} // namespace tabletennis::native
