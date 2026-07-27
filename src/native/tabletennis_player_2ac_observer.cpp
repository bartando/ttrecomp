#include "native/tabletennis_player_2ac_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_player_2ac_renderer.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_2ac_observer, false, "Table Tennis",
    "Capture low-level bound 2AC player motion-composite title draws and "
    "publish only after an exact same-frame translated-backend order/state "
    "join. "
    "Observer-only; never serves or suppresses a guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kPixelShaderHash = 0x2AC059EB5C7A942Full;
constexpr uint64_t kSingleStream32VertexShaderHash = 0x4761A30F65AA309Cull;
constexpr uint64_t kSingleStream96VertexShaderHash = 0x3E233105507CB75Full;
constexpr uint64_t kSkinned36VertexShaderHash = 0x633DEDEA0081898Cull;
constexpr uint64_t kSkinned44VertexShaderHash = 0x435F65388E61D4B5ull;
constexpr std::array<uint64_t, 4> kVertexShaderHashes = {
    kSingleStream32VertexShaderHash,
    kSingleStream96VertexShaderHash,
    kSkinned36VertexShaderHash,
    kSkinned44VertexShaderHash,
};

constexpr uint32_t kTriangleListPrimitive = 4;
constexpr uint32_t kTriangleStripPrimitive = 6;
// The late borrowed COMP callback is a distinct translated render pass from
// tiled MAIN. Live Vulkan evidence exposes it as key 0xC with one host
// sample, while the guest Xenos surface registers still describe the traced
// 4x EDRAM state. Admission follows the backend contract visible to a native
// replacement, and the raw guest draw-state fields below remain exact.
constexpr uint32_t kCompositeRenderPassKey = 0x0000000C;
constexpr uint32_t kCompositeHostSampleCount = 1;
constexpr uint32_t kGameplaySurfacePitch = 1280;
constexpr uint32_t kNormalizedDepthControl = 0x00700736;
constexpr uint32_t kNormalizedColorMask = 0x0000000F;
constexpr uint32_t kColorControl = 0x8700000C;
constexpr uint32_t kBlendControl0 = 0x07060706;
constexpr uint32_t kRasterCullBack = 0x00018002;
constexpr uint32_t kRasterCullFront = 0x00018006;
constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kPrimaryVertexFetchSlot = 95;
constexpr uint32_t kPaletteVertexFetchSlot = 92;
constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kPaletteRecordStride = 28;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kMaximumPaletteBytes = 64 * 1024;
// Structural admission is intentionally broader than the eventual 2AC
// family. Live frames can contain more than 600 compatible title draws before
// the independent backend hash stream selects its ordered subsequence.
constexpr size_t kMaximumTitleCandidates = 2048;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumQueuedBackendEvents = 1024;

struct RawFetch {
  uint32_t slot = 0;
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
  uint32_t endian = 0;
  std::array<uint32_t, 2> words{};
  bool valid = false;
};

struct BackendEvent {
  uint64_t frame_sequence = 0;
  Player2ACDrawIdentity identity{};
  Player2ACTitleKind expected_kind = Player2ACTitleKind::kUnknown;
  Player2ACBackendContract contract{};
};

enum class RejectionReason : uint8_t {
  kEmptyBackend,
  kEmptyTitle,
  kRasterPairs,
  kNoWindow,
  kAmbiguousWindow,
  kTitlePairs,
};

struct FrameLedger {
  uint64_t sequence = 0;
  bool closed = false;
  bool finalized = false;
  uint32_t dropped_title_candidate_count = 0;
  uint32_t guest_read_failures = 0;
  std::vector<Player2ACTitleDrawMetadata> title_candidates;
  std::vector<BackendEvent> backend_events;
};

std::mutex g_observer_mutex;
FrameLedger g_building_frame{.sequence = 1};
std::deque<FrameLedger> g_frames;
std::deque<BackendEvent> g_backend_events;
std::shared_ptr<const Player2ACFrameSnapshot> g_published_frame;
Player2ACObserverTelemetry g_telemetry;
uint64_t g_title_sequence = 1;
uint64_t g_latest_backend_sequence = 0;
bool g_announced_valid_frame = false;
bool g_announced_rejection = false;
bool g_announced_early_hash_callback = false;
bool g_announced_late_hash_callback = false;

uint32_t PhysicalAddressForVirtualAlias(uint32_t virtual_address) {
  if (virtual_address < kHighPhysicalHeapBase) {
    return 0;
  }
  const uint64_t physical_address =
      static_cast<uint64_t>(virtual_address - kHighPhysicalHeapBase) +
      kHighPhysicalHeapHostPageOffset;
  return physical_address <= kPhysicalAddressMask
             ? static_cast<uint32_t>(physical_address)
             : 0;
}

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t &result) {
  if (address == 0 || size == 0) {
    return false;
  }
  const uint64_t start = static_cast<uint64_t>(address) + offset;
  const uint64_t end = start + size;
  if (start > std::numeric_limits<uint32_t>::max() ||
      end > (uint64_t{1} << 32)) {
    return false;
  }
  result = static_cast<uint32_t>(start);
  return true;
}

bool TryCopyGuest(uint8_t *guest_base, uint32_t address, size_t offset,
                  void *destination, size_t size) {
  uint32_t guest_address = 0;
  return guest_base != nullptr && destination != nullptr &&
         CheckedGuestOffset(address, offset, size, guest_address) &&
         GuestTryCopy(destination,
                      guest_base + guest_address +
                          REX_PHYS_HOST_OFFSET(guest_address),
                      size);
}

uint32_t LoadBeU32(const std::byte *source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

RawFetch CaptureVertexFetch(uint8_t *guest_base, uint32_t device,
                            uint32_t slot) {
  RawFetch fetch;
  fetch.slot = slot;
  std::array<std::byte, sizeof(uint32_t) * 2> bytes{};
  if (!TryCopyGuest(guest_base, device,
                    kFetchBankOffset + slot * sizeof(uint32_t) * 2,
                    bytes.data(), bytes.size())) {
    return fetch;
  }
  for (size_t index = 0; index < fetch.words.size(); ++index) {
    fetch.words[index] = LoadBeU32(bytes.data() + index * sizeof(uint32_t));
  }
  fetch.physical_address = fetch.words[0] & ~uint32_t{3};
  fetch.endian = fetch.words[1] & 0x3u;
  fetch.byte_count = fetch.words[1] & 0x03FFFFFCu;
  fetch.valid = (fetch.words[0] & 0x3u) == 3 && fetch.physical_address != 0 &&
                fetch.byte_count != 0;
  return fetch;
}

Player2ACTitleKind KindForStride(uint32_t stride) {
  switch (stride) {
  case 32:
    return Player2ACTitleKind::kSingleStream32;
  case 96:
    return Player2ACTitleKind::kSingleStream96;
  case 36:
    return Player2ACTitleKind::kSkinned36;
  case 44:
    return Player2ACTitleKind::kSkinned44;
  default:
    return Player2ACTitleKind::kUnknown;
  }
}

Player2ACTitleKind KindForVertexShader(uint64_t hash) {
  switch (hash) {
  case kSingleStream32VertexShaderHash:
    return Player2ACTitleKind::kSingleStream32;
  case kSingleStream96VertexShaderHash:
    return Player2ACTitleKind::kSingleStream96;
  case kSkinned36VertexShaderHash:
    return Player2ACTitleKind::kSkinned36;
  case kSkinned44VertexShaderHash:
    return Player2ACTitleKind::kSkinned44;
  default:
    return Player2ACTitleKind::kUnknown;
  }
}

uint32_t ExpectedPrimitive(Player2ACTitleKind kind) {
  return kind == Player2ACTitleKind::kSingleStream96 ? kTriangleListPrimitive
                                                     : kTriangleStripPrimitive;
}

bool IsSingleStream(Player2ACTitleKind kind) {
  return kind == Player2ACTitleKind::kSingleStream32 ||
         kind == Player2ACTitleKind::kSingleStream96;
}

bool IsSkinned(Player2ACTitleKind kind) {
  return kind == Player2ACTitleKind::kSkinned36 ||
         kind == Player2ACTitleKind::kSkinned44;
}

bool SupportedDepthFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

bool IsTargetVertexShader(uint64_t hash) {
  return std::ranges::find(kVertexShaderHashes, hash) !=
         kVertexShaderHashes.end();
}

bool IsExactBackendDraw(const rex::graphics::NativeGuestDrawContext &context) {
  const Player2ACTitleKind kind =
      KindForVertexShader(context.vertex_shader_hash);
  const uint32_t expected_stride =
      kind == Player2ACTitleKind::kSingleStream32   ? 32
      : kind == Player2ACTitleKind::kSingleStream96 ? 96
      : kind == Player2ACTitleKind::kSkinned36      ? 36
                                                    : 44;
  const bool primary_fetch_matches =
      context.primary_vertex_fetch.valid &&
      context.primary_vertex_fetch.endian == kVertexEndian8In32 &&
      context.primary_vertex_fetch.byte_count % expected_stride == 0;
  const bool palette_fetch_matches =
      !IsSkinned(kind) ||
      (context.palette_vertex_fetch.valid &&
       context.palette_vertex_fetch.endian == kVertexEndian8In32 &&
       context.palette_vertex_fetch.byte_count % kPaletteRecordStride == 0);
  const bool raster_matches =
      IsSingleStream(kind)
          ? context.rasterizer_mode_control == kRasterCullBack ||
                context.rasterizer_mode_control == kRasterCullFront
          : IsSkinned(kind) &&
                context.rasterizer_mode_control == kRasterCullBack;
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.backend_frame_sequence != 0 && context.render_pass_key_valid &&
         context.render_pass_key == kCompositeRenderPassKey &&
         context.surface_pitch == kGameplaySurfacePitch && context.indexed &&
         context.guest_index_base_valid && context.guest_index_base != 0 &&
         context.draw_state_contract_valid &&
         context.rasterizer_mode_control_valid && raster_matches &&
         context.borrowed_attachment_contract_valid &&
         kind != Player2ACTitleKind::kUnknown && primary_fetch_matches &&
         palette_fetch_matches &&
         context.pixel_shader_hash == kPixelShaderHash &&
         context.primitive_type == ExpectedPrimitive(kind) &&
         context.guest_vertex_or_index_count != 0 &&
         !context.primitive_restart_enabled &&
         context.normalized_depth_control == kNormalizedDepthControl &&
         context.normalized_color_mask == kNormalizedColorMask &&
         context.color_control == kColorControl &&
         context.blend_control_0 == kBlendControl0 &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format == context.depth_attachment_format &&
         context.sample_count == kCompositeHostSampleCount &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

Player2ACBackendContract
CaptureBackendContract(const rex::graphics::NativeGuestDrawContext &context) {
  Player2ACBackendContract contract;
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
  for (size_t index = 0; index < contract.color_attachment_formats.size();
       ++index) {
    contract.color_attachment_formats[index] =
        static_cast<uint32_t>(context.color_attachment_formats[index]);
  }
  contract.color_attachment_count = context.color_attachment_count;
  contract.depth_attachment_format =
      static_cast<uint32_t>(context.depth_attachment_format);
  contract.stencil_attachment_format =
      static_cast<uint32_t>(context.stencil_attachment_format);
  contract.sample_count = context.sample_count;
  contract.sample_mask = context.sample_mask;
  contract.primary_vertex_fetch = {
      .physical_address = context.primary_vertex_fetch.physical_address,
      .byte_count = context.primary_vertex_fetch.byte_count,
      .endian = context.primary_vertex_fetch.endian,
      .valid = context.primary_vertex_fetch.valid,
  };
  contract.palette_vertex_fetch = {
      .physical_address = context.palette_vertex_fetch.physical_address,
      .byte_count = context.palette_vertex_fetch.byte_count,
      .endian = context.palette_vertex_fetch.endian,
      .valid = context.palette_vertex_fetch.valid,
  };
  contract.primitive_restart_enabled = context.primitive_restart_enabled;
  contract.rasterizer_mode_control_valid =
      context.rasterizer_mode_control_valid;
  contract.valid = IsExactBackendDraw(context);
  return contract;
}

bool SameContractExceptRaster(const Player2ACBackendContract &left,
                              const Player2ACBackendContract &right) {
  return left.valid && right.valid &&
         left.vertex_shader_hash == right.vertex_shader_hash &&
         left.pixel_shader_hash == right.pixel_shader_hash &&
         left.render_pass_key == right.render_pass_key &&
         left.surface_pitch == right.surface_pitch &&
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
         left.primary_vertex_fetch == right.primary_vertex_fetch &&
         left.palette_vertex_fetch == right.palette_vertex_fetch &&
         left.primitive_restart_enabled == right.primitive_restart_enabled &&
         left.rasterizer_mode_control_valid ==
             right.rasterizer_mode_control_valid;
}

bool MatchesBackend(const Player2ACTitleDrawMetadata &title,
                    const BackendEvent &backend) {
  const bool vertex_fetch_matches =
      title.vertices.valid && backend.contract.primary_vertex_fetch.valid &&
      title.vertices.physical_address ==
          backend.contract.primary_vertex_fetch.physical_address &&
      title.vertices.byte_count ==
          backend.contract.primary_vertex_fetch.byte_count &&
      title.vertices.endian == backend.contract.primary_vertex_fetch.endian;
  const bool palette_fetch_matches =
      !IsSkinned(title.kind) ||
      (title.palette.valid && backend.contract.palette_vertex_fetch.valid &&
       title.palette.physical_address ==
           backend.contract.palette_vertex_fetch.physical_address &&
       title.palette.byte_count ==
           backend.contract.palette_vertex_fetch.byte_count &&
       title.palette.endian == backend.contract.palette_vertex_fetch.endian);
  return title.valid && backend.contract.valid && vertex_fetch_matches &&
         palette_fetch_matches && title.program.bound_pixel_shader_valid &&
         title.program.bound_pixel_shader_hash ==
             backend.contract.pixel_shader_hash &&
         title.kind == backend.expected_kind &&
         title.identity == backend.identity;
}

bool ValidateSelectedTitlePairs(
    const std::vector<Player2ACTitleDrawMetadata> &titles,
    const std::vector<size_t> &selected,
    const std::vector<BackendEvent> &events) {
  for (size_t index = 0; index < events.size(); ++index) {
    if (!IsSingleStream(events[index].expected_kind)) {
      continue;
    }
    if (index + 1 >= events.size() || index + 1 >= selected.size() ||
        !(titles[selected[index]].identity ==
          titles[selected[index + 1]].identity) ||
        titles[selected[index]].program.bound_pixel_shader_hash !=
            titles[selected[index + 1]].program.bound_pixel_shader_hash) {
      return false;
    }
    ++index;
  }
  return true;
}

void UpdatePendingCountsLocked() {
  g_telemetry.pending_frames = static_cast<uint32_t>(
      std::ranges::count_if(g_frames, [](const FrameLedger &frame) {
        return frame.closed && !frame.finalized &&
               !frame.title_candidates.empty();
      }));
  g_telemetry.queued_backend_events =
      static_cast<uint32_t>(g_backend_events.size());
}

void ReconcileBackendEventsLocked() {
  auto event = g_backend_events.begin();
  while (event != g_backend_events.end()) {
    const auto frame =
        std::ranges::find_if(g_frames, [&](const FrameLedger &candidate) {
          return !candidate.finalized &&
                 candidate.sequence == event->frame_sequence;
        });
    if (frame != g_frames.end()) {
      frame->backend_events.push_back(std::move(*event));
      event = g_backend_events.erase(event);
    } else if (event->frame_sequence < g_title_sequence) {
      event = g_backend_events.erase(event);
    } else {
      ++event;
    }
  }
}

bool ValidateRasterPairs(const std::vector<BackendEvent> &events) {
  for (size_t index = 0; index < events.size(); ++index) {
    const BackendEvent &event = events[index];
    if (!IsSingleStream(event.expected_kind)) {
      if (!IsSkinned(event.expected_kind) ||
          event.contract.rasterizer_mode_control != kRasterCullBack) {
        return false;
      }
      continue;
    }
    if (index + 1 >= events.size()) {
      return false;
    }
    const BackendEvent &paired = events[index + 1];
    if (paired.expected_kind != event.expected_kind ||
        !(paired.identity == event.identity) ||
        event.contract.rasterizer_mode_control != kRasterCullBack ||
        paired.contract.rasterizer_mode_control != kRasterCullFront ||
        !SameContractExceptRaster(event.contract, paired.contract)) {
      return false;
    }
    ++index;
  }
  return true;
}

std::vector<size_t>
FindEarliestOrderedMatch(const std::vector<Player2ACTitleDrawMetadata> &titles,
                         const std::vector<BackendEvent> &events,
                         size_t &matched_prefix) {
  std::vector<size_t> selected;
  selected.reserve(events.size());
  size_t candidate = 0;
  for (const BackendEvent &event : events) {
    while (candidate < titles.size() &&
           !MatchesBackend(titles[candidate], event)) {
      ++candidate;
    }
    if (candidate == titles.size()) {
      matched_prefix = selected.size();
      return {};
    }
    selected.push_back(candidate++);
  }
  matched_prefix = selected.size();
  return selected;
}

std::vector<size_t>
FindLatestOrderedMatch(const std::vector<Player2ACTitleDrawMetadata> &titles,
                       const std::vector<BackendEvent> &events) {
  std::vector<size_t> selected(events.size());
  size_t candidate = titles.size();
  for (size_t event_index = events.size(); event_index != 0; --event_index) {
    bool found = false;
    while (candidate != 0) {
      --candidate;
      if (MatchesBackend(titles[candidate], events[event_index - 1])) {
        selected[event_index - 1] = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      return {};
    }
  }
  return selected;
}

void LogNoWindowDiagnostic(
    const std::vector<Player2ACTitleDrawMetadata> &titles,
    const std::vector<BackendEvent> &events) {
  if (g_announced_rejection || events.empty()) {
    return;
  }
  const BackendEvent &backend = events.front();
  uint32_t identity_kind_matches = 0;
  uint32_t primary_fetch_matches = 0;
  uint32_t palette_fetch_matches = 0;
  uint32_t vertex_shader_matches = 0;
  uint32_t pixel_shader_matches = 0;
  const Player2ACTitleDrawMetadata *representative = nullptr;
  for (const Player2ACTitleDrawMetadata &title : titles) {
    if (!title.valid || title.kind != backend.expected_kind ||
        !(title.identity == backend.identity)) {
      continue;
    }
    ++identity_kind_matches;
    if (representative == nullptr) {
      representative = &title;
    }
    const bool primary_matches =
        title.vertices.valid && backend.contract.primary_vertex_fetch.valid &&
        title.vertices.physical_address ==
            backend.contract.primary_vertex_fetch.physical_address &&
        title.vertices.byte_count ==
            backend.contract.primary_vertex_fetch.byte_count &&
        title.vertices.endian == backend.contract.primary_vertex_fetch.endian;
    if (!primary_matches) {
      continue;
    }
    ++primary_fetch_matches;
    const bool palette_matches =
        !IsSkinned(title.kind) ||
        (title.palette.valid && backend.contract.palette_vertex_fetch.valid &&
         title.palette.physical_address ==
             backend.contract.palette_vertex_fetch.physical_address &&
         title.palette.byte_count ==
             backend.contract.palette_vertex_fetch.byte_count &&
         title.palette.endian == backend.contract.palette_vertex_fetch.endian);
    if (!palette_matches) {
      continue;
    }
    ++palette_fetch_matches;
    if (!title.program.bound_pixel_shader_valid) {
      continue;
    }
    ++vertex_shader_matches;
    if (title.program.bound_pixel_shader_hash !=
        backend.contract.pixel_shader_hash) {
      continue;
    }
    ++pixel_shader_matches;
  }
  const Player2ACTitleDrawMetadata empty{};
  const Player2ACTitleDrawMetadata &title =
      representative != nullptr ? *representative : empty;
  REXLOG_INFO(
      "Table Tennis 2AC no-window diagnostic: event_kind={} "
      "identity={}/{}/{:08X} backend_vs={:016X} backend_ps={:016X} "
      "backend_vf95={:08X}/{} backend_vf92={:08X}/{} "
      "matches=identity:{} primary:{} palette:{} vs:{} ps:{} "
      "representative_ordinal={} pass_vs={:016X} pass_ps={:016X} "
      "bound_vs={:016X} bound_ps={:016X} "
      "title_vf95={:08X}/{} title_vf92={:08X}/{}",
      static_cast<uint32_t>(backend.expected_kind),
      backend.identity.primitive_type, backend.identity.submitted_index_count,
      backend.identity.guest_index_base, backend.contract.vertex_shader_hash,
      backend.contract.pixel_shader_hash,
      backend.contract.primary_vertex_fetch.physical_address,
      backend.contract.primary_vertex_fetch.byte_count,
      backend.contract.palette_vertex_fetch.physical_address,
      backend.contract.palette_vertex_fetch.byte_count, identity_kind_matches,
      primary_fetch_matches, palette_fetch_matches, vertex_shader_matches,
      pixel_shader_matches, title.ordinal, title.program.vertex_shader_hash,
      title.program.pixel_shader_hash, title.program.bound_vertex_shader_hash,
      title.program.bound_pixel_shader_hash, title.vertices.physical_address,
      title.vertices.byte_count, title.palette.physical_address,
      title.palette.byte_count);
}

const char *RejectionReasonName(RejectionReason reason) {
  switch (reason) {
  case RejectionReason::kEmptyBackend:
    return "empty-backend";
  case RejectionReason::kEmptyTitle:
    return "empty-title";
  case RejectionReason::kRasterPairs:
    return "raster-pairs";
  case RejectionReason::kNoWindow:
    return "no-window";
  case RejectionReason::kAmbiguousWindow:
    return "ambiguous-window";
  case RejectionReason::kTitlePairs:
    return "title-pairs";
  }
  return "unknown";
}

void AddPaletteGroup(Player2ACFrameSnapshot &snapshot,
                     const Player2ACTitleDrawMetadata &title) {
  if (!IsSkinned(title.kind) || !title.palette.valid) {
    return;
  }
  const auto existing = std::ranges::find_if(
      snapshot.palette_groups,
      [&](const Player2ACPaletteOwnershipGroup &group) {
        return group.physical_address == title.palette.physical_address &&
               group.byte_count == title.palette.byte_count &&
               group.payload_fingerprint == title.palette.payload_fingerprint;
      });
  if (existing != snapshot.palette_groups.end()) {
    ++existing->draw_count;
    if (existing->player == 0) {
      existing->player = title.player;
    }
    return;
  }
  snapshot.palette_groups.push_back({
      .player = title.player,
      .physical_address = title.palette.physical_address,
      .byte_count = title.palette.byte_count,
      .record_count = title.palette.record_count,
      .payload_fingerprint = title.palette.payload_fingerprint,
      .draw_count = 1,
  });
}

void PublishRejectedFrameLocked(FrameLedger &frame, RejectionReason reason,
                                uint32_t ambiguity, uint32_t mismatches,
                                size_t longest_prefix = 0) {
  auto published = std::make_shared<Player2ACFrameSnapshot>();
  published->sequence = frame.sequence;
  published->backend_frame_sequence =
      frame.backend_events.empty()
          ? 0
          : frame.backend_events.front().frame_sequence;
  published->title_candidate_count =
      static_cast<uint32_t>(frame.title_candidates.size());
  published->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  published->dropped_title_candidate_count =
      frame.dropped_title_candidate_count;
  published->guest_read_failures = frame.guest_read_failures;
  published->ambiguous_match_count = ambiguity;
  published->sequence_mismatch_count = mismatches;
  g_published_frame = std::move(published);
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  g_telemetry.ambiguous_frames += ambiguity != 0;
  g_telemetry.sequence_mismatches += mismatches;
  g_telemetry.latest_published_sequence = frame.sequence;
  if (!g_announced_rejection) {
    g_announced_rejection = true;
    std::array<uint32_t, 4> event_kind_counts{};
    for (const BackendEvent &event : frame.backend_events) {
      const size_t kind = static_cast<size_t>(event.expected_kind);
      if (kind >= 1 && kind <= event_kind_counts.size()) {
        ++event_kind_counts[kind - 1];
      }
    }
    REXLOG_INFO(
        "Table Tennis 2AC observer: rejected frame={} title_candidates={} "
        "backend_events={} ambiguity={} sequence_mismatches={} dropped={} "
        "reads={} reason={} longest_prefix={} "
        "backend_kinds=32:{}/96:{}/36:{}/44:{} "
        "observer_only=true guest_suppressed=false",
        frame.sequence, frame.title_candidates.size(),
        frame.backend_events.size(), ambiguity, mismatches,
        frame.dropped_title_candidate_count, frame.guest_read_failures,
        RejectionReasonName(reason), longest_prefix, event_kind_counts[0],
        event_kind_counts[1], event_kind_counts[2], event_kind_counts[3]);
  }
  // Rejected frames retain only their value summary. Drop all provisional
  // payload ownership immediately instead of keeping it for the archive's
  // full eight-frame lifetime.
  frame.title_candidates.clear();
  frame.title_candidates.shrink_to_fit();
  frame.backend_events.clear();
  frame.backend_events.shrink_to_fit();
}

void AnalyzeFrameLocked(FrameLedger &frame) {
  if (frame.backend_events.empty()) {
    PublishRejectedFrameLocked(frame, RejectionReason::kEmptyBackend, 0, 1);
    return;
  }
  if (frame.title_candidates.empty()) {
    PublishRejectedFrameLocked(frame, RejectionReason::kEmptyTitle, 0, 1);
    return;
  }
  if (!ValidateRasterPairs(frame.backend_events)) {
    PublishRejectedFrameLocked(frame, RejectionReason::kRasterPairs, 0, 1);
    return;
  }

  size_t matched_prefix = 0;
  const std::vector<size_t> earliest = FindEarliestOrderedMatch(
      frame.title_candidates, frame.backend_events, matched_prefix);
  if (earliest.empty()) {
    LogNoWindowDiagnostic(frame.title_candidates, frame.backend_events);
    PublishRejectedFrameLocked(frame, RejectionReason::kNoWindow, 0, 1,
                               matched_prefix);
    return;
  }

  const std::vector<size_t> latest =
      FindLatestOrderedMatch(frame.title_candidates, frame.backend_events);
  if (latest.empty() || earliest != latest) {
    PublishRejectedFrameLocked(frame, RejectionReason::kAmbiguousWindow, 1, 0,
                               frame.backend_events.size());
    return;
  }

  if (!ValidateSelectedTitlePairs(frame.title_candidates, earliest,
                                  frame.backend_events)) {
    PublishRejectedFrameLocked(frame, RejectionReason::kTitlePairs, 0, 1,
                               frame.backend_events.size());
    return;
  }
  auto published = std::make_shared<Player2ACFrameSnapshot>();
  published->sequence = frame.sequence;
  published->backend_frame_sequence =
      frame.backend_events.front().frame_sequence;
  published->title_candidate_count =
      static_cast<uint32_t>(frame.title_candidates.size());
  published->logical_draw_count =
      static_cast<uint32_t>(frame.backend_events.size());
  published->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  published->unmatched_title_candidate_count = static_cast<uint32_t>(
      frame.title_candidates.size() - frame.backend_events.size());
  published->dropped_title_candidate_count =
      frame.dropped_title_candidate_count;
  published->guest_read_failures = frame.guest_read_failures;
  published->draws.reserve(frame.backend_events.size());

  for (size_t index = 0; index < frame.backend_events.size(); ++index) {
    const Player2ACTitleDrawMetadata &title =
        frame.title_candidates[earliest[index]];
    const BackendEvent &backend = frame.backend_events[index];
    Player2ACDrawProof proof;
    proof.title = title;
    proof.backend = backend.contract;
    proof.backend_occurrence_count = 1;
    proof.rasterizer_modes[0] = backend.contract.rasterizer_mode_control;
    published->total_index_count += title.identity.submitted_index_count;
    AddPaletteGroup(*published, title);
    published->draws.push_back(std::move(proof));
  }

  g_published_frame = std::move(published);
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  g_telemetry.latest_published_sequence = frame.sequence;
  if (g_published_frame->valid()) {
    ++g_telemetry.valid_frames;
    if (!g_announced_valid_frame) {
      g_announced_valid_frame = true;
      REXLOG_INFO(
          "Table Tennis 2AC observer: published immutable frame={} "
          "draws={} indices={} palette_groups={} title_candidates={} "
          "unmatched={} backend_frame={} exact_state=true exact_order=true "
          "observer_only=true guest_suppressed=false",
          g_published_frame->sequence, g_published_frame->draws.size(),
          g_published_frame->total_index_count,
          g_published_frame->palette_groups.size(),
          g_published_frame->title_candidate_count,
          g_published_frame->unmatched_title_candidate_count,
          g_published_frame->backend_frame_sequence);
    }
  } else {
    ++g_telemetry.sequence_mismatches;
    if (!g_announced_rejection) {
      g_announced_rejection = true;
      REXLOG_INFO("Table Tennis 2AC observer: invalid joined frame={} draws={} "
                  "palettes={} dropped={} reads={} observer_only=true "
                  "guest_suppressed=false",
                  g_published_frame->sequence, g_published_frame->draws.size(),
                  g_published_frame->palette_groups.size(),
                  g_published_frame->dropped_title_candidate_count,
                  g_published_frame->guest_read_failures);
    }
  }
  // The immutable published snapshot owns only the uniquely joined draws.
  // Provisional candidates must not multiply retained payload memory across
  // the delayed frame archive.
  frame.title_candidates.clear();
  frame.title_candidates.shrink_to_fit();
  frame.backend_events.clear();
  frame.backend_events.shrink_to_fit();
}

void AnalyzeCompletedFramesLocked() {
  for (FrameLedger &frame : g_frames) {
    if (frame.closed && !frame.finalized &&
        frame.sequence < g_latest_backend_sequence) {
      AnalyzeFrameLocked(frame);
    }
  }
}

void ExpireFramesLocked() {
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized &&
        !g_frames.front().title_candidates.empty()) {
      ++g_telemetry.expired_frames;
    }
    g_frames.pop_front();
  }
}

} // namespace

bool Player2ACDrawProof::valid() const {
  if (!title.valid || !title.identity.valid() || !title.program.valid() ||
      !title.vertices.valid || title.payload == nullptr ||
      !title.payload->valid() || !backend.valid ||
      backend_occurrence_count != 1 ||
      rasterizer_modes[0] != backend.rasterizer_mode_control ||
      backend.pixel_shader_hash != kPixelShaderHash ||
      KindForVertexShader(backend.vertex_shader_hash) != title.kind) {
    return false;
  }
  if (title.payload->vertices->fetch.physical_address !=
          title.vertices.physical_address ||
      title.payload->vertices->fetch.byte_count != title.vertices.byte_count ||
      title.payload->vertices->fetch.endian != title.vertices.endian ||
      title.payload->vertices->fetch.stride != title.vertices.stride ||
      title.payload->vertices->payload_fingerprint !=
          title.vertices.payload_fingerprint ||
      title.payload->indices->physical_address !=
          title.identity.guest_index_base ||
      title.payload->indices->submitted_index_count !=
          title.identity.submitted_index_count) {
    return false;
  }
  if (IsSkinned(title.kind)) {
    return title.palette.valid &&
           title.palette.stride == kPaletteRecordStride &&
           title.payload->palette_required &&
           title.payload->palette != nullptr &&
           title.payload->palette->fetch.physical_address ==
               title.palette.physical_address &&
           title.payload->palette->fetch.byte_count ==
               title.palette.byte_count &&
           title.payload->palette->fetch.endian == title.palette.endian &&
           title.payload->palette->payload_fingerprint ==
               title.palette.payload_fingerprint;
  }
  return IsSingleStream(title.kind) && !title.palette.valid &&
         !title.payload->palette_required && title.payload->palette == nullptr;
}

bool Player2ACFrameSnapshot::valid() const {
  if (sequence == 0 || backend_frame_sequence != sequence ||
      title_candidate_count == 0 || logical_draw_count == 0 ||
      backend_event_count != logical_draw_count ||
      draws.size() != logical_draw_count || total_index_count == 0 ||
      dropped_title_candidate_count != 0 || guest_read_failures != 0 ||
      ambiguous_match_count != 0 || sequence_mismatch_count != 0 ||
      unmatched_title_candidate_count + logical_draw_count !=
          title_candidate_count) {
    return false;
  }
  uint32_t skinned_draw_count = 0;
  for (size_t draw_index = 0; draw_index < draws.size(); ++draw_index) {
    const Player2ACDrawProof &draw = draws[draw_index];
    if (!draw.valid()) {
      return false;
    }
    if (IsSkinned(draw.title.kind)) {
      ++skinned_draw_count;
      if (draw.backend.rasterizer_mode_control != kRasterCullBack) {
        return false;
      }
      continue;
    }
    if (draw_index + 1 >= draws.size()) {
      return false;
    }
    const Player2ACDrawProof &paired = draws[draw_index + 1];
    if (!paired.valid() || paired.title.kind != draw.title.kind ||
        !(paired.title.identity == draw.title.identity) ||
        paired.title.program.bound_pixel_shader_hash !=
            draw.title.program.bound_pixel_shader_hash ||
        draw.backend.rasterizer_mode_control != kRasterCullBack ||
        paired.backend.rasterizer_mode_control != kRasterCullFront ||
        !SameContractExceptRaster(draw.backend, paired.backend)) {
      return false;
    }
    ++draw_index;
  }
  uint32_t grouped_draw_count = 0;
  for (const Player2ACPaletteOwnershipGroup &group : palette_groups) {
    if (!group.valid()) {
      return false;
    }
    grouped_draw_count += group.draw_count;
  }
  if (grouped_draw_count != skinned_draw_count) {
    return false;
  }
  return true;
}

bool Player2ACObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_player_2ac_observer) ||
         NativeFrameSceneCaptureEnabled() ||
         Player2ACCompositeObserverEnabled();
}

void ObservePlayer2ACTitleDraw(uint8_t *guest_base,
                               const SceneCatalogDrawOccurrence &draw) {
  if (!Player2ACObserverEnabled()) {
    return;
  }
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.title_draws_observed;
  }
  if (guest_base == nullptr || !draw.pass.valid || !draw.mesh.valid ||
      draw.frame_sequence == 0 || !draw.bound_shaders.pixel_shader_valid ||
      draw.bound_shaders.pixel_shader_hash != kPixelShaderHash ||
      draw.pass.pass_descriptor == 0 || draw.pass.program_pair == 0 ||
      draw.pass.vertex_shader == 0 || draw.pass.pixel_shader == 0 ||
      !draw.pass.shader_fingerprints_valid || draw.submitted_index_count == 0 ||
      draw.mesh.index_element_size != sizeof(uint16_t) ||
      draw.mesh.index_is_32_bit) {
    return;
  }
  {
    std::lock_guard lock(g_observer_mutex);
    if (g_building_frame.title_candidates.empty()) {
      g_building_frame.sequence = draw.frame_sequence;
      g_title_sequence = draw.frame_sequence;
    } else if (g_building_frame.sequence != draw.frame_sequence) {
      ++g_telemetry.sequence_mismatches;
      return;
    }
  }

  const Player2ACTitleKind kind = KindForStride(draw.mesh.vertex_stride);
  if (kind == Player2ACTitleKind::kUnknown ||
      draw.primitive_type != ExpectedPrimitive(kind) ||
      draw.mesh.aggregate_primitive_type != ExpectedPrimitive(kind)) {
    return;
  }
  const uint64_t requested_index_bytes =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  const uint32_t index_base =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (index_base == 0 || requested_index_bytes == 0 ||
      requested_index_bytes > draw.mesh.index_buffer_bytes) {
    return;
  }

  const RawFetch vertices =
      CaptureVertexFetch(guest_base, draw.device, kPrimaryVertexFetchSlot);
  uint32_t read_failures = !vertices.valid;
  const bool vertices_valid =
      vertices.valid && vertices.endian == kVertexEndian8In32 &&
      vertices.byte_count % draw.mesh.vertex_stride == 0 &&
      vertices.physical_address ==
          PhysicalAddressForVirtualAlias(draw.mesh.vertex_buffer_alias);
  read_failures += !vertices_valid;

  RawFetch palette;
  if (IsSkinned(kind)) {
    palette =
        CaptureVertexFetch(guest_base, draw.device, kPaletteVertexFetchSlot);
    const bool descriptor_valid =
        palette.valid && palette.endian == kVertexEndian8In32 &&
        palette.byte_count % kPaletteRecordStride == 0 &&
        palette.byte_count <= kMaximumPaletteBytes;
    read_failures += !descriptor_valid;
  }
  if (!vertices_valid || read_failures != 0) {
    std::lock_guard lock(g_observer_mutex);
    g_telemetry.title_guest_read_failures += read_failures;
    g_building_frame.guest_read_failures += read_failures;
    return;
  }

  // This is deliberately after the actual low-level bound-PS gate and exact
  // scalar fetch/index admission above. Superset candidates that do not bind
  // 2AC never cause large guest payload copies.
  const Player2ACPayloadCapture payload_capture = CapturePlayer2ACDrawPayload(
      guest_base, draw,
      {
          .physical_address = vertices.physical_address,
          .byte_count = vertices.byte_count,
          .endian = vertices.endian,
          .stride = draw.mesh.vertex_stride,
          .valid = vertices.valid,
      },
      {
          .physical_address = palette.physical_address,
          .byte_count = palette.byte_count,
          .endian = palette.endian,
          .stride = kPaletteRecordStride,
          .valid = palette.valid,
      },
      IsSkinned(kind));
  if (!payload_capture.valid()) {
    std::lock_guard lock(g_observer_mutex);
    g_telemetry.title_guest_read_failures += payload_capture.copy_failures;
    g_building_frame.guest_read_failures += payload_capture.copy_failures;
    return;
  }

  Player2ACTitleDrawMetadata candidate;
  candidate.identity = {
      .primitive_type = draw.primitive_type,
      .submitted_index_count = draw.submitted_index_count,
      .guest_index_base = index_base,
  };
  candidate.program = {
      .pass_descriptor = draw.pass.pass_descriptor,
      .program_pair = draw.pass.program_pair,
      .vertex_shader = draw.pass.vertex_shader,
      .pixel_shader = draw.pass.pixel_shader,
      .vertex_shader_hash = draw.pass.vertex_shader_hash,
      .pixel_shader_hash = draw.pass.pixel_shader_hash,
      .bound_vertex_shader = draw.bound_shaders.vertex_shader,
      .bound_vertex_shader_hash = draw.bound_shaders.vertex_shader_hash,
      .bound_pixel_shader = draw.bound_shaders.pixel_shader,
      .bound_pixel_shader_hash = draw.bound_shaders.pixel_shader_hash,
      .shader_fingerprints_valid = draw.pass.shader_fingerprints_valid,
      .bound_vertex_shader_valid = draw.bound_shaders.vertex_shader_valid,
      .bound_pixel_shader_valid = draw.bound_shaders.pixel_shader_valid,
  };
  candidate.vertices = {
      .slot = kPrimaryVertexFetchSlot,
      .physical_address = vertices.physical_address,
      .byte_count = vertices.byte_count,
      .endian = vertices.endian,
      .stride = draw.mesh.vertex_stride,
      .record_count = vertices.byte_count / draw.mesh.vertex_stride,
      .payload_fingerprint =
          payload_capture.payload->vertices->payload_fingerprint,
      .valid = true,
  };
  if (IsSkinned(kind)) {
    candidate.palette = {
        .slot = kPaletteVertexFetchSlot,
        .physical_address = palette.physical_address,
        .byte_count = palette.byte_count,
        .endian = palette.endian,
        .stride = kPaletteRecordStride,
        .record_count = palette.byte_count / kPaletteRecordStride,
        .payload_fingerprint =
            payload_capture.payload->palette->payload_fingerprint,
        .valid = true,
    };
  }
  candidate.payload = payload_capture.payload;
  candidate.kind = kind;
  candidate.ordinal = draw.ordinal;
  candidate.player = draw.player;
  candidate.owner_kind = static_cast<uint32_t>(draw.owner.kind);
  candidate.owner = draw.owner.owner;
  candidate.valid = candidate.identity.valid() && candidate.program.valid() &&
                    candidate.vertices.valid && candidate.payload != nullptr &&
                    candidate.payload->valid() &&
                    (IsSingleStream(kind) || candidate.palette.valid);

  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.title_structural_candidates;
  g_telemetry.title_single_stream_32 +=
      kind == Player2ACTitleKind::kSingleStream32;
  g_telemetry.title_single_stream_96 +=
      kind == Player2ACTitleKind::kSingleStream96;
  g_telemetry.title_skinned_36 += kind == Player2ACTitleKind::kSkinned36;
  g_telemetry.title_skinned_44 += kind == Player2ACTitleKind::kSkinned44;
  if (g_building_frame.title_candidates.size() == kMaximumTitleCandidates) {
    ++g_building_frame.dropped_title_candidate_count;
    ++g_telemetry.title_candidates_dropped;
    return;
  }
  g_building_frame.title_candidates.push_back(std::move(candidate));
}

void ObservePlayer2ACBackendDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!Player2ACObserverEnabled()) {
    return;
  }
  const bool pixel_matches = context.pixel_shader_hash == kPixelShaderHash;
  const bool vertex_matches =
      pixel_matches && IsTargetVertexShader(context.vertex_shader_hash);
  const bool exact = IsExactBackendDraw(context);

  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.backend_draws_observed;
  g_telemetry.backend_pixel_hash_matches += pixel_matches;
  g_telemetry.backend_vertex_hash_matches += vertex_matches;
  g_telemetry.backend_contract_matches += exact;
  g_latest_backend_sequence =
      std::max(g_latest_backend_sequence, context.backend_frame_sequence);
  g_telemetry.latest_backend_sequence = g_latest_backend_sequence;
  bool &announced_hash_callback = context.render_pass_key_valid
                                      ? g_announced_late_hash_callback
                                      : g_announced_early_hash_callback;
  if (pixel_matches && !announced_hash_callback) {
    announced_hash_callback = true;
    REXLOG_INFO(
        "Table Tennis 2AC backend hash callback: stage={} frame={} "
        "vs={:016X} target_vs={} primitive={} guest_count={} host_count={} "
        "index={:08X}/{} pitch={} state={} depth={:08X} mask={:08X} "
        "color={:08X} blend={:08X} raster={:08X}/{} restart={} "
        "pass={:08X}/{} attachments={} colors={} color0={} depth_fmt={} "
        "stencil_fmt={} samples={} sample_mask={:016X} exact={} "
        "observer_only=true guest_suppressed=false",
        context.render_pass_key_valid ? "late" : "early",
        context.backend_frame_sequence, context.vertex_shader_hash,
        vertex_matches, context.primitive_type,
        context.guest_vertex_or_index_count, context.vertex_or_index_count,
        context.guest_index_base, context.guest_index_base_valid,
        context.surface_pitch, context.draw_state_contract_valid,
        context.normalized_depth_control, context.normalized_color_mask,
        context.color_control, context.blend_control_0,
        context.rasterizer_mode_control, context.rasterizer_mode_control_valid,
        context.primitive_restart_enabled, context.render_pass_key,
        context.render_pass_key_valid,
        context.borrowed_attachment_contract_valid,
        context.color_attachment_count,
        static_cast<uint32_t>(context.color_attachment_formats[0]),
        static_cast<uint32_t>(context.depth_attachment_format),
        static_cast<uint32_t>(context.stencil_attachment_format),
        context.sample_count, context.sample_mask, exact);
  }
  if (!exact) {
    AnalyzeCompletedFramesLocked();
    return;
  }

  BackendEvent event = {
      .frame_sequence = context.backend_frame_sequence,
      .identity =
          {
              .primitive_type = context.primitive_type,
              .submitted_index_count = context.guest_vertex_or_index_count,
              .guest_index_base = context.guest_index_base,
          },
      .expected_kind = KindForVertexShader(context.vertex_shader_hash),
      .contract = CaptureBackendContract(context),
  };
  ++g_telemetry.backend_events;
  if (g_backend_events.size() == kMaximumQueuedBackendEvents) {
    g_backend_events.pop_front();
    ++g_telemetry.backend_events_dropped;
  }
  g_backend_events.push_back(std::move(event));
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  UpdatePendingCountsLocked();
}

void Player2ACObserverFrameEnd() {
  const bool enabled = Player2ACObserverEnabled();
  Player2ACPayloadFrameEnd();
  std::lock_guard lock(g_observer_mutex);
  if (!enabled) {
    g_building_frame = FrameLedger{.sequence = 1};
    g_frames.clear();
    g_backend_events.clear();
    g_published_frame.reset();
    g_telemetry = {};
    g_title_sequence = 1;
    g_latest_backend_sequence = 0;
    g_announced_valid_frame = false;
    g_announced_rejection = false;
    g_announced_early_hash_callback = false;
    g_announced_late_hash_callback = false;
    return;
  }

  ++g_telemetry.title_frames;
  g_telemetry.latest_title_sequence = g_title_sequence;
  g_building_frame.sequence = g_title_sequence;
  g_building_frame.closed = true;
  if (!g_building_frame.title_candidates.empty() ||
      g_building_frame.dropped_title_candidate_count != 0 ||
      g_building_frame.guest_read_failures != 0) {
    g_frames.push_back(std::move(g_building_frame));
  }
  ++g_title_sequence;
  g_building_frame = FrameLedger{.sequence = g_title_sequence};
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  ExpireFramesLocked();
  UpdatePendingCountsLocked();
}

std::shared_ptr<const Player2ACFrameSnapshot> LatestPlayer2ACFrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

Player2ACObserverTelemetry LatestPlayer2ACObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  return g_telemetry;
}

} // namespace tabletennis::native
