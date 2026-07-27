#pragma once

#include "native/tabletennis_vertex_declaration.h"

#include <array>
#include <cstdint>

namespace tabletennis::native {

struct MeshPayloadDescriptor {
  uint32_t stream_selector = 0;
  uint32_t vertex_alias = 0;
  uint32_t vertex_bytes = 0;
  uint32_t vertex_stride = 0;
  uint8_t vertex_endian = 0;
  uint32_t index_alias = 0;
  uint32_t index_bytes = 0;
  uint32_t submitted_index_count = 0;
  uint32_t index_element_size = 0;
  bool index_is_32_bit = false;
  VertexDeclarationProbe declaration{};
};

struct MeshPayloadProbe {
  uint32_t stream_selector = 0;
  uint32_t vertex_alias = 0;
  uint32_t vertex_bytes = 0;
  uint32_t vertex_stride = 0;
  uint32_t vertex_count = 0;
  uint32_t index_alias = 0;
  uint32_t index_bytes = 0;
  uint32_t index_count = 0;
  uint32_t minimum_index = 0;
  uint32_t maximum_index = 0;
  uint32_t out_of_range_indices = 0;
  uint32_t degenerate_triangles = 0;
  uint32_t finite_positions = 0;
  uint32_t non_finite_positions = 0;
  uint32_t non_finite_position_w = 0;
  std::array<float, 3> bounds_min{};
  std::array<float, 3> bounds_max{};
  std::array<float, 4> first_position{};
  uint64_t vertex_fingerprint = 0;
  uint64_t index_fingerprint = 0;
  uint32_t copy_failures = 0;
  bool valid = false;
};

// Copies and validates one draw-time VB/IB payload. This is observer-only and
// returns telemetry; it does not retain data or issue native GPU work.
MeshPayloadProbe ProbeMeshPayload(uint8_t* base,
                                  const MeshPayloadDescriptor& descriptor);

}  // namespace tabletennis::native
