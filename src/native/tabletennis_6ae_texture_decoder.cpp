#include "native/tabletennis_6ae_texture_decoder.h"

#include "native/tabletennis_texture_snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <rex/graphics/pipeline/texture/info.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
namespace xenos = rex::graphics::xenos;

constexpr float kReciprocalDepth24Maximum = 1.0f / 16777215.0f;

nrhi::Format HostColorFormat(const TextureSnapshot& snapshot) {
  switch (rex::graphics::GetBaseFormat(
      static_cast<xenos::TextureFormat>(snapshot.format))) {
    case xenos::TextureFormat::k_8_8_8_8:
      return nrhi::Format::kR8G8B8A8_UNORM;
    case xenos::TextureFormat::k_DXT1:
      return nrhi::Format::kBC1_UNORM;
    case xenos::TextureFormat::k_DXT2_3:
      return nrhi::Format::kBC2_UNORM;
    case xenos::TextureFormat::k_DXT4_5:
      return nrhi::Format::kBC3_UNORM;
    default:
      return nrhi::Format::kUnknown;
  }
}

nrhi::Swizzle DecodeSwizzleComponent(uint32_t component) {
  return component <= static_cast<uint32_t>(nrhi::Swizzle::kOne)
             ? static_cast<nrhi::Swizzle>(component)
             : nrhi::Swizzle::kZero;
}

std::array<nrhi::Swizzle, 4> ColorSwizzle(uint32_t fetch_swizzle) {
  std::array<nrhi::Swizzle, 4> result{};
  for (uint32_t channel = 0; channel < result.size(); ++channel) {
    result[channel] =
        DecodeSwizzleComponent((fetch_swizzle >> (channel * 3)) & 7u);
  }
  return result;
}

std::array<nrhi::Swizzle, 4> DepthSwizzle(uint32_t fetch_swizzle) {
  // RexGlue converts Xenos D24S8 to a single R32_FLOAT component. Compose the
  // fetch swizzle with the format's RRRR base swizzle, instead of exposing
  // nonexistent host G/B/A channels.
  std::array<nrhi::Swizzle, 4> result{};
  for (uint32_t channel = 0; channel < result.size(); ++channel) {
    const uint32_t component =
        (fetch_swizzle >> (channel * 3)) & 7u;
    result[channel] =
        component < 4 ? nrhi::Swizzle::kX
        : component == 5 ? nrhi::Swizzle::kOne
                         : nrhi::Swizzle::kZero;
  }
  return result;
}

bool CopyColorPayload(const TextureSnapshot& snapshot,
                      Player6AEHostTexture& output) {
  output.block_width = snapshot.block_width;
  output.block_height = snapshot.block_height;
  output.bytes_per_block = snapshot.bytes_per_block;
  output.linear_blocks = snapshot.linear_blocks;
  output.mips.reserve(snapshot.mips.size());
  for (const TextureMipSnapshot& mip : snapshot.mips) {
    output.mips.push_back({
        .level = mip.level,
        .width = mip.width,
        .height = mip.height,
        .width_blocks = mip.width_blocks,
        .height_blocks = mip.height_blocks,
        .row_pitch_bytes = mip.row_pitch_bytes,
        .linear_offset = mip.linear_offset,
    });
  }
  return true;
}

bool ConvertDepthPayload(const TextureSnapshot& snapshot,
                         Player6AEHostTexture& output) {
  output.block_width = 1;
  output.block_height = 1;
  output.bytes_per_block = sizeof(float);
  output.mips.reserve(snapshot.mips.size());

  size_t output_size = 0;
  for (const TextureMipSnapshot& mip : snapshot.mips) {
    if (mip.width_blocks != mip.width ||
        mip.height_blocks != mip.height ||
        mip.row_pitch_bytes != mip.width * sizeof(uint32_t) ||
        mip.linear_offset > snapshot.linear_blocks.size()) {
      return false;
    }
    const size_t texel_count =
        static_cast<size_t>(mip.width) * mip.height;
    if (texel_count >
        (std::numeric_limits<size_t>::max() - output_size) / sizeof(float)) {
      return false;
    }
    output.mips.push_back({
        .level = mip.level,
        .width = mip.width,
        .height = mip.height,
        .width_blocks = mip.width,
        .height_blocks = mip.height,
        .row_pitch_bytes =
            static_cast<uint32_t>(mip.width * sizeof(float)),
        .linear_offset = output_size,
    });
    output_size += texel_count * sizeof(float);
  }
  output.linear_blocks.resize(output_size);

  for (size_t mip_index = 0; mip_index < snapshot.mips.size(); ++mip_index) {
    const TextureMipSnapshot& source_mip = snapshot.mips[mip_index];
    const Player6AEHostTextureMip& destination_mip =
        output.mips[mip_index];
    const size_t texel_count =
        static_cast<size_t>(source_mip.width) * source_mip.height;
    const size_t source_bytes = texel_count * sizeof(uint32_t);
    if (source_bytes >
        snapshot.linear_blocks.size() - source_mip.linear_offset) {
      return false;
    }
    const uint8_t* source =
        snapshot.linear_blocks.data() + source_mip.linear_offset;
    uint8_t* destination =
        output.linear_blocks.data() + destination_mip.linear_offset;
    for (size_t texel = 0; texel < texel_count; ++texel) {
      uint32_t packed = 0;
      std::memcpy(&packed, source + texel * sizeof(packed), sizeof(packed));
      const float depth =
          static_cast<float>((packed >> 8u) & 0x00FFFFFFu) *
          kReciprocalDepth24Maximum;
      std::memcpy(destination + texel * sizeof(depth), &depth,
                  sizeof(depth));
    }
  }
  return true;
}

}  // namespace

bool Player6AEHostTexture::valid() const {
  if (format == nrhi::Format::kUnknown || width == 0 || height == 0 ||
      block_width == 0 || block_height == 0 || bytes_per_block == 0 ||
      mips.empty() || linear_blocks.empty()) {
    return false;
  }
  size_t expected_offset = 0;
  for (uint32_t level = 0; level < mips.size(); ++level) {
    const Player6AEHostTextureMip& mip = mips[level];
    if (mip.level != level || mip.width == 0 || mip.height == 0 ||
        mip.width_blocks == 0 || mip.height_blocks == 0 ||
        mip.row_pitch_bytes != mip.width_blocks * bytes_per_block ||
        mip.linear_offset != expected_offset) {
      return false;
    }
    expected_offset +=
        static_cast<size_t>(mip.row_pitch_bytes) * mip.height_blocks;
  }
  return expected_offset == linear_blocks.size();
}

Player6AETextureDecodeFailure DecodePlayer6AETexture(
    const TextureSnapshot& snapshot, Player6AEHostTexture& output) {
  output = {};
  if (!snapshot.full_mip_chain() || snapshot.layer_count != 1) {
    return Player6AETextureDecodeFailure::kInvalidSnapshot;
  }
  output.width = snapshot.width;
  output.height = snapshot.height;

  const xenos::TextureFormat format = rex::graphics::GetBaseFormat(
      static_cast<xenos::TextureFormat>(snapshot.format));
  if (format == xenos::TextureFormat::k_24_8) {
    output.format = nrhi::Format::kR32_FLOAT;
    output.swizzle = DepthSwizzle(snapshot.fetch_swizzle);
    if (!ConvertDepthPayload(snapshot, output)) {
      output = {};
      return Player6AETextureDecodeFailure::kInvalidMipLayout;
    }
  } else {
    output.format = HostColorFormat(snapshot);
    if (output.format == nrhi::Format::kUnknown) {
      output = {};
      return Player6AETextureDecodeFailure::kUnsupportedFormat;
    }
    output.swizzle = ColorSwizzle(snapshot.fetch_swizzle);
    if (!CopyColorPayload(snapshot, output)) {
      output = {};
      return Player6AETextureDecodeFailure::kInvalidMipLayout;
    }
  }
  if (!output.valid()) {
    output = {};
    return Player6AETextureDecodeFailure::kInvalidMipLayout;
  }
  return Player6AETextureDecodeFailure::kNone;
}

const char* Player6AETextureDecodeFailureName(
    Player6AETextureDecodeFailure failure) {
  switch (failure) {
    case Player6AETextureDecodeFailure::kNone:
      return "none";
    case Player6AETextureDecodeFailure::kInvalidSnapshot:
      return "invalid_snapshot";
    case Player6AETextureDecodeFailure::kUnsupportedFormat:
      return "unsupported_format";
    case Player6AETextureDecodeFailure::kInvalidMipLayout:
      return "invalid_mip_layout";
  }
  return "unknown";
}

}  // namespace tabletennis::native
