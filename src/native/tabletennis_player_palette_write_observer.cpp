#include "native/tabletennis_player_palette_write_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_d47_player_observer.h"
#include "native/tabletennis_guest_memory.h"
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
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_palette_write_observer, false, "Table Tennis",
    "Observe completed title-side 28-byte player palette writes and correlate "
    "them with later vf92 draw bindings. Observer-only; never mutates or "
    "suppresses a title draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr size_t kMaximumOwnerScopeDepth = 4;
constexpr size_t kMaximumWriteLedgerEntries = 64;
constexpr size_t kMaximumAlternateBindingEntries = 16;
constexpr size_t kMaximumDrawTokens = 128;
constexpr size_t kMaximumBackendEvents = 128;
constexpr uint32_t kPaletteRecordBytes = 28;
constexpr uint32_t kMaximumPaletteRecords = 256;
constexpr uint32_t kFetchBankOffset = 0x480;
constexpr uint32_t kPaletteFetchSlot = 92;
constexpr uint32_t kVertexEndian8In32 = 2;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
constexpr uint32_t kHighPhysicalHeapBase = 0xE0000000;
constexpr uint32_t kHighPhysicalHeapHostPageOffset = 0x1000;
constexpr uint32_t kAlternateGenerationAddress = 0x825C9010;
constexpr uint32_t kAlternateRingSizeAddress = 0x825C9A6C;
constexpr uint32_t kDrawablePaletteMetadataOffset = 0x08;
constexpr uint32_t kPaletteMatrixCountOffset = 0x0C;
constexpr uint32_t kPaletteBufferHandleOffset = 0x0C;
constexpr uint32_t kPrimaryCachedGenerationOffset = 0xAC;
constexpr uint32_t kPrimaryCachedSourceOffset = 0xB0;
constexpr uint32_t kAlternateCachedGenerationOffset = 0xB4;
constexpr uint32_t kAlternateCachedSourceOffset = 0xB8;
constexpr uint32_t kPaletteRingFirstSlot = 35;
constexpr uint32_t kTriangleStripPrimitive = 0x06;
constexpr uint64_t kD47PixelShaderHash = 0xD47C83252CF2B765ull;
constexpr std::array<uint64_t, 4> kD47BackendVertexShaderHashes = {
    0x20DD150A38FA9949ull,
    0x05AB26C749FFA8B3ull,
    0x9013322A360FE8D5ull,
    0x84D1A8EF1D71CF60ull,
};
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr float kMinimumQuaternionNormSquared = 0.95f * 0.95f;
constexpr float kMaximumQuaternionNormSquared = 1.05f * 1.05f;
constexpr uint32_t kMaximumWriteDiagnosticLogs = 8;
constexpr uint32_t kMaximumCacheProofDiagnosticLogs = 8;

struct PaletteFetch {
  uint32_t physical_address = 0;
  uint32_t size = 0;
  uint32_t endian = 0;
  bool valid = false;
};

struct D47DrawIdentity {
  uint32_t primitive_type = 0;
  uint32_t index_count = 0;
  uint32_t index_base = 0;

  bool operator==(const D47DrawIdentity&) const = default;
};

struct D47DrawToken {
  uint64_t sequence = 0;
  uint64_t generation = 0;
  uint64_t write_sequence = 0;
  uint32_t player = 0;
  PaletteFetch fetch{};
  D47PaletteWritePairProof palette_proof{};
  D47DrawIdentity identity{};
  bool valid = false;
};

struct D47BackendEvent {
  uint64_t sequence = 0;
  uint64_t generation = 0;
  uint64_t vertex_shader_hash = 0;
  D47DrawIdentity identity{};
  bool valid = false;
};

struct PaletteCacheBindingObservation {
  uint64_t frame_sequence = 0;
  uint32_t player = 0;
  uint32_t drawable = 0;
  uint32_t source = 0;
  uint32_t cache_generation = 0;
  uint32_t physical_fetch_base = 0;
  uint32_t fetch_byte_count = 0;
  uint32_t record_count_per_half = 0;
  bool valid = false;
};

thread_local std::array<uint32_t, kMaximumOwnerScopeDepth>
    g_owner_scope_stack{};
thread_local size_t g_owner_scope_depth = 0;

std::mutex g_observer_mutex;
std::array<PlayerPaletteWriteObservation, kMaximumWriteLedgerEntries>
    g_write_ledger{};
size_t g_write_ledger_count = 0;
size_t g_write_ledger_next = 0;
std::array<PaletteCacheBindingObservation,
           kMaximumAlternateBindingEntries>
    g_primary_bindings{};
size_t g_primary_binding_next = 0;
std::array<PaletteCacheBindingObservation,
           kMaximumAlternateBindingEntries>
    g_alternate_bindings{};
size_t g_alternate_binding_next = 0;
PlayerPaletteWriteObserverFrame g_building_frame;
PlayerPaletteWriteObserverFrame g_published_frame;
uint64_t g_frame_sequence = 0;
uint64_t g_write_sequence = 0;
uint64_t g_draw_token_sequence = 0;
uint64_t g_backend_event_sequence = 0;
std::array<D47DrawToken, kMaximumDrawTokens> g_draw_tokens{};
size_t g_draw_token_next = 0;
std::array<D47BackendEvent, kMaximumBackendEvents> g_backend_events{};
size_t g_backend_event_next = 0;
bool g_announced_first_write = false;
bool g_announced_inferred_second_half_owner = false;
bool g_announced_primary_cache_binding = false;
bool g_announced_alternate_cache_binding = false;
bool g_announced_first_correlation = false;
bool g_announced_d47_contract = false;
uint32_t g_write_diagnostic_logs = 0;
uint32_t g_rejected_write_diagnostic_logs = 0;
uint32_t g_cache_proof_diagnostic_logs = 0;

uint32_t CurrentOwner() {
  if (g_owner_scope_depth == 0 ||
      g_owner_scope_depth > g_owner_scope_stack.size()) {
    return 0;
  }
  return g_owner_scope_stack[g_owner_scope_depth - 1];
}

bool CheckedAddress(uint32_t address, size_t offset, size_t size,
                    uint32_t& result) {
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

bool CopyGuest(uint8_t* guest_base, uint32_t address, size_t offset,
               void* destination, size_t size) {
  uint32_t guest_address = 0;
  return guest_base != nullptr && destination != nullptr &&
         CheckedAddress(address, offset, size, guest_address) &&
         GuestTryCopy(
             destination,
             guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address),
             size);
}

uint32_t LoadBeU32(const std::byte* source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

uint16_t LoadBeU16(const std::byte* source) {
  uint16_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::byteswap(value);
}

float LoadBeF32(const std::byte* source) {
  return std::bit_cast<float>(LoadBeU32(source));
}

bool ReadBeU32(uint8_t* guest_base, uint32_t address, size_t offset,
               uint32_t& value) {
  std::array<std::byte, sizeof(uint32_t)> bytes{};
  if (!CopyGuest(guest_base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data());
  return true;
}

bool ReadBeU16(uint8_t* guest_base, uint32_t address, size_t offset,
               uint16_t& value) {
  std::array<std::byte, sizeof(uint16_t)> bytes{};
  if (!CopyGuest(guest_base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  value = LoadBeU16(bytes.data());
  return true;
}

bool CaptureStableBytes(uint8_t* guest_base, uint32_t address, size_t size,
                        std::vector<std::byte>& bytes) {
  bytes.resize(size);
  std::vector<std::byte> verification(size);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    if (!CopyGuest(guest_base, address, 0, bytes.data(), bytes.size()) ||
        !CopyGuest(guest_base, address, 0, verification.data(),
                   verification.size())) {
      bytes.clear();
      return false;
    }
    if (bytes == verification) {
      return true;
    }
  }
  bytes.clear();
  return false;
}

uint64_t Fingerprint(const std::vector<std::byte>& bytes) {
  uint64_t fingerprint = kFnvOffsetBasis;
  for (std::byte value : bytes) {
    fingerprint =
        (fingerprint ^ static_cast<uint8_t>(value)) * kFnvPrime;
  }
  return fingerprint;
}

bool AnalyzeRecords(const std::vector<std::byte>& bytes,
                    uint32_t record_count,
                    uint32_t& nonzero_record_count,
                    uint32_t& normalized_quaternion_count) {
  nonzero_record_count = 0;
  normalized_quaternion_count = 0;
  for (uint32_t record = 0; record < record_count; ++record) {
    const std::byte* source =
        bytes.data() + static_cast<size_t>(record) * kPaletteRecordBytes;
    float quaternion_norm_squared = 0.0f;
    bool nonzero = false;
    for (uint32_t component = 0; component < 7; ++component) {
      const float value = LoadBeF32(source + component * sizeof(float));
      if (!std::isfinite(value)) {
        return false;
      }
      const uint32_t bits = std::bit_cast<uint32_t>(value);
      nonzero |= (bits & 0x7FFFFFFFu) != 0;
      if (component < 4) {
        quaternion_norm_squared += value * value;
      }
    }
    nonzero_record_count += nonzero;
    normalized_quaternion_count +=
        quaternion_norm_squared >= kMinimumQuaternionNormSquared &&
        quaternion_norm_squared <= kMaximumQuaternionNormSquared;
  }
  return nonzero_record_count != 0 &&
         normalized_quaternion_count == record_count;
}

PaletteFetch CapturePaletteFetch(uint8_t* guest_base, uint32_t device) {
  PaletteFetch fetch;
  std::array<std::byte, sizeof(uint32_t) * 2> bytes{};
  if (!CopyGuest(
          guest_base, device,
          kFetchBankOffset +
              kPaletteFetchSlot * sizeof(uint32_t) * 2,
          bytes.data(), bytes.size())) {
    return fetch;
  }
  const uint32_t word0 = LoadBeU32(bytes.data());
  const uint32_t word1 = LoadBeU32(bytes.data() + sizeof(uint32_t));
  fetch.physical_address = word0 & ~uint32_t{3};
  fetch.endian = word1 & 0x3u;
  fetch.size = word1 & 0x03FFFFFCu;
  fetch.valid = (word0 & 0x3u) == 3 &&
                fetch.physical_address != 0 && fetch.size != 0;
  return fetch;
}

uint32_t PhysicalAddressForVirtualAlias(uint32_t virtual_address) {
  if (virtual_address < kHighPhysicalHeapBase) {
    return 0;
  }
  return virtual_address - kHighPhysicalHeapBase +
         kHighPhysicalHeapHostPageOffset;
}

bool IsD47VertexShader(uint64_t hash) {
  return std::ranges::find(kD47BackendVertexShaderHashes, hash) !=
         kD47BackendVertexShaderHashes.end();
}

uint32_t PaletteRecordsPerHalf(uint32_t fetch_byte_count) {
  constexpr uint32_t kBytesPerRecordPair = kPaletteRecordBytes * 2;
  if (fetch_byte_count == 0 ||
      fetch_byte_count % kBytesPerRecordPair != 0) {
    return 0;
  }
  const uint32_t record_count =
      fetch_byte_count / kBytesPerRecordPair;
  return record_count >= 1 &&
                 record_count <= kMaximumPaletteRecords
             ? record_count
             : 0;
}

bool IsExactD47BackendDraw(
    const rex::graphics::NativeGuestDrawContext& context) {
  return context.render_pass_key_valid && context.indexed &&
         context.guest_index_base_valid &&
         context.pixel_shader_hash == kD47PixelShaderHash &&
         IsD47VertexShader(context.vertex_shader_hash) &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.vertex_or_index_count != 0 &&
         context.guest_index_base != 0;
}

bool SameIdentity(const D47DrawIdentity& identity,
                  const rex::graphics::NativeGuestDrawContext& context) {
  return identity.primitive_type == context.primitive_type &&
         identity.index_count == context.vertex_or_index_count &&
         identity.index_base == context.guest_index_base;
}

void StoreWriteLocked(const PlayerPaletteWriteObservation& observation) {
  g_write_ledger[g_write_ledger_next] = observation;
  g_write_ledger_next =
      (g_write_ledger_next + 1) % g_write_ledger.size();
  g_write_ledger_count =
      std::min(g_write_ledger_count + 1, g_write_ledger.size());

  ++g_building_frame.valid_write_count;
  if (g_building_frame.write_count <
      g_building_frame.writes.size()) {
    g_building_frame.writes[g_building_frame.write_count++] = observation;
  } else {
    ++g_building_frame.dropped_write_count;
  }
}

uint32_t InferContiguousSecondHalfOwnerLocked(
    const PlayerPaletteWriteObservation& observation) {
  if (observation.player != 0 || g_write_sequence == 0) {
    return observation.player;
  }
  for (const PlayerPaletteWriteObservation& candidate : g_write_ledger) {
    if (!candidate.valid ||
        candidate.write_sequence != g_write_sequence ||
        candidate.frame_sequence != observation.frame_sequence ||
        candidate.player == 0 ||
        candidate.record_count != observation.record_count ||
        candidate.byte_count != observation.byte_count ||
        candidate.nonzero_record_count != observation.record_count ||
        candidate.normalized_quaternion_count != observation.record_count) {
      continue;
    }
    const uint64_t expected_second_half =
        static_cast<uint64_t>(candidate.physical_destination) +
        candidate.byte_count;
    if (expected_second_half == observation.physical_destination) {
      return candidate.player;
    }
  }
  return 0;
}

void IncrementFrameWriteMatch(PlayerPaletteWriteObserverFrame& frame,
                              uint64_t write_sequence) {
  for (size_t index = 0; index < frame.write_count; ++index) {
    if (frame.writes[index].write_sequence == write_sequence) {
      ++frame.writes[index].matched_draw_count;
      return;
    }
  }
}

void RecordExactD47MatchLocked(const D47DrawToken& token,
                               uint64_t vertex_shader_hash) {
  for (PlayerPaletteWriteObservation& write : g_write_ledger) {
    if (write.valid && write.write_sequence == token.write_sequence &&
        write.frame_sequence == token.generation) {
      ++write.matched_draw_count;
      break;
    }
  }

  if (token.generation == g_frame_sequence + 1) {
    ++g_building_frame.correlated_draw_count;
    ++g_building_frame.d47_contract_draw_count;
    IncrementFrameWriteMatch(g_building_frame, token.write_sequence);
  } else if (g_published_frame.sequence == token.generation) {
    ++g_published_frame.correlated_draw_count;
    ++g_published_frame.d47_contract_draw_count;
    IncrementFrameWriteMatch(g_published_frame, token.write_sequence);
  }

  if (!g_announced_first_correlation) {
    g_announced_first_correlation = true;
    REXLOG_INFO(
        "Table Tennis player palette write observer: exact D47 correlation "
        "write={} generation={} player={:08X} vf92={:08X}+{} "
        "vs={:016X} ps={:016X} primitive={} indices={} "
        "index_base={:08X} observer_only=true",
        token.write_sequence, token.generation, token.player,
        token.fetch.physical_address, token.fetch.size, vertex_shader_hash,
        kD47PixelShaderHash, token.identity.primitive_type,
        token.identity.index_count, token.identity.index_base);
  }
  if (!g_announced_d47_contract) {
    const PlayerPaletteWriteObservation* matched_write = nullptr;
    for (const PlayerPaletteWriteObservation& write : g_write_ledger) {
      if (write.valid && write.write_sequence == token.write_sequence &&
          write.frame_sequence == token.generation) {
        matched_write = &write;
        break;
      }
    }
    if (matched_write == nullptr) {
      const PlayerPaletteWriteObserverFrame* frame =
          g_published_frame.sequence == token.generation
              ? &g_published_frame
              : &g_building_frame;
      for (size_t index = 0; index < frame->write_count; ++index) {
        if (frame->writes[index].write_sequence == token.write_sequence) {
          matched_write = &frame->writes[index];
          break;
        }
      }
    }
    if (token.palette_proof.valid &&
        token.palette_proof.primary_cache_valid &&
        token.palette_proof.alternate_cache_valid) {
      g_announced_d47_contract = true;
      REXLOG_INFO(
          "Table Tennis player palette write observer: verified exact "
          "{}-record-per-half D47 cache contract player={:08X} "
          "vf92={:08X}+{} primary={:08X}/{:08X} "
          "alternate={:08X}/{:08X} cache_generation={} "
          "write={} fingerprint={:016X} write_corroborated={} "
          "cache_contract_valid=true active_vf92=true "
          "indexed_strip=true observer_only=true",
          token.palette_proof.record_count_per_half, token.player,
          token.fetch.physical_address, token.fetch.size,
          token.palette_proof.primary_drawable,
          token.palette_proof.primary_source,
          token.palette_proof.alternate_drawable,
          token.palette_proof.alternate_source,
          token.palette_proof.primary_cache_generation,
          matched_write != nullptr ? matched_write->write_sequence : 0,
          matched_write != nullptr ? matched_write->payload_fingerprint : 0,
          matched_write != nullptr);
    }
  }
}

bool MatchBackendEventLocked(D47DrawToken& token) {
  D47BackendEvent* best = nullptr;
  for (D47BackendEvent& event : g_backend_events) {
    if (!event.valid || event.generation != token.generation ||
        event.identity != token.identity) {
      continue;
    }
    if (best == nullptr || event.sequence < best->sequence) {
      best = &event;
    }
  }
  if (best == nullptr) {
    return false;
  }
  RecordExactD47MatchLocked(token, best->vertex_shader_hash);
  best->valid = false;
  token.valid = false;
  return true;
}

bool MatchDrawTokenLocked(const rex::graphics::NativeGuestDrawContext& context) {
  D47DrawToken* best = nullptr;
  for (D47DrawToken& token : g_draw_tokens) {
    if (!token.valid || token.generation < g_frame_sequence ||
        !SameIdentity(token.identity, context)) {
      continue;
    }
    if (best == nullptr || token.generation > best->generation ||
        (token.generation == best->generation &&
         token.sequence < best->sequence)) {
      best = &token;
    }
  }
  if (best == nullptr) {
    return false;
  }
  RecordExactD47MatchLocked(*best, context.vertex_shader_hash);
  best->valid = false;
  return true;
}

bool HasCurrentGenerationD47CacheProofLocked() {
  const uint64_t generation = g_frame_sequence + 1;
  for (const PaletteCacheBindingObservation& primary :
       g_primary_bindings) {
    if (!primary.valid || primary.frame_sequence != generation ||
        primary.player == 0) {
      continue;
    }
    const bool alternate_found = std::ranges::any_of(
        g_alternate_bindings,
        [&](const PaletteCacheBindingObservation& alternate) {
          return alternate.valid &&
                 alternate.frame_sequence == generation &&
                 alternate.drawable == primary.drawable &&
                 alternate.cache_generation == primary.cache_generation &&
                 alternate.physical_fetch_base ==
                     primary.physical_fetch_base &&
                 alternate.fetch_byte_count == primary.fetch_byte_count &&
                 alternate.record_count_per_half ==
                     primary.record_count_per_half;
        });
    if (alternate_found) {
      return true;
    }
  }
  return false;
}

bool CapturePaletteCacheBinding(
    uint8_t* guest_base, uint32_t drawable, uint32_t source,
    uint32_t requested_record_count, uint32_t cached_generation_offset,
    uint32_t cached_source_offset, uint32_t player,
    PaletteCacheBindingObservation& observation) {
  if (guest_base == nullptr || drawable == 0 || source == 0) {
    return false;
  }

  uint32_t generation = 0;
  uint32_t ring_size = 0;
  uint32_t palette_metadata = 0;
  uint32_t buffer_object = 0;
  uint32_t encoded_buffer_address = 0;
  uint32_t cached_generation = 0;
  uint32_t cached_source = 0;
  uint16_t metadata_record_count = 0;
  if (!ReadBeU32(guest_base, kAlternateGenerationAddress, 0, generation) ||
      !ReadBeU32(guest_base, kAlternateRingSizeAddress, 0, ring_size) ||
      ring_size == 0 ||
      !ReadBeU32(guest_base, drawable, kDrawablePaletteMetadataOffset,
                 palette_metadata) ||
      !ReadBeU16(guest_base, palette_metadata, kPaletteMatrixCountOffset,
                 metadata_record_count) ||
      metadata_record_count == 0 ||
      metadata_record_count > kMaximumPaletteRecords) {
    return false;
  }

  const uint32_t record_count =
      requested_record_count == std::numeric_limits<uint32_t>::max()
          ? metadata_record_count
          : requested_record_count;
  if (record_count != metadata_record_count ||
      record_count > kMaximumPaletteRecords) {
    return false;
  }

  const uint64_t ring_slot =
      static_cast<uint64_t>(generation % ring_size +
                            kPaletteRingFirstSlot) *
      sizeof(uint32_t);
  if (ring_slot > std::numeric_limits<uint32_t>::max() ||
      !ReadBeU32(guest_base, drawable, static_cast<uint32_t>(ring_slot),
                 buffer_object) ||
      buffer_object == 0 ||
      !ReadBeU32(guest_base, buffer_object, kPaletteBufferHandleOffset,
                 encoded_buffer_address) ||
      !ReadBeU32(guest_base, drawable, cached_generation_offset,
                 cached_generation) ||
      !ReadBeU32(guest_base, drawable, cached_source_offset, cached_source)) {
    return false;
  }

  observation.player = player;
  observation.drawable = drawable;
  observation.source = source;
  observation.cache_generation = generation;
  observation.physical_fetch_base =
      PhysicalAddressForVirtualAlias(encoded_buffer_address & ~uint32_t{3});
  observation.record_count_per_half = record_count;
  observation.fetch_byte_count =
      record_count * kPaletteRecordBytes * 2;
  observation.valid =
      observation.physical_fetch_base != 0 &&
      cached_generation == generation && cached_source == source;
  return observation.valid;
}

}  // namespace

void BeginPlayerPaletteWriteOwnerScope(uint32_t player) {
  if (g_owner_scope_depth < g_owner_scope_stack.size()) {
    if (PlayerPaletteWriteObserverEnabled() && player == 0) {
      player = CurrentOwner();
    }
    g_owner_scope_stack[g_owner_scope_depth] =
        PlayerPaletteWriteObserverEnabled() ? player : 0;
  }
  ++g_owner_scope_depth;
}

void EndPlayerPaletteWriteOwnerScope() {
  if (g_owner_scope_depth != 0) {
    --g_owner_scope_depth;
  }
}

void ObserveCompletedPlayerPaletteWrite(uint8_t* guest_base,
                                        uint32_t destination,
                                        uint32_t source,
                                        uint32_t record_count) {
  if (!PlayerPaletteWriteObserverEnabled()) {
    return;
  }

  {
    std::lock_guard lock(g_observer_mutex);
    ++g_building_frame.write_attempt_count;
  }

  if (guest_base == nullptr || destination == 0 || source == 0 ||
      record_count == 0 || record_count > kMaximumPaletteRecords) {
    std::lock_guard lock(g_observer_mutex);
    ++g_building_frame.rejected_write_count;
    if (g_rejected_write_diagnostic_logs++ <
        kMaximumWriteDiagnosticLogs) {
      REXLOG_INFO(
          "Table Tennis player palette write observer: rejected pack "
          "owner={:08X} destination={:08X} source={:08X} records={} "
          "reason=arguments observer_only=true",
          CurrentOwner(), destination, source, record_count);
    }
    return;
  }

  const size_t byte_count =
      static_cast<size_t>(record_count) * kPaletteRecordBytes;
  std::vector<std::byte> bytes;
  if (!CaptureStableBytes(guest_base, destination, byte_count, bytes)) {
    std::lock_guard lock(g_observer_mutex);
    ++g_building_frame.rejected_write_count;
    ++g_building_frame.guest_read_failure_count;
    if (g_rejected_write_diagnostic_logs++ <
        kMaximumWriteDiagnosticLogs) {
      REXLOG_INFO(
          "Table Tennis player palette write observer: rejected pack "
          "owner={:08X} destination={:08X} source={:08X} records={} "
          "reason=unstable-read observer_only=true",
          CurrentOwner(), destination, source, record_count);
    }
    return;
  }

  PlayerPaletteWriteObservation observation;
  observation.player = CurrentOwner();
  observation.destination = destination;
  observation.physical_destination =
      destination >= kHighPhysicalHeapBase
          ? PhysicalAddressForVirtualAlias(destination)
          : destination & kPhysicalAddressMask;
  observation.source = source;
  observation.record_count = record_count;
  observation.byte_count = static_cast<uint32_t>(byte_count);
  observation.payload_fingerprint = Fingerprint(bytes);
  observation.valid =
      AnalyzeRecords(bytes, record_count,
                     observation.nonzero_record_count,
                     observation.normalized_quaternion_count);

  std::lock_guard lock(g_observer_mutex);
  if (!observation.valid) {
    ++g_building_frame.rejected_write_count;
    if (g_rejected_write_diagnostic_logs++ <
        kMaximumWriteDiagnosticLogs) {
      REXLOG_INFO(
          "Table Tennis player palette write observer: rejected pack "
          "owner={:08X} destination={:08X} physical={:08X} source={:08X} "
          "records={} nonzero={} normalized={} reason=record-contract "
          "observer_only=true",
          observation.player, observation.destination,
          observation.physical_destination, observation.source,
          observation.record_count, observation.nonzero_record_count,
          observation.normalized_quaternion_count);
    }
    return;
  }
  observation.frame_sequence = g_frame_sequence + 1;
  const bool owner_was_missing = observation.player == 0;
  observation.player =
      InferContiguousSecondHalfOwnerLocked(observation);
  const bool owner_was_inferred =
      owner_was_missing && observation.player != 0;
  observation.write_sequence = ++g_write_sequence;
  StoreWriteLocked(observation);

  if (g_write_diagnostic_logs++ < kMaximumWriteDiagnosticLogs) {
    REXLOG_INFO(
        "Table Tennis player palette write observer: accepted pack "
        "write={} generation={} owner={:08X} destination={:08X} "
        "physical={:08X} source={:08X} records={} bytes={} inferred={} "
        "observer_only=true",
        observation.write_sequence, observation.frame_sequence,
        observation.player, observation.destination,
        observation.physical_destination, observation.source,
        observation.record_count, observation.byte_count,
        owner_was_inferred);
  }

  if (owner_was_inferred && observation.player != 0 &&
      !g_announced_inferred_second_half_owner) {
    g_announced_inferred_second_half_owner = true;
    REXLOG_INFO(
        "Table Tennis player palette write observer: proved contiguous "
        "second-half owner write={} player={:08X} physical={:08X} "
        "records={} bytes={} immediately_follows_first_half=true "
        "observer_only=true",
        observation.write_sequence, observation.player,
        observation.physical_destination, observation.record_count,
        observation.byte_count);
  }

  if (!g_announced_first_write) {
    g_announced_first_write = true;
    REXLOG_INFO(
        "Table Tennis player palette write observer: completed title pack "
        "write={} player={:08X} destination={:08X} physical={:08X} "
        "source={:08X} records={} bytes={} nonzero_records={} "
        "normalized_quaternions={} fingerprint={:016X} observer_only=true",
        observation.write_sequence, observation.player,
        observation.destination, observation.physical_destination,
        observation.source, observation.record_count,
        observation.byte_count, observation.nonzero_record_count,
        observation.normalized_quaternion_count,
        observation.payload_fingerprint);
  }
}

void ObserveCompletedPrimaryPlayerPaletteBinding(
    uint8_t* guest_base, uint32_t drawable, uint32_t source,
    uint32_t requested_record_count) {
  if (!PlayerPaletteWriteObserverEnabled()) {
    return;
  }

  PaletteCacheBindingObservation observation;
  if (!CapturePaletteCacheBinding(
          guest_base, drawable, source, requested_record_count,
          kPrimaryCachedGenerationOffset, kPrimaryCachedSourceOffset,
          CurrentOwner(), observation) ||
      observation.player == 0) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  observation.frame_sequence = g_frame_sequence + 1;
  g_primary_bindings[g_primary_binding_next] = observation;
  g_primary_binding_next =
      (g_primary_binding_next + 1) % g_primary_bindings.size();
  if (!g_announced_primary_cache_binding) {
    g_announced_primary_cache_binding = true;
    REXLOG_INFO(
        "Table Tennis player palette write observer: verified primary "
        "cache binding generation={} player={:08X} drawable={:08X} "
        "source={:08X} vf92={:08X}+{} records_per_half={} "
        "cache_current=true observer_only=true",
        observation.cache_generation, observation.player,
        observation.drawable, observation.source,
        observation.physical_fetch_base, observation.fetch_byte_count,
        observation.record_count_per_half);
  }
}

void ObserveCompletedAlternatePlayerPaletteBinding(
    uint8_t* guest_base, uint32_t drawable, uint32_t source) {
  if (!PlayerPaletteWriteObserverEnabled()) {
    return;
  }

  PaletteCacheBindingObservation observation;
  if (!CapturePaletteCacheBinding(
          guest_base, drawable, source,
          std::numeric_limits<uint32_t>::max(),
          kAlternateCachedGenerationOffset, kAlternateCachedSourceOffset,
          CurrentOwner(), observation)) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  observation.frame_sequence = g_frame_sequence + 1;
  g_alternate_bindings[g_alternate_binding_next] = observation;
  g_alternate_binding_next =
      (g_alternate_binding_next + 1) % g_alternate_bindings.size();
  if (!g_announced_alternate_cache_binding) {
    g_announced_alternate_cache_binding = true;
    REXLOG_INFO(
        "Table Tennis player palette write observer: verified alternate "
        "cache binding generation={} drawable={:08X} source={:08X} "
        "vf92={:08X}+{} records_per_half={} cache_current=true "
        "observer_only=true",
        observation.cache_generation, observation.drawable,
        observation.source, observation.physical_fetch_base,
        observation.fetch_byte_count,
        observation.record_count_per_half);
  }
}

void ObservePlayerPaletteWriteCatalogDraw(
    uint8_t* guest_base, const SceneCatalogDrawOccurrence& draw) {
  if (!PlayerPaletteWriteObserverEnabled() || guest_base == nullptr) {
    return;
  }

  if (!IsStructuralD47PlayerTitleDraw(draw)) {
    return;
  }

  const PaletteFetch fetch =
      CapturePaletteFetch(guest_base, draw.device);
  const uint32_t records_per_half =
      PaletteRecordsPerHalf(fetch.size);
  if (!fetch.valid || fetch.endian != kVertexEndian8In32 ||
      records_per_half == 0) {
    return;
  }

  const uint32_t index_base =
      PhysicalAddressForVirtualAlias(draw.mesh.index_buffer_alias);
  if (index_base == 0) {
    return;
  }

  const D47PaletteWritePairProof palette_proof =
      CurrentD47PaletteWritePairProof(
          draw.player, fetch.physical_address, fetch.size);
  if (!palette_proof.valid) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  const uint64_t generation = g_frame_sequence + 1;
  D47DrawToken token;
  token.sequence = ++g_draw_token_sequence;
  token.generation = generation;
  token.write_sequence = palette_proof.write_sequences[0];
  token.player = draw.player;
  token.fetch = fetch;
  token.palette_proof = palette_proof;
  token.identity = {
      .primitive_type = draw.primitive_type,
      .index_count = draw.submitted_index_count,
      .index_base = index_base,
  };
  token.valid = true;
  g_draw_tokens[g_draw_token_next] = token;
  D47DrawToken& stored = g_draw_tokens[g_draw_token_next];
  g_draw_token_next = (g_draw_token_next + 1) % g_draw_tokens.size();
  MatchBackendEventLocked(stored);
}

void ObservePlayerPaletteWriteBackendDraw(
    const rex::graphics::NativeGuestDrawContext& context) {
  if (!PlayerPaletteWriteObserverEnabled() ||
      !IsExactD47BackendDraw(context)) {
    return;
  }

  std::lock_guard lock(g_observer_mutex);
  if (MatchDrawTokenLocked(context)) {
    return;
  }
  // A backend event can precede the post-title catalog hook on another
  // thread, but only after this generation's two cache/buffer postconditions
  // exist. Never carry an unowned hash collision into the next frame.
  if (!HasCurrentGenerationD47CacheProofLocked()) {
    return;
  }

  D47BackendEvent event;
  event.sequence = ++g_backend_event_sequence;
  event.generation = g_frame_sequence + 1;
  event.vertex_shader_hash = context.vertex_shader_hash;
  event.identity = {
      .primitive_type = context.primitive_type,
      .index_count = context.vertex_or_index_count,
      .index_base = context.guest_index_base,
  };
  event.valid = true;
  g_backend_events[g_backend_event_next] = event;
  g_backend_event_next =
      (g_backend_event_next + 1) % g_backend_events.size();
}

D47PaletteWritePairProof D47PaletteWritePairProofForFrame(
    uint64_t frame_sequence, uint32_t player,
    uint32_t physical_fetch_base, uint32_t fetch_byte_count) {
  D47PaletteWritePairProof proof;
  const uint32_t record_count_per_half =
      PaletteRecordsPerHalf(fetch_byte_count);
  if (!PlayerPaletteWriteObserverEnabled() || frame_sequence == 0 ||
      player == 0 ||
      physical_fetch_base == 0 ||
      physical_fetch_base > kPhysicalAddressMask ||
      record_count_per_half == 0) {
    return proof;
  }

  const uint32_t half_bytes =
      record_count_per_half * kPaletteRecordBytes;
  const uint64_t second_half_base =
      static_cast<uint64_t>(physical_fetch_base) + half_bytes;
  const uint64_t fetch_end =
      static_cast<uint64_t>(physical_fetch_base) + fetch_byte_count;
  if (second_half_base > kPhysicalAddressMask ||
      fetch_end > static_cast<uint64_t>(kPhysicalAddressMask) + 1) {
    return proof;
  }

  std::lock_guard lock(g_observer_mutex);
  const uint64_t generation = frame_sequence;
  const PlayerPaletteWriteObservation* first_half = nullptr;
  for (const PlayerPaletteWriteObservation& write : g_write_ledger) {
    if (!write.valid || write.frame_sequence != generation ||
        write.player != player ||
        write.record_count != record_count_per_half ||
        write.byte_count != half_bytes ||
        write.nonzero_record_count != record_count_per_half ||
        write.normalized_quaternion_count != record_count_per_half) {
      continue;
    }
    if (write.physical_destination != physical_fetch_base) {
      continue;
    }
    if (first_half == nullptr ||
        write.write_sequence > first_half->write_sequence) {
      first_half = &write;
    }
  }

  const PaletteCacheBindingObservation* primary = nullptr;
  for (const PaletteCacheBindingObservation& candidate :
       g_primary_bindings) {
    if (!candidate.valid || candidate.frame_sequence != generation ||
        candidate.player != player ||
        candidate.physical_fetch_base != physical_fetch_base ||
        candidate.fetch_byte_count != fetch_byte_count ||
        candidate.record_count_per_half != record_count_per_half) {
      continue;
    }
    primary = &candidate;
    break;
  }

  const PaletteCacheBindingObservation* alternate = nullptr;
  for (const PaletteCacheBindingObservation& candidate :
       g_alternate_bindings) {
    if (!candidate.valid || candidate.frame_sequence != generation ||
        primary == nullptr || candidate.drawable != primary->drawable ||
        candidate.cache_generation != primary->cache_generation ||
        candidate.physical_fetch_base != physical_fetch_base ||
        candidate.fetch_byte_count != fetch_byte_count ||
        candidate.record_count_per_half != record_count_per_half) {
      continue;
    }
    alternate = &candidate;
    break;
  }
  if (primary == nullptr || alternate == nullptr) {
    if (g_cache_proof_diagnostic_logs++ <
        kMaximumCacheProofDiagnosticLogs) {
      const PaletteCacheBindingObservation* observed_primary = nullptr;
      const PaletteCacheBindingObservation* observed_alternate = nullptr;
      for (const PaletteCacheBindingObservation& candidate :
           g_primary_bindings) {
        if (candidate.valid && candidate.frame_sequence == generation) {
          observed_primary = &candidate;
          break;
        }
      }
      for (const PaletteCacheBindingObservation& candidate :
           g_alternate_bindings) {
        if (candidate.valid && candidate.frame_sequence == generation) {
          observed_alternate = &candidate;
          break;
        }
      }
      const PaletteCacheBindingObservation empty{};
      const PaletteCacheBindingObservation& seen_primary =
          observed_primary != nullptr ? *observed_primary : empty;
      const PaletteCacheBindingObservation& seen_alternate =
          observed_alternate != nullptr ? *observed_alternate : empty;
      REXLOG_INFO(
          "Table Tennis D47 cache proof miss: generation={} "
          "requested_player={:08X} requested_vf92={:08X}+{} "
          "requested_records={} exact_primary={} exact_alternate={} "
          "seen_primary_player={:08X} drawable={:08X} source={:08X} "
          "vf92={:08X}+{} records={} cache_generation={} "
          "seen_alternate_player={:08X} drawable={:08X} source={:08X} "
          "vf92={:08X}+{} records={} cache_generation={} "
          "observer_only=true",
          generation, player, physical_fetch_base, fetch_byte_count,
          record_count_per_half, primary != nullptr, alternate != nullptr,
          seen_primary.player, seen_primary.drawable, seen_primary.source,
          seen_primary.physical_fetch_base, seen_primary.fetch_byte_count,
          seen_primary.record_count_per_half,
          seen_primary.cache_generation, seen_alternate.player,
          seen_alternate.drawable, seen_alternate.source,
          seen_alternate.physical_fetch_base,
          seen_alternate.fetch_byte_count,
          seen_alternate.record_count_per_half,
          seen_alternate.cache_generation);
    }
    return proof;
  }

  proof.frame_sequence = generation;
  proof.player = player;
  proof.physical_fetch_base = physical_fetch_base;
  proof.fetch_byte_count = fetch_byte_count;
  proof.record_count_per_half = record_count_per_half;
  proof.primary_drawable = primary->drawable;
  proof.primary_source = primary->source;
  proof.primary_cache_generation = primary->cache_generation;
  proof.alternate_drawable = alternate->drawable;
  proof.alternate_source = alternate->source;
  proof.alternate_cache_generation = alternate->cache_generation;
  if (first_half != nullptr) {
    proof.write_sequences[0] = first_half->write_sequence;
    proof.payload_fingerprints[0] = first_half->payload_fingerprint;
  }
  proof.primary_cache_valid = true;
  proof.alternate_cache_valid = true;
  proof.valid = true;
  return proof;
}

D47PaletteWritePairProof CurrentD47PaletteWritePairProof(
    uint32_t player, uint32_t physical_fetch_base,
    uint32_t fetch_byte_count) {
  uint64_t frame_sequence = 0;
  {
    std::lock_guard lock(g_observer_mutex);
    frame_sequence = g_frame_sequence + 1;
  }
  return D47PaletteWritePairProofForFrame(
      frame_sequence, player, physical_fetch_base, fetch_byte_count);
}

void PlayerPaletteWriteObserverFrameEnd() {
  const bool enabled = PlayerPaletteWriteObserverEnabled();
  std::lock_guard lock(g_observer_mutex);
  ++g_frame_sequence;
  if (!enabled) {
    g_building_frame = {};
    g_published_frame = {};
    g_write_ledger = {};
    g_write_ledger_count = 0;
    g_write_ledger_next = 0;
    g_primary_bindings = {};
    g_primary_binding_next = 0;
    g_alternate_bindings = {};
    g_alternate_binding_next = 0;
    g_draw_tokens = {};
    g_draw_token_next = 0;
    g_backend_events = {};
    g_backend_event_next = 0;
    g_announced_first_write = false;
    g_announced_inferred_second_half_owner = false;
    g_announced_primary_cache_binding = false;
    g_announced_alternate_cache_binding = false;
    g_announced_first_correlation = false;
    g_announced_d47_contract = false;
    g_write_diagnostic_logs = 0;
    g_rejected_write_diagnostic_logs = 0;
    g_cache_proof_diagnostic_logs = 0;
    return;
  }

  g_building_frame.sequence = g_frame_sequence;
  g_published_frame = g_building_frame;
  g_building_frame = {};

  // Write and cache-binding observations are already bounded rings. Retain
  // prior generations until overwritten so asynchronous backend observers
  // can finalize frame N after the title has begun N+1.
  for (D47DrawToken& token : g_draw_tokens) {
    if (token.valid && token.generation < g_frame_sequence) {
      token = {};
    }
  }
  for (D47BackendEvent& event : g_backend_events) {
    if (event.valid && event.generation < g_frame_sequence) {
      event = {};
    }
  }
}

bool PlayerPaletteWriteObserverEnabled() {
  return REXCVAR_GET(
             tabletennis_native_player_palette_write_observer) ||
         D47PlayerObserverEnabled();
}

PlayerPaletteWriteObserverFrame
LatestPlayerPaletteWriteObserverFrame() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

}  // namespace tabletennis::native
