#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::graphics {
struct NativeGuestDrawContext;
}

namespace tabletennis::native {

struct SceneCatalogDrawOccurrence;

struct PlayerPaletteWriteObservation {
  uint64_t write_sequence = 0;
  uint64_t frame_sequence = 0;
  uint32_t player = 0;
  uint32_t destination = 0;
  uint32_t physical_destination = 0;
  uint32_t source = 0;
  uint32_t record_count = 0;
  uint32_t byte_count = 0;
  uint32_t nonzero_record_count = 0;
  uint32_t normalized_quaternion_count = 0;
  uint64_t payload_fingerprint = 0;
  uint32_t matched_draw_count = 0;
  bool valid = false;
};

struct PlayerPaletteWriteObserverFrame {
  static constexpr size_t kMaxWrites = 32;

  uint64_t sequence = 0;
  uint32_t write_attempt_count = 0;
  uint32_t valid_write_count = 0;
  uint32_t rejected_write_count = 0;
  uint32_t guest_read_failure_count = 0;
  uint32_t correlated_draw_count = 0;
  uint32_t d47_contract_draw_count = 0;
  uint32_t dropped_write_count = 0;
  uint32_t write_count = 0;
  std::array<PlayerPaletteWriteObservation, kMaxWrites> writes{};
};

// Immutable proof for both equal record halves of a live player vf92 fetch.
// The 0x8225C668 and 0x8225C720 postconditions prove their bound buffer,
// cached generation, cached sources and derived record count all match the
// active fetch. The primary binder runs under the exact player callback
// owner. The stable copied payload then validates every record in both halves.
// A same-generation packer write is optional corroboration because either
// binder may correctly reuse its cache. No guest pointer escapes.
struct D47PaletteWritePairProof {
  uint64_t frame_sequence = 0;
  uint32_t player = 0;
  uint32_t physical_fetch_base = 0;
  uint32_t fetch_byte_count = 0;
  uint32_t record_count_per_half = 0;
  uint32_t primary_drawable = 0;
  uint32_t primary_source = 0;
  uint32_t primary_cache_generation = 0;
  uint32_t alternate_drawable = 0;
  uint32_t alternate_source = 0;
  uint32_t alternate_cache_generation = 0;
  std::array<uint64_t, 2> write_sequences{};
  std::array<uint64_t, 2> payload_fingerprints{};
  bool primary_cache_valid = false;
  bool alternate_cache_valid = false;
  bool valid = false;
};

// The existing pongPlayer callbacks provide exact synchronous ownership for
// the first nested title-side palette packer call.
void BeginPlayerPaletteWriteOwnerScope(uint32_t player);
void EndPlayerPaletteWriteOwnerScope();

// Observe only after sub_8225C510 has returned. The original ABI arguments
// must be preserved by its wrapper and passed here unchanged.
void ObserveCompletedPlayerPaletteWrite(uint8_t* guest_base,
                                        uint32_t destination,
                                        uint32_t source,
                                        uint32_t record_count);

// Observe 0x8225C668 after it has either refreshed or reused the primary
// half. This call runs under the exact player render scope.
void ObserveCompletedPrimaryPlayerPaletteBinding(
    uint8_t* guest_base, uint32_t drawable, uint32_t source,
    uint32_t requested_record_count);

// Observe 0x8225C720 after it has either refreshed or reused the alternate
// half. Its cached generation/source fields are the title's proof that the
// bound second half already represents the current source.
void ObserveCompletedAlternatePlayerPaletteBinding(
    uint8_t* guest_base, uint32_t drawable, uint32_t source);

// Capture the exact title-side draw identity and live vf92 binding from the
// generic scene catalog. This only creates a bounded same-generation token;
// the token is not counted until the backend independently proves the exact
// D47 shader family and indexed triangle-strip identity.
void ObservePlayerPaletteWriteCatalogDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw);

// Read-only backend tap. It observes the exact translated shader hashes and
// never claims, suppresses, or replaces a draw.
void ObservePlayerPaletteWriteBackendDraw(
    const rex::graphics::NativeGuestDrawContext& context);

// Query the current, still-open title generation. A result is valid only when
// both exact cache/buffer postconditions cover the D47 fetch.
D47PaletteWritePairProof CurrentD47PaletteWritePairProof(
    uint32_t player, uint32_t physical_fetch_base,
    uint32_t fetch_byte_count);

// Late backend joins must query the exact archived title generation rather
// than whichever generation is currently being built. Cache-binding
// observations are retained in a bounded ring and addressed by this sequence.
D47PaletteWritePairProof D47PaletteWritePairProofForFrame(
    uint64_t frame_sequence, uint32_t player,
    uint32_t physical_fetch_base, uint32_t fetch_byte_count);

void PlayerPaletteWriteObserverFrameEnd();
bool PlayerPaletteWriteObserverEnabled();
PlayerPaletteWriteObserverFrame LatestPlayerPaletteWriteObserverFrame();

}  // namespace tabletennis::native
