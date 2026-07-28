#include "native/tabletennis_texture_snapshot.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_material_observer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>

namespace tabletennis::native {
namespace {

namespace texture_conversion = rex::graphics::texture_conversion;
namespace texture_util = rex::graphics::texture_util;
namespace xenos = rex::graphics::xenos;

// The title keeps several immutable HUD/material frame publications alive
// while the translated backend catches up. 256 entries was smaller than that
// legitimate live working set and turned the cache into a correctness gate.
constexpr size_t kMaximumSnapshots = 1024;
constexpr size_t kMaximumGuestTextureBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumSnapshotPayloadBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumSnapshotCacheBytes = 512 * 1024 * 1024;
constexpr uint32_t kPhysicalAliasBase = 0xA0000000u;
constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFFu;

std::mutex g_texture_snapshot_mutex;
std::vector<std::shared_ptr<const TextureSnapshot>> g_texture_snapshots;
bool g_logged_texture_cache_exhaustion = false;
uint64_t g_texture_snapshot_capture_count = 0;
uint64_t g_texture_snapshot_success_logs_suppressed = 0;

size_t CachedPayloadBytes() {
  return std::accumulate(
      g_texture_snapshots.begin(), g_texture_snapshots.end(), size_t{0},
      [](size_t total, const auto &snapshot) {
        return total + snapshot->linear_blocks.size();
      });
}

void EvictUnreferencedSnapshotsFor(size_t requested_bytes,
                                   bool adding_entry) {
  size_t cached_bytes = CachedPayloadBytes();
  for (auto snapshot = g_texture_snapshots.begin();
       snapshot != g_texture_snapshots.end() &&
       ((adding_entry && g_texture_snapshots.size() >= kMaximumSnapshots) ||
        requested_bytes >
            kMaximumSnapshotCacheBytes -
                std::min(cached_bytes, kMaximumSnapshotCacheBytes));) {
    // Immutable family/frame publications retain the snapshots they still
    // need. A cache-only entry is old observer data and can be dropped safely.
    if (snapshot->use_count() == 1) {
      cached_bytes -= (*snapshot)->linear_blocks.size();
      snapshot = g_texture_snapshots.erase(snapshot);
    } else {
      ++snapshot;
    }
  }
}

uint64_t Fingerprint(const uint8_t *bytes, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  if (bytes != nullptr) {
    for (size_t index = 0; index < size; ++index) {
      hash = (hash ^ bytes[index]) * 1099511628211ull;
    }
  }
  return hash;
}

uint64_t Fingerprint(const std::vector<uint8_t> &bytes) {
  return Fingerprint(bytes.data(), bytes.size());
}

bool CheckedAdd(size_t left, size_t right, size_t &result) {
  if (right > std::numeric_limits<size_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool CheckedMultiply(size_t left, size_t right, size_t &result) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool TryCopyGuestPhysical(uint8_t *guest_base, uint32_t physical_address,
                          void *destination, size_t size) {
  if (guest_base == nullptr || destination == nullptr ||
      physical_address == 0 || physical_address > kPhysicalAddressMask ||
      size == 0 ||
      size > static_cast<size_t>(kPhysicalAddressMask) + 1 - physical_address) {
    return false;
  }
  const uint32_t guest_address = kPhysicalAliasBase | physical_address;
  return GuestTryCopy(
      destination,
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address), size);
}

struct StableGuestStorage {
  uint32_t physical_address = 0;
  size_t size = 0;
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> verification;

  bool present() const { return physical_address != 0 && size != 0; }
};

bool PrepareGuestStorage(uint32_t physical_address, size_t size,
                         StableGuestStorage &storage) {
  if (physical_address == 0 || size == 0 || size > kMaximumGuestTextureBytes ||
      size > static_cast<size_t>(kPhysicalAddressMask) + 1 - physical_address) {
    return false;
  }
  storage.physical_address = physical_address;
  storage.size = size;
  storage.bytes.resize(size);
  storage.verification.resize(size);
  return true;
}

bool CopyGuestStorage(uint8_t *guest_base, StableGuestStorage &storage,
                      bool verification) {
  if (!storage.present()) {
    return true;
  }
  std::vector<uint8_t> &destination =
      verification ? storage.verification : storage.bytes;
  return TryCopyGuestPhysical(guest_base, storage.physical_address,
                              destination.data(), destination.size());
}

bool CaptureStableGuestStorage(uint8_t *guest_base, StableGuestStorage &base,
                               StableGuestStorage &mips) {
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    // Read the complete base/mip pair twice so publication cannot combine a
    // base from one mutation window with mips from another.
    if (!CopyGuestStorage(guest_base, base, false) ||
        !CopyGuestStorage(guest_base, mips, false) ||
        !CopyGuestStorage(guest_base, base, true) ||
        !CopyGuestStorage(guest_base, mips, true)) {
      return false;
    }
    if (base.bytes == base.verification && mips.bytes == mips.verification) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<const TextureSnapshot>
FindCaptured(uint32_t owner_shader, uint32_t encoded_handle,
             const std::array<uint32_t, 6> &fetch_words,
             TextureMipCapture mip_capture) {
  std::lock_guard lock(g_texture_snapshot_mutex);
  const auto found =
      std::find_if(g_texture_snapshots.begin(), g_texture_snapshots.end(),
                   [&](const auto &snapshot) {
                     return snapshot->owner_shader == owner_shader &&
                            snapshot->encoded_handle == encoded_handle &&
                            snapshot->fetch_words == fetch_words &&
                            (mip_capture == TextureMipCapture::kMip0Only ||
                             snapshot->full_mip_chain());
                   });
  return found == g_texture_snapshots.end() ? nullptr : *found;
}

bool DecodeMip(
    const rex::graphics::texture_util::TextureGuestLayout &guest_layout,
    const rex::graphics::FormatInfo *format_info, xenos::TextureFormat format,
    xenos::Endian endianness, bool is_tiled, uint32_t width, uint32_t height,
    uint32_t layer_count, uint32_t level,
    const StableGuestStorage &base_storage,
    const StableGuestStorage &mip_storage, TextureMipSnapshot &mip,
    std::vector<uint8_t> &linear_blocks) {
  if (format_info == nullptr || layer_count == 0 ||
      level > guest_layout.max_level) {
    return false;
  }

  const uint32_t block_width = format_info->block_width;
  const uint32_t block_height = format_info->block_height;
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  mip.level = level;
  mip.width = std::max(width >> level, 1u);
  mip.height = std::max(height >> level, 1u);
  mip.width_blocks = (mip.width + block_width - 1) / block_width;
  mip.height_blocks = (mip.height + block_height - 1) / block_height;
  mip.row_pitch_bytes = mip.width_blocks * bytes_per_block;
  if (!CheckedMultiply(mip.row_pitch_bytes, mip.height_blocks,
                       mip.layer_stride_bytes)) {
    return false;
  }
  size_t mip_bytes = 0;
  size_t output_end = 0;
  if (!CheckedMultiply(mip.layer_stride_bytes, layer_count, mip_bytes) ||
      !CheckedAdd(linear_blocks.size(), mip_bytes, output_end) ||
      output_end > kMaximumSnapshotPayloadBytes) {
    return false;
  }
  mip.linear_offset = linear_blocks.size();
  linear_blocks.resize(output_end);

  const bool is_base = level == 0;
  const uint32_t packed_level = guest_layout.packed_level;
  const uint32_t stored_level = is_base ? 0 : std::min(level, packed_level);
  if (!is_base && stored_level >= xenos::kTextureMaxMips) {
    return false;
  }
  const auto &level_layout =
      is_base ? guest_layout.base : guest_layout.mips[stored_level];
  const StableGuestStorage &storage = is_base ? base_storage : mip_storage;
  const size_t stored_level_offset =
      is_base ? 0 : guest_layout.mip_offsets_bytes[stored_level];
  if (!storage.present() || level_layout.row_pitch_bytes == 0 ||
      level_layout.array_slice_stride_bytes == 0 ||
      level_layout.row_pitch_bytes % bytes_per_block != 0 ||
      stored_level_offset > storage.bytes.size()) {
    return false;
  }

  uint32_t offset_x = 0;
  uint32_t offset_y = 0;
  uint32_t offset_z = 0;
  if (level >= packed_level &&
      !texture_util::GetPackedMipOffset(width, height, 1, format, level,
                                        offset_x, offset_y, offset_z)) {
    return false;
  }
  if (offset_z != 0) {
    return false;
  }

  const uint32_t input_pitch_blocks =
      level_layout.row_pitch_bytes / bytes_per_block;
  const uint32_t bytes_per_block_log2 =
      static_cast<uint32_t>(std::countr_zero(bytes_per_block));
  auto copy_block = [endianness](void *output, const void *input,
                                 size_t length) {
    texture_conversion::CopySwapBlock(endianness, output, input, length);
  };

  for (uint32_t layer = 0; layer < layer_count; ++layer) {
    size_t layer_offset = 0;
    size_t input_offset = 0;
    if (!CheckedMultiply(level_layout.array_slice_stride_bytes, layer,
                         layer_offset) ||
        !CheckedAdd(stored_level_offset, layer_offset, input_offset) ||
        input_offset > storage.bytes.size()) {
      return false;
    }
    const uint8_t *const input = storage.bytes.data() + input_offset;
    uint8_t *const output = linear_blocks.data() + mip.linear_offset +
                            mip.layer_stride_bytes * layer;

    size_t referenced_bytes = 0;
    if (is_tiled) {
      referenced_bytes = texture_util::GetTiledAddressUpperBound2D(
          offset_x + mip.width_blocks, offset_y + mip.height_blocks,
          input_pitch_blocks, bytes_per_block_log2);
    } else {
      size_t row_offset = 0;
      size_t last_row_offset = 0;
      if (!CheckedMultiply(offset_y, level_layout.row_pitch_bytes,
                           row_offset) ||
          !CheckedAdd(row_offset,
                      static_cast<size_t>(offset_x) * bytes_per_block,
                      row_offset) ||
          !CheckedMultiply(mip.height_blocks - 1, level_layout.row_pitch_bytes,
                           last_row_offset) ||
          !CheckedAdd(row_offset, last_row_offset, last_row_offset) ||
          !CheckedAdd(last_row_offset, mip.row_pitch_bytes, referenced_bytes)) {
        return false;
      }
    }
    if (referenced_bytes == 0 ||
        referenced_bytes > storage.bytes.size() - input_offset) {
      return false;
    }

    if (is_tiled) {
      texture_conversion::UntileInfo untile_info{
          .offset_x = offset_x,
          .offset_y = offset_y,
          .width = mip.width_blocks,
          .height = mip.height_blocks,
          .input_pitch = input_pitch_blocks,
          .output_pitch = mip.width_blocks,
          .input_format_info = format_info,
          .output_format_info = format_info,
          .copy_callback = copy_block,
      };
      texture_conversion::Untile(output, input, &untile_info);
    } else {
      const size_t first_block_offset =
          static_cast<size_t>(offset_y) * level_layout.row_pitch_bytes +
          static_cast<size_t>(offset_x) * bytes_per_block;
      for (uint32_t row = 0; row < mip.height_blocks; ++row) {
        copy_block(output + static_cast<size_t>(row) * mip.row_pitch_bytes,
                   input + first_block_offset +
                       static_cast<size_t>(row) * level_layout.row_pitch_bytes,
                   mip.row_pitch_bytes);
      }
    }
  }
  mip.payload_fingerprint =
      Fingerprint(linear_blocks.data() + mip.linear_offset, mip_bytes);
  return mip.valid(linear_blocks.size(), layer_count);
}

} // namespace

std::shared_ptr<const TextureSnapshot> CaptureTextureSnapshot(
    uint8_t *guest_base, uint32_t owner_shader, uint32_t encoded_handle,
    const std::array<uint32_t, 6> &fetch_words, TextureMipCapture mip_capture) {
  if (guest_base == nullptr) {
    return nullptr;
  }
  if (const auto cached = FindCaptured(owner_shader, encoded_handle,
                                       fetch_words, mip_capture)) {
    return cached;
  }

  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.dword_0 = fetch_words[0];
  fetch.dword_1 = fetch_words[1];
  fetch.dword_2 = fetch_words[2];
  fetch.dword_3 = fetch_words[3];
  fetch.dword_4 = fetch_words[4];
  fetch.dword_5 = fetch_words[5];
  const xenos::TextureFormat layout_format =
      rex::graphics::GetBaseFormat(fetch.format);
  const rex::graphics::FormatInfo *format_info =
      rex::graphics::FormatInfo::Get(uint32_t(layout_format));
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      format_info == nullptr || format_info->block_width == 0 ||
      format_info->block_height == 0 || format_info->bytes_per_block() == 0) {
    return nullptr;
  }

  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info) || info.is_stacked ||
      info.memory.base_address == 0) {
    return nullptr;
  }
  const bool is_2d = info.dimension == xenos::DataDimension::k2DOrStacked;
  const bool is_cube = info.dimension == xenos::DataDimension::kCube;
  if (!is_2d && !is_cube) {
    return nullptr;
  }

  uint32_t width_minus_one = 0;
  uint32_t height_minus_one = 0;
  uint32_t depth_or_array_size_minus_one = 0;
  uint32_t base_page = 0;
  uint32_t mip_page = 0;
  uint32_t descriptor_mip_min_level = 0;
  uint32_t descriptor_mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, &width_minus_one, &height_minus_one,
      &depth_or_array_size_minus_one, &base_page, &mip_page,
      &descriptor_mip_min_level, &descriptor_mip_max_level);
  const uint32_t width = width_minus_one + 1;
  const uint32_t height = height_minus_one + 1;
  const uint32_t layer_count =
      is_cube ? depth_or_array_size_minus_one + 1 : 1;
  const uint32_t maximum_dimension = std::max(width, height);
  const uint32_t maximum_mip_level =
      maximum_dimension == 0
          ? 0
          : static_cast<uint32_t>(std::bit_width(maximum_dimension) - 1);
  if (base_page == 0 || descriptor_mip_min_level != 0 ||
      (is_cube && layer_count != 6) ||
      descriptor_mip_max_level > maximum_mip_level ||
      descriptor_mip_max_level >= xenos::kTextureMaxMips ||
      (mip_capture == TextureMipCapture::kFullFetchRange &&
       descriptor_mip_max_level != 0 && mip_page == 0)) {
    return nullptr;
  }

  const uint32_t block_width = format_info->block_width;
  const uint32_t block_height = format_info->block_height;
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if ((bytes_per_block & (bytes_per_block - 1)) != 0) {
    return nullptr;
  }

  const uint32_t captured_mip_max_level =
      mip_capture == TextureMipCapture::kFullFetchRange
          ? descriptor_mip_max_level
          : 0;
  const texture_util::TextureGuestLayout guest_layout =
      texture_util::GetGuestTextureLayout(info.dimension, fetch.pitch, width,
                                          height, layer_count, info.is_tiled,
                                          layout_format, info.has_packed_mips,
                                          true, captured_mip_max_level);
  if (guest_layout.array_size != layer_count ||
      guest_layout.max_level != captured_mip_max_level ||
      guest_layout.base.level_data_extent_bytes == 0 ||
      guest_layout.base.level_data_extent_bytes > kMaximumGuestTextureBytes ||
      (captured_mip_max_level != 0 &&
       (guest_layout.mips_total_extent_bytes == 0 ||
        guest_layout.mips_total_extent_bytes > kMaximumGuestTextureBytes))) {
    return nullptr;
  }

  StableGuestStorage base_storage;
  StableGuestStorage mip_storage;
  const uint32_t base_address =
      base_page << xenos::kTextureSubresourceAlignmentBytesLog2;
  const uint32_t mip_address =
      mip_page << xenos::kTextureSubresourceAlignmentBytesLog2;
  if (!PrepareGuestStorage(base_address,
                           guest_layout.base.level_data_extent_bytes,
                           base_storage)) {
    return nullptr;
  }
  if (captured_mip_max_level != 0 &&
      !PrepareGuestStorage(mip_address,
                           guest_layout.mips_total_extent_bytes, mip_storage)) {
    return nullptr;
  }
  if (!CaptureStableGuestStorage(guest_base, base_storage, mip_storage)) {
    return nullptr;
  }

  auto snapshot = std::make_shared<TextureSnapshot>();
  snapshot->owner_shader = owner_shader;
  snapshot->encoded_handle = encoded_handle;
  snapshot->fetch_words = fetch_words;
  snapshot->width = width;
  snapshot->height = height;
  snapshot->dimension = static_cast<uint32_t>(info.dimension);
  snapshot->layer_count = layer_count;
  snapshot->format = uint32_t(fetch.format);
  snapshot->endianness = uint32_t(info.endianness);
  snapshot->fetch_swizzle = fetch.swizzle;
  snapshot->block_width = block_width;
  snapshot->block_height = block_height;
  snapshot->bytes_per_block = bytes_per_block;
  snapshot->descriptor_mip_min_level = descriptor_mip_min_level;
  snapshot->descriptor_mip_max_level = descriptor_mip_max_level;
  snapshot->base_address = base_address;
  snapshot->mip_address = mip_address;
  snapshot->is_tiled = info.is_tiled;
  snapshot->has_packed_mips = info.has_packed_mips;
  snapshot->mips.reserve(captured_mip_max_level + 1);
  for (uint32_t level = 0; level <= captured_mip_max_level; ++level) {
    TextureMipSnapshot mip;
    if (!DecodeMip(guest_layout, format_info, layout_format, info.endianness,
                   info.is_tiled, width, height, layer_count, level,
                   base_storage, mip_storage, mip, snapshot->linear_blocks)) {
      return nullptr;
    }
    snapshot->mips.push_back(std::move(mip));
  }
  snapshot->row_pitch_bytes = snapshot->mips.front().row_pitch_bytes;
  snapshot->payload_fingerprint = Fingerprint(snapshot->linear_blocks);
  if (!snapshot->valid() ||
      (mip_capture == TextureMipCapture::kFullFetchRange &&
       !snapshot->full_mip_chain())) {
    return nullptr;
  }

  uint64_t capture_count = 0;
  uint64_t suppressed_success_logs = 0;
  bool log_capture = false;
  {
    std::lock_guard lock(g_texture_snapshot_mutex);
    const auto duplicate = std::find_if(
        g_texture_snapshots.begin(), g_texture_snapshots.end(),
        [&](const auto &existing) {
          return existing->owner_shader == snapshot->owner_shader &&
                 existing->encoded_handle == snapshot->encoded_handle &&
                 existing->fetch_words == snapshot->fetch_words;
        });
    const bool upgrades_mip0 =
        duplicate != g_texture_snapshots.end() &&
        mip_capture == TextureMipCapture::kFullFetchRange &&
        snapshot->full_mip_chain() && !(*duplicate)->full_mip_chain();
    if (duplicate != g_texture_snapshots.end() && !upgrades_mip0) {
      return *duplicate;
    }
    const bool adds_entry = duplicate == g_texture_snapshots.end();
    if (adds_entry) {
      EvictUnreferencedSnapshotsFor(snapshot->linear_blocks.size(), true);
    }
    size_t cached_bytes = CachedPayloadBytes();
    if (upgrades_mip0) {
      cached_bytes -= (*duplicate)->linear_blocks.size();
    }
    if ((adds_entry && g_texture_snapshots.size() >= kMaximumSnapshots) ||
        snapshot->linear_blocks.size() >
            kMaximumSnapshotCacheBytes -
                std::min(cached_bytes, kMaximumSnapshotCacheBytes)) {
      if (!g_logged_texture_cache_exhaustion) {
        g_logged_texture_cache_exhaustion = true;
        REXLOG_ERROR("Table Tennis texture snapshot: cache budget exhausted "
                     "(entries={}/{}, bytes={}/{}, requested={}); "
                     "publishing uncached snapshots and suppressing further "
                     "failures",
                     g_texture_snapshots.size(), kMaximumSnapshots,
                     cached_bytes, kMaximumSnapshotCacheBytes,
                     snapshot->linear_blocks.size());
      }
      // The cache is an optimization. A fully decoded, double-read-verified
      // immutable snapshot remains valid even if the cache can't retain it.
      return snapshot;
    }
    if (upgrades_mip0) {
      *duplicate = snapshot;
    } else {
      g_texture_snapshots.push_back(snapshot);
    }
    capture_count = ++g_texture_snapshot_capture_count;
    log_capture = capture_count <= 8 || (capture_count % 256) == 0;
    if (log_capture) {
      suppressed_success_logs = g_texture_snapshot_success_logs_suppressed;
      g_texture_snapshot_success_logs_suppressed = 0;
    } else {
      ++g_texture_snapshot_success_logs_suppressed;
    }
  }

  if (log_capture) {
    REXLOG_INFO(
        "Table Tennis texture snapshot: captured milestone={} "
        "suppressed_success_logs={} owner={:08X} handle={:08X} "
        "{}x{} layers={} dimension={} format={} levels=0-{} "
        "packed={} tiled={} row_bytes={} payload={:016X} observer-only",
        capture_count, suppressed_success_logs, snapshot->owner_shader,
        snapshot->encoded_handle, snapshot->width, snapshot->height,
        snapshot->layer_count, snapshot->dimension, snapshot->format,
        snapshot->mips.size() - 1, snapshot->has_packed_mips,
        snapshot->is_tiled, snapshot->row_pitch_bytes,
        snapshot->payload_fingerprint);
  }
  return snapshot;
}

void TryCaptureTableTextureSnapshot(
    uint8_t *guest_base,
    const MaterialTextureParameterObservation &observation) {
  if (guest_base == nullptr || !observation.valid ||
      !observation.gpu_binding_words_stable ||
      !observation.decoded_fetch.valid) {
    return;
  }
  CaptureTextureSnapshot(guest_base, observation.owner_shader,
                         observation.encoded_handle,
                         observation.texture_fetch_words);
}

std::vector<std::shared_ptr<const TextureSnapshot>> LatestTextureSnapshots() {
  std::lock_guard lock(g_texture_snapshot_mutex);
  return g_texture_snapshots;
}

std::vector<std::shared_ptr<const TableTextureSnapshot>>
LatestTableTextureSnapshots() {
  return LatestTextureSnapshots();
}

} // namespace tabletennis::native
