#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct MaterialTextureParameterObservation;

enum class TextureMipCapture : uint32_t {
  // Preserve the original generic snapshot contract for callers that only
  // replay mip 0.
  kMip0Only = 0,
  // Require every level selected by the authoritative fetch descriptor.
  // Capture fails closed if any base/mip storage range can't be proven.
  kFullFetchRange = 1,
};

struct TextureMipSnapshot {
  uint32_t level = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  uint32_t row_pitch_bytes = 0;
  size_t linear_offset = 0;
  size_t layer_stride_bytes = 0;
  uint64_t payload_fingerprint = 0;

  bool valid(size_t payload_size, uint32_t layer_count) const {
    if (width == 0 || height == 0 || width_blocks == 0 || height_blocks == 0 ||
        row_pitch_bytes == 0 || layer_stride_bytes == 0 || layer_count == 0 ||
        layer_stride_bytes !=
            static_cast<size_t>(row_pitch_bytes) * height_blocks) {
      return false;
    }
    const size_t remaining =
        linear_offset <= payload_size ? payload_size - linear_offset : 0;
    return layer_stride_bytes <= remaining / static_cast<size_t>(layer_count);
  }
};

struct TextureSnapshot {
  uint32_t owner_shader = 0;
  uint32_t encoded_handle = 0;
  std::array<uint32_t, 6> fetch_words{};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dimension = 0;
  uint32_t layer_count = 0;
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t fetch_swizzle = 0;
  uint32_t block_width = 0;
  uint32_t block_height = 0;
  uint32_t bytes_per_block = 0;
  uint32_t row_pitch_bytes = 0;
  uint32_t descriptor_mip_min_level = 0;
  uint32_t descriptor_mip_max_level = 0;
  uint32_t base_address = 0;
  uint32_t mip_address = 0;
  bool is_tiled = false;
  bool has_packed_mips = false;
  uint64_t payload_fingerprint = 0;
  std::vector<TextureMipSnapshot> mips;
  std::vector<uint8_t> linear_blocks;

  bool valid() const {
    return width != 0 && height != 0 && layer_count != 0 && block_width != 0 &&
           block_height != 0 && bytes_per_block != 0 && row_pitch_bytes != 0 &&
           !mips.empty() && mips.front().level == 0 &&
           mips.front().width == width && mips.front().height == height &&
           mips.front().row_pitch_bytes == row_pitch_bytes &&
           mips.front().valid(linear_blocks.size(), layer_count);
  }

  bool full_mip_chain() const {
    if (!valid() || descriptor_mip_min_level != 0 ||
        descriptor_mip_max_level + 1 != mips.size()) {
      return false;
    }
    size_t expected_offset = 0;
    for (uint32_t level = 0; level < mips.size(); ++level) {
      if (mips[level].level != level ||
          mips[level].linear_offset != expected_offset ||
          !mips[level].valid(linear_blocks.size(), layer_count)) {
        return false;
      }
      expected_offset += mips[level].layer_stride_bytes * layer_count;
    }
    return expected_offset == linear_blocks.size();
  }
};

using TableTextureSnapshot = TextureSnapshot;

// Capture and untile an authoritative six-dword texture fetch. The default
// preserves the original mip-0-only behavior. kFullFetchRange copies and
// validates every descriptor-selected level, including packed tails and all
// six cube faces. The returned immutable snapshot is cached by owner, handle,
// fetch words and capture completeness. Two identical guest copies of every
// referenced storage range are required before publishing.
std::shared_ptr<const TextureSnapshot> CaptureTextureSnapshot(
    uint8_t *guest_base, uint32_t owner_shader, uint32_t encoded_handle,
    const std::array<uint32_t, 6> &fetch_words,
    TextureMipCapture mip_capture = TextureMipCapture::kMip0Only);

// Capture and untile mip 0 from a texture already joined to a proven table
// material. Two identical guest copies are required before publishing.
void TryCaptureTableTextureSnapshot(
    uint8_t *guest_base,
    const MaterialTextureParameterObservation &observation);

std::vector<std::shared_ptr<const TextureSnapshot>> LatestTextureSnapshots();

std::vector<std::shared_ptr<const TableTextureSnapshot>>
LatestTableTextureSnapshots();

} // namespace tabletennis::native
