#include "native/tabletennis_logical_draw_replay_packet.h"

#include "native/tabletennis_player_a406_observer.h"
#include "native/tabletennis_venue_full_family.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace tabletennis::native {
namespace {

template <size_t Size>
LogicalReplayConstantRange ConstantRange(ReplayShaderStage stage,
                                         uint32_t first_vector,
                                         const std::array<float, Size> &values) {
  return {
      .stage = stage,
      .first_vector = first_vector,
      .values = {values.begin(), values.end()},
  };
}

bool IsFiniteMatrix(const std::array<float, 16> &matrix) {
  return std::ranges::all_of(
      matrix, [](float value) { return std::isfinite(value); });
}

LogicalReplayFixedState
CopyFixedState(const PlayerA406BackendContract &source) {
  return {
      .render_pass_key = source.render_pass_key,
      .rb_color_info_0 = source.rb_color_info_0,
      .rb_depth_info = source.rb_depth_info,
      .rb_surface_info = source.rb_surface_info,
      .rb_modecontrol = source.rb_modecontrol,
      .color_edram_base = source.color_edram_base,
      .depth_edram_base = source.depth_edram_base,
      .surface_pitch = source.surface_pitch,
      .edram_mode = source.edram_mode,
      .normalized_depth_control = source.normalized_depth_control,
      .normalized_color_mask = source.normalized_color_mask,
      .color_control = source.color_control,
      .blend_control_0 = source.blend_control_0,
      .rasterizer_mode_control = source.rasterizer_mode_control,
      .primitive_restart_index = source.primitive_restart_index,
      .color_attachment_formats = source.color_attachment_formats,
      .color_attachment_count = source.color_attachment_count,
      .depth_attachment_format = source.depth_attachment_format,
      .stencil_attachment_format = source.stencil_attachment_format,
      .sample_count = source.sample_count,
      .sample_mask = source.sample_mask,
      .primitive_restart_enabled = source.primitive_restart_enabled,
      .source_contract_valid = source.valid,
      .render_target_state_valid = source.render_target_state_valid,
      .rasterizer_mode_control_valid =
          source.rasterizer_mode_control_valid,
  };
}

} // namespace

bool LogicalReplayGeometry::valid() const {
  if (primitive_type == 0 || vertex_stride == 0 || vertex_count == 0 ||
      index_element_size == 0 || index_count == 0 ||
      minimum_index > maximum_index || maximum_index >= vertex_count ||
      vertex_fingerprint == 0 || index_fingerprint == 0) {
    return false;
  }
  return vertex_bytes.size() ==
             static_cast<size_t>(vertex_stride) * vertex_count &&
         index_bytes.size() ==
             static_cast<size_t>(index_element_size) * index_count;
}

bool LogicalReplayFixedState::valid() const {
  return source_contract_valid && render_target_state_valid &&
         rasterizer_mode_control_valid &&
         color_attachment_count <= color_attachment_formats.size() &&
         sample_count != 0;
}

bool LogicalDrawReplayPacket::valid() const {
  return frame_sequence != 0 && vertex_shader_hash != 0 &&
         pixel_shader_hash != 0 && vertex_declaration.valid() &&
         geometry.valid() && fixed_state.valid() && !constants.empty() &&
         std::ranges::all_of(constants, [](const auto &range) {
           return range.valid() &&
                  std::ranges::all_of(range.values, [](float value) {
                    return std::isfinite(value);
                  });
         }) &&
         !textures.empty() &&
         std::ranges::all_of(textures, [](const auto &texture) {
           return texture.descriptor_valid();
         }) &&
         IsFiniteMatrix(world) && IsFiniteMatrix(world_view_projection);
}

LogicalDrawReplayPacket PrepareLogicalReplayPacket(
    const VenueFullFamilyDrawSnapshot &draw,
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &token) {
  constexpr uint64_t kVertexShaderHash = 0x0E9982BE6B1E99A1ull;
  constexpr uint64_t kPixelShaderHash = 0x328FA02B07C392DCull;
  LogicalDrawReplayPacket packet;
  if (!draw.valid() || !draw.source.world_valid ||
      !draw.source.world_view_projection_valid || !token.valid ||
      token.backend != rex::graphics::NativeGuestOutputBackend::kVulkan ||
      token.backend_frame_sequence == 0 || token.opaque_token == 0 ||
      token.pipeline_generation == 0 || token.pipeline_layout_generation == 0 ||
      !token.indexed ||
      token.vertex_shader_hash != kVertexShaderHash ||
      token.pixel_shader_hash != kPixelShaderHash ||
      token.guest_primitive_type != draw.source.primitive_type ||
      token.guest_vertex_or_index_count !=
          draw.source.submitted_index_count ||
      !token.guest_index_base_valid ||
      token.guest_index_base !=
          draw.captured.mesh->source_index_physical_address ||
      token.color_attachment_count == 0 ||
      token.color_attachment_count >
          token.color_attachment_formats.size() ||
      token.sample_count == 0) {
    return packet;
  }

  const VenueMeshSnapshot &mesh = *draw.captured.mesh;
  const VenueMaterialSnapshot &material = draw.captured.material;
  const VertexDeclarationProbe &declaration =
      material.vertex_declaration_identity;
  packet.frame_sequence = draw.source.frame_sequence;
  packet.ordinal = draw.source.ordinal;
  packet.player = draw.source.player;
  packet.geometry_index = draw.source.scope.geometry_index;
  packet.lod = draw.source.scope.lod;
  packet.vertex_shader_hash = kVertexShaderHash;
  packet.pixel_shader_hash = kPixelShaderHash;

  packet.vertex_declaration.element_count = declaration.element_count;
  packet.vertex_declaration.max_stream = declaration.max_stream;
  packet.vertex_declaration.stream_mask_lo = declaration.stream_mask_lo;
  packet.vertex_declaration.stream_mask_hi = declaration.stream_mask_hi;
  packet.vertex_declaration.elements.reserve(declaration.element_count);
  for (uint32_t index = 0; index < declaration.element_count; ++index) {
    const VertexDeclarationElement &element = declaration.elements[index];
    packet.vertex_declaration.elements.push_back({
        .stream = element.stream,
        .byte_offset = element.byte_offset,
        .packed_type = element.packed_type,
        .method = element.method,
        .usage = element.usage,
        .usage_index = element.usage_index,
    });
  }

  packet.geometry = {
      .primitive_type = mesh.primitive_type,
      .vertex_stride = mesh.source_vertex_stride,
      .vertex_endian = draw.source.mesh.vertex_endian,
      .vertex_count =
          static_cast<uint32_t>(mesh.raw_vertex_bytes.size() /
                                mesh.source_vertex_stride),
      .index_element_size = sizeof(uint16_t),
      .index_count = mesh.submitted_index_count,
      .minimum_index = mesh.minimum_index,
      .maximum_index = mesh.maximum_index,
      .vertex_fingerprint = mesh.vertex_fingerprint,
      .index_fingerprint = mesh.index_fingerprint,
      .vertex_bytes = mesh.raw_vertex_bytes,
      .index_bytes = mesh.raw_index_bytes,
  };

  packet.constants = {
      ConstantRange(ReplayShaderStage::kVertex, 0,
                    material.vertex_constants_0_6),
      ConstantRange(ReplayShaderStage::kVertex, 12,
                    material.vertex_constants_12_15),
      ConstantRange(ReplayShaderStage::kPixel, 20,
                    material.pixel_constant_20),
      ConstantRange(ReplayShaderStage::kPixel, 46,
                    material.pixel_constant_46),
  };
  packet.textures.reserve(material.texture_fetches.size());
  for (uint32_t slot = 0; slot < material.texture_fetches.size(); ++slot) {
    packet.textures.push_back({
        .slot = slot,
        .fetch_words = material.texture_fetches[slot],
        .texture = material.textures[slot],
    });
  }

  LogicalReplayFixedState &state = packet.fixed_state;
  state.color_attachment_count = token.color_attachment_count;
  for (uint32_t index = 0; index < token.color_attachment_count; ++index) {
    state.color_attachment_formats[index] =
        static_cast<uint32_t>(token.color_attachment_formats[index]);
  }
  state.depth_attachment_format =
      static_cast<uint32_t>(token.depth_attachment_format);
  state.stencil_attachment_format =
      static_cast<uint32_t>(token.stencil_attachment_format);
  state.sample_count = token.sample_count;
  state.sample_mask = token.sample_mask;
  state.source_contract_valid = true;
  state.render_target_state_valid = true;
  state.rasterizer_mode_control_valid = true;
  state.viewport = token.viewport;
  state.scissor_offset = token.scissor_offset;
  state.scissor_extent = token.scissor_extent;
  state.depth_bias_constant_factor = token.depth_bias_constant_factor;
  state.depth_bias_slope_factor = token.depth_bias_slope_factor;
  state.blend_constants = token.blend_constants;
  state.stencil_compare_mask_front = token.stencil_compare_mask_front;
  state.stencil_compare_mask_back = token.stencil_compare_mask_back;
  state.stencil_write_mask_front = token.stencil_write_mask_front;
  state.stencil_write_mask_back = token.stencil_write_mask_back;
  state.stencil_reference_front = token.stencil_reference_front;
  state.stencil_reference_back = token.stencil_reference_back;
  state.translated_token_state_valid = true;
  state.backend_frame_sequence = token.backend_frame_sequence;
  state.opaque_replay_token = token.opaque_token;
  state.pipeline_generation = token.pipeline_generation;
  state.pipeline_layout_generation = token.pipeline_layout_generation;
  state.host_primitive_type = token.host_primitive_type;
  state.processed_index_buffer_type = token.processed_index_buffer_type;
  state.host_index_format = token.host_index_format;
  state.host_index_count = token.host_vertex_or_index_count;
  state.translated_resources_stable =
      token.resources_stable_for_deferred_replay;

  packet.world = draw.source.world;
  packet.world_view_projection = draw.captured.world_view_projection;
  packet.missing = LogicalReplayMissing::kTranslatedVertexProgram |
                   LogicalReplayMissing::kTranslatedPixelProgram |
                   LogicalReplayMissing::kTranslatedReplayState;
  if (token.resources_stable_for_deferred_replay) {
    packet.missing = static_cast<LogicalReplayMissing>(
        static_cast<uint32_t>(packet.missing) &
        ~static_cast<uint32_t>(LogicalReplayMissing::kTranslatedReplayState));
  }
  JoinLogicalReplayShaderArtifacts(
      packet, token.vertex_shader_modification, true,
      token.pixel_shader_modification, true);
  if (std::ranges::any_of(packet.textures, [](const auto &texture) {
        return !texture.payload_owned();
      })) {
    packet.missing =
        packet.missing | LogicalReplayMissing::kTexturePayload;
  }
  return packet.valid() ? packet : LogicalDrawReplayPacket{};
}

bool LogicalReplayFrame::valid() const {
  return sequence != 0 && !draws.empty() &&
         std::ranges::all_of(draws, [this](const auto &draw) {
           return draw.valid() && draw.frame_sequence == sequence;
         }) &&
         std::ranges::is_sorted(draws, {}, &LogicalDrawReplayPacket::ordinal);
}

bool LogicalReplayFrame::submission_ready() const {
  return valid() &&
         std::ranges::all_of(draws, [](const auto &draw) {
           return draw.submission_ready();
         });
}

LogicalDrawReplayPacket
PrepareLogicalReplayPacket(const PlayerA406DrawSnapshot &draw) {
  LogicalDrawReplayPacket packet;
  if (!draw.valid()) {
    return packet;
  }
  const PlayerA406TitleDrawSnapshot &title = *draw.title;
  packet.frame_sequence = title.sequence;
  packet.ordinal = title.ordinal;
  packet.player = title.player;
  packet.geometry_index = title.geometry_index;
  packet.lod = title.lod;
  packet.vertex_shader_hash = draw.backend.vertex_shader_hash;
  packet.pixel_shader_hash = draw.backend.pixel_shader_hash;

  packet.vertex_declaration.element_count =
      title.vertex_declaration.element_count;
  packet.vertex_declaration.max_stream = title.vertex_declaration.max_stream;
  packet.vertex_declaration.stream_mask_lo =
      title.vertex_declaration.stream_mask_lo;
  packet.vertex_declaration.stream_mask_hi =
      title.vertex_declaration.stream_mask_hi;
  packet.vertex_declaration.elements.reserve(
      title.vertex_declaration.element_count);
  for (uint32_t index = 0; index < title.vertex_declaration.element_count;
       ++index) {
    const PlayerA406VertexElementIdentity &element =
        title.vertex_declaration.elements[index];
    packet.vertex_declaration.elements.push_back({
        .stream = element.stream,
        .byte_offset = element.byte_offset,
        .packed_type = element.packed_type,
        .method = element.method,
        .usage = element.usage,
        .usage_index = element.usage_index,
    });
  }

  packet.geometry = {
      .primitive_type = title.identity.primitive_type,
      .vertex_stride = title.vertices->kStride,
      .vertex_endian = title.identity.guest_vertex_endian,
      .vertex_count = title.vertices->vertex_count,
      .index_element_size = sizeof(uint16_t),
      .index_count = title.indices->submitted_index_count,
      .minimum_index = title.indices->minimum_index,
      .maximum_index = title.indices->maximum_index,
      .vertex_fingerprint = title.vertices->payload_fingerprint,
      .index_fingerprint = title.indices->payload_fingerprint,
      .vertex_bytes = title.vertices->raw_bytes,
      .index_bytes = title.indices->raw_bytes,
  };

  const PlayerA406MaterialSnapshot &material = title.material;
  packet.constants = {
      ConstantRange(ReplayShaderStage::kVertex, 0,
                    material.vertex_constants_0_3),
      ConstantRange(ReplayShaderStage::kVertex, 12,
                    material.vertex_constants_12_15),
      ConstantRange(ReplayShaderStage::kVertex, 19,
                    material.vertex_constant_19),
      ConstantRange(ReplayShaderStage::kVertex, 29,
                    material.vertex_constants_29_36),
      ConstantRange(ReplayShaderStage::kVertex, 46,
                    material.vertex_constants_46_47),
      ConstantRange(ReplayShaderStage::kPixel, 19,
                    material.pixel_constant_19),
      ConstantRange(ReplayShaderStage::kPixel, 21,
                    material.pixel_constants_21_27),
      ConstantRange(ReplayShaderStage::kPixel, 46,
                    material.pixel_constants_46_73),
      ConstantRange(ReplayShaderStage::kPixel, 254,
                    material.pixel_constants_254_255),
  };

  static constexpr std::array<uint32_t,
                              PlayerA406MaterialSnapshot::kOwnedTextureCount>
      kOwnedSlots = {0, 1, 2, 6};
  packet.textures.reserve(material.texture_fetches.size());
  for (uint32_t slot = 0; slot < material.texture_fetches.size(); ++slot) {
    LogicalReplayTextureBinding binding{
        .slot = slot,
        .fetch_words = material.texture_fetches[slot],
    };
    const auto found = std::ranges::find(kOwnedSlots, slot);
    if (found != kOwnedSlots.end()) {
      const size_t owned_index =
          static_cast<size_t>(found - kOwnedSlots.begin());
      binding.texture = material.owned_textures[owned_index];
    }
    packet.textures.push_back(std::move(binding));
  }

  packet.fixed_state = CopyFixedState(draw.backend);
  packet.world = title.world;
  packet.world_view_projection = title.world_view_projection;
  packet.missing = LogicalReplayMissing::kTranslatedVertexProgram |
                   LogicalReplayMissing::kTranslatedPixelProgram |
                   LogicalReplayMissing::kVertexModificationIdentity |
                   LogicalReplayMissing::kPixelModificationIdentity |
                   LogicalReplayMissing::kTranslatedReplayState;
  if (std::ranges::any_of(packet.textures, [](const auto &texture) {
        return !texture.payload_owned();
      })) {
    packet.missing =
        packet.missing | LogicalReplayMissing::kTexturePayload;
  }
  return packet;
}

LogicalReplayFrame
PrepareLogicalReplayFrame(const PlayerA406FrameSnapshot &frame) {
  LogicalReplayFrame prepared;
  if (!frame.valid()) {
    return prepared;
  }
  prepared.sequence = frame.sequence;
  prepared.draws.reserve(frame.draws.size());
  for (const PlayerA406DrawSnapshot &draw : frame.draws) {
    prepared.draws.push_back(PrepareLogicalReplayPacket(draw));
  }
  return prepared.valid() ? prepared : LogicalReplayFrame{};
}

void JoinLogicalReplayShaderArtifacts(
    LogicalDrawReplayPacket &packet, uint64_t vertex_modification,
    bool vertex_modification_valid, uint64_t pixel_modification,
    bool pixel_modification_valid) {
  auto clear_missing = [&packet](LogicalReplayMissing bit) {
    packet.missing = static_cast<LogicalReplayMissing>(
        static_cast<uint32_t>(packet.missing) &
        ~static_cast<uint32_t>(bit));
  };
  auto set_missing = [&packet](LogicalReplayMissing bit) {
    packet.missing = packet.missing | bit;
  };
  packet.vertex_shader_modification = vertex_modification;
  packet.pixel_shader_modification = pixel_modification;
  packet.vertex_shader_modification_valid = vertex_modification_valid;
  packet.pixel_shader_modification_valid = pixel_modification_valid;
  packet.vertex_shader.reset();
  packet.pixel_shader.reset();
  set_missing(LogicalReplayMissing::kTranslatedVertexProgram);
  set_missing(LogicalReplayMissing::kTranslatedPixelProgram);
  set_missing(LogicalReplayMissing::kVertexModificationIdentity);
  set_missing(LogicalReplayMissing::kPixelModificationIdentity);

  if (vertex_modification_valid) {
    clear_missing(LogicalReplayMissing::kVertexModificationIdentity);
    const TranslatedShaderArtifactKey key{
        .shader_hash = packet.vertex_shader_hash,
        .modification = vertex_modification,
        .stage = TranslatedShaderStage::kVertex,
    };
    packet.vertex_shader = FindTranslatedShaderArtifact(key);
    if (packet.vertex_shader == nullptr) {
      RequestTranslatedShaderArtifact(key);
    } else {
      clear_missing(LogicalReplayMissing::kTranslatedVertexProgram);
    }
  }
  if (pixel_modification_valid) {
    clear_missing(LogicalReplayMissing::kPixelModificationIdentity);
    const TranslatedShaderArtifactKey key{
        .shader_hash = packet.pixel_shader_hash,
        .modification = pixel_modification,
        .stage = TranslatedShaderStage::kPixel,
    };
    packet.pixel_shader = FindTranslatedShaderArtifact(key);
    if (packet.pixel_shader == nullptr) {
      RequestTranslatedShaderArtifact(key);
    } else {
      clear_missing(LogicalReplayMissing::kTranslatedPixelProgram);
    }
  }
}

} // namespace tabletennis::native
