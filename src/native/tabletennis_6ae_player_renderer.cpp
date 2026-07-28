#include "native/tabletennis_6ae_player_renderer.h"

#include "native/shaders/tabletennis_player_6ae_geometry_probe_spirv.h"
#include "native/tabletennis_6ae_player_observer.h"
#include "native/tabletennis_6ae_texture_decoder.h"
#include "native/tabletennis_texture_snapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_6ae_observer_overlay, false, "Table Tennis",
    "Draw one immutable real 6AE player geometry group over untouched guest "
    "output using the native geometry probe. Observer-only; never replaces, "
    "serves or suppresses a guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    tabletennis_native_player_6ae_observer_geometry_group, 0, "Table Tennis",
    "Nth unique immutable vf95/index geometry group drawn by the 6AE observer "
    "overlay.")
    .range(0, 255)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint32_t kVertexStride = 36;
constexpr size_t kMaximumStaticPayloads = 128;
constexpr size_t kMaximumTextures = 64;

struct alignas(16) ProbeConstants {
  std::array<float, 16> world_to_clip{};
  std::array<float, 4> vertex_constant_19{};
  std::array<float, 32> vertex_constants_29_36{};
  std::array<float, 4> vertex_constant_46{};
  std::array<float, 4> vertex_constant_47{};
  std::array<float, 4> vertex_constant_255{};
  std::array<float, 4> pixel_constant_19{};
  std::array<float, 28> pixel_constants_21_27{};
  std::array<float, 100> pixel_constants_46_70{};
  std::array<float, 8> pixel_constants_254_255{};
  std::array<uint32_t, 4> buffer_layout{};
  std::array<float, 4> probe_tint{};
};

static_assert(sizeof(ProbeConstants) == 53 * sizeof(float) * 4);

struct GpuVertex {
  std::shared_ptr<const Player6AEVertexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
  uint32_t size_bytes = 0;
};

struct GpuIndex {
  std::shared_ptr<const Player6AEIndexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
  uint32_t size_bytes = 0;
};

struct GpuTexture {
  std::shared_ptr<const TextureSnapshot> snapshot;
  nrhi::Texture *texture = nullptr;
  nrhi::TextureView *view = nullptr;
  bool transient = false;
};

struct PreparedDraw {
  size_t source_draw_index = 0;
  nrhi::Buffer *vertex = nullptr;
  nrhi::Buffer *index = nullptr;
  // Shader unique-binding order tf0..tf5. This deliberately differs from
  // the Xenos fetch-slot order retained by the material snapshot.
  std::array<nrhi::TextureView *,
             Player6AEMaterialSnapshot::kTextureFetchSlotCount>
      textures{};
  uint32_t index_bytes = 0;
  uint32_t index_count = 0;
};

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::BindingLayout *layout = nullptr;
  nrhi::Shader *vertex_shader = nullptr;
  nrhi::Shader *pixel_shader = nullptr;
  nrhi::Pipeline *pipeline = nullptr;
  nrhi::Format pipeline_format = nrhi::Format::kUnknown;
  nrhi::Texture *depth = nullptr;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;
  std::vector<GpuVertex> vertices;
  std::vector<GpuIndex> indices;
  std::vector<GpuTexture> textures;
  nrhi::Buffer *palette = nullptr;
  nrhi::Buffer *constants = nullptr;
  std::shared_ptr<const Player6AEFrameSnapshot> prepared_frame;
  PreparedDraw prepared{};
  int32_t prepared_group = -1;
  uint64_t announced_sequence = 0;
  int32_t announced_invalid_group = -1;
  bool failed = false;
};

Resources g_resources;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

template <size_t Size> bool AllFinite(const std::array<float, Size> &values) {
  return std::ranges::all_of(values,
                             [](float value) { return std::isfinite(value); });
}

void ReleaseResources() {
  if (g_resources.device != nullptr) {
    g_resources.device->DestroyDeferred(g_resources.vertex_shader);
    g_resources.device->DestroyDeferred(g_resources.pixel_shader);
    g_resources.device->DestroyDeferred(g_resources.pipeline);
    g_resources.device->DestroyDeferred(g_resources.depth);
    g_resources.device->DestroyDeferred(g_resources.palette);
    g_resources.device->DestroyDeferred(g_resources.constants);
    for (GpuVertex &vertex : g_resources.vertices) {
      g_resources.device->DestroyDeferred(vertex.buffer);
    }
    for (GpuIndex &index : g_resources.indices) {
      g_resources.device->DestroyDeferred(index.buffer);
    }
    for (GpuTexture &texture : g_resources.textures) {
      g_resources.device->DestroyDeferred(texture.view);
      g_resources.device->DestroyDeferred(texture.texture);
    }
  }
  // Binding layouts follow the device lifetime.
  g_resources = {};
}

void ReleaseTransientTextures() {
  if (g_resources.device == nullptr) {
    return;
  }
  std::erase_if(g_resources.textures, [](GpuTexture &texture) {
    if (!texture.transient) {
      return false;
    }
    g_resources.device->DestroyDeferred(texture.view);
    g_resources.device->DestroyDeferred(texture.texture);
    return true;
  });
}

bool EnsureDevice(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (context.device == nullptr || context.cmd == nullptr) {
    return false;
  }
  if (g_resources.device != nullptr && g_resources.device != context.device) {
    ReleaseResources();
  }
  g_resources.device = context.device;
  return !g_resources.failed;
}

nrhi::Buffer *CreateAndFillBuffer(nrhi::Device *device, const void *source,
                                  uint64_t source_size, uint64_t buffer_size,
                                  nrhi::BufferBindClass bind_class) {
  if (device == nullptr || source == nullptr || source_size == 0 ||
      source_size > buffer_size) {
    return nullptr;
  }
  nrhi::BufferDesc desc;
  desc.size = buffer_size;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = bind_class;
  nrhi::Buffer *const buffer = device->CreateBuffer(desc);
  if (buffer == nullptr) {
    return nullptr;
  }
  uint8_t *const mapped = static_cast<uint8_t *>(device->Map(buffer));
  if (mapped == nullptr) {
    device->DestroyDeferred(buffer);
    return nullptr;
  }
  std::memset(mapped, 0, static_cast<size_t>(buffer_size));
  std::memcpy(mapped, source, static_cast<size_t>(source_size));
  device->Unmap(buffer);
  return buffer;
}

GpuVertex *
EnsureVertex(const std::shared_ptr<const Player6AEVertexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      snapshot->stride != kVertexStride || snapshot->raw_bytes.empty() ||
      snapshot->raw_bytes.size() > std::numeric_limits<uint32_t>::max()) {
    return nullptr;
  }
  const auto found =
      std::ranges::find(g_resources.vertices, snapshot, &GpuVertex::snapshot);
  if (found != g_resources.vertices.end()) {
    return &*found;
  }
  if (g_resources.vertices.size() == kMaximumStaticPayloads) {
    return nullptr;
  }
  nrhi::Buffer *const buffer = CreateAndFillBuffer(
      g_resources.device, snapshot->raw_bytes.data(),
      snapshot->raw_bytes.size(), snapshot->raw_bytes.size(),
      nrhi::BufferBindClass::kFull);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis 6AE overlay: vf95 upload failed");
    return nullptr;
  }
  g_resources.vertices.push_back({
      .snapshot = snapshot,
      .buffer = buffer,
      .size_bytes = static_cast<uint32_t>(snapshot->raw_bytes.size()),
  });
  return &g_resources.vertices.back();
}

GpuIndex *
EnsureIndex(const std::shared_ptr<const Player6AEIndexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() || snapshot->indices.empty() ||
      snapshot->indices.size() >
          std::numeric_limits<uint32_t>::max() / sizeof(uint16_t)) {
    return nullptr;
  }
  const auto found =
      std::ranges::find(g_resources.indices, snapshot, &GpuIndex::snapshot);
  if (found != g_resources.indices.end()) {
    return &*found;
  }
  if (g_resources.indices.size() == kMaximumStaticPayloads) {
    return nullptr;
  }
  const uint32_t size_bytes =
      static_cast<uint32_t>(snapshot->indices.size() * sizeof(uint16_t));
  nrhi::Buffer *const buffer = CreateAndFillBuffer(
      g_resources.device, snapshot->indices.data(), size_bytes, size_bytes,
      nrhi::BufferBindClass::kVertexIndex);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis 6AE overlay: index upload failed");
    return nullptr;
  }
  g_resources.indices.push_back({
      .snapshot = snapshot,
      .buffer = buffer,
      .size_bytes = size_bytes,
  });
  return &g_resources.indices.back();
}

GpuTexture *
EnsureTexture(const rex::graphics::NativeGuestOutputRenderContext &context,
              const std::shared_ptr<const TextureSnapshot> &snapshot,
              bool transient) {
  if (snapshot == nullptr || !snapshot->full_mip_chain() ||
      snapshot->layer_count != 1) {
    return nullptr;
  }
  const auto found =
      std::ranges::find(g_resources.textures, snapshot, &GpuTexture::snapshot);
  if (found != g_resources.textures.end()) {
    return &*found;
  }
  if (g_resources.textures.size() == kMaximumTextures ||
      snapshot->mips.size() > std::numeric_limits<uint32_t>::max()) {
    return nullptr;
  }
  Player6AEHostTexture decoded;
  const Player6AETextureDecodeFailure decode_failure =
      DecodePlayer6AETexture(*snapshot, decoded);
  if (decode_failure != Player6AETextureDecodeFailure::kNone) {
    REXLOG_ERROR("Table Tennis 6AE overlay: texture decode failed ({})",
                 Player6AETextureDecodeFailureName(decode_failure));
    return nullptr;
  }

  struct UploadRegion {
    const Player6AEHostTextureMip *mip = nullptr;
    uint32_t row_pitch = 0;
    uint32_t footprint_width = 0;
    uint32_t footprint_height = 0;
    uint64_t offset = 0;
  };
  constexpr uint64_t kPlacementAlignment = 512;
  std::vector<UploadRegion> regions;
  uint64_t upload_bytes = 0;
  for (const Player6AEHostTextureMip &mip : decoded.mips) {
    upload_bytes = AlignUp(upload_bytes, kPlacementAlignment);
    const uint32_t row_pitch = static_cast<uint32_t>(
        AlignUp(mip.row_pitch_bytes, nrhi::kRowPitchAlignment));
    const uint64_t mip_bytes =
        static_cast<uint64_t>(row_pitch) * mip.height_blocks;
    if (mip_bytes > std::numeric_limits<uint64_t>::max() - upload_bytes) {
      return nullptr;
    }
    regions.push_back({
        .mip = &mip,
        .row_pitch = row_pitch,
        .footprint_width = mip.width_blocks * decoded.block_width,
        .footprint_height = mip.height_blocks * decoded.block_height,
        .offset = upload_bytes,
    });
    upload_bytes += mip_bytes;
  }
  if (upload_bytes == 0 || upload_bytes > std::numeric_limits<size_t>::max()) {
    return nullptr;
  }

  nrhi::TextureDesc texture_desc;
  texture_desc.width = decoded.width;
  texture_desc.height = decoded.height;
  texture_desc.mip_levels = static_cast<uint32_t>(decoded.mips.size());
  texture_desc.format = decoded.format;
  texture_desc.initial_state = nrhi::ResourceState::kCopyDest;
  nrhi::Texture *const texture = context.device->CreateTexture(texture_desc);
  nrhi::BufferDesc upload_desc;
  upload_desc.size = upload_bytes;
  upload_desc.heap = nrhi::HeapKind::kUpload;
  upload_desc.bind_class = nrhi::BufferBindClass::kCopySrc;
  nrhi::Buffer *const upload = context.device->CreateBuffer(upload_desc);
  if (texture == nullptr || upload == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  uint8_t *const mapped = static_cast<uint8_t *>(context.device->Map(upload));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  std::memset(mapped, 0, static_cast<size_t>(upload_bytes));
  for (const UploadRegion &region : regions) {
    const uint8_t *const source =
        decoded.linear_blocks.data() + region.mip->linear_offset;
    for (uint32_t row = 0; row < region.mip->height_blocks; ++row) {
      std::memcpy(mapped + region.offset +
                      static_cast<uint64_t>(row) * region.row_pitch,
                  source +
                      static_cast<size_t>(row) * region.mip->row_pitch_bytes,
                  region.mip->row_pitch_bytes);
    }
  }
  context.device->Unmap(upload);

  nrhi::TextureViewDesc view_desc;
  view_desc.mip_levels = static_cast<uint32_t>(decoded.mips.size());
  std::copy(decoded.swizzle.begin(), decoded.swizzle.end(),
            view_desc.swizzle);
  nrhi::TextureView *const view =
      context.device->CreateTextureView(texture, view_desc);
  if (view == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  for (const UploadRegion &region : regions) {
    context.cmd->CopyBufferToTexture(
        texture, region.mip->level, 0, upload, region.offset, region.row_pitch,
        region.footprint_width, region.footprint_height, 1);
  }
  context.cmd->Barrier(texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.device->DestroyDeferred(upload);
  g_resources.textures.push_back({snapshot, texture, view, transient});
  return &g_resources.textures.back();
}

bool EnsurePipeline(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (!EnsureDevice(context) || context.guest_output == nullptr) {
    return false;
  }
  nrhi::Device *const device = context.device;
  if (g_resources.layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 4;
    layout.params[0] = {nrhi::BindingParamKind::kConstantBuffer, 0, 1,
                        nrhi::Visibility::kAll};
    layout.params[1] = {nrhi::BindingParamKind::kBufferSrv, 0, 1,
                        nrhi::Visibility::kVertex};
    layout.params[2] = {nrhi::BindingParamKind::kBufferSrv, 1, 1,
                        nrhi::Visibility::kVertex};
    layout.params[3] = {
        nrhi::BindingParamKind::kTextureTable, 2,
        Player6AEMaterialSnapshot::kTextureFetchSlotCount,
                        nrhi::Visibility::kPixel};
    layout.static_sampler_count = 3;
    // tf0, tf1 and tf5 are the material repeat paths. NRHI cannot express their
    // independently observed mip-point choices, so the observer retains the
    // existing anisotropic approximation and reports it in telemetry.
    layout.static_samplers[0] = {0, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kWrap, 2};
    // tf3/tf4 are the exact point-clamped resolved depth / lookup paths.
    layout.static_samplers[1] = {1, nrhi::Filter::kPoint,
                                 nrhi::AddressMode::kClamp, 1};
    // tf2 is the trace-proven linearly filtered, clamp-to-edge shared mask.
    layout.static_samplers[2] = {2, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kClamp, 2};
    layout.allow_input_layout = false;
    g_resources.layout = device->CreateBindingLayout(layout);
    if (g_resources.layout == nullptr) {
      g_resources.failed = true;
      return false;
    }
  }
  if (g_resources.vertex_shader == nullptr ||
      g_resources.pixel_shader == nullptr) {
    nrhi::ShaderDesc vertex_desc;
    vertex_desc.stage = nrhi::ShaderStage::kVertex;
    vertex_desc.name = "tabletennis_player_6ae_geometry_probe.hlsl";
    vertex_desc.hlsl_source = player_6ae_geometry_probe_shader::kHlsl;
    vertex_desc.entry_point = "vs_main";
    vertex_desc.spirv = reinterpret_cast<const uint32_t *>(
        player_6ae_geometry_probe_shader::kVertexSpirv);
    vertex_desc.spirv_size_bytes =
        player_6ae_geometry_probe_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc pixel_desc;
    pixel_desc.stage = nrhi::ShaderStage::kPixel;
    pixel_desc.name = "tabletennis_player_6ae_geometry_probe.hlsl";
    pixel_desc.hlsl_source = player_6ae_geometry_probe_shader::kHlsl;
    pixel_desc.entry_point = "ps_main";
    pixel_desc.spirv = reinterpret_cast<const uint32_t *>(
        player_6ae_geometry_probe_shader::kPixelSpirv);
    pixel_desc.spirv_size_bytes =
        player_6ae_geometry_probe_shader::kPixelSpirvBytes;
    g_resources.vertex_shader = device->CreateShader(vertex_desc);
    g_resources.pixel_shader = device->CreateShader(pixel_desc);
    if (g_resources.vertex_shader == nullptr ||
        g_resources.pixel_shader == nullptr) {
      g_resources.failed = true;
      return false;
    }
  }

  const nrhi::Format output_format = context.guest_output->format();
  if (g_resources.pipeline == nullptr ||
      g_resources.pipeline_format != output_format) {
    device->DestroyDeferred(g_resources.pipeline);
    g_resources.pipeline = nullptr;
    nrhi::GraphicsPipelineDesc pipeline;
    pipeline.layout = g_resources.layout;
    pipeline.vs = g_resources.vertex_shader;
    pipeline.ps = g_resources.pixel_shader;
    pipeline.input_elements = nullptr;
    pipeline.input_element_count = 0;
    pipeline.vertex_stride = 0;
    pipeline.cull = nrhi::CullMode::kNone;
    pipeline.depth_clip = true;
    pipeline.depth.test_enable = true;
    pipeline.depth.write_enable = true;
    pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
    pipeline.blend.enable = true;
    pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
    pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    pipeline.blend.src_alpha = nrhi::BlendFactor::kOne;
    pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    pipeline.blend.write_mask = 0xF;
    pipeline.rtv_format = output_format;
    pipeline.dsv_format = nrhi::Format::kD32_FLOAT;
    pipeline.sample_count = 1;
    g_resources.pipeline = device->CreateGraphicsPipeline(pipeline);
    if (g_resources.pipeline == nullptr) {
      g_resources.failed = true;
      return false;
    }
    g_resources.pipeline_format = output_format;
  }
  if (g_resources.depth == nullptr ||
      g_resources.depth_width != context.guest_output_width ||
      g_resources.depth_height != context.guest_output_height) {
    nrhi::TextureDesc depth_desc;
    depth_desc.width = context.guest_output_width;
    depth_desc.height = context.guest_output_height;
    depth_desc.format = nrhi::Format::kD32_FLOAT;
    depth_desc.usage = nrhi::kTextureUsageDepthStencil;
    depth_desc.initial_state = nrhi::ResourceState::kDepthWrite;
    nrhi::Texture *const depth = device->CreateTexture(depth_desc);
    if (depth == nullptr) {
      return false;
    }
    device->DestroyDeferred(g_resources.depth);
    g_resources.depth = depth;
    g_resources.depth_width = context.guest_output_width;
    g_resources.depth_height = context.guest_output_height;
  }
  return true;
}

struct GeometryKey {
  uint64_t vertices = 0;
  uint64_t indices = 0;
  bool operator==(const GeometryKey &) const = default;
};

std::optional<size_t> SelectDraw(const Player6AEFrameSnapshot &frame,
                                 int32_t group) {
  std::vector<GeometryKey> groups;
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    const Player6AEDrawSnapshot &draw = frame.draws[index];
    if (draw.vertices == nullptr || draw.indices == nullptr) {
      continue;
    }
    const GeometryKey key = {
        draw.vertices->payload_fingerprint,
        draw.indices->payload_fingerprint,
    };
    if (std::ranges::find(groups, key) != groups.end()) {
      continue;
    }
    if (groups.size() == static_cast<size_t>(group)) {
      return index;
    }
    groups.push_back(key);
  }
  return std::nullopt;
}

bool DrawReady(const Player6AEDrawSnapshot &draw) {
  if (!draw.valid || !draw.backend.valid || draw.vertices == nullptr ||
      !draw.vertices->valid() || draw.indices == nullptr ||
      !draw.indices->valid() || draw.palette == nullptr ||
      !draw.palette->valid() || !draw.material.valid ||
      !draw.material.sampler_contract_valid ||
      draw.identity.primitive_type != 0x06 ||
      draw.identity.submitted_index_count !=
          draw.indices->submitted_index_count ||
      !AllFinite(draw.material.vertex_constants_12_15) ||
      !AllFinite(draw.material.vertex_constants_46_47) ||
      !AllFinite(draw.material.vertex_constant_255)) {
    return false;
  }
  for (size_t index = 0; index < draw.material.material_textures.size();
       ++index) {
    const uint32_t fetch_slot =
        Player6AEMaterialSnapshot::kMaterialTextureFetchSlots[index];
    const auto &texture = draw.material.material_textures[index];
    if (texture == nullptr || !texture->full_mip_chain() ||
        texture->fetch_words != draw.material.texture_fetches[fetch_slot] ||
        texture->fetch_swizzle !=
            draw.material.texture_view_swizzles[fetch_slot]) {
      return false;
    }
  }
  for (size_t index = 0; index < draw.material.shared_textures.size();
       ++index) {
    const uint32_t fetch_slot =
        Player6AEMaterialSnapshot::kSharedTextureFetchSlots[index];
    const auto &texture = draw.material.shared_textures[index];
    if (texture == nullptr || !texture->full_mip_chain() ||
        texture->fetch_words != draw.material.texture_fetches[fetch_slot] ||
        texture->fetch_swizzle !=
            draw.material.texture_view_swizzles[fetch_slot]) {
      return false;
    }
  }
  return std::ranges::all_of(draw.indices->indices, [&](uint16_t index) {
    return index < draw.vertices->vertex_count;
  });
}

bool PreparedMatches(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Player6AEFrameSnapshot> &frame, int32_t group) {
  return frame != nullptr && g_resources.device == context.device &&
         g_resources.prepared_frame == frame &&
         g_resources.prepared_group == group &&
         g_resources.prepared.vertex != nullptr &&
         g_resources.prepared.index != nullptr &&
         std::ranges::all_of(
             g_resources.prepared.textures,
             [](nrhi::TextureView *view) { return view != nullptr; }) &&
         g_resources.palette != nullptr && g_resources.constants != nullptr &&
         g_resources.pipeline != nullptr && g_resources.depth != nullptr;
}

} // namespace

bool Player6AEObserverOverlayEnabled() {
  return REXCVAR_GET(tabletennis_native_player_6ae_observer_overlay);
}

bool PreparePlayer6AEObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Player6AEFrameSnapshot> &frame) {
  if (!Player6AEObserverOverlayEnabled() || frame == nullptr ||
      !frame->valid() || !EnsurePipeline(context)) {
    return false;
  }
  const int32_t group =
      REXCVAR_GET(tabletennis_native_player_6ae_observer_geometry_group);
  if (PreparedMatches(context, frame, group)) {
    return true;
  }
  const std::optional<size_t> selected = SelectDraw(*frame, group);
  if (!selected || !DrawReady(frame->draws[*selected])) {
    if (g_resources.announced_invalid_group != group) {
      g_resources.announced_invalid_group = group;
      REXLOG_WARN("Table Tennis 6AE overlay: geometry group {} unavailable "
                  "in current exact frame",
                  group);
    }
    return false;
  }
  g_resources.announced_invalid_group = -1;
  const Player6AEDrawSnapshot &draw = frame->draws[*selected];
  if (g_resources.prepared_frame != frame) {
    ReleaseTransientTextures();
  }
  GpuVertex *const vertex = EnsureVertex(draw.vertices);
  GpuIndex *const index = EnsureIndex(draw.indices);
  const std::array<std::shared_ptr<const TextureSnapshot>,
                   Player6AEMaterialSnapshot::kTextureFetchSlotCount>
      texture_snapshots = {
          draw.material.material_textures[0],
          draw.material.material_textures[1],
          draw.material.shared_textures[0],
          draw.material.shared_textures[1],
          draw.material.shared_textures[2],
          draw.material.material_textures[2],
      };
  std::array<nrhi::TextureView *,
             Player6AEMaterialSnapshot::kTextureFetchSlotCount>
      textures{};
  for (size_t binding = 0; binding < textures.size(); ++binding) {
    const bool transient = binding >= 2 && binding <= 4;
    GpuTexture *const texture =
        EnsureTexture(context, texture_snapshots[binding], transient);
    if (texture == nullptr) {
      return false;
    }
    textures[binding] = texture->view;
  }
  if (vertex == nullptr || index == nullptr) {
    return false;
  }

  ProbeConstants constants;
  constants.world_to_clip = draw.material.vertex_constants_12_15;
  constants.vertex_constant_19 = draw.material.vertex_constant_19;
  constants.vertex_constants_29_36 =
      draw.material.vertex_constants_29_36;
  std::copy_n(draw.material.vertex_constants_46_47.begin(), 4,
              constants.vertex_constant_46.begin());
  std::copy_n(draw.material.vertex_constants_46_47.begin() + 4, 4,
              constants.vertex_constant_47.begin());
  constants.vertex_constant_255 = draw.material.vertex_constant_255;
  constants.pixel_constant_19 = draw.material.pixel_constant_19;
  constants.pixel_constants_21_27 =
      draw.material.pixel_constants_21_27;
  constants.pixel_constants_46_70 =
      draw.material.pixel_constants_46_70;
  constants.pixel_constants_254_255 =
      draw.material.pixel_constants_254_255;
  constants.buffer_layout = {0, 0, draw.palette->record_count,
                             draw.vertices->stride};
  constants.probe_tint = {1.0f, 0.35f, 0.85f, 0.72f};
  nrhi::Buffer *const palette = CreateAndFillBuffer(
      context.device, draw.palette->raw_bytes.data(),
      draw.palette->raw_bytes.size(),
      AlignUp(draw.palette->raw_bytes.size(), nrhi::kBufferOffsetAlignment),
      nrhi::BufferBindClass::kFull);
  nrhi::Buffer *const constant_buffer = CreateAndFillBuffer(
      context.device, &constants, sizeof(constants),
      AlignUp(sizeof(constants), nrhi::kBufferOffsetAlignment),
      nrhi::BufferBindClass::kFull);
  if (palette == nullptr || constant_buffer == nullptr) {
    context.device->DestroyDeferred(palette);
    context.device->DestroyDeferred(constant_buffer);
    return false;
  }
  context.device->DestroyDeferred(g_resources.palette);
  context.device->DestroyDeferred(g_resources.constants);
  g_resources.palette = palette;
  g_resources.constants = constant_buffer;
  g_resources.prepared_frame = frame;
  g_resources.prepared_group = group;
  g_resources.prepared = {
      .source_draw_index = *selected,
      .vertex = vertex->buffer,
      .index = index->buffer,
      .textures = textures,
      .index_bytes = index->size_bytes,
      .index_count = draw.indices->submitted_index_count,
  };
  return true;
}

uint32_t RenderPlayer6AEObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Player6AEFrameSnapshot> &frame) {
  const int32_t group =
      REXCVAR_GET(tabletennis_native_player_6ae_observer_geometry_group);
  if (!Player6AEObserverOverlayEnabled() ||
      !PreparedMatches(context, frame, group) || context.cmd == nullptr) {
    return 0;
  }
  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.pipeline);
  cmd->SetRenderTargets(context.guest_output, g_resources.depth);
  cmd->ClearDepth(g_resources.depth, 1.0f);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  cmd->SetConstantBuffer(0, g_resources.constants, 0);
  cmd->SetBufferSrv(1, g_resources.prepared.vertex, 0);
  cmd->SetBufferSrv(2, g_resources.palette, 0);
  cmd->SetTextures(3, g_resources.prepared.textures.data(),
                   static_cast<uint32_t>(
                       g_resources.prepared.textures.size()));
  cmd->SetIndexBuffer(g_resources.prepared.index, 0,
                      g_resources.prepared.index_bytes);
  if (!cmd->DrawIndexedChecked(g_resources.prepared.index_count, 0, 0)) {
    return 0;
  }
  if (g_resources.announced_sequence != frame->sequence) {
    g_resources.announced_sequence = frame->sequence;
    const Player6AEDrawSnapshot &draw =
        frame->draws[g_resources.prepared.source_draw_index];
    REXLOG_INFO(
        "Table Tennis 6AE overlay: drew geometry group {} from real immutable "
        "payloads over untouched guest output (frame={} ordinal={} "
        "player={:08X} vf95={:016X} vf92={:016X} ib={:016X} indices={} "
        "material_stage=instructions_19_59 displayed_intermediate=diffuse "
        "texture_fetch=2 texture={:016X} mip_point_approximation=true "
        "private_d32=true "
        "probe_material=true observer_only=true guest_suppressed=false)",
        group, frame->sequence, draw.ordinal, draw.player,
        draw.vertices->payload_fingerprint, draw.palette->payload_fingerprint,
        draw.indices->payload_fingerprint, draw.indices->submitted_index_count,
        draw.material.material_textures[1]->payload_fingerprint);
  }
  return 1;
}

void ShutdownPlayer6AEObserverRenderer() { ReleaseResources(); }

} // namespace tabletennis::native
