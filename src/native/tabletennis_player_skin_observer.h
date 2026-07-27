#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

// Draw-time fetch descriptor copied from the title's live device bank.
// Addresses and sizes are byte-based host values after Xbox endian decoding.
struct PlayerSkinVertexFetchObservation {
  uint32_t slot = 0;
  // Physical GPU address from the fetch constant. This must not be compared
  // numerically with the title's F0... virtual resource alias.
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t endian = 0;
  std::array<uint32_t, 2> words{};
  bool valid = false;
};

// First vertex decoded with the exact CA9BBF96B0928616 fetch contract:
// float3 position, normalized 8_8_8_8 weights and byte bone indices.
struct PlayerSkinVertexSample {
  std::array<float, 3> position{};
  std::array<float, 4> weights{};
  std::array<uint8_t, 4> bone_indices{};
  uint64_t payload_fingerprint = 0;
  bool valid = false;
};

// The shader consumes a float4 quaternion followed by float3 translation for
// each entry. This is the actual vf92 GPU palette, not the creature's source
// 4x4 matrix array.
struct PlayerSkinPaletteSample {
  uint32_t record_count = 0;
  uint64_t payload_fingerprint = 0;
  std::array<float, 4> quaternion{};
  std::array<float, 3> translation{};
  bool valid = false;
};

struct PlayerSkinDrawObservation {
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t shader = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t submitted_index_count = 0;
  uint32_t vertex_buffer_virtual_alias = 0;
  uint32_t index_buffer_virtual_alias = 0;
  uint32_t vertex_stride = 0;
  bool alternate_pass = false;

  PlayerSkinVertexFetchObservation vertices{};
  PlayerSkinVertexFetchObservation palette{};
  PlayerSkinVertexSample vertex_sample{};
  PlayerSkinPaletteSample palette_sample{};
  bool primary_fetch_matches_mesh = false;

  std::array<std::array<uint32_t, 6>, 3> texture_fetches{};
  // Xenos fetch-constant view swizzle, not the tfetch instruction's
  // destination write swizzle. The verified CA9 block uses 0x688 (RGBA) for
  // all three views.
  std::array<uint32_t, 3> texture_view_swizzles{};
  std::array<float, 16> vertex_constants_12_15{};
  std::array<float, 4> vertex_constant_19{};
  std::array<float, 36> vertex_constants_46_54{};
  std::array<float, 4> vertex_constant_255{};
  std::array<float, 92> pixel_constants_46_68{};
  std::array<float, 4> pixel_constant_254{};
  std::array<float, 4> pixel_constant_255{};
  bool pixel_control_constants_valid = false;
  bool texture_view_swizzles_valid = false;

  uint32_t guest_read_failures = 0;
  bool valid = false;
};

struct PlayerSkinFrameObservation {
  static constexpr size_t kMaxDraws = 256;

  uint64_t sequence = 0;
  uint32_t candidate_draw_count = 0;
  uint32_t valid_draw_count = 0;
  uint32_t dropped_draw_count = 0;
  uint32_t guest_read_failures = 0;
  std::array<PlayerSkinDrawObservation, kMaxDraws> draws{};
};

// Observer-only capture for the proven 44-byte skinned player vertex family.
// It never suppresses, replaces, or mutates a title draw.
void ObservePlayerSkinDraw(uint8_t* guest_base,
                           const SceneCatalogDrawOccurrence& draw);
void PlayerSkinObserverFrameEnd();

bool PlayerSkinObserverEnabled();
PlayerSkinFrameObservation LatestPlayerSkinFrameObservation();

}  // namespace tabletennis::native
