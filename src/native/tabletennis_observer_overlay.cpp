#include "native/tabletennis_observer_overlay.h"

#include "native/shaders/tabletennis_observer_overlay_spirv.h"
#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_crowd_observer_renderer.h"
#include "native/tabletennis_crowd_replacement_prewarm.h"
#include "native/tabletennis_mesh_snapshot.h"
#include "native/tabletennis_native_capture.h"
#include "native/tabletennis_native_scene_transaction.h"
#include "native/tabletennis_native_scene_targets.h"
#include "native/tabletennis_player_observer_renderer.h"
#include "native/tabletennis_player_replacement_candidates.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_texture_snapshot.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_14d_renderer.h"
#include "native/tabletennis_venue_observer_renderer.h"
#include "native/tabletennis_venue_full_family.h"
#include "native/tabletennis_venue_snapshot.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_observer_overlay, false, "Table Tennis",
    "Overlay one captured real table/net mesh on the emulated frame to prove "
    "camera and geometry alignment. Observer-only; never suppresses the game.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    tabletennis_native_observer_overlay_flat, false, "Table Tennis",
    "Render the observer mesh in flat magenta, bypassing all captured texture "
    "and material inputs. Diagnostic only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

using rex::graphics::NativeGuestOutputBackend;
using rex::graphics::NativeGuestOutputRenderContext;
namespace nrhi = rex::graphics::nrhi;
namespace xenos = rex::graphics::xenos;

constexpr std::array<uint32_t, 2> kVisibleTextureHandles = {
    0x00080002, 0x00100006};

struct OverlayVertex {
  std::array<float, 3> position;
  std::array<float, 2> texcoord0;
  std::array<float, 2> texcoord1;
  std::array<float, 4> color;
};

static_assert(sizeof(OverlayVertex) == sizeof(float) * 11);

struct VenueGpuDrawRange {
  uint32_t start_index = 0;
  int32_t base_vertex = 0;
  uint32_t index_count = 0;
  uint32_t primitive_type = 0;
};

struct OverlayResources {
  nrhi::Device* device = nullptr;
  nrhi::BindingLayout* layout = nullptr;
  nrhi::Pipeline* pipeline = nullptr;
  nrhi::Buffer* vertex_buffer = nullptr;
  nrhi::Buffer* index_buffer = nullptr;
  nrhi::Buffer* venue_vertex_buffer = nullptr;
  nrhi::Buffer* venue_index_buffer = nullptr;
  std::array<nrhi::Texture*, 2> textures{};
  std::array<nrhi::TextureView*, 2> texture_views{};
  nrhi::Format pipeline_format = nrhi::Format::kUnknown;
  std::shared_ptr<const TableMeshSnapshot> uploaded_mesh;
  std::array<std::shared_ptr<const TableTextureSnapshot>, 2>
      uploaded_textures{};
  std::vector<std::shared_ptr<const VenueMeshSnapshot>>
      uploaded_venue_meshes;
  std::vector<VenueGpuDrawRange> venue_draw_ranges;
  uint32_t venue_vertex_bytes = 0;
  uint32_t venue_index_bytes = 0;
  bool failed = false;
  bool announced_draw = false;
  bool announced_venue_draw = false;
};

OverlayResources g_resources;

void ReleaseGpuResources() {
  if (g_resources.device != nullptr) {
    g_resources.device->DestroyDeferred(g_resources.pipeline);
    g_resources.device->DestroyDeferred(g_resources.vertex_buffer);
    g_resources.device->DestroyDeferred(g_resources.index_buffer);
    g_resources.device->DestroyDeferred(g_resources.venue_vertex_buffer);
    g_resources.device->DestroyDeferred(g_resources.venue_index_buffer);
    for (size_t slot = 0; slot < g_resources.textures.size(); ++slot) {
      g_resources.device->DestroyDeferred(
          g_resources.texture_views[slot]);
      g_resources.device->DestroyDeferred(g_resources.textures[slot]);
    }
  }
  // BindingLayout follows the graphics device lifetime; NRHI intentionally
  // has no standalone destruction entry for it.
  g_resources = {};
}

bool EnsureDevice(const NativeGuestOutputRenderContext& context) {
  if (context.device == nullptr) {
    return false;
  }
  if (g_resources.device != nullptr &&
      g_resources.device != context.device) {
    ReleaseGpuResources();
  }
  g_resources.device = context.device;
  return !g_resources.failed;
}

bool EnsurePipeline(const NativeGuestOutputRenderContext& context) {
  if (!EnsureDevice(context) || context.guest_output == nullptr) {
    return false;
  }
  nrhi::Device* const device = context.device;

  if (g_resources.layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 2;
    layout.params[0] = {nrhi::BindingParamKind::kConstants, 0, 24,
                        nrhi::Visibility::kAll};
    layout.params[1] = {nrhi::BindingParamKind::kTextureTable, 0, 2,
                        nrhi::Visibility::kPixel};
    layout.static_sampler_count = 2;
    layout.static_samplers[0] = {
        0, nrhi::Filter::kLinear, nrhi::AddressMode::kWrap, 1};
    layout.static_samplers[1] = {
        1, nrhi::Filter::kLinear, nrhi::AddressMode::kWrap, 1};
    layout.allow_input_layout = true;
    g_resources.layout = device->CreateBindingLayout(layout);
    if (g_resources.layout == nullptr) {
      REXLOG_ERROR(
          "Table Tennis observer overlay: binding layout creation failed");
      g_resources.failed = true;
      return false;
    }
  }

  const nrhi::Format output_format = context.guest_output->format();
  if (g_resources.pipeline != nullptr &&
      g_resources.pipeline_format == output_format) {
    return true;
  }
  device->DestroyDeferred(g_resources.pipeline);
  g_resources.pipeline = nullptr;

  nrhi::ShaderDesc vertex_desc;
  vertex_desc.stage = nrhi::ShaderStage::kVertex;
  vertex_desc.name = "tabletennis_observer_overlay.hlsl";
  vertex_desc.hlsl_source = overlay_shader::kHlsl;
  vertex_desc.entry_point = "vs_main";
  vertex_desc.spirv = overlay_shader::kVertexSpirv;
  vertex_desc.spirv_size_bytes = overlay_shader::kVertexSpirvBytes;
  nrhi::ShaderDesc pixel_desc;
  pixel_desc.stage = nrhi::ShaderStage::kPixel;
  pixel_desc.name = "tabletennis_observer_overlay.hlsl";
  pixel_desc.hlsl_source = overlay_shader::kHlsl;
  pixel_desc.entry_point = "ps_main";
  pixel_desc.spirv = overlay_shader::kPixelSpirv;
  pixel_desc.spirv_size_bytes = overlay_shader::kPixelSpirvBytes;
  nrhi::Shader* const vertex_shader = device->CreateShader(vertex_desc);
  nrhi::Shader* const pixel_shader = device->CreateShader(pixel_desc);
  if (vertex_shader == nullptr || pixel_shader == nullptr) {
    device->DestroyDeferred(vertex_shader);
    device->DestroyDeferred(pixel_shader);
    REXLOG_ERROR("Table Tennis observer overlay: shader creation failed");
    g_resources.failed = true;
    return false;
  }

  constexpr std::array<nrhi::InputElementDesc, 4> kVertexInputs = {{
      {"POSITION", 0, 0, nrhi::Format::kR32G32B32_FLOAT,
       offsetof(OverlayVertex, position)},
      {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT,
       offsetof(OverlayVertex, texcoord0)},
      {"TEXCOORD", 1, 2, nrhi::Format::kR32G32_FLOAT,
       offsetof(OverlayVertex, texcoord1)},
      {"COLOR", 0, 3, nrhi::Format::kR32G32B32A32_FLOAT,
       offsetof(OverlayVertex, color)},
  }};
  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = g_resources.layout;
  pipeline.vs = vertex_shader;
  pipeline.ps = pixel_shader;
  pipeline.input_elements = kVertexInputs.data();
  pipeline.input_element_count =
      static_cast<uint32_t>(kVertexInputs.size());
  pipeline.vertex_stride = sizeof(OverlayVertex);
  pipeline.cull = nrhi::CullMode::kNone;
  pipeline.depth_clip = true;
  pipeline.depth.test_enable = false;
  pipeline.depth.write_enable = false;
  pipeline.blend.enable = true;
  pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op = nrhi::BlendOp::kAdd;
  pipeline.blend.src_alpha = nrhi::BlendFactor::kOne;
  pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op_alpha = nrhi::BlendOp::kAdd;
  pipeline.rtv_format = output_format;
  pipeline.sample_count = 1;
  g_resources.pipeline = device->CreateGraphicsPipeline(pipeline);
  device->DestroyDeferred(vertex_shader);
  device->DestroyDeferred(pixel_shader);
  if (g_resources.pipeline == nullptr) {
    REXLOG_ERROR("Table Tennis observer overlay: pipeline creation failed");
    g_resources.failed = true;
    return false;
  }
  g_resources.pipeline_format = output_format;
  return true;
}

nrhi::Buffer* CreateUploadBuffer(nrhi::Device* device, size_t size) {
  nrhi::BufferDesc description;
  description.size = size;
  description.heap = nrhi::HeapKind::kUpload;
  description.bind_class = nrhi::BufferBindClass::kVertexIndex;
  return device->CreateBuffer(description);
}

bool EnsureMeshBuffers(
    const NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const TableMeshSnapshot>& mesh) {
  if (mesh == nullptr || !mesh->valid()) {
    return false;
  }
  if (g_resources.uploaded_mesh == mesh &&
      g_resources.vertex_buffer != nullptr &&
      g_resources.index_buffer != nullptr) {
    return true;
  }

  nrhi::Device* const device = context.device;
  const size_t vertex_bytes =
      mesh->positions.size() * sizeof(OverlayVertex);
  const size_t index_bytes =
      mesh->indices.size() * sizeof(mesh->indices.front());
  nrhi::Buffer* const vertex_buffer =
      CreateUploadBuffer(device, vertex_bytes);
  nrhi::Buffer* const index_buffer =
      CreateUploadBuffer(device, index_bytes);
  if (vertex_buffer == nullptr || index_buffer == nullptr) {
    device->DestroyDeferred(vertex_buffer);
    device->DestroyDeferred(index_buffer);
    REXLOG_ERROR(
        "Table Tennis observer overlay: mesh buffer creation failed");
    g_resources.failed = true;
    return false;
  }

  void* const mapped_vertices = device->Map(vertex_buffer);
  void* const mapped_indices = device->Map(index_buffer);
  if (mapped_vertices == nullptr || mapped_indices == nullptr) {
    if (mapped_vertices != nullptr) {
      device->Unmap(vertex_buffer);
    }
    if (mapped_indices != nullptr) {
      device->Unmap(index_buffer);
    }
    device->DestroyDeferred(vertex_buffer);
    device->DestroyDeferred(index_buffer);
    REXLOG_ERROR("Table Tennis observer overlay: mesh upload map failed");
    g_resources.failed = true;
    return false;
  }
  std::vector<OverlayVertex> vertices(mesh->positions.size());
  for (size_t index = 0; index < vertices.size(); ++index) {
    vertices[index] = {
        .position = mesh->positions[index],
        .texcoord0 = mesh->texcoords0[index],
        .texcoord1 = mesh->texcoords1[index],
        .color = mesh->colors[index],
    };
  }
  std::memcpy(mapped_vertices, vertices.data(), vertex_bytes);
  std::memcpy(mapped_indices, mesh->indices.data(), index_bytes);
  device->Unmap(vertex_buffer);
  device->Unmap(index_buffer);

  device->DestroyDeferred(g_resources.vertex_buffer);
  device->DestroyDeferred(g_resources.index_buffer);
  g_resources.vertex_buffer = vertex_buffer;
  g_resources.index_buffer = index_buffer;
  g_resources.uploaded_mesh = mesh;
  return true;
}

bool SameVenueGeometry(const VenueFrameSnapshot& frame) {
  if (frame.draws.size() != g_resources.uploaded_venue_meshes.size()) {
    return false;
  }
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    if (frame.draws[index].mesh !=
        g_resources.uploaded_venue_meshes[index]) {
      return false;
    }
  }
  return true;
}

bool EnsureVenueBuffers(
    const NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const VenueFrameSnapshot>& frame) {
  if (frame == nullptr || !frame->valid()) {
    return false;
  }
  if (g_resources.venue_vertex_buffer != nullptr &&
      g_resources.venue_index_buffer != nullptr &&
      SameVenueGeometry(*frame)) {
    return true;
  }

  size_t vertex_count = 0;
  size_t index_count = 0;
  for (const VenueDrawSnapshot& draw : frame->draws) {
    if (draw.mesh == nullptr || !draw.mesh->valid()) {
      return false;
    }
    vertex_count += draw.mesh->positions.size();
    index_count += draw.mesh->indices.size();
  }
  if (vertex_count == 0 || index_count == 0 ||
      vertex_count > std::numeric_limits<uint32_t>::max() /
                         sizeof(OverlayVertex) ||
      index_count > std::numeric_limits<uint32_t>::max() /
                        sizeof(uint16_t) ||
      vertex_count > static_cast<size_t>(
                         std::numeric_limits<int32_t>::max())) {
    return false;
  }

  const size_t vertex_bytes = vertex_count * sizeof(OverlayVertex);
  const size_t index_bytes = index_count * sizeof(uint16_t);
  nrhi::Device* const device = context.device;
  nrhi::Buffer* const vertex_buffer =
      CreateUploadBuffer(device, vertex_bytes);
  nrhi::Buffer* const index_buffer =
      CreateUploadBuffer(device, index_bytes);
  if (vertex_buffer == nullptr || index_buffer == nullptr) {
    device->DestroyDeferred(vertex_buffer);
    device->DestroyDeferred(index_buffer);
    REXLOG_ERROR(
        "Table Tennis venue observer: buffer creation failed");
    return false;
  }

  auto* const mapped_vertices =
      static_cast<OverlayVertex*>(device->Map(vertex_buffer));
  auto* const mapped_indices =
      static_cast<uint16_t*>(device->Map(index_buffer));
  if (mapped_vertices == nullptr || mapped_indices == nullptr) {
    if (mapped_vertices != nullptr) {
      device->Unmap(vertex_buffer);
    }
    if (mapped_indices != nullptr) {
      device->Unmap(index_buffer);
    }
    device->DestroyDeferred(vertex_buffer);
    device->DestroyDeferred(index_buffer);
    REXLOG_ERROR("Table Tennis venue observer: buffer map failed");
    return false;
  }

  size_t vertex_offset = 0;
  size_t index_offset = 0;
  std::vector<VenueGpuDrawRange> ranges;
  std::vector<std::shared_ptr<const VenueMeshSnapshot>> uploaded_meshes;
  ranges.reserve(frame->draws.size());
  uploaded_meshes.reserve(frame->draws.size());
  for (const VenueDrawSnapshot& draw : frame->draws) {
    const VenueMeshSnapshot& mesh = *draw.mesh;
    for (size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
      mapped_vertices[vertex_offset + vertex] = {
          .position = mesh.positions[vertex],
          .texcoord0 = {},
          .texcoord1 = {},
          .color = {1.0f, 1.0f, 1.0f, 1.0f},
      };
    }
    std::memcpy(mapped_indices + index_offset, mesh.indices.data(),
                mesh.indices.size() * sizeof(uint16_t));
    ranges.push_back({
        .start_index = static_cast<uint32_t>(index_offset),
        .base_vertex = static_cast<int32_t>(vertex_offset),
        .index_count = static_cast<uint32_t>(mesh.indices.size()),
        .primitive_type = mesh.primitive_type,
    });
    uploaded_meshes.push_back(draw.mesh);
    vertex_offset += mesh.positions.size();
    index_offset += mesh.indices.size();
  }
  device->Unmap(vertex_buffer);
  device->Unmap(index_buffer);

  device->DestroyDeferred(g_resources.venue_vertex_buffer);
  device->DestroyDeferred(g_resources.venue_index_buffer);
  g_resources.venue_vertex_buffer = vertex_buffer;
  g_resources.venue_index_buffer = index_buffer;
  g_resources.uploaded_venue_meshes = std::move(uploaded_meshes);
  g_resources.venue_draw_ranges = std::move(ranges);
  g_resources.venue_vertex_bytes = static_cast<uint32_t>(vertex_bytes);
  g_resources.venue_index_bytes = static_cast<uint32_t>(index_bytes);
  g_resources.announced_venue_draw = false;
  REXLOG_INFO(
      "Table Tennis venue observer: uploaded {} real guest meshes "
      "vertices={} indices={}",
      frame->draws.size(), vertex_count, index_count);
  return true;
}

nrhi::Format HostTextureFormat(const TableTextureSnapshot& texture) {
  switch (rex::graphics::GetBaseFormat(
      static_cast<xenos::TextureFormat>(texture.format))) {
    case xenos::TextureFormat::k_DXT1:
      return nrhi::Format::kBC1_UNORM;
    case xenos::TextureFormat::k_DXT4_5:
      return nrhi::Format::kBC3_UNORM;
    default:
      return nrhi::Format::kUnknown;
  }
}

void ComposeTextureSwizzle(uint32_t fetch_swizzle,
                           nrhi::Swizzle output[4]) {
  for (uint32_t channel = 0; channel < 4; ++channel) {
    output[channel] =
        static_cast<nrhi::Swizzle>((fetch_swizzle >> (channel * 3)) & 7u);
  }
}

std::array<std::shared_ptr<const TableTextureSnapshot>, 2>
VisibleTextureSnapshots() {
  std::array<std::shared_ptr<const TableTextureSnapshot>, 2> selected{};
  const auto snapshots = LatestTableTextureSnapshots();
  for (size_t slot = 0; slot < selected.size(); ++slot) {
    const auto found = std::find_if(
        snapshots.begin(), snapshots.end(), [&](const auto& texture) {
          return texture != nullptr && texture->valid() &&
                 texture->encoded_handle == kVisibleTextureHandles[slot];
        });
    if (found != snapshots.end()) {
      selected[slot] = *found;
    }
  }
  return selected;
}

bool EnsureTexture(
    const NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const TableTextureSnapshot>& snapshot,
    size_t slot) {
  if (snapshot == nullptr || !snapshot->valid()) {
    return false;
  }
  if (g_resources.uploaded_textures[slot] == snapshot &&
      g_resources.textures[slot] != nullptr &&
      g_resources.texture_views[slot] != nullptr) {
    return true;
  }

  const nrhi::Format format = HostTextureFormat(*snapshot);
  if (format == nrhi::Format::kUnknown) {
    REXLOG_ERROR(
        "Table Tennis observer overlay: unsupported texture format {}",
        snapshot->format);
    return false;
  }

  const uint32_t host_width =
      ((snapshot->width + snapshot->block_width - 1) /
       snapshot->block_width) *
      snapshot->block_width;
  const uint32_t host_height =
      ((snapshot->height + snapshot->block_height - 1) /
       snapshot->block_height) *
      snapshot->block_height;
  const uint32_t block_rows = host_height / snapshot->block_height;
  const uint32_t upload_row_pitch =
      (snapshot->row_pitch_bytes + nrhi::kRowPitchAlignment - 1) &
      ~(nrhi::kRowPitchAlignment - 1);
  const size_t upload_size =
      static_cast<size_t>(upload_row_pitch) * block_rows;

  nrhi::TextureDesc texture_desc;
  texture_desc.width = host_width;
  texture_desc.height = host_height;
  texture_desc.format = format;
  texture_desc.initial_state = nrhi::ResourceState::kCopyDest;
  nrhi::Texture* const texture =
      context.device->CreateTexture(texture_desc);
  nrhi::BufferDesc upload_desc;
  upload_desc.size = upload_size;
  upload_desc.heap = nrhi::HeapKind::kUpload;
  upload_desc.bind_class = nrhi::BufferBindClass::kCopySrc;
  nrhi::Buffer* const upload = context.device->CreateBuffer(upload_desc);
  if (texture == nullptr || upload == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR(
        "Table Tennis observer overlay: texture resource creation failed");
    return false;
  }

  uint8_t* const mapped =
      static_cast<uint8_t*>(context.device->Map(upload));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR(
        "Table Tennis observer overlay: texture upload map failed");
    return false;
  }
  for (uint32_t row = 0; row < block_rows; ++row) {
    uint8_t* const destination =
        mapped + static_cast<size_t>(row) * upload_row_pitch;
    const uint8_t* const source =
        snapshot->linear_blocks.data() +
        static_cast<size_t>(row) * snapshot->row_pitch_bytes;
    std::memcpy(destination, source, snapshot->row_pitch_bytes);
    std::memset(destination + snapshot->row_pitch_bytes, 0,
                upload_row_pitch - snapshot->row_pitch_bytes);
  }
  context.device->Unmap(upload);

  nrhi::TextureViewDesc view_desc;
  ComposeTextureSwizzle(snapshot->fetch_swizzle, view_desc.swizzle);
  nrhi::TextureView* const texture_view =
      context.device->CreateTextureView(texture, view_desc);
  if (texture_view == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR(
        "Table Tennis observer overlay: texture view creation failed");
    return false;
  }

  context.cmd->CopyBufferToTexture(
      texture, 0, 0, upload, 0, upload_row_pitch, host_width,
      host_height, 1);
  context.cmd->Barrier(texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.device->DestroyDeferred(upload);

  context.device->DestroyDeferred(g_resources.texture_views[slot]);
  context.device->DestroyDeferred(g_resources.textures[slot]);
  g_resources.textures[slot] = texture;
  g_resources.texture_views[slot] = texture_view;
  g_resources.uploaded_textures[slot] = snapshot;
  g_resources.announced_draw = false;
  REXLOG_INFO(
      "Table Tennis observer overlay: uploaded guest texture "
      "slot={} handle={:08X} {}x{} format={} payload={:016X}",
      slot, snapshot->encoded_handle, snapshot->width,
      snapshot->height, snapshot->format,
      snapshot->payload_fingerprint);
  return true;
}

void PostProcess(const NativeGuestOutputRenderContext& context, void*) {
  const bool replacement_requested =
      VenueFamilyReplacementDrawCount() != 0;
  if (!ObserverOverlayEnabled() && !replacement_requested) {
    rex::graphics::RequestNativeGuestOutputPostProcess(false);
    return;
  }
  if (context.cmd == nullptr || context.guest_output == nullptr ||
      (context.backend != NativeGuestOutputBackend::kD3D12 &&
       context.backend != NativeGuestOutputBackend::kVulkan)) {
    return;
  }

  const CapturedFrame frame = LatestCapturedFrame();
  if (!frame.gameplay_active) {
    return;
  }

  ObserveNativeSceneTransaction(context);

  const bool table_requested =
      REXCVAR_GET(tabletennis_native_observer_overlay);
  const std::shared_ptr<const TableMeshSnapshot> table_mesh =
      table_requested ? LatestTableMeshSnapshot() : nullptr;
  std::array<std::shared_ptr<const TableTextureSnapshot>, 2>
      table_textures{};
  if (table_requested) {
    table_textures = VisibleTextureSnapshots();
  }
  const bool table_structurally_ready =
      table_requested && frame.camera.valid &&
      frame.camera.direct_context_verified &&
      frame.camera.verified_candidates == 2 && table_mesh != nullptr &&
      table_textures[0] != nullptr && table_textures[1] != nullptr;

  const bool venue_observer_requested = VenueFamilyObserverEnabled();
  const std::shared_ptr<const VenueFrameSnapshot> venue_frame =
      (venue_observer_requested || replacement_requested)
          ? LatestVenueFrameSnapshot()
          : nullptr;
  const bool venue_capture_ready =
      venue_frame != nullptr && venue_frame->valid();
  const bool replacement_ready =
      replacement_requested && venue_capture_ready &&
      PrepareVenueReplacement(context, venue_frame);
  const bool venue_observer_ready =
      venue_observer_requested && venue_capture_ready;
  const bool full_family_requested =
      VenueFullFamilyOverlayEnabled();
  const std::shared_ptr<const VenueFullFamilyFrame> full_family_frame =
      full_family_requested ? LatestVenueFullFamilyFrame() : nullptr;
  const bool full_family_capture_ready =
      full_family_frame != nullptr && full_family_frame->valid();
  const bool full_family_ready =
      full_family_requested && full_family_capture_ready &&
      PrepareVenueFullFamilyOverlay(context, full_family_frame);
  const bool venue_14d_requested = Venue14DRendererEnabled();
  const std::shared_ptr<const Venue14DFrameSnapshot> venue_14d_frame =
      venue_14d_requested ? LatestVenue14DFrameSnapshot() : nullptr;
  const bool venue_14d_capture_ready =
      venue_14d_frame != nullptr && venue_14d_frame->valid();
  const bool venue_14d_ready =
      venue_14d_requested && venue_14d_capture_ready &&
      PrepareVenue14DObserver(context, venue_14d_frame);
  const bool player_overlay_requested =
      PlayerObserverOverlayEnabled();
  const bool player_prepare_requested =
      player_overlay_requested ||
      PlayerReplacementPrewarmEnabled();
  const std::shared_ptr<const PlayerSkinFrameSnapshot> player_frame =
      player_prepare_requested ? LatestPlayerSkinFrameSnapshot() : nullptr;
  const bool player_capture_ready =
      player_frame != nullptr && player_frame->valid();
  const bool player_resources_ready =
      player_prepare_requested && player_capture_ready &&
      PreparePlayerObserverOverlay(context, player_frame);
  const bool player_ready =
      player_overlay_requested && player_resources_ready;
  const bool crowd_requested = CrowdObserverOverlayEnabled();
  const bool crowd_prewarm_requested =
      CrowdReplacementPrewarmEnabled();
  const bool crowd_prepare_requested =
      crowd_requested || crowd_prewarm_requested;
  const std::shared_ptr<const CrowdFrameSnapshot> crowd_frame =
      crowd_prewarm_requested
          ? LatestCrowdBackendProofFrameSnapshot()
          : (crowd_requested ? LatestCrowdFrameSnapshot() : nullptr);
  const bool crowd_capture_ready =
      crowd_frame != nullptr && crowd_frame->valid();
  const bool crowd_resources_ready =
      crowd_prepare_requested && crowd_capture_ready &&
      (crowd_prewarm_requested
           ? PrepareCrowdReplacementPrewarmResources(
                 context, crowd_frame)
           : PrepareCrowdObserverOverlay(context, crowd_frame));
  const bool crowd_ready =
      crowd_requested && crowd_resources_ready;
  if (!table_structurally_ready && !venue_observer_ready &&
      !full_family_ready && !venue_14d_ready && !player_ready &&
      !crowd_ready) {
    // Resource prewarming is the only reason this post-process was requested.
    // The in-order callback will consume it on a later guest draw.
    return;
  }
  const bool table_ready =
      table_structurally_ready &&
      EnsurePipeline(context) &&
      EnsureMeshBuffers(context, table_mesh) &&
      EnsureTexture(context, table_textures[0], 0) &&
      EnsureTexture(context, table_textures[1], 1);
  const bool venue_ready = venue_observer_ready;
  if (!table_ready && !venue_ready && !full_family_ready &&
      !venue_14d_ready && !player_ready && !crowd_ready) {
    return;
  }
  (void)replacement_ready;

  nrhi::Cmd* const cmd = context.cmd;
  const nrhi::Viewport viewport = {
      0.0f, 0.0f, static_cast<float>(context.guest_output_width),
      static_cast<float>(context.guest_output_height), 0.0f, 1.0f};
  const nrhi::Rect scissor = {
      0, 0, static_cast<int32_t>(context.guest_output_width),
      static_cast<int32_t>(context.guest_output_height)};

  cmd->ProfileRegion(nrhi::ProfileStage::k2d);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(context.guest_output, nullptr);
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);

  if (table_ready) {
    cmd->SetBindingLayout(g_resources.layout);
    cmd->SetPipeline(g_resources.pipeline);
    std::array<float, 24> constants{};
    std::copy(frame.camera.view_projection.begin(),
              frame.camera.view_projection.end(), constants.begin());
    constexpr std::array<float, 4> kOverlayColor = {
        1.0f, 1.0f, 1.0f, 0.72f};
    std::copy(kOverlayColor.begin(), kOverlayColor.end(),
              constants.begin() + 16);
    constants[20] =
        REXCVAR_GET(tabletennis_native_observer_overlay_flat) ? 1.0f
                                                              : 0.0f;
    const uint32_t vertex_bytes =
        static_cast<uint32_t>(table_mesh->positions.size() *
                              sizeof(OverlayVertex));
    const uint32_t index_bytes =
        static_cast<uint32_t>(table_mesh->indices.size() *
                              sizeof(table_mesh->indices.front()));
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    cmd->SetRootConstants(0, constants.size(), constants.data(), 0);
    cmd->SetTexturePair(1, g_resources.texture_views[0],
                        g_resources.texture_views[1]);
    cmd->SetVertexBuffer(g_resources.vertex_buffer, 0, vertex_bytes,
                         sizeof(OverlayVertex));
    cmd->SetIndexBuffer(g_resources.index_buffer, 0, index_bytes);
    cmd->DrawIndexed(
        static_cast<uint32_t>(table_mesh->indices.size()), 0, 0);
  }

  const uint32_t venue_draw_count =
      venue_ready ? RenderVenueObserver(context, venue_frame) : 0;
  const uint32_t full_family_draw_count =
      full_family_ready
          ? RenderVenueFullFamilyOverlay(context, full_family_frame)
          : 0;
  const uint32_t venue_14d_draw_count =
      venue_14d_ready
          ? RenderVenue14DObserver(context, venue_14d_frame)
          : 0;
  const uint32_t player_draw_count =
      player_ready
          ? RenderPlayerObserverOverlay(context, player_frame)
          : 0;
  const uint32_t crowd_draw_count =
      crowd_ready
          ? RenderCrowdObserverOverlay(context, crowd_frame)
          : 0;
  (void)full_family_draw_count;
  (void)venue_14d_draw_count;
  (void)player_draw_count;
  (void)crowd_draw_count;

  cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  cmd->ProfileRegion(nrhi::ProfileStage::kTail);

  if (table_ready && !g_resources.announced_draw) {
    g_resources.announced_draw = true;
    REXLOG_INFO(
        "Table Tennis observer overlay: drew captured real mesh "
        "vertices={} indices={} texture_handles={:08X},{:08X} "
        "camera_generation={} candidates={}",
        table_mesh->positions.size(), table_mesh->indices.size(),
        table_textures[0]->encoded_handle,
        table_textures[1]->encoded_handle,
        frame.camera.constant_generation,
        frame.camera.verified_candidates);
  }
  if (venue_draw_count != 0 &&
      !g_resources.announced_venue_draw) {
    g_resources.announced_venue_draw = true;
    REXLOG_INFO(
        "Table Tennis venue observer: drew {} trace-verified real "
        "venue meshes over the untouched guest frame (observer-only)",
        venue_draw_count);
  }
}

}  // namespace

bool ObserverOverlayEnabled() {
  return REXCVAR_GET(tabletennis_native_observer_overlay) ||
         NativeSceneTransactionObserverEnabled() ||
         VenueFamilyObserverEnabled() ||
         VenueFullFamilyOverlayEnabled() ||
         Venue14DRendererEnabled() ||
         PlayerObserverOverlayEnabled() ||
         PlayerReplacementPrewarmEnabled() ||
         CrowdObserverOverlayEnabled() ||
         CrowdReplacementPrewarmEnabled();
}

void RequestObserverOverlayForFrame(bool gameplay_active,
                                    bool camera_valid) {
  const bool table_ready =
      REXCVAR_GET(tabletennis_native_observer_overlay) &&
      camera_valid && HasTableMeshSnapshot();
  const bool venue_ready =
      VenueFamilyObserverEnabled() && HasVenueFrameSnapshot();
  const std::shared_ptr<const VenueFullFamilyFrame> full_family_frame =
      VenueFullFamilyOverlayEnabled()
          ? LatestVenueFullFamilyFrame()
          : nullptr;
  const bool full_family_ready =
      full_family_frame != nullptr && full_family_frame->valid();
  const std::shared_ptr<const Venue14DFrameSnapshot> venue_14d_frame =
      Venue14DRendererEnabled()
          ? LatestVenue14DFrameSnapshot()
          : nullptr;
  const bool venue_14d_ready =
      venue_14d_frame != nullptr && venue_14d_frame->valid();
  const bool replacement_ready =
      VenueFamilyReplacementDrawCount() != 0 &&
      HasVenueFrameSnapshot();
  const std::shared_ptr<const PlayerSkinFrameSnapshot> player_frame =
      (PlayerObserverOverlayEnabled() ||
       PlayerReplacementPrewarmEnabled())
          ? LatestPlayerSkinFrameSnapshot()
          : nullptr;
  const bool player_ready =
      player_frame != nullptr && player_frame->valid();
  const std::shared_ptr<const CrowdFrameSnapshot> crowd_frame =
      CrowdReplacementPrewarmEnabled()
          ? LatestCrowdBackendProofFrameSnapshot()
          : (CrowdObserverOverlayEnabled()
                 ? LatestCrowdFrameSnapshot()
                 : nullptr);
  const bool crowd_ready =
      crowd_frame != nullptr && crowd_frame->valid();
  rex::graphics::RequestNativeGuestOutputPostProcess(
      gameplay_active &&
      (NativeSceneTransactionObserverEnabled() || table_ready ||
       venue_ready || full_family_ready ||
       venue_14d_ready || replacement_ready || player_ready ||
       crowd_ready));
}

void InstallObserverOverlay() {
  rex::graphics::SetNativeGuestOutputPostProcessor(&PostProcess, nullptr);
  REXLOG_INFO(
      "Table Tennis real-mesh observer overlays registered "
      "(tabletennis_native_observer_overlay, "
      "tabletennis_native_scene_transaction_observer, "
      "tabletennis_native_venue_observer, "
      "tabletennis_native_venue_full_family_overlay, "
      "tabletennis_native_venue_14d_renderer, "
      "tabletennis_native_player_observer_overlay, "
      "tabletennis_native_player_replacement_prewarm, "
      "tabletennis_native_crowd_observer, "
      "tabletennis_native_crowd_replacement_prewarm, "
      "tabletennis_native_venue_replace_draws)");
}

void ShutdownObserverOverlay() {
  rex::graphics::RequestNativeGuestOutputPostProcess(false);
  rex::graphics::SetNativeGuestOutputPostProcessor(nullptr, nullptr);
  ShutdownCrowdObserverRenderer();
  ShutdownPlayerObserverRenderer();
  ShutdownVenue14DRenderer();
  ShutdownVenueObserverRenderer();
  ShutdownNativeSceneRenderTargets();
  ReleaseGpuResources();
}

}  // namespace tabletennis::native
