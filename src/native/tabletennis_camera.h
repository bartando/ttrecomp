#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tabletennis::native {

struct CapturedCamera {
  using Matrix = std::array<float, 16>;

  Matrix view{};
  Matrix projection{};
  Matrix view_projection{};
  Matrix inverse_view{};
  std::array<float, 3> position{};
  uint64_t constant_generation = 0;
  uint32_t source_model = 0;
  uint32_t source_shader_group = 0;
  uint32_t source_index_count = 0;
  uint32_t source_render_context = 0;
  uint32_t verified_candidates = 0;
  bool direct_context_verified = false;
  bool valid = false;
};

// Decode and cross-check the camera matrices staged by the title for a proven
// world draw. The bank is Xbox big-endian data containing 256 float4 rows.
// Table Tennis stages WorldView at c8, WorldViewProjection at c12, and
// ViewInverse at c16. The currently proven table/net draw has identity World,
// so these become View and ViewProjection; Projection is derived from
// ViewInverse * ViewProjection.
bool DecodeCameraConstantBank(std::span<const std::byte> bank,
                              uint32_t source_model,
                              uint32_t source_shader_group,
                              uint32_t source_index_count,
                              CapturedCamera& camera);

// Decode the authoritative title render-context block beginning at ctx+0x80:
// World, WorldView, WVP, ViewInverse, View, then Projection. This is the
// producer read used to verify the later draw-time constant-bank capture.
bool DecodeCameraRenderContext(std::span<const std::byte> context_block,
                               uint32_t source_render_context,
                               CapturedCamera& camera);

bool CameraMatchesRenderContext(const CapturedCamera& draw_camera,
                                const CapturedCamera& context_camera);

}  // namespace tabletennis::native
