#include "native/tabletennis_player_skin_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_player_observer_renderer.h"
#include "native/tabletennis_player_replacement_candidates.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_scene_draw_catalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_skin_observer, false, "Table Tennis",
    "Capture the proven live player vertex, packed palette and material "
    "contract. Observer-only; never suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kVertexConstantBankOffset = 0x780;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr uint32_t kPrimaryVertexFetchSlot = 95;
constexpr uint32_t kPaletteVertexFetchSlot = 92;
constexpr uint32_t kPlayerVertexStride = 44;
constexpr uint32_t kPaletteRecordStride = 28;
constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kTextureViewSwizzleShift = 1;
constexpr uint32_t kTextureViewSwizzleMask = 0xFFF;
constexpr uint32_t kExpectedTextureViewSwizzle = 0x688;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr size_t kConstantRowBytes = sizeof(float) * 4;
constexpr std::array<uint32_t, 4> kInfluenceByteOrder = {2, 1, 0, 3};
constexpr std::array<float, 4> kExpectedPixelConstant254 = {
    2.0f, -1.0f, -0.5f, 0.3f};
constexpr std::array<float, 4> kExpectedPixelConstant255 = {
    3.0f, 1.0f, 10.0f / 13.0f, 4.0f};

std::mutex g_observer_mutex;
PlayerSkinFrameObservation g_building_frame;
PlayerSkinFrameObservation g_published_frame;
uint64_t g_frame_sequence = 0;
bool g_announced = false;
bool g_announced_rejection = false;

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t& result) {
  if (address == 0) {
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

bool TryCopyGuest(uint8_t* guest_base, uint32_t address, size_t offset,
                  void* destination, size_t size) {
  uint32_t guest_address = 0;
  return guest_base != nullptr &&
         CheckedGuestOffset(address, offset, size, guest_address) &&
         GuestTryCopy(
             destination,
             guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address),
             size);
}

uint32_t PhysicalAddressForVirtualAlias(uint32_t virtual_address) {
  if (virtual_address < kHighPhysicalHeapBase) {
    return 0;
  }
  return virtual_address - kHighPhysicalHeapBase +
         kHighPhysicalHeapHostPageOffset;
}

uint32_t LoadBeU32(const std::byte* source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const std::byte* source) {
  return std::bit_cast<float>(LoadBeU32(source));
}

template <size_t Size>
bool CaptureBeWords(uint8_t* guest_base, uint32_t address, size_t offset,
                    std::array<uint32_t, Size>& words) {
  std::array<std::byte, Size * sizeof(uint32_t)> bytes;
  if (!TryCopyGuest(guest_base, address, offset, bytes.data(),
                    bytes.size())) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    words[index] =
        LoadBeU32(bytes.data() + index * sizeof(uint32_t));
  }
  return true;
}

template <size_t Size>
bool CaptureBeFloats(uint8_t* guest_base, uint32_t address, size_t offset,
                     std::array<float, Size>& values) {
  std::array<uint32_t, Size> words;
  if (!CaptureBeWords(guest_base, address, offset, words)) {
    return false;
  }
  for (size_t index = 0; index < Size; ++index) {
    values[index] = std::bit_cast<float>(words[index]);
  }
  return true;
}

PlayerSkinVertexFetchObservation CaptureVertexFetch(
    uint8_t* guest_base, uint32_t device, uint32_t slot) {
  PlayerSkinVertexFetchObservation fetch;
  fetch.slot = slot;
  if (!CaptureBeWords(
          guest_base, device,
          kFetchBankOffset + slot * sizeof(uint32_t) * 2,
          fetch.words)) {
    return fetch;
  }

  fetch.physical_address = fetch.words[0] & ~uint32_t{3};
  fetch.endian = fetch.words[1] & 0x3u;
  fetch.size = fetch.words[1] & 0x03FFFFFCu;
  fetch.valid = (fetch.words[0] & 0x3u) == 3 &&
                fetch.physical_address != 0 && fetch.size != 0;
  return fetch;
}

PlayerSkinVertexSample CaptureVertexSample(
    const PlayerSkinVertexPayload& payload) {
  PlayerSkinVertexSample sample;
  if (!payload.valid()) {
    return sample;
  }

  sample.payload_fingerprint = payload.payload_fingerprint;
  for (size_t vertex = 0; vertex < payload.vertex_count; ++vertex) {
    const std::byte* source =
        reinterpret_cast<const std::byte*>(
            payload.raw_bytes.data() + vertex * kPlayerVertexStride);
    PlayerSkinVertexSample candidate;
    candidate.payload_fingerprint = sample.payload_fingerprint;
    for (size_t axis = 0; axis < candidate.position.size(); ++axis) {
      candidate.position[axis] =
          LoadBeF32(source + axis * sizeof(float));
    }
    const uint32_t weight_word = LoadBeU32(source + 12);
    const uint32_t index_word = LoadBeU32(source + 16);
    float weight_sum = 0.0f;
    for (size_t influence = 0; influence < candidate.weights.size();
         ++influence) {
      const uint32_t shift = kInfluenceByteOrder[influence] * 8;
      candidate.weights[influence] =
          static_cast<float>((weight_word >> shift) & 0xFFu) / 255.0f;
      candidate.bone_indices[influence] =
          static_cast<uint8_t>((index_word >> shift) & 0xFFu);
      weight_sum += candidate.weights[influence];
    }
    candidate.valid =
        std::all_of(candidate.position.begin(), candidate.position.end(),
                    [](float value) {
                      return std::isfinite(value) &&
                             std::abs(value) < 1000000.0f;
                    }) &&
        std::isfinite(weight_sum) &&
        std::abs(weight_sum - 1.0f) < 0.02f;
    if (candidate.valid) {
      return candidate;
    }
  }
  return sample;
}

PlayerSkinPaletteSample CapturePaletteSample(
    const PlayerSkinPalettePayload& payload) {
  PlayerSkinPaletteSample sample;
  if (!payload.valid() || payload.records.empty()) {
    return sample;
  }
  sample.record_count = payload.record_count;
  sample.payload_fingerprint = payload.payload_fingerprint;
  sample.quaternion = payload.records.front().quaternion;
  sample.translation = payload.records.front().translation;
  sample.valid =
      std::all_of(sample.quaternion.begin(), sample.quaternion.end(),
                  [](float value) { return std::isfinite(value); }) &&
      std::all_of(sample.translation.begin(), sample.translation.end(),
                  [](float value) { return std::isfinite(value); });
  return sample;
}

template <size_t Size>
bool AllFinite(const std::array<float, Size>& values) {
  return std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value); });
}

bool TextureFetchesValid(
    const std::array<std::array<uint32_t, 6>, 3>& fetches) {
  return std::all_of(
      fetches.begin(), fetches.end(), [](const auto& fetch) {
        return (fetch[0] & 0x3u) == 2 && fetch[1] != 0;
      });
}

template <size_t Size>
bool ApproximatelyEqual(const std::array<float, Size>& values,
                        const std::array<float, Size>& expected) {
  for (size_t index = 0; index < Size; ++index) {
    if (!std::isfinite(values[index])) {
      return false;
    }
    const float tolerance =
        0.00001f * std::max(1.0f, std::abs(expected[index]));
    if (std::abs(values[index] - expected[index]) > tolerance) {
      return false;
    }
  }
  return true;
}

}  // namespace

void ObservePlayerSkinDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw) {
  if (!PlayerSkinObserverEnabled() || guest_base == nullptr ||
      draw.player == 0 ||
      !IsVerifiedPlayerSkinPassDescriptor(
          draw.pass.pass_descriptor) ||
      draw.mesh.vertex_stride != kPlayerVertexStride) {
    return;
  }

  PlayerSkinDrawObservation observation;
  observation.ordinal = draw.ordinal;
  observation.player = draw.player;
  observation.shader = draw.scope.shader;
  observation.model = draw.scope.model;
  observation.geometry_index = draw.scope.geometry_index;
  observation.pass_descriptor = draw.pass.pass_descriptor;
  observation.program_pair = draw.pass.program_pair;
  observation.vertex_shader = draw.pass.vertex_shader;
  observation.pixel_shader = draw.pass.pixel_shader;
  observation.submitted_index_count = draw.submitted_index_count;
  observation.vertex_buffer_virtual_alias =
      draw.mesh.vertex_buffer_alias;
  observation.index_buffer_virtual_alias =
      draw.mesh.index_buffer_alias;
  observation.vertex_stride = draw.mesh.vertex_stride;
  observation.alternate_pass = draw.scope.alternate_pass;

  observation.vertices = CaptureVertexFetch(
      guest_base, draw.device, kPrimaryVertexFetchSlot);
  observation.palette = CaptureVertexFetch(
      guest_base, draw.device, kPaletteVertexFetchSlot);
  observation.guest_read_failures += !observation.vertices.valid;
  observation.guest_read_failures += !observation.palette.valid;
  observation.primary_fetch_matches_mesh =
      observation.vertices.physical_address ==
      PhysicalAddressForVirtualAlias(
          observation.vertex_buffer_virtual_alias);

  std::array<uint32_t, 18> texture_words;
  if (CaptureBeWords(guest_base, draw.device, kFetchBankOffset,
                     texture_words)) {
    for (size_t slot = 0; slot < observation.texture_fetches.size();
         ++slot) {
      std::copy_n(texture_words.begin() + slot * 6, 6,
                  observation.texture_fetches[slot].begin());
      observation.texture_view_swizzles[slot] =
          (observation.texture_fetches[slot][3] >>
           kTextureViewSwizzleShift) &
          kTextureViewSwizzleMask;
    }
  } else {
    ++observation.guest_read_failures;
  }

  auto capture_constants = [&](size_t bank_offset, size_t first_row,
                               auto& destination) {
    if (!CaptureBeFloats(
            guest_base, draw.device,
            bank_offset + first_row * kConstantRowBytes,
            destination)) {
      ++observation.guest_read_failures;
    }
  };
  capture_constants(kVertexConstantBankOffset, 12,
                    observation.vertex_constants_12_15);
  capture_constants(kVertexConstantBankOffset, 19,
                    observation.vertex_constant_19);
  capture_constants(kVertexConstantBankOffset, 46,
                    observation.vertex_constants_46_54);
  capture_constants(kVertexConstantBankOffset, 255,
                    observation.vertex_constant_255);
  capture_constants(kPixelConstantBankOffset, 46,
                    observation.pixel_constants_46_68);
  capture_constants(kPixelConstantBankOffset, 254,
                    observation.pixel_constant_254);
  capture_constants(kPixelConstantBankOffset, 255,
                    observation.pixel_constant_255);

  observation.pixel_control_constants_valid =
      ApproximatelyEqual(observation.pixel_constant_254,
                         kExpectedPixelConstant254) &&
      ApproximatelyEqual(observation.pixel_constant_255,
                         kExpectedPixelConstant255);
  observation.texture_view_swizzles_valid =
      std::all_of(observation.texture_view_swizzles.begin(),
                  observation.texture_view_swizzles.end(),
                  [](uint32_t swizzle) {
                    return swizzle == kExpectedTextureViewSwizzle;
                  });

  const bool scalar_contract_valid =
      observation.guest_read_failures == 0 && draw.pass.valid &&
      draw.mesh.valid && observation.primary_fetch_matches_mesh &&
      observation.vertices.endian == kVertexEndian8In32 &&
      observation.vertices.size % kPlayerVertexStride == 0 &&
      observation.palette.endian == kVertexEndian8In32 &&
      observation.palette.size % kPaletteRecordStride == 0 &&
      TextureFetchesValid(observation.texture_fetches) &&
      AllFinite(observation.vertex_constants_12_15) &&
      AllFinite(observation.vertex_constant_19) &&
      AllFinite(observation.vertex_constants_46_54) &&
      AllFinite(observation.vertex_constant_255) &&
      AllFinite(observation.pixel_constants_46_68) &&
      observation.pixel_control_constants_valid &&
      observation.texture_view_swizzles_valid;

  PlayerSkinPayloadCapture payloads;
  if (scalar_contract_valid) {
    payloads =
        CapturePlayerSkinPayloads(guest_base, draw, observation);
    observation.guest_read_failures += payloads.copy_failures;
    if (payloads.vertices != nullptr) {
      observation.vertex_sample =
          CaptureVertexSample(*payloads.vertices);
    }
    if (payloads.palette != nullptr) {
      observation.palette_sample =
          CapturePaletteSample(*payloads.palette);
    }
  }
  observation.valid =
      scalar_contract_valid && payloads.valid() &&
      observation.vertex_sample.valid &&
      observation.palette_sample.valid;
  ObservePlayerSkinSnapshot(draw, observation, payloads);

  std::lock_guard lock(g_observer_mutex);
  ++g_building_frame.candidate_draw_count;
  g_building_frame.valid_draw_count += observation.valid;
  g_building_frame.guest_read_failures +=
      observation.guest_read_failures;
  if (g_building_frame.candidate_draw_count >
      g_building_frame.draws.size()) {
    ++g_building_frame.dropped_draw_count;
    return;
  }
  g_building_frame.draws[g_building_frame.candidate_draw_count - 1] =
      observation;
}

void PlayerSkinObserverFrameEnd() {
  const bool enabled = PlayerSkinObserverEnabled();
  std::lock_guard lock(g_observer_mutex);
  ++g_frame_sequence;
  if (!enabled) {
    g_building_frame = {};
    g_published_frame = {};
    g_announced = false;
    g_announced_rejection = false;
    return;
  }

  g_building_frame.sequence = g_frame_sequence;
  g_published_frame = g_building_frame;
  g_building_frame = {};
  if (!g_announced_rejection &&
      g_published_frame.candidate_draw_count != 0 &&
      g_published_frame.valid_draw_count == 0) {
    g_announced_rejection = true;
    const PlayerSkinDrawObservation& first =
        g_published_frame.draws[0];
    REXLOG_INFO(
        "Table Tennis player skin observer: candidate rejected "
        "ordinal={} indices={} pass={:08X} vs={:08X} ps={:08X} "
        "mesh_valid={} vf{}_valid={} vf{}_valid={} "
        "fetch_matches_mesh={} vertex_sample={} palette_sample={} textures={} "
        "ps_controls={} texture_swizzles={} read_failures={} "
        "observer_only=true",
        first.ordinal, first.submitted_index_count, first.pass_descriptor,
        first.vertex_shader,
        first.pixel_shader, first.vertex_stride == kPlayerVertexStride,
        first.vertices.slot, first.vertices.valid, first.palette.slot,
        first.palette.valid, first.primary_fetch_matches_mesh,
        first.vertex_sample.valid,
        first.palette_sample.valid,
        TextureFetchesValid(first.texture_fetches),
        first.pixel_control_constants_valid,
        first.texture_view_swizzles_valid,
        first.guest_read_failures);
    const size_t captured_count = std::min<size_t>(
        g_published_frame.candidate_draw_count,
        g_published_frame.draws.size());
    for (size_t index = 0; index < captured_count; ++index) {
      const PlayerSkinDrawObservation& candidate =
          g_published_frame.draws[index];
      REXLOG_INFO(
          "  player_skin_candidate position={} ordinal={} player={:08X} "
          "indices={} pass={:08X} vs={:08X} ps={:08X} "
          "vb_virtual={:08X} vf95_physical={:08X} expected={:08X} "
          "vf95_bytes={} vf92_physical={:08X} vf92_bytes={} "
          "mesh_match={} vertex={} palette={} textures={} reads={}",
          index, candidate.ordinal, candidate.player,
          candidate.submitted_index_count, candidate.pass_descriptor,
          candidate.vertex_shader, candidate.pixel_shader,
          candidate.vertex_buffer_virtual_alias,
          candidate.vertices.physical_address,
          PhysicalAddressForVirtualAlias(
              candidate.vertex_buffer_virtual_alias),
          candidate.vertices.size, candidate.palette.physical_address,
          candidate.palette.size, candidate.primary_fetch_matches_mesh,
          candidate.vertex_sample.valid, candidate.palette_sample.valid,
          TextureFetchesValid(candidate.texture_fetches),
          candidate.guest_read_failures);
    }
  }
  if (!g_announced && g_published_frame.valid_draw_count != 0) {
    g_announced = true;
    const auto captured_end =
        g_published_frame.draws.begin() +
        std::min<size_t>(g_published_frame.candidate_draw_count,
                         g_published_frame.draws.size());
    const auto first_valid = std::find_if(
        g_published_frame.draws.begin(), captured_end,
        [](const PlayerSkinDrawObservation& draw) {
          return draw.valid;
        });
    if (first_valid == captured_end) {
      return;
    }
    REXLOG_INFO(
        "Table Tennis player skin observer: frame={} candidates={} "
        "valid={} dropped={} vf{}={:08X}/{} vf{}={:08X}/{} "
        "palette_records={} observer_only=true",
        g_published_frame.sequence,
        g_published_frame.candidate_draw_count,
        g_published_frame.valid_draw_count,
        g_published_frame.dropped_draw_count, first_valid->vertices.slot,
        first_valid->vertices.physical_address,
        first_valid->vertices.size, first_valid->palette.slot,
        first_valid->palette.physical_address,
        first_valid->palette.size,
        first_valid->palette_sample.record_count);
  }
}

bool PlayerSkinObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_player_skin_observer) ||
         PlayerObserverOverlayEnabled() ||
         PlayerReplacementPrewarmEnabled() ||
         NativeFrameSceneCaptureEnabled();
}

PlayerSkinFrameObservation LatestPlayerSkinFrameObservation() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

}  // namespace tabletennis::native
