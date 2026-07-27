#include "native/tabletennis_6ae_player_observer.h"

#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_6ae_player_observer, false, "Table Tennis",
    "Capture immutable 6AE player meshes, live skin palettes and material "
    "state, then verify the complete translated backend block. "
    "Observer-only; never suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kExpectedSurfacePitch = 1280;
constexpr uint32_t kExpectedDepthControl = 0x00700736;
constexpr uint32_t kExpectedColorMask = 0x0000000F;
constexpr uint32_t kExpectedColorControl = 0x87000005;
constexpr uint32_t kExpectedBlendControl = 0x00010001;
constexpr uint32_t kRequiredTileBlockCount = 3;
constexpr uint64_t kVertexShaderHash = 0xBBB580AA5620D2A6ull;
constexpr uint64_t kPixelShaderHash = 0x6AE43640A86B33D8ull;
constexpr size_t kMaximumTitleDraws = 128;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumQueuedBackendEvents = 2048;
constexpr size_t kMaximumLoggedLearnedPrograms = 8;

struct TitleToken {
  Player6AEDrawIdentity identity{};
  SceneCatalogPassIdentity program{};
  std::shared_ptr<const Player6AEDrawSnapshot> snapshot;
};

struct BackendEvent {
  uint64_t frame_sequence = 0;
  Player6AEDrawIdentity identity{};
  Player6AEBackendContract contract{};
};

struct FrameLedger {
  uint64_t sequence = 0;
  bool closed = false;
  bool finalized = false;
  uint32_t dropped_draw_count = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  std::vector<TitleToken> tokens;
  std::vector<size_t> selected_token_indices;
  std::vector<BackendEvent> backend_events;
  std::vector<Player6AEBackendContract> first_block_contracts;
};

std::mutex g_observer_mutex;
std::deque<FrameLedger> g_frames;
std::deque<BackendEvent> g_backend_events;
std::shared_ptr<const Player6AEFrameSnapshot> g_published_frame;
Player6AEObserverTelemetry g_telemetry;
uint64_t g_title_sequence = 0;
size_t g_largest_logged_draw_count = 0;
bool g_announced_rejection = false;
uint64_t g_latest_backend_frame_sequence = 0;
std::vector<SceneCatalogPassIdentity> g_logged_learned_programs;

bool SupportedDepthFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

bool IsExactBackendDraw(const rex::graphics::NativeGuestDrawContext &context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.render_pass_key_valid && context.indexed &&
         context.guest_index_base_valid && context.draw_state_contract_valid &&
         context.borrowed_attachment_contract_valid &&
         context.vertex_shader_hash == kVertexShaderHash &&
         context.pixel_shader_hash == kPixelShaderHash &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.guest_vertex_or_index_count != 0 &&
         context.guest_index_base != 0 &&
         context.surface_pitch == kExpectedSurfacePitch &&
         context.normalized_depth_control == kExpectedDepthControl &&
         context.normalized_color_mask == kExpectedColorMask &&
         context.color_control == kExpectedColorControl &&
         context.blend_control_0 == kExpectedBlendControl &&
         !context.primitive_restart_enabled &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format == context.depth_attachment_format &&
         context.sample_count == 4 &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

Player6AEBackendContract
CaptureBackendContract(const rex::graphics::NativeGuestDrawContext &context) {
  Player6AEBackendContract contract;
  contract.vertex_shader_hash = context.vertex_shader_hash;
  contract.pixel_shader_hash = context.pixel_shader_hash;
  contract.surface_pitch = context.surface_pitch;
  contract.render_pass_key = context.render_pass_key;
  contract.normalized_depth_control = context.normalized_depth_control;
  contract.normalized_color_mask = context.normalized_color_mask;
  contract.color_control = context.color_control;
  contract.blend_control_0 = context.blend_control_0;
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
  contract.valid = IsExactBackendDraw(context);
  return contract;
}

bool SameBackendContract(const Player6AEBackendContract &left,
                         const Player6AEBackendContract &right) {
  return left.valid && right.valid &&
         left.vertex_shader_hash == right.vertex_shader_hash &&
         left.pixel_shader_hash == right.pixel_shader_hash &&
         left.surface_pitch == right.surface_pitch &&
         left.render_pass_key == right.render_pass_key &&
         left.normalized_depth_control == right.normalized_depth_control &&
         left.normalized_color_mask == right.normalized_color_mask &&
         left.color_control == right.color_control &&
         left.blend_control_0 == right.blend_control_0 &&
         left.primitive_restart_index == right.primitive_restart_index &&
         left.color_attachment_formats == right.color_attachment_formats &&
         left.color_attachment_count == right.color_attachment_count &&
         left.depth_attachment_format == right.depth_attachment_format &&
         left.stencil_attachment_format == right.stencil_attachment_format &&
         left.sample_count == right.sample_count &&
         left.sample_mask == right.sample_mask &&
         left.primitive_restart_enabled == right.primitive_restart_enabled;
}

FrameLedger &EnsureFrameLocked(uint64_t sequence) {
  const auto existing =
      std::ranges::find_if(g_frames, [sequence](const FrameLedger &frame) {
        return frame.sequence == sequence;
      });
  if (existing != g_frames.end()) {
    return *existing;
  }
  g_frames.push_back(FrameLedger{.sequence = sequence});
  return g_frames.back();
}

FrameLedger *FindPendingFrameLocked(uint64_t sequence) {
  const auto found =
      std::ranges::find_if(g_frames, [sequence](const FrameLedger &frame) {
        return frame.sequence == sequence && !frame.finalized &&
               !frame.tokens.empty();
      });
  return found == g_frames.end() ? nullptr : &*found;
}

void PublishFrameLocked(FrameLedger &ledger) {
  auto published = std::make_shared<Player6AEFrameSnapshot>();
  published->sequence = ledger.sequence;
  published->title_candidate_count =
      static_cast<uint32_t>(ledger.selected_token_indices.size());
  published->dropped_draw_count = ledger.dropped_draw_count;
  published->guest_read_failures = ledger.guest_read_failures;
  published->payload_copy_failures = ledger.payload_copy_failures;
  published->texture_capture_failures = ledger.texture_capture_failures;
  published->backend_tile_blocks_matched = ledger.backend_tile_blocks_matched;
  published->backend_sequence_mismatches = ledger.backend_sequence_mismatches;
  published->draws.reserve(ledger.selected_token_indices.size());

  for (size_t index = 0; index < ledger.selected_token_indices.size();
       ++index) {
    const size_t selected = ledger.selected_token_indices[index];
    if (selected >= ledger.tokens.size()) {
      continue;
    }
    const TitleToken &token = ledger.tokens[selected];
    if (token.snapshot == nullptr ||
        index >= ledger.first_block_contracts.size()) {
      continue;
    }
    Player6AEDrawSnapshot draw = *token.snapshot;
    draw.backend = ledger.first_block_contracts[index];
    draw.valid = draw.valid && draw.backend.valid;
    if (draw.valid) {
      published->draws.push_back(std::move(draw));
    }
  }
  published->admitted_draw_count =
      static_cast<uint32_t>(published->draws.size());
  g_published_frame = std::move(published);
  ObserveMainCoverageFamilyFrame(g_published_frame);
  ledger.finalized = true;

  ++g_telemetry.finalized_frames;
  g_telemetry.latest_published_sequence = ledger.sequence;
  if (g_published_frame->valid()) {
    ++g_telemetry.valid_frames;
    if (g_published_frame->draws.size() > g_largest_logged_draw_count) {
      g_largest_logged_draw_count = g_published_frame->draws.size();
      const Player6AEDrawSnapshot &first = g_published_frame->draws.front();
      REXLOG_INFO("Table Tennis 6AE player observer: frame={} draws={} "
                  "backend_tile_blocks=3 vf95={:08X}/{} stride=36 indices={} "
                  "vf92={:08X}/{} palette_records={} referenced_records={} "
                  "material_textures=3 texture_bindings=6 observer_only=true "
                  "guest_suppressed=false",
                  g_published_frame->sequence, g_published_frame->draws.size(),
                  first.vertices->fetch.physical_address,
                  first.vertices->raw_bytes.size(),
                  first.indices->submitted_index_count,
                  first.palette->fetch.physical_address,
                  first.palette->raw_bytes.size(), first.palette->record_count,
                  first.palette->referenced_record_count);
    }
  } else {
    ++g_telemetry.rejected_frames;
    if (!g_announced_rejection) {
      g_announced_rejection = true;
      REXLOG_INFO(
          "Table Tennis 6AE player observer: rejected frame={} "
          "candidates={} admitted={} dropped={} reads={} payload_copies={} "
          "textures={} backend_blocks={} sequence_mismatches={} "
          "observer_only=true guest_suppressed=false",
          g_published_frame->sequence, g_published_frame->title_candidate_count,
          g_published_frame->admitted_draw_count,
          g_published_frame->dropped_draw_count,
          g_published_frame->guest_read_failures,
          g_published_frame->payload_copy_failures,
          g_published_frame->texture_capture_failures,
          g_published_frame->backend_tile_blocks_matched,
          g_published_frame->backend_sequence_mismatches);
    }
  }
}

void ReconcileBackendEventsLocked() {
  auto event = g_backend_events.begin();
  while (event != g_backend_events.end()) {
    if (FrameLedger *frame = FindPendingFrameLocked(event->frame_sequence)) {
      frame->backend_events.push_back(std::move(*event));
      event = g_backend_events.erase(event);
    } else if (event->frame_sequence <= g_title_sequence) {
      event = g_backend_events.erase(event);
    } else {
      ++event;
    }
  }
}

void AnalyzeFrameLocked(FrameLedger &frame) {
  const size_t event_count = frame.backend_events.size();
  if (event_count == 0 || event_count % kRequiredTileBlockCount != 0) {
    ++frame.backend_sequence_mismatches;
    ++g_telemetry.backend_sequence_mismatches;
    PublishFrameLocked(frame);
    return;
  }
  const size_t draws_per_tile = event_count / kRequiredTileBlockCount;
  for (size_t tile = 1; tile < kRequiredTileBlockCount; ++tile) {
    for (size_t draw = 0; draw < draws_per_tile; ++draw) {
      const BackendEvent &expected = frame.backend_events[draw];
      const BackendEvent &observed =
          frame.backend_events[tile * draws_per_tile + draw];
      if (!(expected.identity == observed.identity) ||
          !SameBackendContract(expected.contract, observed.contract)) {
        ++frame.backend_sequence_mismatches;
        ++g_telemetry.backend_sequence_mismatches;
        PublishFrameLocked(frame);
        return;
      }
    }
  }

  size_t search_index = 0;
  frame.selected_token_indices.reserve(draws_per_tile);
  frame.first_block_contracts.reserve(draws_per_tile);
  for (size_t draw = 0; draw < draws_per_tile; ++draw) {
    const BackendEvent &event = frame.backend_events[draw];
    size_t selected = frame.tokens.size();
    for (size_t candidate = search_index; candidate < frame.tokens.size();
         ++candidate) {
      if (frame.tokens[candidate].identity == event.identity) {
        selected = candidate;
        break;
      }
    }
    if (selected == frame.tokens.size()) {
      ++frame.backend_sequence_mismatches;
      ++g_telemetry.backend_sequence_mismatches;
      PublishFrameLocked(frame);
      return;
    }
    search_index = selected + 1;
    frame.selected_token_indices.push_back(selected);
    frame.first_block_contracts.push_back(event.contract);
  }
  frame.backend_tile_blocks_matched = kRequiredTileBlockCount;
  g_telemetry.backend_tile_blocks_matched += kRequiredTileBlockCount;

  for (size_t selected : frame.selected_token_indices) {
    const SceneCatalogPassIdentity &program = frame.tokens[selected].program;
    const bool seen = std::ranges::any_of(
        g_logged_learned_programs,
        [&](const SceneCatalogPassIdentity &candidate) {
          return candidate.pass_descriptor == program.pass_descriptor &&
                 candidate.program_pair == program.program_pair;
        });
    if (!seen &&
        g_logged_learned_programs.size() < kMaximumLoggedLearnedPrograms) {
      g_logged_learned_programs.push_back(program);
      REXLOG_INFO(
          "Table Tennis 6AE learned title program: frame={} pass={:08X} "
          "program={:08X} vs={:08X}/{:016X} ps={:08X}/{:016X} "
          "backend_tiles=3 observer_only=true",
          frame.sequence, program.pass_descriptor, program.program_pair,
          program.vertex_shader, program.vertex_shader_hash,
          program.pixel_shader, program.pixel_shader_hash);
    }
  }
  PublishFrameLocked(frame);
}

void AnalyzeCompletedFramesLocked() {
  for (FrameLedger &frame : g_frames) {
    if (!frame.finalized && frame.sequence < g_latest_backend_frame_sequence) {
      AnalyzeFrameLocked(frame);
    }
  }
}

void ExpireFramesLocked() {
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized && !g_frames.front().tokens.empty()) {
      ++g_telemetry.expired_frames;
    }
    g_frames.pop_front();
  }
}

void UpdatePendingCountsLocked() {
  g_telemetry.pending_frames = static_cast<uint32_t>(
      std::ranges::count_if(g_frames, [](const FrameLedger &frame) {
        return !frame.finalized && !frame.tokens.empty();
      }));
  g_telemetry.queued_backend_events =
      static_cast<uint32_t>(g_backend_events.size());
}

} // namespace

void ObservePlayer6AECatalogDraw(uint8_t *guest_base,
                                 const SceneCatalogDrawOccurrence &draw) {
  if (!Player6AEObserverEnabled() || !IsStructuralPlayer6AETitleDraw(draw)) {
    return;
  }

  Player6AETitleCapture capture = CapturePlayer6AETitleDraw(guest_base, draw);
  if (!capture.family_candidate || !capture.identity.valid()) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  if (draw.frame_sequence == 0) {
    return;
  }
  g_title_sequence = std::max(g_title_sequence, draw.frame_sequence);
  FrameLedger &frame = EnsureFrameLocked(draw.frame_sequence);
  ++g_telemetry.title_candidates;
  frame.guest_read_failures += capture.guest_read_failures;
  frame.payload_copy_failures += capture.payload_copy_failures;
  frame.texture_capture_failures += capture.texture_capture_failures;
  if (frame.tokens.size() == kMaximumTitleDraws) {
    ++frame.dropped_draw_count;
    return;
  }
  frame.tokens.push_back({
      .identity = capture.identity,
      .program = draw.pass,
      .snapshot = std::move(capture.snapshot),
  });
  g_telemetry.admitted_draws += frame.tokens.back().snapshot != nullptr;
}

void ObservePlayer6AEBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!Player6AEObserverEnabled()) {
    return;
  }
  const bool exact_backend_draw = IsExactBackendDraw(context);

  std::lock_guard lock(g_observer_mutex);
  // Sequence advance is global, not family-local. A non-6AE callback from
  // N+1 must still close and publish the pending 6AE proof for frame N.
  g_latest_backend_frame_sequence =
      std::max(g_latest_backend_frame_sequence, context.backend_frame_sequence);
  if (exact_backend_draw) {
    BackendEvent event = {
        .frame_sequence = context.backend_frame_sequence,
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

void Player6AEObserverFrameEnd() {
  Player6AESnapshotFrameEnd();
  const bool enabled = Player6AEObserverEnabled();
  std::lock_guard lock(g_observer_mutex);
  if (!enabled) {
    g_frames.clear();
    g_backend_events.clear();
    g_published_frame.reset();
    g_telemetry = {};
    g_title_sequence = 0;
    g_largest_logged_draw_count = 0;
    g_announced_rejection = false;
    g_latest_backend_frame_sequence = 0;
    g_logged_learned_programs.clear();
    return;
  }

  ++g_telemetry.title_frames;
  g_telemetry.latest_title_sequence = g_title_sequence;
  for (FrameLedger &frame : g_frames) {
    if (frame.sequence <= g_title_sequence) {
      frame.closed = true;
    }
  }
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  ExpireFramesLocked();
  UpdatePendingCountsLocked();
}

bool Player6AEObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_6ae_player_observer) ||
         NativeFrameSceneCaptureEnabled();
}

std::shared_ptr<const Player6AEFrameSnapshot> LatestPlayer6AEFrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

Player6AEObserverTelemetry LatestPlayer6AEObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  return g_telemetry;
}

} // namespace tabletennis::native
