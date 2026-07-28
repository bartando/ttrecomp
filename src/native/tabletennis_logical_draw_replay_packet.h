#pragma once

#include "native/tabletennis_texture_snapshot.h"
#include "native/tabletennis_translated_shader_artifact_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct PlayerA406DrawSnapshot;
struct PlayerA406FrameSnapshot;
struct VenueFullFamilyDrawSnapshot;

enum class ReplayShaderStage : uint8_t {
  kVertex,
  kPixel,
};

// One contiguous title constant-bank range. Values are copied rather than
// retaining a device-bank pointer because later guest draws overwrite it.
struct LogicalReplayConstantRange {
  ReplayShaderStage stage = ReplayShaderStage::kVertex;
  uint32_t first_vector = 0;
  std::vector<float> values;

  bool valid() const {
    return !values.empty() && values.size() % 4 == 0;
  }
};

struct LogicalReplayVertexElement {
  uint16_t stream = 0;
  uint16_t byte_offset = 0;
  uint32_t packed_type = 0;
  uint8_t method = 0;
  uint8_t usage = 0;
  uint8_t usage_index = 0;
};

struct LogicalReplayVertexDeclaration {
  uint32_t element_count = 0;
  uint32_t max_stream = 0;
  uint64_t stream_mask_lo = 0;
  uint64_t stream_mask_hi = 0;
  std::vector<LogicalReplayVertexElement> elements;

  bool valid() const {
    return element_count != 0 && elements.size() == element_count;
  }
};

struct LogicalReplayGeometry {
  uint32_t primitive_type = 0;
  uint32_t vertex_stride = 0;
  uint32_t vertex_endian = 0;
  uint32_t vertex_count = 0;
  uint32_t index_element_size = 0;
  uint32_t index_count = 0;
  uint32_t minimum_index = 0;
  uint32_t maximum_index = 0;
  uint64_t vertex_fingerprint = 0;
  uint64_t index_fingerprint = 0;
  std::vector<uint8_t> vertex_bytes;
  std::vector<uint8_t> index_bytes;

  bool valid() const;
};

// A descriptor is retained even when texels have not yet been promoted to an
// immutable snapshot. This distinction is part of readiness, not guessed.
struct LogicalReplayTextureBinding {
  uint32_t slot = 0;
  std::array<uint32_t, 6> fetch_words{};
  std::shared_ptr<const TextureSnapshot> texture;

  bool descriptor_valid() const {
    return (fetch_words[0] & 0x3u) == 2 && fetch_words[1] != 0;
  }
  bool payload_owned() const {
    return texture != nullptr && texture->valid() &&
           texture->fetch_words == fetch_words;
  }
};

struct LogicalReplayFixedState {
  uint32_t render_pass_key = 0;
  uint32_t rb_color_info_0 = 0;
  uint32_t rb_depth_info = 0;
  uint32_t rb_surface_info = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t color_edram_base = 0;
  uint32_t depth_edram_base = 0;
  uint32_t surface_pitch = 0;
  uint32_t edram_mode = 0;
  uint32_t normalized_depth_control = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t color_control = 0;
  uint32_t blend_control_0 = 0;
  uint32_t rasterizer_mode_control = 0;
  uint32_t primitive_restart_index = 0;
  std::array<uint32_t, 4> color_attachment_formats{};
  uint32_t color_attachment_count = 0;
  uint32_t depth_attachment_format = 0;
  uint32_t stencil_attachment_format = 0;
  uint32_t sample_count = 0;
  uint64_t sample_mask = 0;
  bool primitive_restart_enabled = false;
  bool source_contract_valid = false;
  bool render_target_state_valid = false;
  bool rasterizer_mode_control_valid = false;
  // Exact translated-pipeline dynamic state, when this packet was joined
  // against a backend replay token rather than only a legacy family contract.
  std::array<float, 6> viewport{};
  std::array<int32_t, 2> scissor_offset{};
  std::array<uint32_t, 2> scissor_extent{};
  float depth_bias_constant_factor = 0.0f;
  float depth_bias_slope_factor = 0.0f;
  std::array<float, 4> blend_constants{};
  uint32_t stencil_compare_mask_front = 0;
  uint32_t stencil_compare_mask_back = 0;
  uint32_t stencil_write_mask_front = 0;
  uint32_t stencil_write_mask_back = 0;
  uint32_t stencil_reference_front = 0;
  uint32_t stencil_reference_back = 0;
  bool translated_token_state_valid = false;
  uint64_t backend_frame_sequence = 0;
  uint64_t opaque_replay_token = 0;
  uint64_t pipeline_generation = 0;
  uint64_t pipeline_layout_generation = 0;
  uint32_t host_primitive_type = 0;
  uint32_t processed_index_buffer_type = 0;
  uint32_t host_index_format = 0;
  uint32_t host_index_count = 0;
  bool translated_resources_stable = false;

  bool valid() const;
};

enum class LogicalReplayMissing : uint32_t {
  kNone = 0,
  // The A406 observer currently publishes shader identity, but no immutable
  // translated artifact has been joined to this logical draw yet.
  kTranslatedVertexProgram = 1u << 0,
  kTranslatedPixelProgram = 1u << 1,
  // One or more live descriptors do not yet own descriptor-selected texels.
  kTexturePayload = 1u << 2,
  // Shader hash alone cannot select a translated artifact. These remain set
  // until the exact draw-selected translation modifications are observed.
  kVertexModificationIdentity = 1u << 3,
  kPixelModificationIdentity = 1u << 4,
  // Exact artifacts don't by themselves prove replayable pipeline, dynamic
  // state and resource lifetime.
  kTranslatedReplayState = 1u << 5,
};

constexpr LogicalReplayMissing operator|(LogicalReplayMissing lhs,
                                         LogicalReplayMissing rhs) {
  return static_cast<LogicalReplayMissing>(static_cast<uint32_t>(lhs) |
                                           static_cast<uint32_t>(rhs));
}

struct LogicalDrawReplayPacket {
  uint64_t frame_sequence = 0;
  uint32_t ordinal = 0;
  uint32_t player = 0;
  uint32_t geometry_index = 0;
  uint32_t lod = 0;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint64_t vertex_shader_modification = 0;
  uint64_t pixel_shader_modification = 0;
  bool vertex_shader_modification_valid = false;
  bool pixel_shader_modification_valid = false;
  std::shared_ptr<const TranslatedShaderArtifact> vertex_shader;
  std::shared_ptr<const TranslatedShaderArtifact> pixel_shader;
  LogicalReplayVertexDeclaration vertex_declaration;
  LogicalReplayGeometry geometry;
  std::vector<LogicalReplayConstantRange> constants;
  std::vector<LogicalReplayTextureBinding> textures;
  LogicalReplayFixedState fixed_state;
  std::array<float, 16> world{};
  std::array<float, 16> world_view_projection{};
  LogicalReplayMissing missing = LogicalReplayMissing::kNone;

  // Complete immutable preparation evidence. This deliberately differs from
  // submission_ready(): an observer packet may be valid while naming the
  // remaining resources needed by an independent native submission.
  bool valid() const;
  bool submission_ready() const {
    return valid() && missing == LogicalReplayMissing::kNone;
  }
};

struct LogicalReplayFrame {
  uint64_t sequence = 0;
  std::vector<LogicalDrawReplayPacket> draws;

  bool valid() const;
  bool submission_ready() const;
};

// First generic adapter: convert the live-verified A406 MAIN family into the
// renderer-independent packet above. The adapter deep-copies VB/IB bytes,
// declaration, constants and fetch descriptors. Texture snapshots are shared
// immutable owners of their copied texels.
LogicalDrawReplayPacket
PrepareLogicalReplayPacket(const PlayerA406DrawSnapshot &draw);
LogicalReplayFrame
PrepareLogicalReplayFrame(const PlayerA406FrameSnapshot &frame);

// First indexed venue adapter. Admission requires the exact immutable PS328
// title snapshot and the exact same-draw translated backend token. The packet
// owns guest VB/IB bytes, declaration, constants and two texture payloads;
// no guest draw is suppressed or presented by this operation.
LogicalDrawReplayPacket PrepareLogicalReplayPacket(
    const VenueFullFamilyDrawSnapshot &draw,
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &token);

// Join only with exact draw-selected translation identities. Missing exact
// artifacts are requested from the store for a subsequent translated draw.
// Passing hashes without valid modification identities is intentionally not
// supported.
void JoinLogicalReplayShaderArtifacts(
    LogicalDrawReplayPacket &packet, uint64_t vertex_modification,
    bool vertex_modification_valid, uint64_t pixel_modification,
    bool pixel_modification_valid);

} // namespace tabletennis::native
