#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

// Immutable copy of the matrix palette used by one pongCreature immediately
// before its pongDrawable submits models. This is observer data only.
struct PlayerPaletteSnapshot {
  uint64_t frame_sequence = 0;
  uint32_t player = 0;
  uint32_t creature = 0;
  uint32_t drawable = 0;
  uint32_t palette_metadata = 0;
  uint32_t skeleton_metadata = 0;
  uint32_t matrix_palette = 0;
  uint64_t payload_fingerprint = 0;
  std::vector<std::array<float, 16>> matrices;

  bool valid() const {
    return player != 0 && creature != 0 && drawable != 0 &&
           matrix_palette != 0 && !matrices.empty();
  }
};

struct PlayerPaletteFrame {
  uint64_t sequence = 0;
  std::vector<PlayerPaletteSnapshot> players;

  bool valid() const {
    if (players.empty()) {
      return false;
    }
    for (const PlayerPaletteSnapshot& player : players) {
      if (!player.valid()) {
        return false;
      }
    }
    return true;
  }
};

// The player render callback owns the player identity. The nested creature
// callback owns the matrix-palette source. pongDrawable submission is the
// point where both have been resolved and the current palette is ready.
void BeginPlayerPalettePlayerScope(uint32_t player);
void EndPlayerPalettePlayerScope();
// The player callbacks expose player->creature before the creature renderer
// may run from a later bucket. Retain this value-only ownership join so the
// asynchronous creature scope can recover the exact player.
void RegisterPlayerPaletteCreatureOwner(uint32_t player, uint32_t creature);
uint32_t ResolvePlayerPaletteCreatureOwner(uint32_t creature_interface);
void BeginPlayerPaletteCreatureScope(uint32_t creature_interface);
void EndPlayerPaletteCreatureScope();
// pongCreature_RenderDrawable exposes the exact drawable passed to the
// palette bind before the later alternate-palette pass runs.
void RegisterPlayerPaletteDrawableOwner(uint32_t player,
                                        uint32_t drawable);
// Resolves the exact owner learned while pongPlayer and pongCreature scopes
// overlap. This lets pongDrawable::SubmitModels retain player ownership for
// its full nested model-list walk.
uint32_t ResolvePlayerPaletteOwner(uint32_t drawable);
void ObservePlayerDrawablePalette(uint8_t* guest_base, uint32_t drawable);
void PlayerPaletteFrameEnd();

bool PlayerPaletteObserverEnabled();
std::shared_ptr<const PlayerPaletteFrame> LatestPlayerPaletteFrame();

}  // namespace tabletennis::native
