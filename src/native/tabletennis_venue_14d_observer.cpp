#include "native/tabletennis_venue_14d_observer.h"

#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_venue_14d_renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_venue_14d_observer, false, "Table Tennis",
    "Capture immutable five-texture 14D venue candidates and publish only "
    "after an independent complete three-tile backend hash/order proof. "
    "Observer-only; never suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kPixelShaderHash = 0x14D6B61CBC3D853Cull;
constexpr std::array<uint64_t, 2> kVertexShaderHashes = {
    0x4EAEC701E97DCDADull,
    0x08D6210341AD63F6ull,
};
constexpr uint32_t kGameplayRenderPassKey = 0x0000000E;
constexpr uint32_t kGameplaySurfacePitch = 1280;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kRequiredTileBlockCount = 3;
constexpr size_t kMaximumTitleCandidates = 128;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumQueuedBackendEvents = 4096;

struct TitleToken {
  Venue14DDrawIdentity identity{};
  std::shared_ptr<const Venue14DTitleDrawSnapshot> snapshot;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
};

struct BackendEvent {
  uint64_t backend_frame_sequence = 0;
  Venue14DDrawIdentity identity{};
  Venue14DBackendContract contract{};
};

struct BuildingFrame {
  uint64_t sequence = 0;
  uint32_t dropped_candidate_count = 0;
  std::vector<TitleToken> candidates;
};

struct FrameLedger {
  uint64_t sequence = 0;
  bool finalized = false;
  BuildingFrame title{};
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  uint32_t matched_index_count = 0;
  std::vector<size_t> selected_candidate_indices;
  std::vector<Venue14DBackendContract> first_block_contracts;
  std::vector<BackendEvent> backend_events;
};

std::mutex g_observer_mutex;
BuildingFrame g_building_frame;
std::deque<FrameLedger> g_frames;
std::deque<BackendEvent> g_backend_events;
std::shared_ptr<const Venue14DFrameSnapshot> g_published_frame;
Venue14DObserverTelemetry g_telemetry;
bool g_announced_rejection = false;
bool g_announced_mismatch = false;

bool IsTargetVertexShader(uint64_t hash) {
  return std::ranges::find(kVertexShaderHashes, hash) !=
         kVertexShaderHashes.end();
}

bool SupportedDepthFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

bool IsExactBackendDraw(const rex::graphics::NativeGuestDrawContext &context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.render_pass_key_valid &&
         context.render_pass_key == kGameplayRenderPassKey &&
         context.surface_pitch == kGameplaySurfacePitch && context.indexed &&
         context.guest_index_base_valid && context.guest_index_base != 0 &&
         context.draw_state_contract_valid &&
         context.rasterizer_mode_control_valid &&
         context.borrowed_attachment_contract_valid &&
         IsTargetVertexShader(context.vertex_shader_hash) &&
         context.pixel_shader_hash == kPixelShaderHash &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.guest_vertex_or_index_count != 0 &&
         !context.primitive_restart_enabled &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format == context.depth_attachment_format &&
         context.sample_count == 4 &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

Venue14DBackendContract
CaptureBackendContract(const rex::graphics::NativeGuestDrawContext &context) {
  Venue14DBackendContract contract;
  contract.vertex_shader_hash = context.vertex_shader_hash;
  contract.pixel_shader_hash = context.pixel_shader_hash;
  contract.render_pass_key = context.render_pass_key;
  contract.surface_pitch = context.surface_pitch;
  contract.normalized_depth_control = context.normalized_depth_control;
  contract.normalized_color_mask = context.normalized_color_mask;
  contract.color_control = context.color_control;
  contract.blend_control_0 = context.blend_control_0;
  contract.rasterizer_mode_control = context.rasterizer_mode_control;
  contract.primitive_restart_index = context.primitive_restart_index;
  for (size_t attachment = 0;
       attachment < contract.color_attachment_formats.size(); ++attachment) {
    contract.color_attachment_formats[attachment] =
        static_cast<uint32_t>(context.color_attachment_formats[attachment]);
  }
  contract.color_attachment_count = context.color_attachment_count;
  contract.depth_attachment_format =
      static_cast<uint32_t>(context.depth_attachment_format);
  contract.stencil_attachment_format =
      static_cast<uint32_t>(context.stencil_attachment_format);
  contract.sample_count = context.sample_count;
  contract.sample_mask = context.sample_mask;
  contract.primitive_restart_enabled = context.primitive_restart_enabled;
  contract.rasterizer_mode_control_valid =
      context.rasterizer_mode_control_valid;
  contract.valid = IsExactBackendDraw(context);
  return contract;
}

bool SameBackendContract(const Venue14DBackendContract &left,
                         const Venue14DBackendContract &right) {
  return left.valid && right.valid &&
         left.vertex_shader_hash == right.vertex_shader_hash &&
         left.pixel_shader_hash == right.pixel_shader_hash &&
         left.render_pass_key == right.render_pass_key &&
         left.surface_pitch == right.surface_pitch &&
         left.normalized_depth_control == right.normalized_depth_control &&
         left.normalized_color_mask == right.normalized_color_mask &&
         left.color_control == right.color_control &&
         left.blend_control_0 == right.blend_control_0 &&
         left.rasterizer_mode_control == right.rasterizer_mode_control &&
         left.primitive_restart_index == right.primitive_restart_index &&
         left.color_attachment_formats == right.color_attachment_formats &&
         left.color_attachment_count == right.color_attachment_count &&
         left.depth_attachment_format == right.depth_attachment_format &&
         left.stencil_attachment_format == right.stencil_attachment_format &&
         left.sample_count == right.sample_count &&
         left.sample_mask == right.sample_mask &&
         left.rasterizer_mode_control_valid ==
             right.rasterizer_mode_control_valid &&
         left.primitive_restart_enabled == right.primitive_restart_enabled;
}

FrameLedger *FindPendingFrameLocked(uint64_t sequence) {
  const auto found =
      std::ranges::find_if(g_frames, [&](const FrameLedger &frame) {
        return !frame.finalized && frame.sequence == sequence;
      });
  return found == g_frames.end() ? nullptr : &*found;
}

void PublishFrameLocked(FrameLedger &frame) {
  auto published = std::make_shared<Venue14DFrameSnapshot>();
  published->sequence = frame.sequence;
  published->backend_frame_sequence = frame.sequence;
  published->title_candidate_count =
      static_cast<uint32_t>(frame.title.candidates.size());
  published->matched_draw_count =
      static_cast<uint32_t>(frame.selected_candidate_indices.size());
  published->matched_index_count = frame.matched_index_count;
  published->unmatched_title_candidate_count =
      published->title_candidate_count - published->matched_draw_count;
  published->dropped_candidate_count = frame.title.dropped_candidate_count;
  published->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  published->backend_draws_per_tile = published->matched_draw_count;
  published->backend_indices_per_tile = published->matched_index_count;
  published->backend_tile_blocks_matched = frame.backend_tile_blocks_matched;
  published->backend_sequence_mismatches = frame.backend_sequence_mismatches;
  published->draws.reserve(frame.selected_candidate_indices.size());
  for (size_t index = 0; index < frame.selected_candidate_indices.size();
       ++index) {
    const size_t candidate_index = frame.selected_candidate_indices[index];
    if (candidate_index >= frame.title.candidates.size() ||
        index >= frame.first_block_contracts.size()) {
      continue;
    }
    const TitleToken &candidate = frame.title.candidates[candidate_index];
    published->guest_read_failures += candidate.guest_read_failures;
    published->payload_copy_failures += candidate.payload_copy_failures;
    published->texture_capture_failures += candidate.texture_capture_failures;
    published->draws.push_back({
        .title = candidate.snapshot,
        .backend = frame.first_block_contracts[index],
    });
  }
  g_published_frame = std::move(published);
  ObserveMainCoverageFamilyFrame(g_published_frame);
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  g_telemetry.latest_published_sequence = frame.sequence;

  if (g_published_frame->valid()) {
    ++g_telemetry.valid_frames;
    std::vector<uint32_t> rasterizer_modes;
    bool rasterizer_modes_valid = true;
    for (const Venue14DDrawSnapshot &draw : g_published_frame->draws) {
      rasterizer_modes_valid &=
          draw.backend.rasterizer_mode_control_valid;
      if (std::ranges::find(rasterizer_modes,
                            draw.backend.rasterizer_mode_control) ==
          rasterizer_modes.end()) {
        rasterizer_modes.push_back(draw.backend.rasterizer_mode_control);
      }
    }
    REXLOG_INFO("Table Tennis 14D venue observer: frame={} candidates={} "
                "matched={} unmatched_title={} indices={} "
                "backend_events={} backend_tile_blocks=3 "
                "raster={:08X}/{} unique_raster={} "
                "textures_per_draw=5 full_constant_banks=true "
                "observer_only=true guest_suppressed=false",
                g_published_frame->sequence,
                g_published_frame->title_candidate_count,
                g_published_frame->matched_draw_count,
                g_published_frame->unmatched_title_candidate_count,
                g_published_frame->matched_index_count,
                g_published_frame->backend_event_count,
                rasterizer_modes.empty() ? 0 : rasterizer_modes.front(),
                rasterizer_modes_valid, rasterizer_modes.size());
    return;
  }

  ++g_telemetry.rejected_frames;
  if (!g_announced_rejection) {
    g_announced_rejection = true;
    REXLOG_INFO("Table Tennis 14D venue observer: rejected frame={} "
                "candidates={} matched={} indices={} dropped={} reads={} "
                "payload_copies={} textures={} backend_events={} "
                "backend_blocks={} mismatches={} "
                "observer_only=true guest_suppressed=false",
                g_published_frame->sequence,
                g_published_frame->title_candidate_count,
                g_published_frame->matched_draw_count,
                g_published_frame->matched_index_count,
                g_published_frame->dropped_candidate_count,
                g_published_frame->guest_read_failures,
                g_published_frame->payload_copy_failures,
                g_published_frame->texture_capture_failures,
                g_published_frame->backend_event_count,
                g_published_frame->backend_tile_blocks_matched,
                g_published_frame->backend_sequence_mismatches);
  }
}

void RecordMismatch(FrameLedger &frame) {
  ++frame.backend_sequence_mismatches;
  ++g_telemetry.backend_sequence_mismatches;
}

void AnnounceAnalysisMismatch(const FrameLedger &frame, const char *reason,
                              size_t event_index, const BackendEvent *expected,
                              const BackendEvent *observed) {
  if (g_announced_mismatch) {
    return;
  }
  g_announced_mismatch = true;
  const BackendEvent empty{};
  const BackendEvent &expected_event = expected != nullptr ? *expected : empty;
  const BackendEvent &observed_event = observed != nullptr ? *observed : empty;
  REXLOG_INFO("Table Tennis 14D venue observer: frame-bucket mismatch "
              "title_frame={} backend_frame={} reason={} event={} "
              "backend_events={} title_candidates={} "
              "expected[primitive={} indices={} base={:08X}] "
              "observed[primitive={} indices={} base={:08X}] "
              "observer_only=true",
              frame.sequence, observed_event.backend_frame_sequence, reason,
              event_index, frame.backend_events.size(),
              frame.title.candidates.size(),
              expected_event.identity.primitive_type,
              expected_event.identity.submitted_index_count,
              expected_event.identity.guest_index_base,
              observed_event.identity.primitive_type,
              observed_event.identity.submitted_index_count,
              observed_event.identity.guest_index_base);
}

void AnalyzeFrameLocked(FrameLedger &frame) {
  ++g_telemetry.backend_frames_analyzed;
  const size_t event_count = frame.backend_events.size();
  if (event_count == 0 || event_count % kRequiredTileBlockCount != 0) {
    AnnounceAnalysisMismatch(frame, "event-count-not-three-blocks", 0, nullptr,
                             nullptr);
    RecordMismatch(frame);
    PublishFrameLocked(frame);
    return;
  }

  const size_t draws_per_tile = event_count / kRequiredTileBlockCount;
  if (draws_per_tile == 0 || draws_per_tile > frame.title.candidates.size()) {
    AnnounceAnalysisMismatch(frame, "draw-count-exceeds-title", 0, nullptr,
                             &frame.backend_events.front());
    RecordMismatch(frame);
    PublishFrameLocked(frame);
    return;
  }

  for (size_t tile = 1; tile < kRequiredTileBlockCount; ++tile) {
    for (size_t draw = 0; draw < draws_per_tile; ++draw) {
      const BackendEvent &expected = frame.backend_events[draw];
      const BackendEvent &observed =
          frame.backend_events[tile * draws_per_tile + draw];
      if (expected.backend_frame_sequence != frame.sequence ||
          observed.backend_frame_sequence != frame.sequence ||
          !(expected.identity == observed.identity) ||
          !SameBackendContract(expected.contract, observed.contract)) {
        AnnounceAnalysisMismatch(frame, "tile-block-contract",
                                 tile * draws_per_tile + draw, &expected,
                                 &observed);
        RecordMismatch(frame);
        PublishFrameLocked(frame);
        return;
      }
    }
  }

  std::vector<uint8_t> candidate_used(frame.title.candidates.size(),
                                      uint8_t{0});
  frame.selected_candidate_indices.reserve(draws_per_tile);
  frame.first_block_contracts.reserve(draws_per_tile);
  for (size_t draw = 0; draw < draws_per_tile; ++draw) {
    const BackendEvent &event = frame.backend_events[draw];
    const auto found = std::ranges::find_if(
        frame.title.candidates, [&](const TitleToken &candidate) {
          const size_t index =
              static_cast<size_t>(&candidate - frame.title.candidates.data());
          return candidate_used[index] == 0 &&
                 candidate.identity == event.identity;
        });
    if (found == frame.title.candidates.end()) {
      AnnounceAnalysisMismatch(frame, "backend-title-join", draw, nullptr,
                               &event);
      RecordMismatch(frame);
      PublishFrameLocked(frame);
      return;
    }
    const size_t candidate_index =
        static_cast<size_t>(found - frame.title.candidates.begin());
    candidate_used[candidate_index] = 1;
    frame.selected_candidate_indices.push_back(candidate_index);
    frame.first_block_contracts.push_back(event.contract);
    frame.matched_index_count += event.identity.submitted_index_count;
  }

  frame.backend_tile_blocks_matched = kRequiredTileBlockCount;
  g_telemetry.backend_tile_blocks_matched += kRequiredTileBlockCount;
  PublishFrameLocked(frame);
}

void ReconcileBackendEventsLocked() {
  auto event = g_backend_events.begin();
  while (event != g_backend_events.end()) {
    FrameLedger *frame = FindPendingFrameLocked(event->backend_frame_sequence);
    if (frame != nullptr) {
      frame->backend_events.push_back(std::move(*event));
      event = g_backend_events.erase(event);
      continue;
    }
    if (!g_frames.empty() &&
        event->backend_frame_sequence < g_frames.front().sequence) {
      ++g_telemetry.backend_events_without_title_frame;
      event = g_backend_events.erase(event);
      continue;
    }
    ++event;
  }
}

void AnalyzeCompletedFramesLocked() {
  const uint64_t completed_before = g_telemetry.latest_backend_frame_sequence;
  for (FrameLedger &frame : g_frames) {
    if (!frame.finalized && frame.sequence < completed_before) {
      AnalyzeFrameLocked(frame);
    }
  }
}

void ExpireFramesLocked() {
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized) {
      ++g_telemetry.expired_frames;
    }
    g_frames.pop_front();
  }
}

void UpdatePendingCountsLocked() {
  g_telemetry.pending_frames = static_cast<uint32_t>(
      std::ranges::count_if(g_frames, [](const FrameLedger &frame) {
        return !frame.finalized && !frame.title.candidates.empty();
      }));
  g_telemetry.queued_backend_events =
      static_cast<uint32_t>(g_backend_events.size());
}

} // namespace

bool Venue14DObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_venue_14d_observer) ||
         Venue14DRendererEnabled() || NativeFrameSceneCaptureEnabled();
}

void ObserveVenue14DTitleDraw(uint8_t *guest_base,
                              const SceneCatalogDrawOccurrence &draw) {
  if (!Venue14DObserverEnabled()) {
    return;
  }
  Venue14DTitleCapture capture = CaptureVenue14DTitleDraw(guest_base, draw);
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.title_draws_observed;
  }
  if (!capture.family_candidate || !capture.identity.valid()) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  if (draw.frame_sequence == 0) {
    ++g_building_frame.dropped_candidate_count;
    return;
  }
  if (g_building_frame.sequence == 0) {
    g_building_frame.sequence = draw.frame_sequence;
  } else if (g_building_frame.sequence != draw.frame_sequence) {
    ++g_building_frame.dropped_candidate_count;
    return;
  }
  ++g_telemetry.title_candidates;
  g_telemetry.valid_title_snapshots += capture.snapshot != nullptr;
  g_telemetry.title_guest_read_failures += capture.guest_read_failures;
  g_telemetry.title_payload_copy_failures += capture.payload_copy_failures;
  g_telemetry.title_texture_capture_failures +=
      capture.texture_capture_failures;
  if (g_building_frame.candidates.size() == kMaximumTitleCandidates) {
    ++g_building_frame.dropped_candidate_count;
    return;
  }
  g_building_frame.candidates.push_back({
      .identity = capture.identity,
      .snapshot = std::move(capture.snapshot),
      .guest_read_failures = capture.guest_read_failures,
      .payload_copy_failures = capture.payload_copy_failures,
      .texture_capture_failures = capture.texture_capture_failures,
  });
}

void ObserveVenue14DBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!Venue14DObserverEnabled()) {
    return;
  }
  const bool pixel_hash_matches = context.pixel_shader_hash == kPixelShaderHash;
  const bool shader_pair_matches =
      pixel_hash_matches && IsTargetVertexShader(context.vertex_shader_hash);
  const bool exact_backend_draw = IsExactBackendDraw(context);
  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.backend_draws_observed;
  g_telemetry.latest_backend_frame_sequence =
      std::max(g_telemetry.latest_backend_frame_sequence,
               context.backend_frame_sequence);
  g_telemetry.backend_pixel_hash_matches += pixel_hash_matches;
  g_telemetry.backend_shader_pair_matches += shader_pair_matches;
  if (exact_backend_draw && context.backend_frame_sequence != 0) {
    BackendEvent event = {
        .backend_frame_sequence = context.backend_frame_sequence,
        .identity =
            {
                .primitive_type = context.primitive_type,
                .submitted_index_count = context.guest_vertex_or_index_count,
                .guest_index_base = context.guest_index_base,
            },
        .contract = CaptureBackendContract(context),
    };
    ++g_telemetry.backend_events;
    if (g_backend_events.size() == kMaximumQueuedBackendEvents) {
      g_backend_events.pop_front();
      ++g_telemetry.backend_events_dropped;
    }
    g_backend_events.push_back(std::move(event));
  }
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  UpdatePendingCountsLocked();
}

void Venue14DObserverFrameEnd() {
  const bool enabled = Venue14DObserverEnabled();
  std::lock_guard lock(g_observer_mutex);
  if (!enabled) {
    g_building_frame = {};
    g_frames.clear();
    g_backend_events.clear();
    g_published_frame.reset();
    g_telemetry = {};
    g_announced_rejection = false;
    g_announced_mismatch = false;
    return;
  }

  ++g_telemetry.title_frames;
  if (!g_building_frame.candidates.empty()) {
    FrameLedger frame;
    frame.sequence = g_building_frame.sequence;
    frame.title = std::move(g_building_frame);
    g_frames.push_back(std::move(frame));
  }
  g_building_frame = {};
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  ExpireFramesLocked();
  UpdatePendingCountsLocked();
}

std::shared_ptr<const Venue14DFrameSnapshot> LatestVenue14DFrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

Venue14DObserverTelemetry LatestVenue14DObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  return g_telemetry;
}

} // namespace tabletennis::native
