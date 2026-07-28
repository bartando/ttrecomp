#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/graphics/native_rhi.h>

namespace tabletennis::native {

struct TextureSnapshot;

struct Player6AEHostTextureMip {
  uint32_t level = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  uint32_t row_pitch_bytes = 0;
  size_t linear_offset = 0;
};

struct Player6AEHostTexture {
  rex::graphics::nrhi::Format format =
      rex::graphics::nrhi::Format::kUnknown;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t block_width = 0;
  uint32_t block_height = 0;
  uint32_t bytes_per_block = 0;
  std::array<rex::graphics::nrhi::Swizzle, 4> swizzle{};
  std::vector<Player6AEHostTextureMip> mips;
  std::vector<uint8_t> linear_blocks;

  bool valid() const;
};

enum class Player6AETextureDecodeFailure : uint32_t {
  kNone = 0,
  kInvalidSnapshot,
  kUnsupportedFormat,
  kInvalidMipLayout,
};

// Converts one already-untiled immutable 6AE snapshot into an NRHI upload
// payload. Color and BC formats remain byte-identical. Xenos D24S8 is
// converted to the R32_FLOAT sampling representation used by RexGlue's
// ordinary texture cache; stencil is intentionally not exposed to shaders.
Player6AETextureDecodeFailure DecodePlayer6AETexture(
    const TextureSnapshot& snapshot, Player6AEHostTexture& output);

const char* Player6AETextureDecodeFailureName(
    Player6AETextureDecodeFailure failure);

}  // namespace tabletennis::native
