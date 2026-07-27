#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/graphics/native_rhi.h>

namespace tabletennis::native {

struct CrowdTextureArrayPayload;

struct CrowdSamplerContract {
  rex::graphics::nrhi::Filter filter =
      rex::graphics::nrhi::Filter::kLinear;
  rex::graphics::nrhi::AddressMode address =
      rex::graphics::nrhi::AddressMode::kWrap;
  uint32_t max_anisotropy = 1;
  bool point_volume = false;

  bool operator==(const CrowdSamplerContract&) const = default;
};

struct DecodedCrowdVolume {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  CrowdSamplerContract sampler{};
  std::vector<uint8_t> rgba8;

  bool valid() const {
    return width != 0 && height != 0 && depth == 8 &&
           rgba8.size() ==
               static_cast<size_t>(width) * height * depth * 4;
  }
};

enum class CrowdTextureDecodeFailure : uint32_t {
  kNone = 0,
  kInvalidSnapshot,
  kUnprovenFetch,
  kUnsupportedLayout,
  kOutOfBounds,
};

// Converts the title's true tiled 3D DXT1 mip-zero payload into a portable
// RGBA8 volume. This runs only while preparing an observer frame.
CrowdTextureDecodeFailure DecodeCrowdTextureVolume(
    const CrowdTextureArrayPayload& snapshot,
    DecodedCrowdVolume& output);

const char* CrowdTextureDecodeFailureName(
    CrowdTextureDecodeFailure failure);

}  // namespace tabletennis::native
