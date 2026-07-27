#include "native/tabletennis_player_palette.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_player_skin_observer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_palette_observer, false, "Table Tennis",
    "Capture the two players' live skin-matrix palettes at pongDrawable "
    "submission. Observer-only; never suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint32_t kPlayerCreatureOffset = 0x1C4;
constexpr uint32_t kCreatureRenderInterfaceOffset = 0x10;
constexpr uint32_t kCreaturePaletteMetadataOffset = 0xAC;
constexpr uint32_t kPaletteSkeletonMetadataOffset = 0x04;
constexpr uint32_t kPaletteMatrixArrayOffset = 0x14;
constexpr uint32_t kSkeletonMatrixCountOffset = 0x0C;
constexpr uint32_t kMatrixBytes = 64;
constexpr uint32_t kMaximumMatrixCount = 256;
constexpr uint32_t kPongDrawableVtable = 0x8204DD9C;
constexpr size_t kMaximumScopeDepth = 4;
constexpr size_t kMaximumDrawableOwners = 4;
constexpr size_t kMaximumCreatureOwners = 4;

thread_local std::array<uint32_t, kMaximumScopeDepth> g_player_stack{};
thread_local size_t g_player_depth = 0;
thread_local std::array<uint32_t, kMaximumScopeDepth> g_creature_stack{};
thread_local size_t g_creature_depth = 0;

std::mutex g_palette_mutex;
PlayerPaletteFrame g_building_frame;
std::shared_ptr<const PlayerPaletteFrame> g_published_frame;
uint64_t g_frame_sequence = 0;
bool g_announced_capture = false;
std::array<std::pair<uint32_t, uint32_t>, kMaximumDrawableOwners>
    g_drawable_owners{};
std::array<std::pair<uint32_t, uint32_t>, kMaximumCreatureOwners>
    g_creature_owners{};

uint16_t LoadBeU16(const std::byte* source) {
  uint16_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

uint32_t LoadBeU32(const std::byte* source) {
  uint32_t value;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const std::byte* source) {
  return std::bit_cast<float>(LoadBeU32(source));
}

bool CheckedAddress(uint32_t guest_address_base, uint32_t offset, size_t size,
                    uint32_t& address) {
  if (guest_address_base == 0) {
    return false;
  }
  const uint64_t start =
      static_cast<uint64_t>(guest_address_base) + offset;
  const uint64_t end = start + size;
  if (start > std::numeric_limits<uint32_t>::max() ||
      end > (uint64_t{1} << 32)) {
    return false;
  }
  address = static_cast<uint32_t>(start);
  return true;
}

bool CopyGuest(uint8_t* guest_base, uint32_t guest_address_base,
               uint32_t offset, void* destination, size_t size) {
  uint32_t address;
  return guest_base != nullptr &&
         CheckedAddress(guest_address_base, offset, size, address) &&
         GuestTryCopy(
             destination,
             guest_base + address + REX_PHYS_HOST_OFFSET(address), size);
}

bool ReadBeU32(uint8_t* guest_base, uint32_t guest_address_base,
               uint32_t offset, uint32_t& value) {
  std::array<std::byte, sizeof(uint32_t)> bytes;
  if (!CopyGuest(guest_base, guest_address_base, offset, bytes.data(),
                 bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data());
  return true;
}

bool ReadBeU16(uint8_t* guest_base, uint32_t guest_address_base,
               uint32_t offset, uint16_t& value) {
  std::array<std::byte, sizeof(uint16_t)> bytes;
  if (!CopyGuest(guest_base, guest_address_base, offset, bytes.data(),
                 bytes.size())) {
    return false;
  }
  value = LoadBeU16(bytes.data());
  return true;
}

uint64_t Fingerprint(const std::vector<std::byte>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (std::byte value : bytes) {
    hash =
        (hash ^ static_cast<uint8_t>(value)) * 1099511628211ull;
  }
  return hash;
}

uint32_t CurrentPlayer() {
  return g_player_depth != 0 && g_player_depth <= g_player_stack.size()
             ? g_player_stack[g_player_depth - 1]
             : 0;
}

uint32_t CurrentCreature() {
  if (g_creature_depth == 0 ||
      g_creature_depth > g_creature_stack.size()) {
    return 0;
  }
  const uint32_t render_interface =
      g_creature_stack[g_creature_depth - 1];
  return render_interface >= kCreatureRenderInterfaceOffset
             ? render_interface - kCreatureRenderInterfaceOffset
             : 0;
}

}  // namespace

void BeginPlayerPalettePlayerScope(uint32_t player) {
  if (g_player_depth < g_player_stack.size()) {
    g_player_stack[g_player_depth] =
        (PlayerPaletteObserverEnabled() || PlayerSkinObserverEnabled())
            ? player
            : 0;
  }
  ++g_player_depth;
}

void EndPlayerPalettePlayerScope() {
  if (g_player_depth != 0) {
    --g_player_depth;
  }
}

void RegisterPlayerPaletteCreatureOwner(uint32_t player,
                                        uint32_t creature) {
  if (player == 0 || creature == 0) {
    return;
  }
  std::lock_guard lock(g_palette_mutex);
  auto owner = std::find_if(
      g_creature_owners.begin(), g_creature_owners.end(),
      [creature](const auto &candidate) {
        return candidate.first == creature || candidate.first == 0;
      });
  if (owner == g_creature_owners.end()) {
    owner = g_creature_owners.begin();
  }
  *owner = {creature, player};
}

uint32_t ResolvePlayerPaletteCreatureOwner(
    uint32_t creature_interface) {
  if (const uint32_t scoped_player = CurrentPlayer();
      scoped_player != 0) {
    return scoped_player;
  }
  const uint32_t creature =
      creature_interface >= kCreatureRenderInterfaceOffset
          ? creature_interface - kCreatureRenderInterfaceOffset
          : 0;
  if (creature == 0) {
    return 0;
  }
  std::lock_guard lock(g_palette_mutex);
  const auto owner = std::find_if(
      g_creature_owners.begin(), g_creature_owners.end(),
      [creature](const auto &candidate) {
        return candidate.first == creature;
      });
  return owner == g_creature_owners.end() ? 0 : owner->second;
}

void BeginPlayerPaletteCreatureScope(uint32_t creature_interface) {
  if (g_creature_depth < g_creature_stack.size()) {
    g_creature_stack[g_creature_depth] =
        (PlayerPaletteObserverEnabled() || PlayerSkinObserverEnabled())
            ? creature_interface
            : 0;
  }
  ++g_creature_depth;
}

void EndPlayerPaletteCreatureScope() {
  if (g_creature_depth != 0) {
    --g_creature_depth;
  }
}

void RegisterPlayerPaletteDrawableOwner(uint32_t player,
                                        uint32_t drawable) {
  if (player == 0 || drawable == 0) {
    return;
  }
  std::lock_guard lock(g_palette_mutex);
  auto owner = std::find_if(
      g_drawable_owners.begin(), g_drawable_owners.end(),
      [drawable](const auto& candidate) {
        return candidate.first == drawable || candidate.first == 0;
      });
  if (owner == g_drawable_owners.end()) {
    owner = g_drawable_owners.begin();
  }
  *owner = {drawable, player};
}

uint32_t ResolvePlayerPaletteOwner(uint32_t drawable) {
  if (const uint32_t scoped_player = CurrentPlayer();
      scoped_player != 0) {
    return scoped_player;
  }
  if (drawable == 0) {
    return 0;
  }
  std::lock_guard lock(g_palette_mutex);
  const auto owner = std::find_if(
      g_drawable_owners.begin(), g_drawable_owners.end(),
      [drawable](const auto& candidate) {
        return candidate.first == drawable;
      });
  return owner == g_drawable_owners.end() ? 0 : owner->second;
}

void ObservePlayerDrawablePalette(uint8_t* guest_base,
                                  uint32_t drawable) {
  const bool capture_palette = PlayerPaletteObserverEnabled();
  if ((!capture_palette && !PlayerSkinObserverEnabled()) ||
      guest_base == nullptr) {
    return;
  }
  const uint32_t player = CurrentPlayer();
  const uint32_t creature = CurrentCreature();
  if (player == 0 || creature == 0 || drawable == 0) {
    return;
  }

  uint32_t player_creature = 0;
  uint32_t drawable_vtable = 0;
  uint32_t palette_metadata = 0;
  uint32_t skeleton_metadata = 0;
  uint32_t matrix_palette = 0;
  uint16_t matrix_count = 0;
  if (!ReadBeU32(guest_base, player, kPlayerCreatureOffset,
                 player_creature) ||
      player_creature != creature ||
      !ReadBeU32(guest_base, drawable, 0, drawable_vtable) ||
      drawable_vtable != kPongDrawableVtable ||
      !ReadBeU32(guest_base, creature, kCreaturePaletteMetadataOffset,
                 palette_metadata) ||
      !ReadBeU32(guest_base, palette_metadata,
                 kPaletteSkeletonMetadataOffset, skeleton_metadata) ||
      !ReadBeU16(guest_base, skeleton_metadata,
                 kSkeletonMatrixCountOffset, matrix_count) ||
      !ReadBeU32(guest_base, palette_metadata, kPaletteMatrixArrayOffset,
                 matrix_palette) ||
      matrix_count == 0 || matrix_count > kMaximumMatrixCount) {
    return;
  }

  RegisterPlayerPaletteDrawableOwner(player, drawable);

  if (!capture_palette) {
    return;
  }

  const size_t payload_size =
      static_cast<size_t>(matrix_count) * kMatrixBytes;
  std::vector<std::byte> payload(payload_size);
  if (!CopyGuest(guest_base, matrix_palette, 0, payload.data(),
                 payload.size())) {
    return;
  }

  PlayerPaletteSnapshot snapshot;
  snapshot.player = player;
  snapshot.creature = creature;
  snapshot.drawable = drawable;
  snapshot.palette_metadata = palette_metadata;
  snapshot.skeleton_metadata = skeleton_metadata;
  snapshot.matrix_palette = matrix_palette;
  snapshot.payload_fingerprint = Fingerprint(payload);
  snapshot.matrices.resize(matrix_count);
  for (uint32_t matrix_index = 0; matrix_index < matrix_count;
       ++matrix_index) {
    std::array<float, 16>& matrix = snapshot.matrices[matrix_index];
    const std::byte* source =
        payload.data() + static_cast<size_t>(matrix_index) * kMatrixBytes;
    for (size_t component = 0; component < matrix.size(); ++component) {
      const float value =
          LoadBeF32(source + component * sizeof(float));
      if (!std::isfinite(value) || std::abs(value) > 100000000.0f) {
        return;
      }
      matrix[component] = value;
    }
  }

  std::lock_guard lock(g_palette_mutex);
  const auto existing = std::find_if(
      g_building_frame.players.begin(), g_building_frame.players.end(),
      [&](const PlayerPaletteSnapshot& candidate) {
        return candidate.player == player;
      });
  if (existing == g_building_frame.players.end()) {
    g_building_frame.players.push_back(std::move(snapshot));
  } else {
    *existing = std::move(snapshot);
  }
}

void PlayerPaletteFrameEnd() {
  const bool enabled = PlayerPaletteObserverEnabled();
  std::lock_guard lock(g_palette_mutex);
  ++g_frame_sequence;
  if (!enabled) {
    g_building_frame = {};
    g_published_frame.reset();
    g_announced_capture = false;
    return;
  }

  g_building_frame.sequence = g_frame_sequence;
  for (PlayerPaletteSnapshot& player : g_building_frame.players) {
    player.frame_sequence = g_frame_sequence;
  }
  if (g_building_frame.valid()) {
    g_published_frame = std::make_shared<const PlayerPaletteFrame>(
        std::move(g_building_frame));
    if (!g_announced_capture) {
      g_announced_capture = true;
      REXLOG_INFO(
          "Table Tennis player palette observer: captured {} live "
          "pongCreature palettes at pongDrawable submission "
          "(observer-only)",
          g_published_frame->players.size());
      for (const PlayerPaletteSnapshot& player :
           g_published_frame->players) {
        REXLOG_INFO(
            "  player_palette player={:08X} creature={:08X} "
            "drawable={:08X} matrices={} source={:08X} "
            "fingerprint={:016X}",
            player.player, player.creature, player.drawable,
            player.matrices.size(), player.matrix_palette,
            player.payload_fingerprint);
      }
    }
  } else {
    g_published_frame.reset();
  }
  g_building_frame = {};
}

bool PlayerPaletteObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_player_palette_observer);
}

std::shared_ptr<const PlayerPaletteFrame> LatestPlayerPaletteFrame() {
  std::lock_guard lock(g_palette_mutex);
  return g_published_frame;
}

}  // namespace tabletennis::native
