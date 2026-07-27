#include "native/tabletennis_crowd_texture_decoder.h"

#include "native/tabletennis_crowd_snapshot.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
namespace texture_conversion = rex::graphics::texture_conversion;
namespace texture_util = rex::graphics::texture_util;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kCrowdDepth = 8;
constexpr uint32_t kCrowdSwizzle = 0x688;

bool IsProvenCrowdExtent(uint32_t value) {
  return value == 128 || value == 256;
}

xenos::xe_gpu_texture_fetch_t RebuildFetch(
    const std::array<uint32_t, 6>& words) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  return fetch;
}

bool IsExactProvenFetch(const xenos::xe_gpu_texture_fetch_t& fetch,
                        const rex::graphics::TextureInfo& info) {
  return fetch.type == xenos::FetchConstantType::kTexture &&
         info.dimension == xenos::DataDimension::k3D &&
         !info.is_stacked && info.is_tiled &&
         rex::graphics::GetBaseFormat(info.format) ==
             xenos::TextureFormat::k_DXT1 &&
         IsProvenCrowdExtent(info.width + 1) &&
         IsProvenCrowdExtent(info.height + 1) &&
         info.depth + 1 == kCrowdDepth &&
         fetch.swizzle == kCrowdSwizzle &&
         fetch.sign_x == xenos::TextureSign::kUnsigned &&
         fetch.sign_y == xenos::TextureSign::kUnsigned &&
         fetch.sign_z == xenos::TextureSign::kUnsigned &&
         fetch.sign_w == xenos::TextureSign::kUnsigned &&
         fetch.clamp_x == xenos::ClampMode::kRepeat &&
         fetch.clamp_y == xenos::ClampMode::kRepeat &&
         fetch.clamp_z == xenos::ClampMode::kRepeat &&
         fetch.mag_filter == xenos::TextureFilter::kLinear &&
         fetch.min_filter == xenos::TextureFilter::kLinear &&
         fetch.mip_filter == xenos::TextureFilter::kPoint &&
         fetch.vol_mag_filter == 0 && fetch.vol_min_filter == 0 &&
         fetch.aniso_filter == xenos::AnisoFilter::kMax_2_1 &&
         // This deprecated field has no dedicated "disabled" enum. The
         // traced fetch programs the zero encoding (k2x4Sym), but filtering
         // is still governed by the normal min/mag/aniso fields above.
         fetch.arbitrary_filter ==
             xenos::ArbitraryFilter::k2x4Sym &&
         fetch.exp_adjust == 0 && fetch.lod_bias == 0 &&
         fetch.grad_exp_adjust_h == 0 &&
         fetch.grad_exp_adjust_v == 0 &&
         fetch.mip_min_level == 0 && fetch.mip_max_level == 0 &&
         fetch.mip_address == 0 && !fetch.packed_mips &&
         !fetch.border_size;
}

uint8_t Expand5(uint32_t value) {
  return static_cast<uint8_t>((value << 3u) | (value >> 2u));
}

uint8_t Expand6(uint32_t value) {
  return static_cast<uint8_t>((value << 2u) | (value >> 4u));
}

std::array<uint8_t, 4> Decode565(uint16_t value) {
  return {
      Expand5((value >> 11u) & 31u),
      Expand6((value >> 5u) & 63u),
      Expand5(value & 31u),
      255u,
  };
}

std::array<uint8_t, 4> Interpolate(
    const std::array<uint8_t, 4>& first,
    const std::array<uint8_t, 4>& second,
    uint32_t first_weight, uint32_t second_weight,
    uint32_t divisor) {
  std::array<uint8_t, 4> result{};
  for (size_t component = 0; component < 3; ++component) {
    result[component] = static_cast<uint8_t>(
        (uint32_t(first[component]) * first_weight +
         uint32_t(second[component]) * second_weight) /
        divisor);
  }
  result[3] = 255;
  return result;
}

void DecodeBc1Block(const uint8_t* block, uint8_t* volume,
                    uint32_t width, uint32_t height, uint32_t z,
                    uint32_t block_x, uint32_t block_y) {
  const uint16_t color0 =
      uint16_t(block[0]) | (uint16_t(block[1]) << 8u);
  const uint16_t color1 =
      uint16_t(block[2]) | (uint16_t(block[3]) << 8u);
  std::array<std::array<uint8_t, 4>, 4> colors{};
  colors[0] = Decode565(color0);
  colors[1] = Decode565(color1);
  if (color0 > color1) {
    colors[2] = Interpolate(colors[0], colors[1], 2, 1, 3);
    colors[3] = Interpolate(colors[0], colors[1], 1, 2, 3);
  } else {
    colors[2] = Interpolate(colors[0], colors[1], 1, 1, 2);
    colors[3] = {0, 0, 0, 0};
  }

  const uint32_t selectors =
      uint32_t(block[4]) | (uint32_t(block[5]) << 8u) |
      (uint32_t(block[6]) << 16u) | (uint32_t(block[7]) << 24u);
  for (uint32_t y = 0; y < 4; ++y) {
    const uint32_t output_y = block_y * 4 + y;
    if (output_y >= height) {
      continue;
    }
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t output_x = block_x * 4 + x;
      if (output_x >= width) {
        continue;
      }
      const uint32_t selector =
          (selectors >> (2u * (y * 4 + x))) & 3u;
      const size_t destination =
          (static_cast<size_t>(z) * height * width +
           static_cast<size_t>(output_y) * width + output_x) *
          4;
      std::memcpy(volume + destination, colors[selector].data(), 4);
    }
  }
}

}  // namespace

CrowdTextureDecodeFailure DecodeCrowdTextureVolume(
    const CrowdTextureArrayPayload& snapshot,
    DecodedCrowdVolume& output) {
  output = {};
  if (!snapshot.valid() || snapshot.raw_bytes.empty()) {
    return CrowdTextureDecodeFailure::kInvalidSnapshot;
  }

  const xenos::xe_gpu_texture_fetch_t fetch =
      RebuildFetch(snapshot.fetch_words);
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info) ||
      !IsExactProvenFetch(fetch, info) || !snapshot.volume ||
      snapshot.dimension !=
          static_cast<uint32_t>(xenos::DataDimension::k3D) ||
      snapshot.stacked || !snapshot.tiled) {
    return CrowdTextureDecodeFailure::kUnprovenFetch;
  }

  const rex::graphics::FormatInfo* const format_info =
      rex::graphics::FormatInfo::Get(uint32_t(info.format));
  if (format_info == nullptr || format_info->block_width != 4 ||
      format_info->block_height != 4 ||
      format_info->bytes_per_block() != 8) {
    return CrowdTextureDecodeFailure::kUnsupportedLayout;
  }

  const uint32_t width = info.width + 1;
  const uint32_t height = info.height + 1;
  const uint32_t depth = info.depth + 1;
  const uint32_t width_blocks = (width + 3) / 4;
  const uint32_t height_blocks = (height + 3) / 4;
  const texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(
          info.dimension, fetch.pitch, width, height, depth,
          info.is_tiled, info.format, info.has_packed_mips,
          true, 0);
  if (layout.array_size != 1 || layout.base.row_pitch_bytes == 0 ||
      layout.base.z_slice_stride_block_rows == 0 ||
      layout.base.x_extent_blocks < width_blocks ||
      layout.base.y_extent_blocks < height_blocks ||
      layout.base.z_extent < depth) {
    return CrowdTextureDecodeFailure::kUnsupportedLayout;
  }

  const uint32_t bytes_per_block = format_info->bytes_per_block();
  const uint32_t input_pitch_blocks =
      layout.base.row_pitch_bytes / bytes_per_block;
  const uint32_t input_height_blocks =
      layout.base.z_slice_stride_block_rows;
  const uint32_t bytes_per_block_log2 =
      std::countr_zero(bytes_per_block);
  std::vector<uint8_t> linear_blocks(
      static_cast<size_t>(width_blocks) * height_blocks * depth *
      bytes_per_block);

  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height_blocks; ++y) {
      for (uint32_t x = 0; x < width_blocks; ++x) {
        const int32_t source_offset =
            texture_util::GetTiledOffset3D(
                static_cast<int32_t>(x), static_cast<int32_t>(y),
                static_cast<int32_t>(z), input_pitch_blocks,
                input_height_blocks, bytes_per_block_log2);
        if (source_offset < 0 ||
            static_cast<uint64_t>(source_offset) + bytes_per_block >
                snapshot.raw_bytes.size()) {
          return CrowdTextureDecodeFailure::kOutOfBounds;
        }
        const size_t destination =
            ((static_cast<size_t>(z) * height_blocks + y) *
                 width_blocks +
             x) *
            bytes_per_block;
        texture_conversion::CopySwapBlock(
            info.endianness, linear_blocks.data() + destination,
            snapshot.raw_bytes.data() + source_offset, bytes_per_block);
      }
    }
  }

  output.width = width;
  output.height = height;
  output.depth = depth;
  output.sampler = {
      .filter = nrhi::Filter::kAnisotropic,
      .address = nrhi::AddressMode::kWrap,
      .max_anisotropy = 2,
      .point_volume = true,
  };
  output.rgba8.resize(
      static_cast<size_t>(width) * height * depth * 4);
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height_blocks; ++y) {
      for (uint32_t x = 0; x < width_blocks; ++x) {
        const size_t source =
            ((static_cast<size_t>(z) * height_blocks + y) *
                 width_blocks +
             x) *
            bytes_per_block;
        DecodeBc1Block(
            linear_blocks.data() + source, output.rgba8.data(),
            width, height, z, x, y);
      }
    }
  }
  return output.valid() ? CrowdTextureDecodeFailure::kNone
                        : CrowdTextureDecodeFailure::kUnsupportedLayout;
}

const char* CrowdTextureDecodeFailureName(
    CrowdTextureDecodeFailure failure) {
  switch (failure) {
    case CrowdTextureDecodeFailure::kNone:
      return "none";
    case CrowdTextureDecodeFailure::kInvalidSnapshot:
      return "invalid_snapshot";
    case CrowdTextureDecodeFailure::kUnprovenFetch:
      return "unproven_fetch";
    case CrowdTextureDecodeFailure::kUnsupportedLayout:
      return "unsupported_layout";
    case CrowdTextureDecodeFailure::kOutOfBounds:
      return "out_of_bounds";
  }
  return "unknown";
}

}  // namespace tabletennis::native
