#include "native/tabletennis_venue_full_family.h"

#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_ps328_tile_invariance.h"
#include "native/tabletennis_translated_shader_artifact_store.h"
#include <cstddef>
#include <deque>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_venue_full_family_observer, false, "Table Tennis",
    "Capture every exact ordered PS328 venue-family title draw, including "
    "duplicate counts. Observer-only; never suppresses or replaces guest "
    "draws.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    tabletennis_native_venue_full_family_overlay, false, "Table Tennis",
    "Draw the complete valid captured PS328 venue family over untouched guest "
    "output. Observer-only; never suppresses or replaces guest draws.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr size_t kMaximumFullFamilyDraws = 256;
constexpr size_t kMaximumRetainedFullFamilyFrames = 8;

std::mutex g_full_family_mutex;
VenueFullFamilyFrame g_building_frame;
std::shared_ptr<const VenueFullFamilyFrame> g_published_frame;
std::deque<std::shared_ptr<const VenueFullFamilyFrame>> g_frame_history;
size_t g_largest_logged_family = 0;

std::string OrderedCountList(const VenueFullFamilyFrame &frame) {
  std::string result;
  result.reserve(frame.draws.size() * 5);
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    if (index != 0) {
      result.push_back(',');
    }
    result += std::to_string(frame.draws[index].source.submitted_index_count);
  }
  return result;
}

} // namespace

bool VenueFullFamilyObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_venue_full_family_observer) ||
         VenueFullFamilyOverlayEnabled() ||
         MainCoverageLedgerEnabled() ||
         (!Ps328TitleCaptureRetired() &&
          (NativeFrameSceneCaptureEnabled() ||
           TranslatedShaderArtifactStoreEnabled()));
}

bool VenueFullFamilyOverlayEnabled() {
  return REXCVAR_GET(tabletennis_native_venue_full_family_overlay);
}

void ObserveVenueFullFamilyDraw(const SceneCatalogDrawOccurrence &source,
                                const VenueDrawSnapshot &captured) {
  if (!VenueFullFamilyObserverEnabled()) {
    return;
  }

  std::lock_guard lock(g_full_family_mutex);
  if (source.frame_sequence == 0) {
    ++g_building_frame.dropped_draws;
    return;
  }
  if (g_building_frame.sequence == 0) {
    g_building_frame.sequence = source.frame_sequence;
  } else if (g_building_frame.sequence != source.frame_sequence) {
    ++g_building_frame.dropped_draws;
    return;
  }
  if (g_building_frame.draws.size() == kMaximumFullFamilyDraws) {
    ++g_building_frame.dropped_draws;
    return;
  }

  g_building_frame.submitted_index_count += source.submitted_index_count;
  if (captured.mesh == nullptr || !captured.mesh->valid() ||
      !captured.material.valid) {
    ++g_building_frame.copy_failures;
  }
  g_building_frame.draws.push_back({
      .source = source,
      .captured = captured,
  });
}

void VenueFullFamilyFrameEnd() {
  const bool enabled = VenueFullFamilyObserverEnabled();
  std::lock_guard lock(g_full_family_mutex);
  if (!enabled) {
    g_building_frame = {};
    g_published_frame.reset();
    g_frame_history.clear();
    g_largest_logged_family = 0;
    return;
  }

  if (g_building_frame.draws.empty()) {
    g_published_frame.reset();
  } else {
    g_published_frame = std::make_shared<const VenueFullFamilyFrame>(
        std::move(g_building_frame));
    g_frame_history.push_back(g_published_frame);
    while (g_frame_history.size() > kMaximumRetainedFullFamilyFrames) {
      g_frame_history.pop_front();
    }
    ObserveMainCoverageFamilyFrame(g_published_frame);
    if (g_published_frame->draws.size() > g_largest_logged_family) {
      g_largest_logged_family = g_published_frame->draws.size();
      size_t adjacent_duplicate_counts = 0;
      for (size_t index = 1; index < g_published_frame->draws.size(); ++index) {
        adjacent_duplicate_counts +=
            g_published_frame->draws[index - 1].source.submitted_index_count ==
            g_published_frame->draws[index].source.submitted_index_count;
      }
      REXLOG_INFO(
          "Table Tennis full PS328 observer: captured {} exact ordered "
          "title draws submitted_indices={} adjacent_duplicate_counts={} "
          "copy_failures={} dropped={} observer-only",
          g_published_frame->draws.size(),
          g_published_frame->submitted_index_count, adjacent_duplicate_counts,
          g_published_frame->copy_failures, g_published_frame->dropped_draws);
      REXLOG_INFO("  full_ps328_counts={}",
                  OrderedCountList(*g_published_frame));
      if (g_published_frame->copy_failures != 0) {
        for (size_t index = 0; index < g_published_frame->draws.size();
             ++index) {
          const VenueFullFamilyDrawSnapshot &draw =
              g_published_frame->draws[index];
          if (draw.valid()) {
            continue;
          }
          REXLOG_INFO(
              "  full_ps328_copy_failure position={} ordinal={} "
              "indices={} mesh={} material={} vb={:08X} ib={:08X}",
              index, draw.source.ordinal, draw.source.submitted_index_count,
              draw.captured.mesh != nullptr && draw.captured.mesh->valid(),
              draw.captured.material.valid,
              draw.source.mesh.vertex_buffer_alias,
              draw.source.mesh.index_buffer_alias);
        }
      }
    }
  }
  g_building_frame = {};
}

std::shared_ptr<const VenueFullFamilyFrame> LatestVenueFullFamilyFrame() {
  std::lock_guard lock(g_full_family_mutex);
  return g_published_frame;
}

std::shared_ptr<const VenueFullFamilyFrame>
VenueFullFamilyFrameForSequence(uint64_t sequence) {
  if (sequence == 0) {
    return nullptr;
  }
  std::lock_guard lock(g_full_family_mutex);
  const auto found = std::ranges::find(
      g_frame_history, sequence,
      [](const std::shared_ptr<const VenueFullFamilyFrame> &frame) {
        return frame != nullptr ? frame->sequence : 0;
      });
  return found != g_frame_history.end() ? *found : nullptr;
}

} // namespace tabletennis::native
