#pragma once

#include "native/tabletennis_mesh_payload_probe.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct TableMeshSnapshot {
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 2>> texcoords0;
  std::vector<std::array<float, 2>> texcoords1;
  std::vector<std::array<float, 4>> colors;
  std::vector<uint16_t> indices;
  uint32_t stream_selector = 0;
  uint32_t source_vertex_alias = 0;
  uint32_t source_index_alias = 0;
  uint64_t vertex_fingerprint = 0;
  uint64_t index_fingerprint = 0;

  bool valid() const {
    return !positions.empty() && texcoords0.size() == positions.size() &&
           texcoords1.size() == positions.size() &&
           colors.size() == positions.size() &&
           !indices.empty() &&
           indices.size() % 3 == 0;
  }
};

// Capture the first structurally proven table/net payload into host-endian,
// immutable geometry. Streaming faults leave the capture unclaimed so a later
// submission can retry.
void TryCaptureTableMeshSnapshot(
    uint8_t* guest_base, const MeshPayloadDescriptor& descriptor);
bool HasTableMeshSnapshot();
std::shared_ptr<const TableMeshSnapshot> LatestTableMeshSnapshot();

}  // namespace tabletennis::native
