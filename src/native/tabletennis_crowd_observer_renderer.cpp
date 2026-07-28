#include "native/tabletennis_crowd_observer_renderer.h"

#include "native/shaders/tabletennis_crowd_observer_spirv.h"
#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_crowd_replacement_prewarm.h"
#include "native/tabletennis_crowd_snapshot.h"
#include "native/tabletennis_crowd_texture_decoder.h"
#include "native/tabletennis_native_scene_compositor.h"
#include "native/tabletennis_native_scene_pass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, tabletennis_native_crowd_observer);
REXCVAR_DEFINE_INT32(
    tabletennis_native_crowd_observer_geometry_group, 0, "Table Tennis",
    "Render the Nth unique immutable crowd geometry payload group from a "
    "fully verified frame. -1 renders every verified crowd draw.")
    .range(-1, 255)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint32_t kCrowdVertexStride = 36;
constexpr uint32_t kNativeSceneRasterizerMode = 0x00018002;
constexpr uint32_t kConstantSliceBytes = 2 * nrhi::kBufferOffsetAlignment;
constexpr size_t kMaximumStaticBufferCacheEntries = 256;
constexpr size_t kMaximumTextureCacheEntries = 32;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct alignas(16) CrowdConstantsGpu {
  std::array<float, 16> instance_transform{};
  std::array<float, 16> view_projection{};
  std::array<float, 12> light_position_inverse_radius{};
  std::array<float, 12> light_color_intensity{};
  std::array<float, 4> ambient{};
  std::array<float, 4> decode_constants{};
  std::array<uint32_t, 4> buffer_layout{};
};

static_assert(sizeof(CrowdConstantsGpu) == 272);
static_assert(sizeof(CrowdConstantsGpu) <= kConstantSliceBytes);

struct CrowdGeometryIdentity {
  uint32_t crowd = 0;
  uint32_t drawable = 0;
  uint32_t submitted_model = 0;
  uint32_t model = 0;
  uint32_t vertex_physical = 0;
  uint32_t vertex_size = 0;
  uint64_t vertex_fingerprint = 0;
  uint32_t index_physical = 0;
  uint32_t index_count = 0;
  uint64_t index_fingerprint = 0;

  bool operator==(const CrowdGeometryIdentity &) const = default;
};

struct GpuVertexPayload {
  std::shared_ptr<const CrowdVertexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
  uint32_t size_bytes = 0;
};

struct GpuIndexPayload {
  std::shared_ptr<const CrowdIndexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
  uint32_t size_bytes = 0;
};

struct GpuTexture {
  std::shared_ptr<const CrowdTextureArrayPayload> snapshot;
  CrowdSamplerContract sampler{};
  nrhi::Texture *texture = nullptr;
  nrhi::TextureView *view = nullptr;
};

struct PreparedDraw {
  nrhi::Buffer *vertex_buffer = nullptr;
  nrhi::Buffer *index_buffer = nullptr;
  nrhi::TextureView *texture_view = nullptr;
  size_t source_draw_index = 0;
  uint32_t index_bytes = 0;
  uint32_t index_count = 0;
  uint32_t primitive_type = 0;
  uint64_t palette_offset = 0;
  uint64_t constant_offset = 0;
};

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::BindingLayout *layout = nullptr;
  nrhi::Shader *vertex_shader = nullptr;
  nrhi::Shader *pixel_shader = nullptr;
  nrhi::Pipeline *pipeline = nullptr;
  nrhi::Pipeline *borrowed_pipeline = nullptr;
  nrhi::Pipeline *native_scene_pipeline = nullptr;
  nrhi::Format borrowed_pipeline_depth_format = nrhi::Format::kUnknown;
  nrhi::Format pipeline_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_color_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_depth_format = nrhi::Format::kUnknown;
  uint32_t native_scene_sample_count = 0;
  CrowdSamplerContract pipeline_sampler{};
  bool pipeline_sampler_valid = false;
  nrhi::Texture *depth = nullptr;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;
  std::vector<GpuVertexPayload> vertices;
  std::vector<GpuIndexPayload> indices;
  std::vector<GpuTexture> textures;
  nrhi::Buffer *palette_buffer = nullptr;
  nrhi::Buffer *constant_buffer = nullptr;
  uint32_t palette_bytes = 0;
  uint32_t constant_bytes = 0;
  std::shared_ptr<const CrowdFrameSnapshot> prepared_frame;
  std::shared_ptr<const CrowdFrameSnapshot> native_scene_frame;
  std::vector<PreparedDraw> prepared_draws;
  int32_t prepared_geometry_group = 0;
  uint64_t prepared_signature = 0;
  uint64_t announced_signature = 0;
  uint32_t announced_native_scene_failure_mask = 0;
  int32_t announced_invalid_geometry_group =
      std::numeric_limits<int32_t>::min();
  uint64_t announced_rejected_texture = 0;
  bool failed = false;
};

Resources g_resources;

template <typename Value> uint64_t AlignUp(Value value, uint64_t alignment) {
  const uint64_t wide = static_cast<uint64_t>(value);
  return (wide + alignment - 1) & ~(alignment - 1);
}

template <size_t Size> bool AllFinite(const std::array<float, Size> &values) {
  return std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value); });
}

void ReleaseResources() {
  if (g_resources.device != nullptr) {
    g_resources.device->DestroyDeferred(g_resources.vertex_shader);
    g_resources.device->DestroyDeferred(g_resources.pixel_shader);
    g_resources.device->DestroyDeferred(g_resources.pipeline);
    g_resources.device->DestroyDeferred(g_resources.borrowed_pipeline);
    g_resources.device->DestroyDeferred(g_resources.native_scene_pipeline);
    g_resources.device->DestroyDeferred(g_resources.depth);
    g_resources.device->DestroyDeferred(g_resources.palette_buffer);
    g_resources.device->DestroyDeferred(g_resources.constant_buffer);
    for (GpuVertexPayload &vertex : g_resources.vertices) {
      g_resources.device->DestroyDeferred(vertex.buffer);
    }
    for (GpuIndexPayload &index : g_resources.indices) {
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

bool EnsureDevice(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (context.device == nullptr) {
    return false;
  }
  if (g_resources.device != nullptr && g_resources.device != context.device) {
    ReleaseResources();
  }
  g_resources.device = context.device;
  return !g_resources.failed;
}

nrhi::Buffer *CreateUploadBuffer(nrhi::Device *device, uint64_t size,
                                 nrhi::BufferBindClass bind_class) {
  if (size == 0) {
    return nullptr;
  }
  nrhi::BufferDesc desc;
  desc.size = size;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = bind_class;
  return device->CreateBuffer(desc);
}

nrhi::Buffer *CreateAndFillBuffer(nrhi::Device *device, const void *source,
                                  uint64_t size,
                                  nrhi::BufferBindClass bind_class) {
  nrhi::Buffer *const buffer = CreateUploadBuffer(device, size, bind_class);
  if (buffer == nullptr) {
    return nullptr;
  }
  void *const mapped = device->Map(buffer);
  if (mapped == nullptr) {
    device->DestroyDeferred(buffer);
    return nullptr;
  }
  std::memcpy(mapped, source, static_cast<size_t>(size));
  device->Unmap(buffer);
  return buffer;
}

GpuVertexPayload *
EnsureVertexPayload(const std::shared_ptr<const CrowdVertexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      snapshot->stride != kCrowdVertexStride || snapshot->raw_bytes.empty() ||
      snapshot->raw_bytes.size() > std::numeric_limits<uint32_t>::max()) {
    return nullptr;
  }
  const auto found =
      std::find_if(g_resources.vertices.begin(), g_resources.vertices.end(),
                   [&](const GpuVertexPayload &vertex) {
                     return vertex.snapshot == snapshot;
                   });
  if (found != g_resources.vertices.end()) {
    return &*found;
  }
  if (g_resources.vertices.size() >= kMaximumStaticBufferCacheEntries) {
    REXLOG_ERROR("Table Tennis crowd observer: vertex cache exhausted");
    return nullptr;
  }
  nrhi::Buffer *const buffer = CreateAndFillBuffer(
      g_resources.device, snapshot->raw_bytes.data(),
      snapshot->raw_bytes.size(), nrhi::BufferBindClass::kFull);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis crowd observer: raw vf95 upload failed");
    return nullptr;
  }
  g_resources.vertices.push_back({
      .snapshot = snapshot,
      .buffer = buffer,
      .size_bytes = static_cast<uint32_t>(snapshot->raw_bytes.size()),
  });
  return &g_resources.vertices.back();
}

GpuVertexPayload *
FindVertexPayload(const std::shared_ptr<const CrowdVertexPayload> &snapshot) {
  const auto found =
      std::find_if(g_resources.vertices.begin(), g_resources.vertices.end(),
                   [&](const GpuVertexPayload &vertex) {
                     return vertex.snapshot == snapshot;
                   });
  return found != g_resources.vertices.end() ? &*found : nullptr;
}

GpuIndexPayload *
EnsureIndexPayload(const std::shared_ptr<const CrowdIndexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() || snapshot->indices.empty() ||
      snapshot->indices.size() >
          std::numeric_limits<uint32_t>::max() / sizeof(uint16_t)) {
    return nullptr;
  }
  const auto found = std::find_if(
      g_resources.indices.begin(), g_resources.indices.end(),
      [&](const GpuIndexPayload &index) { return index.snapshot == snapshot; });
  if (found != g_resources.indices.end()) {
    return &*found;
  }
  if (g_resources.indices.size() >= kMaximumStaticBufferCacheEntries) {
    REXLOG_ERROR("Table Tennis crowd observer: index cache exhausted");
    return nullptr;
  }
  const uint32_t size_bytes =
      static_cast<uint32_t>(snapshot->indices.size() * sizeof(uint16_t));
  nrhi::Buffer *const buffer =
      CreateAndFillBuffer(g_resources.device, snapshot->indices.data(),
                          size_bytes, nrhi::BufferBindClass::kVertexIndex);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis crowd observer: decoded index upload failed");
    return nullptr;
  }
  g_resources.indices.push_back({
      .snapshot = snapshot,
      .buffer = buffer,
      .size_bytes = size_bytes,
  });
  return &g_resources.indices.back();
}

GpuIndexPayload *
FindIndexPayload(const std::shared_ptr<const CrowdIndexPayload> &snapshot) {
  const auto found = std::find_if(
      g_resources.indices.begin(), g_resources.indices.end(),
      [&](const GpuIndexPayload &index) { return index.snapshot == snapshot; });
  return found != g_resources.indices.end() ? &*found : nullptr;
}

GpuTexture *
EnsureTexture(const rex::graphics::NativeGuestOutputRenderContext &context,
              const std::shared_ptr<const CrowdTextureArrayPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid()) {
    return nullptr;
  }
  const auto found = std::find_if(
      g_resources.textures.begin(), g_resources.textures.end(),
      [&](const GpuTexture &texture) { return texture.snapshot == snapshot; });
  if (found != g_resources.textures.end()) {
    return &*found;
  }
  if (g_resources.textures.size() >= kMaximumTextureCacheEntries) {
    REXLOG_ERROR("Table Tennis crowd observer: texture cache exhausted");
    return nullptr;
  }

  DecodedCrowdVolume decoded;
  const CrowdTextureDecodeFailure failure =
      DecodeCrowdTextureVolume(*snapshot, decoded);
  if (failure != CrowdTextureDecodeFailure::kNone) {
    if (g_resources.announced_rejected_texture !=
        snapshot->payload_fingerprint) {
      g_resources.announced_rejected_texture = snapshot->payload_fingerprint;
      REXLOG_ERROR("Table Tennis crowd observer: rejected unproven volume "
                   "fetch/payload={} fingerprint={:016X} "
                   "words={{{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}}} "
                   "shape={}x{}x{} dimension={} format={} endian={} pitch={} "
                   "swizzle={:03X} tiled={} stacked={} volume={} bytes={}",
                   CrowdTextureDecodeFailureName(failure),
                   snapshot->payload_fingerprint, snapshot->fetch_words[0],
                   snapshot->fetch_words[1], snapshot->fetch_words[2],
                   snapshot->fetch_words[3], snapshot->fetch_words[4],
                   snapshot->fetch_words[5], snapshot->width, snapshot->height,
                   snapshot->layers, snapshot->dimension, snapshot->format,
                   snapshot->endianness, snapshot->pitch_blocks,
                   snapshot->fetch_swizzle, snapshot->tiled, snapshot->stacked,
                   snapshot->volume, snapshot->byte_size);
    }
    return nullptr;
  }

  const uint32_t source_row_pitch = decoded.width * 4;
  const uint32_t upload_row_pitch = static_cast<uint32_t>(
      AlignUp(source_row_pitch, nrhi::kRowPitchAlignment));
  const uint64_t upload_size =
      static_cast<uint64_t>(upload_row_pitch) * decoded.height * decoded.depth;

  nrhi::TextureDesc texture_desc;
  texture_desc.kind = nrhi::TextureKind::k3D;
  texture_desc.width = decoded.width;
  texture_desc.height = decoded.height;
  texture_desc.depth = decoded.depth;
  texture_desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  texture_desc.initial_state = nrhi::ResourceState::kCopyDest;
  nrhi::Texture *const texture = context.device->CreateTexture(texture_desc);
  nrhi::Buffer *const upload = CreateUploadBuffer(
      context.device, upload_size, nrhi::BufferBindClass::kCopySrc);
  if (texture == nullptr || upload == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR(
        "Table Tennis crowd observer: volume resource creation failed");
    return nullptr;
  }

  uint8_t *const mapped = static_cast<uint8_t *>(context.device->Map(upload));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR("Table Tennis crowd observer: volume upload map failed");
    return nullptr;
  }
  std::memset(mapped, 0, static_cast<size_t>(upload_size));
  for (uint32_t z = 0; z < decoded.depth; ++z) {
    for (uint32_t y = 0; y < decoded.height; ++y) {
      const size_t row = static_cast<size_t>(z) * decoded.height + y;
      std::memcpy(mapped + row * upload_row_pitch,
                  decoded.rgba8.data() + row * source_row_pitch,
                  source_row_pitch);
    }
  }
  context.device->Unmap(upload);

  nrhi::TextureViewDesc view_desc;
  view_desc.dimension = nrhi::ViewDimension::k3D;
  nrhi::TextureView *const view =
      context.device->CreateTextureView(texture, view_desc);
  if (view == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR("Table Tennis crowd observer: volume view creation failed");
    return nullptr;
  }

  context.cmd->CopyBufferToTexture(texture, 0, 0, upload, 0, upload_row_pitch,
                                   decoded.width, decoded.height,
                                   decoded.depth);
  context.cmd->Barrier(texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.device->DestroyDeferred(upload);
  g_resources.textures.push_back({
      .snapshot = snapshot,
      .sampler = decoded.sampler,
      .texture = texture,
      .view = view,
  });
  REXLOG_INFO("Table Tennis crowd observer: uploaded exact tiled 3D DXT1 "
              "payload as RGBA8 volume {}x{}x{} fingerprint={:016X}",
              decoded.width, decoded.height, decoded.depth,
              snapshot->payload_fingerprint);
  return &g_resources.textures.back();
}

GpuTexture *
FindTexture(const std::shared_ptr<const CrowdTextureArrayPayload> &snapshot) {
  const auto found = std::find_if(
      g_resources.textures.begin(), g_resources.textures.end(),
      [&](const GpuTexture &texture) { return texture.snapshot == snapshot; });
  return found != g_resources.textures.end() ? &*found : nullptr;
}

bool EnsurePipelineResources(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const CrowdSamplerContract &sampler) {
  if (!EnsureDevice(context) || context.guest_output == nullptr ||
      !sampler.point_volume || sampler.filter != nrhi::Filter::kAnisotropic ||
      sampler.address != nrhi::AddressMode::kWrap ||
      sampler.max_anisotropy != 2) {
    return false;
  }
  nrhi::Device *const device = context.device;
  if (g_resources.pipeline_sampler_valid &&
      g_resources.pipeline_sampler != sampler) {
    REXLOG_ERROR("Table Tennis crowd observer: sampler contract changed; "
                 "refusing an unproven pipeline");
    return false;
  }

  if (g_resources.layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 4;
    layout.params[0] = {nrhi::BindingParamKind::kConstantBuffer, 0, 1,
                        nrhi::Visibility::kAll};
    layout.params[1] = {nrhi::BindingParamKind::kBufferSrv, 0, 1,
                        nrhi::Visibility::kVertex};
    layout.params[2] = {nrhi::BindingParamKind::kBufferSrv, 1, 1,
                        nrhi::Visibility::kVertex};
    layout.params[3] = {nrhi::BindingParamKind::kTextureTable, 2, 1,
                        nrhi::Visibility::kPixel};
    layout.static_sampler_count = 1;
    layout.static_samplers[0] = {0, sampler.filter, sampler.address,
                                 sampler.max_anisotropy};
    layout.allow_input_layout = false;
    g_resources.layout = device->CreateBindingLayout(layout);
    if (g_resources.layout == nullptr) {
      REXLOG_ERROR(
          "Table Tennis crowd observer: binding layout creation failed");
      g_resources.failed = true;
      return false;
    }
    g_resources.pipeline_sampler = sampler;
    g_resources.pipeline_sampler_valid = true;
  }

  if (g_resources.vertex_shader == nullptr ||
      g_resources.pixel_shader == nullptr) {
    nrhi::ShaderDesc vertex_desc;
    vertex_desc.stage = nrhi::ShaderStage::kVertex;
    vertex_desc.name = "tabletennis_crowd_observer.hlsl";
    vertex_desc.hlsl_source = crowd_observer_shader::kHlsl;
    vertex_desc.entry_point = "vs_main";
    vertex_desc.spirv = crowd_observer_shader::kVertexSpirv;
    vertex_desc.spirv_size_bytes = crowd_observer_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc pixel_desc;
    pixel_desc.stage = nrhi::ShaderStage::kPixel;
    pixel_desc.name = "tabletennis_crowd_observer.hlsl";
    pixel_desc.hlsl_source = crowd_observer_shader::kHlsl;
    pixel_desc.entry_point = "ps_main";
    pixel_desc.spirv = crowd_observer_shader::kPixelSpirv;
    pixel_desc.spirv_size_bytes = crowd_observer_shader::kPixelSpirvBytes;
    g_resources.vertex_shader = device->CreateShader(vertex_desc);
    g_resources.pixel_shader = device->CreateShader(pixel_desc);
    if (g_resources.vertex_shader == nullptr ||
        g_resources.pixel_shader == nullptr) {
      REXLOG_ERROR("Table Tennis crowd observer: shader creation failed");
      g_resources.failed = true;
      return false;
    }
  }

  return true;
}

bool EnsurePipeline(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const CrowdSamplerContract &sampler) {
  if (!EnsurePipelineResources(context, sampler)) {
    return false;
  }
  nrhi::Device *const device = context.device;
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
    // The captured PA_SU state requests back-face culling, but NRHI's host
    // front-face convention is not yet proven equivalent. Cull-none is the
    // deliberately visible observer discrepancy, not a guest replacement.
    pipeline.cull = nrhi::CullMode::kNone;
    pipeline.depth_clip = true;
    pipeline.depth.test_enable = true;
    pipeline.depth.write_enable = true;
    pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
    pipeline.blend.enable = false;
    pipeline.blend.write_mask = 0xF;
    pipeline.rtv_format = output_format;
    pipeline.dsv_format = nrhi::Format::kD32_FLOAT;
    pipeline.sample_count = 1;
    g_resources.pipeline = device->CreateGraphicsPipeline(pipeline);
    if (g_resources.pipeline == nullptr) {
      REXLOG_ERROR("Table Tennis crowd observer: graphics pipeline creation "
                   "failed");
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
      REXLOG_ERROR(
          "Table Tennis crowd observer: private depth creation failed");
      return false;
    }
    device->DestroyDeferred(g_resources.depth);
    g_resources.depth = depth;
    g_resources.depth_width = context.guest_output_width;
    g_resources.depth_height = context.guest_output_height;
  }
  return true;
}

bool EnsureNativeScenePipeline(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const CrowdSamplerContract &sampler,
    const NativeScenePassTargets &targets) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      !EnsurePipelineResources(context, sampler)) {
    return false;
  }

  const nrhi::Format color_format = targets.color->format();
  const nrhi::Format depth_format = targets.depth->format();
  if (g_resources.native_scene_pipeline != nullptr &&
      g_resources.native_scene_color_format == color_format &&
      g_resources.native_scene_depth_format == depth_format &&
      g_resources.native_scene_sample_count == targets.sample_count) {
    return true;
  }
  context.device->DestroyDeferred(g_resources.native_scene_pipeline);
  g_resources.native_scene_pipeline = nullptr;

  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = g_resources.layout;
  pipeline.vs = g_resources.vertex_shader;
  pipeline.ps = g_resources.pixel_shader;
  pipeline.input_elements = nullptr;
  pipeline.input_element_count = 0;
  pipeline.vertex_stride = 0;
  pipeline.cull = nrhi::CullMode::kBack;
  pipeline.front_face_clockwise = false;
  pipeline.depth_clip = true;
  pipeline.depth.test_enable = true;
  pipeline.depth.write_enable = true;
  pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
  pipeline.blend.alpha_to_coverage = false;
  pipeline.blend.enable = false;
  pipeline.blend.write_mask = 0x7;
  pipeline.rtv_format = color_format;
  pipeline.dsv_format = depth_format;
  pipeline.sample_count = targets.sample_count;
  g_resources.native_scene_pipeline =
      context.device->CreateGraphicsPipeline(pipeline);
  if (g_resources.native_scene_pipeline == nullptr) {
    REXLOG_ERROR(
        "Table Tennis C6 native scene: shared-pass pipeline creation failed");
    return false;
  }
  g_resources.native_scene_color_format = color_format;
  g_resources.native_scene_depth_format = depth_format;
  g_resources.native_scene_sample_count = targets.sample_count;
  return true;
}

CrowdGeometryIdentity GeometryIdentityForDraw(const CrowdDrawSnapshot &draw) {
  return {
      .crowd = draw.owner.crowd,
      .drawable = draw.owner.submitted_drawable,
      .submitted_model = draw.owner.submitted_model,
      .model = draw.model,
      .vertex_physical = draw.vertices->fetch.physical_address,
      .vertex_size = draw.vertices->fetch.size,
      .vertex_fingerprint = draw.vertices->payload_fingerprint,
      .index_physical = draw.indices->physical_address,
      .index_count = draw.indices->submitted_index_count,
      .index_fingerprint = draw.indices->payload_fingerprint,
  };
}

struct DrawSelection {
  std::vector<size_t> indices;
  bool group_found = false;
};

DrawSelection SelectDraws(const CrowdFrameSnapshot &frame,
                          int32_t geometry_group) {
  DrawSelection selection;
  if (geometry_group < 0) {
    selection.indices.reserve(frame.draws.size());
    for (size_t index = 0; index < frame.draws.size(); ++index) {
      selection.indices.push_back(index);
    }
    selection.group_found = !selection.indices.empty();
    return selection;
  }

  std::vector<CrowdGeometryIdentity> groups;
  groups.reserve(frame.draws.size());
  CrowdGeometryIdentity selected{};
  for (const CrowdDrawSnapshot &draw : frame.draws) {
    const CrowdGeometryIdentity identity = GeometryIdentityForDraw(draw);
    if (std::find(groups.begin(), groups.end(), identity) != groups.end()) {
      continue;
    }
    if (groups.size() == static_cast<size_t>(geometry_group)) {
      selected = identity;
      selection.group_found = true;
      break;
    }
    groups.push_back(identity);
  }
  if (!selection.group_found) {
    return selection;
  }
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    if (GeometryIdentityForDraw(frame.draws[index]) == selected) {
      selection.indices.push_back(index);
    }
  }
  return selection;
}

bool MaterialReady(const CrowdMaterialSnapshot &material) {
  return material.valid && material.declaration.valid &&
         material.texture != nullptr && material.texture->valid() &&
         material.texture_fetch == material.texture->fetch_words &&
         material.decode_constants_verified &&
         AllFinite(material.instance_transform) &&
         AllFinite(material.view_projection) &&
         AllFinite(material.lighting_spheres) && AllFinite(material.ambient) &&
         AllFinite(material.decode_constants);
}

bool DrawReady(const CrowdDrawSnapshot &draw) {
  return draw.valid && draw.owner.valid && draw.owner.submitted_drawable != 0 &&
         draw.owner.submitted_model != 0 && draw.vertices != nullptr &&
         draw.vertices->valid() &&
         draw.vertices->stride == kCrowdVertexStride &&
         draw.indices != nullptr && draw.indices->valid() &&
         draw.palette != nullptr && draw.palette->valid() &&
         draw.submitted_index_count == draw.indices->submitted_index_count &&
         draw.primitive_type == 0x06 && MaterialReady(draw.material);
}

bool FrameReady(const CrowdFrameSnapshot &frame) {
  return frame.valid() && std::all_of(frame.draws.begin(), frame.draws.end(),
                                      [](const CrowdDrawSnapshot &draw) {
                                        return DrawReady(draw);
                                      });
}

bool NativeSceneFrameReady(const CrowdFrameSnapshot &frame) {
  const CrowdBackendBlockContractTelemetry &contract =
      frame.backend_last_contract;
  return FrameReady(frame) && frame.backend_block_proof_observed &&
         contract.block_uniform && contract.rasterizer_mode_control_valid &&
         contract.rasterizer_mode_control == kNativeSceneRasterizerMode;
}

CrowdConstantsGpu ConstantsForDraw(const CrowdDrawSnapshot &draw) {
  CrowdConstantsGpu constants;
  constants.instance_transform = draw.material.instance_transform;
  constants.view_projection = draw.material.view_projection;
  std::copy_n(draw.material.lighting_spheres.begin(), 12,
              constants.light_position_inverse_radius.begin());
  std::copy_n(draw.material.lighting_spheres.begin() + 12, 12,
              constants.light_color_intensity.begin());
  constants.ambient = draw.material.ambient;
  constants.decode_constants = draw.material.decode_constants;
  constants.buffer_layout = {0, 0, draw.palette->record_count,
                             draw.vertices->stride};
  return constants;
}

uint64_t GeometrySignature(const CrowdFrameSnapshot &frame,
                           int32_t geometry_group,
                           const std::vector<PreparedDraw> &draws) {
  uint64_t signature = kFnvOffsetBasis;
  auto append = [&](uint64_t value) {
    signature ^= value;
    signature *= kFnvPrime;
  };
  append(static_cast<uint32_t>(geometry_group));
  append(draws.size());
  for (const PreparedDraw &prepared : draws) {
    const CrowdDrawSnapshot &draw = frame.draws[prepared.source_draw_index];
    append(draw.owner.crowd);
    append(draw.owner.submitted_drawable);
    append(draw.owner.submitted_model);
    append(draw.model);
    append(draw.vertices->fetch.physical_address);
    append(draw.vertices->fetch.size);
    append(draw.vertices->payload_fingerprint);
    append(draw.indices->physical_address);
    append(draw.indices->submitted_index_count);
    append(draw.indices->payload_fingerprint);
    // The palette contents are intentionally dynamic and normally change
    // every frame. Use only their binding shape in the announcement
    // signature so animation does not turn one useful structural message
    // into per-frame log spam.
    append(draw.palette->fetch.size);
    append(draw.palette->record_count);
    append(draw.material.texture->payload_fingerprint);
  }
  return signature;
}

struct PaletteSlice {
  std::shared_ptr<const CrowdPalettePayload> snapshot;
  uint64_t offset = 0;
};

bool PrepareDynamicFrame(const std::shared_ptr<const CrowdFrameSnapshot> &frame,
                         int32_t geometry_group,
                         std::vector<PreparedDraw> prepared_draws) {
  std::vector<PaletteSlice> palettes;
  palettes.reserve(prepared_draws.size());
  uint64_t palette_bytes = 0;
  for (const PreparedDraw &prepared : prepared_draws) {
    const CrowdDrawSnapshot &draw = frame->draws[prepared.source_draw_index];
    const auto found = std::find_if(palettes.begin(), palettes.end(),
                                    [&](const PaletteSlice &palette) {
                                      return palette.snapshot == draw.palette;
                                    });
    if (found != palettes.end()) {
      continue;
    }
    palette_bytes = AlignUp(palette_bytes, nrhi::kBufferOffsetAlignment);
    palettes.push_back({
        .snapshot = draw.palette,
        .offset = palette_bytes,
    });
    palette_bytes += draw.palette->raw_bytes.size();
  }
  palette_bytes = AlignUp(palette_bytes, nrhi::kBufferOffsetAlignment);
  const uint64_t constant_bytes =
      static_cast<uint64_t>(prepared_draws.size()) * kConstantSliceBytes;
  if (palette_bytes == 0 || constant_bytes == 0 ||
      palette_bytes > std::numeric_limits<uint32_t>::max() ||
      constant_bytes > std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  nrhi::Buffer *const palette_buffer = CreateUploadBuffer(
      g_resources.device, palette_bytes, nrhi::BufferBindClass::kFull);
  nrhi::Buffer *const constant_buffer = CreateUploadBuffer(
      g_resources.device, constant_bytes, nrhi::BufferBindClass::kFull);
  if (palette_buffer == nullptr || constant_buffer == nullptr) {
    g_resources.device->DestroyDeferred(palette_buffer);
    g_resources.device->DestroyDeferred(constant_buffer);
    REXLOG_ERROR("Table Tennis crowd observer: per-frame buffer creation "
                 "failed");
    return false;
  }

  uint8_t *const mapped_palettes =
      static_cast<uint8_t *>(g_resources.device->Map(palette_buffer));
  uint8_t *const mapped_constants =
      static_cast<uint8_t *>(g_resources.device->Map(constant_buffer));
  if (mapped_palettes == nullptr || mapped_constants == nullptr) {
    if (mapped_palettes != nullptr) {
      g_resources.device->Unmap(palette_buffer);
    }
    if (mapped_constants != nullptr) {
      g_resources.device->Unmap(constant_buffer);
    }
    g_resources.device->DestroyDeferred(palette_buffer);
    g_resources.device->DestroyDeferred(constant_buffer);
    REXLOG_ERROR("Table Tennis crowd observer: per-frame buffer map failed");
    return false;
  }
  std::memset(mapped_palettes, 0, static_cast<size_t>(palette_bytes));
  std::memset(mapped_constants, 0, static_cast<size_t>(constant_bytes));
  for (const PaletteSlice &palette : palettes) {
    std::memcpy(mapped_palettes + palette.offset,
                palette.snapshot->raw_bytes.data(),
                palette.snapshot->raw_bytes.size());
  }
  for (size_t index = 0; index < prepared_draws.size(); ++index) {
    const CrowdDrawSnapshot &draw =
        frame->draws[prepared_draws[index].source_draw_index];
    const auto palette = std::find_if(
        palettes.begin(), palettes.end(), [&](const PaletteSlice &candidate) {
          return candidate.snapshot == draw.palette;
        });
    if (palette == palettes.end()) {
      g_resources.device->Unmap(palette_buffer);
      g_resources.device->Unmap(constant_buffer);
      g_resources.device->DestroyDeferred(palette_buffer);
      g_resources.device->DestroyDeferred(constant_buffer);
      return false;
    }
    prepared_draws[index].palette_offset = palette->offset;
    prepared_draws[index].constant_offset =
        static_cast<uint64_t>(index) * kConstantSliceBytes;
    const CrowdConstantsGpu constants = ConstantsForDraw(draw);
    std::memcpy(mapped_constants + prepared_draws[index].constant_offset,
                &constants, sizeof(constants));
  }
  g_resources.device->Unmap(palette_buffer);
  g_resources.device->Unmap(constant_buffer);

  g_resources.device->DestroyDeferred(g_resources.palette_buffer);
  g_resources.device->DestroyDeferred(g_resources.constant_buffer);
  g_resources.palette_buffer = palette_buffer;
  g_resources.constant_buffer = constant_buffer;
  g_resources.palette_bytes = static_cast<uint32_t>(palette_bytes);
  g_resources.constant_bytes = static_cast<uint32_t>(constant_bytes);
  g_resources.prepared_frame = frame;
  g_resources.prepared_geometry_group = geometry_group;
  g_resources.prepared_signature =
      GeometrySignature(*frame, geometry_group, prepared_draws);
  g_resources.prepared_draws = std::move(prepared_draws);
  return true;
}

bool PrepareExactReplacementFrameFromWarmCaches(
    const std::shared_ptr<const CrowdFrameSnapshot> &frame) {
  if (frame == nullptr || !FrameReady(*frame)) {
    return false;
  }
  const DrawSelection selection = SelectDraws(*frame, -1);
  if (selection.indices.empty()) {
    return false;
  }

  std::vector<PreparedDraw> prepared_draws;
  prepared_draws.reserve(selection.indices.size());
  for (size_t source_draw_index : selection.indices) {
    const CrowdDrawSnapshot &draw = frame->draws[source_draw_index];
    GpuVertexPayload *const vertex = FindVertexPayload(draw.vertices);
    GpuIndexPayload *const index = FindIndexPayload(draw.indices);
    GpuTexture *const texture = FindTexture(draw.material.texture);
    if (vertex == nullptr || index == nullptr || texture == nullptr) {
      return false;
    }
    prepared_draws.push_back({
        .vertex_buffer = vertex->buffer,
        .index_buffer = index->buffer,
        .texture_view = texture->view,
        .source_draw_index = source_draw_index,
        .index_bytes = index->size_bytes,
        .index_count = draw.submitted_index_count,
        .primitive_type = draw.primitive_type,
    });
  }

  // The output callback has already uploaded all immutable mesh and volume
  // texture resources. Only host-visible/coherent upload buffers are created
  // and filled here, so no Vulkan copy or barrier is recorded in the borrowed
  // guest render scope.
  return PrepareDynamicFrame(frame, -1, std::move(prepared_draws));
}

bool PreparedFrameMatches(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame,
    int32_t geometry_group) {
  return frame != nullptr && g_resources.prepared_frame == frame &&
         g_resources.prepared_geometry_group == geometry_group &&
         g_resources.device == context.device &&
         g_resources.layout != nullptr && g_resources.pipeline != nullptr &&
         g_resources.depth != nullptr &&
         g_resources.depth_width == context.guest_output_width &&
         g_resources.depth_height == context.guest_output_height &&
         g_resources.palette_buffer != nullptr &&
         g_resources.constant_buffer != nullptr &&
         !g_resources.prepared_draws.empty();
}

bool PreparedNativeSceneFrameMatches(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      frame == nullptr || g_resources.native_scene_frame != frame ||
      g_resources.prepared_frame != frame ||
      g_resources.prepared_geometry_group != -1 ||
      g_resources.device != context.device || g_resources.layout == nullptr ||
      g_resources.native_scene_pipeline == nullptr ||
      g_resources.native_scene_color_format != targets.color->format() ||
      g_resources.native_scene_depth_format != targets.depth->format() ||
      g_resources.native_scene_sample_count != targets.sample_count ||
      g_resources.palette_buffer == nullptr ||
      g_resources.constant_buffer == nullptr ||
      g_resources.prepared_draws.size() != frame->draws.size()) {
    return false;
  }
  for (size_t index = 0; index < g_resources.prepared_draws.size(); ++index) {
    const PreparedDraw &prepared = g_resources.prepared_draws[index];
    const CrowdDrawSnapshot &draw = frame->draws[index];
    if (prepared.source_draw_index != index ||
        prepared.vertex_buffer == nullptr || prepared.index_buffer == nullptr ||
        prepared.texture_view == nullptr ||
        prepared.index_count != draw.submitted_index_count ||
        prepared.primitive_type != draw.primitive_type) {
      return false;
    }
  }
  return true;
}

bool EnsureBorrowedPipeline(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (context.backend != rex::graphics::NativeGuestOutputBackend::kVulkan ||
      context.device == nullptr || g_resources.device != context.device ||
      g_resources.layout == nullptr || g_resources.vertex_shader == nullptr ||
      g_resources.pixel_shader == nullptr ||
      !g_resources.pipeline_sampler_valid) {
    return false;
  }
  if (g_resources.borrowed_pipeline != nullptr &&
      g_resources.borrowed_pipeline_depth_format ==
          context.depth_attachment_format) {
    return true;
  }
  context.device->DestroyDeferred(g_resources.borrowed_pipeline);
  g_resources.borrowed_pipeline = nullptr;
  g_resources.borrowed_pipeline_depth_format = nrhi::Format::kUnknown;

  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = g_resources.layout;
  pipeline.vs = g_resources.vertex_shader;
  pipeline.ps = g_resources.pixel_shader;
  pipeline.input_elements = nullptr;
  pipeline.input_element_count = 0;
  pipeline.vertex_stride = 0;
  pipeline.cull = nrhi::CullMode::kBack;
  pipeline.depth_clip = true;
  pipeline.depth.test_enable = true;
  pipeline.depth.write_enable = true;
  pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
  pipeline.blend.alpha_to_coverage = false;
  pipeline.blend.enable = false;
  pipeline.blend.write_mask = 0x7;
  pipeline.rtv_format = nrhi::Format::kR8G8B8A8_UNORM;
  pipeline.dsv_format = context.depth_attachment_format;
  pipeline.sample_count = 4;
  g_resources.borrowed_pipeline =
      context.device->CreateGraphicsPipeline(pipeline);
  if (g_resources.borrowed_pipeline == nullptr) {
    REXLOG_ERROR("Table Tennis crowd prewarm: exact borrowed RGBA8/D32S8 4x "
                 "pipeline creation failed");
    return false;
  }
  g_resources.borrowed_pipeline_depth_format = context.depth_attachment_format;
  return true;
}

} // namespace

bool CrowdObserverOverlayEnabled() {
  return REXCVAR_GET(tabletennis_native_crowd_observer);
}

static bool PrepareCrowdResources(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame,
    int32_t geometry_group,
    const NativeScenePassTargets *native_scene_targets = nullptr) {
  if (frame == nullptr || !FrameReady(*frame) || context.cmd == nullptr ||
      !EnsureDevice(context)) {
    return false;
  }
  if (native_scene_targets != nullptr
          ? PreparedNativeSceneFrameMatches(context, *native_scene_targets,
                                            frame)
          : PreparedFrameMatches(context, frame, geometry_group)) {
    return true;
  }

  const DrawSelection selection = SelectDraws(*frame, geometry_group);
  if (selection.indices.empty()) {
    if (g_resources.announced_invalid_geometry_group != geometry_group) {
      g_resources.announced_invalid_geometry_group = geometry_group;
      REXLOG_WARN("Table Tennis crowd observer: geometry group {} is "
                  "outside the current exact crowd frame",
                  geometry_group);
    }
    return false;
  }
  g_resources.announced_invalid_geometry_group =
      std::numeric_limits<int32_t>::min();

  std::vector<PreparedDraw> prepared_draws;
  prepared_draws.reserve(selection.indices.size());
  CrowdSamplerContract sampler{};
  bool sampler_valid = false;
  for (size_t source_draw_index : selection.indices) {
    const CrowdDrawSnapshot &draw = frame->draws[source_draw_index];
    GpuVertexPayload *const vertex = EnsureVertexPayload(draw.vertices);
    GpuIndexPayload *const index = EnsureIndexPayload(draw.indices);
    GpuTexture *const texture = EnsureTexture(context, draw.material.texture);
    if (vertex == nullptr || index == nullptr || texture == nullptr) {
      return false;
    }
    if (!sampler_valid) {
      sampler = texture->sampler;
      sampler_valid = true;
    } else if (sampler != texture->sampler) {
      REXLOG_ERROR("Table Tennis crowd observer: selected draws disagree on "
                   "the proven sampler contract");
      return false;
    }
    prepared_draws.push_back({
        .vertex_buffer = vertex->buffer,
        .index_buffer = index->buffer,
        .texture_view = texture->view,
        .source_draw_index = source_draw_index,
        .index_bytes = index->size_bytes,
        .index_count = draw.submitted_index_count,
        .primitive_type = draw.primitive_type,
    });
  }
  if (!sampler_valid ||
      (native_scene_targets != nullptr
           ? !EnsureNativeScenePipeline(context, sampler, *native_scene_targets)
           : !EnsurePipeline(context, sampler))) {
    return false;
  }
  if (!PrepareDynamicFrame(frame, geometry_group, std::move(prepared_draws))) {
    return false;
  }
  if (native_scene_targets != nullptr) {
    g_resources.native_scene_frame = frame;
  }
  return true;
}

bool PrepareCrowdObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame) {
  if (!CrowdObserverOverlayEnabled()) {
    return false;
  }
  return PrepareCrowdResources(
      context, frame,
      REXCVAR_GET(tabletennis_native_crowd_observer_geometry_group));
}

bool PrepareCrowdReplacementPrewarmResources(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame) {
  return CrowdReplacementPrewarmEnabled() &&
         PrepareCrowdResources(context, frame, -1);
}

uint32_t RenderCrowdObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame) {
  const int32_t geometry_group =
      REXCVAR_GET(tabletennis_native_crowd_observer_geometry_group);
  const int32_t prepared_geometry_group = g_resources.prepared_geometry_group;
  if (!CrowdObserverOverlayEnabled() ||
      !PreparedFrameMatches(context, frame, prepared_geometry_group) ||
      (prepared_geometry_group != geometry_group &&
       prepared_geometry_group != -1) ||
      context.cmd == nullptr) {
    return 0;
  }

  DrawSelection overlay_selection;
  const bool filter_prepared_draws = prepared_geometry_group != geometry_group;
  if (filter_prepared_draws) {
    overlay_selection = SelectDraws(*frame, geometry_group);
    if (overlay_selection.indices.empty()) {
      return 0;
    }
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.pipeline);
  cmd->SetRenderTargets(context.guest_output, g_resources.depth);
  cmd->ClearDepth(g_resources.depth, 1.0f);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  uint32_t draw_count = 0;
  const CrowdDrawSnapshot *first_draw = nullptr;
  for (const PreparedDraw &draw : g_resources.prepared_draws) {
    if (draw.primitive_type != 0x06 ||
        (filter_prepared_draws &&
         std::find(overlay_selection.indices.begin(),
                   overlay_selection.indices.end(), draw.source_draw_index) ==
             overlay_selection.indices.end())) {
      continue;
    }
    if (first_draw == nullptr) {
      first_draw = &frame->draws[draw.source_draw_index];
    }
    cmd->SetConstantBuffer(0, g_resources.constant_buffer,
                           draw.constant_offset);
    cmd->SetBufferSrv(1, draw.vertex_buffer, 0);
    cmd->SetBufferSrv(2, g_resources.palette_buffer, draw.palette_offset);
    cmd->SetTexture(3, draw.texture_view);
    cmd->SetIndexBuffer(draw.index_buffer, 0, draw.index_bytes);
    cmd->DrawIndexed(draw.index_count, 0, 0);
    ++draw_count;
  }

  if (draw_count != 0 &&
      g_resources.announced_signature != g_resources.prepared_signature) {
    g_resources.announced_signature = g_resources.prepared_signature;
    const CrowdDrawSnapshot &first = *first_draw;
    REXLOG_INFO(
        "Table Tennis crowd observer: drew geometry group {} as {} "
        "captured draws in guest order over untouched output "
        "(crowd={:08X} drawable={:08X} model={:08X} "
        "vf95={:08X}/{:016X} ib={:08X}/{:016X} indices={} "
        "family_contract=true trace_parity={} private_d32=true "
        "cull_none_discrepancy=true observer_only=true)",
        geometry_group, draw_count, first.owner.crowd,
        first.owner.submitted_drawable, first.owner.submitted_model,
        first.vertices->fetch.physical_address,
        first.vertices->payload_fingerprint, first.indices->physical_address,
        first.indices->payload_fingerprint,
        first.indices->submitted_index_count, frame->trace_parity_verified);
  }
  return draw_count;
}

bool PrepareCrowdNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame) {
  constexpr uint32_t kInvalidTargets = 1u << 0;
  constexpr uint32_t kMissingFrame = 1u << 1;
  constexpr uint32_t kInvalidFrame = 1u << 2;
  constexpr uint32_t kMissingBackendProof = 1u << 3;
  constexpr uint32_t kInvalidBackendContract = 1u << 4;
  constexpr uint32_t kResourcePreparationFailed = 1u << 5;

  uint32_t failure = 0;
  const NativeScenePassTargetValidation target_validation =
      ValidateNativeScenePassTargets(context, targets);
  if (target_validation != NativeScenePassTargetValidation::kValid) {
    failure = kInvalidTargets;
  } else if (frame == nullptr) {
    failure = kMissingFrame;
  } else if (!FrameReady(*frame)) {
    failure = kInvalidFrame;
  } else if (!frame->backend_block_proof_observed) {
    failure = kMissingBackendProof;
  } else if (!frame->backend_last_contract.block_uniform ||
             !frame->backend_last_contract.rasterizer_mode_control_valid ||
             frame->backend_last_contract.rasterizer_mode_control !=
                 kNativeSceneRasterizerMode) {
    failure = kInvalidBackendContract;
  } else if (!PrepareCrowdResources(context, frame, -1, &targets)) {
    failure = kResourcePreparationFailed;
  } else {
    return true;
  }

  if ((g_resources.announced_native_scene_failure_mask & failure) == 0) {
    g_resources.announced_native_scene_failure_mask |= failure;
    const CrowdBackendBlockContractTelemetry contract =
        frame != nullptr ? frame->backend_last_contract
                         : CrowdBackendBlockContractTelemetry{};
    REXLOG_INFO(
        "Table Tennis C6 native scene: preparation rejected reason={} "
        "target={} frame={} draws={} frame_valid={} proof={} "
        "contract[uniform={} raster={:08X}/{}] "
        "resources[failed={} vertices={} indices={} textures={} "
        "pipeline={} prepared={}]",
        failure, static_cast<uint32_t>(target_validation),
        frame != nullptr ? frame->sequence : 0,
        frame != nullptr ? frame->draws.size() : 0,
        frame != nullptr && FrameReady(*frame),
        frame != nullptr && frame->backend_block_proof_observed,
        contract.block_uniform, contract.rasterizer_mode_control,
        contract.rasterizer_mode_control_valid, g_resources.failed,
        g_resources.vertices.size(), g_resources.indices.size(),
        g_resources.textures.size(),
        g_resources.native_scene_pipeline != nullptr,
        frame != nullptr && g_resources.prepared_frame == frame);
  }
  return false;
}

CrowdNativeSceneRecordResult RecordPreparedCrowdNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const CrowdFrameSnapshot> &frame,
    const NativeSceneDrawRef &draw_ref) {
  if (ValidateNativeScenePassTargets(context, targets) !=
      NativeScenePassTargetValidation::kValid) {
    return CrowdNativeSceneRecordResult::kInvalidTarget;
  }
  if (draw_ref.family != NativeSceneDrawFamily::kCrowdC6) {
    return CrowdNativeSceneRecordResult::kWrongFamily;
  }
  if (frame == nullptr || !NativeSceneFrameReady(*frame)) {
    return CrowdNativeSceneRecordResult::kInvalidFrame;
  }
  if (!PreparedNativeSceneFrameMatches(context, targets, frame)) {
    return CrowdNativeSceneRecordResult::kResourcesNotPrepared;
  }
  if (draw_ref.family_draw_index >= frame->draws.size()) {
    return CrowdNativeSceneRecordResult::kDrawIndexOutOfRange;
  }

  const CrowdDrawSnapshot &draw = frame->draws[draw_ref.family_draw_index];
  if (draw_ref.ordinal == 0 || draw.ordinal != draw_ref.ordinal) {
    return CrowdNativeSceneRecordResult::kOrdinalMismatch;
  }
  const auto prepared = std::find_if(
      g_resources.prepared_draws.begin(), g_resources.prepared_draws.end(),
      [&](const PreparedDraw &candidate) {
        return candidate.source_draw_index == draw_ref.family_draw_index;
      });
  if (prepared == g_resources.prepared_draws.end()) {
    return CrowdNativeSceneRecordResult::kPreparedDrawMissing;
  }
  if (draw.primitive_type != 0x06 ||
      prepared->primitive_type != draw.primitive_type ||
      prepared->index_count != draw.submitted_index_count ||
      prepared->vertex_buffer == nullptr || prepared->index_buffer == nullptr ||
      prepared->texture_view == nullptr) {
    return CrowdNativeSceneRecordResult::kPreparedDrawIdentityMismatch;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.native_scene_pipeline);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  cmd->SetConstantBuffer(0, g_resources.constant_buffer,
                         prepared->constant_offset);
  cmd->SetBufferSrv(1, prepared->vertex_buffer, 0);
  cmd->SetBufferSrv(2, g_resources.palette_buffer, prepared->palette_offset);
  cmd->SetTexture(3, prepared->texture_view);
  cmd->SetIndexBuffer(prepared->index_buffer, 0, prepared->index_bytes);
  if (!cmd->DrawIndexedChecked(prepared->index_count, 0, 0)) {
    return CrowdNativeSceneRecordResult::kRhiDrawStateRejected;
  }
  return CrowdNativeSceneRecordResult::kRecorded;
}

const char *
CrowdNativeSceneRecordResultName(CrowdNativeSceneRecordResult result) {
  switch (result) {
  case CrowdNativeSceneRecordResult::kRecorded:
    return "recorded";
  case CrowdNativeSceneRecordResult::kInvalidTarget:
    return "invalid_target";
  case CrowdNativeSceneRecordResult::kWrongFamily:
    return "wrong_family";
  case CrowdNativeSceneRecordResult::kInvalidFrame:
    return "invalid_frame";
  case CrowdNativeSceneRecordResult::kResourcesNotPrepared:
    return "resources_not_prepared";
  case CrowdNativeSceneRecordResult::kDrawIndexOutOfRange:
    return "draw_index_out_of_range";
  case CrowdNativeSceneRecordResult::kOrdinalMismatch:
    return "ordinal_mismatch";
  case CrowdNativeSceneRecordResult::kPreparedDrawMissing:
    return "prepared_draw_missing";
  case CrowdNativeSceneRecordResult::kPreparedDrawIdentityMismatch:
    return "prepared_draw_identity_mismatch";
  case CrowdNativeSceneRecordResult::kRhiDrawStateRejected:
    return "rhi_draw_state_rejected";
  }
  return "unknown";
}

CrowdReplacementPreflightResult PreflightCrowdReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context,
    const CrowdReplacementCandidate &candidate) {
  if (context.backend != rex::graphics::NativeGuestOutputBackend::kVulkan) {
    return CrowdReplacementPreflightResult::kWrongBackend;
  }
  if (context.cmd == nullptr || context.device == nullptr) {
    return CrowdReplacementPreflightResult::kMissingContext;
  }
  if (!candidate.valid() || !MatchesCrowdReplacementPrewarmContract(context)) {
    return CrowdReplacementPreflightResult::kInvalidCandidate;
  }
  if (g_resources.device != context.device) {
    return CrowdReplacementPreflightResult::kDeviceMismatch;
  }
  if (g_resources.layout == nullptr || g_resources.vertex_shader == nullptr ||
      g_resources.pixel_shader == nullptr ||
      !g_resources.pipeline_sampler_valid) {
    return CrowdReplacementPreflightResult::kMissingPipelineResources;
  }

  if (g_resources.prepared_frame != candidate.frame) {
    (void)PrepareExactReplacementFrameFromWarmCaches(candidate.frame);
  }
  if (g_resources.prepared_frame != candidate.frame) {
    return CrowdReplacementPreflightResult::kFrameMismatch;
  }
  if (g_resources.palette_buffer == nullptr ||
      g_resources.constant_buffer == nullptr) {
    return CrowdReplacementPreflightResult::kMissingDynamicBuffers;
  }
  if (!EnsureBorrowedPipeline(context)) {
    return CrowdReplacementPreflightResult::kMissingPipelineResources;
  }

  const auto prepared = std::find_if(
      g_resources.prepared_draws.begin(), g_resources.prepared_draws.end(),
      [&](const PreparedDraw &draw) {
        return draw.source_draw_index == candidate.draw_index;
      });
  if (prepared == g_resources.prepared_draws.end()) {
    return CrowdReplacementPreflightResult::kPreparedDrawMissing;
  }
  if (prepared->primitive_type != candidate.primitive_type ||
      prepared->index_count != candidate.submitted_index_count ||
      prepared->vertex_buffer == nullptr || prepared->index_buffer == nullptr ||
      prepared->texture_view == nullptr) {
    return CrowdReplacementPreflightResult::kPreparedDrawIdentityMismatch;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.borrowed_pipeline);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  cmd->SetConstantBuffer(0, g_resources.constant_buffer,
                         prepared->constant_offset);
  cmd->SetBufferSrv(1, prepared->vertex_buffer, 0);
  cmd->SetBufferSrv(2, g_resources.palette_buffer, prepared->palette_offset);
  cmd->SetTexture(3, prepared->texture_view);
  cmd->SetIndexBuffer(prepared->index_buffer, 0, prepared->index_bytes);
  if (!cmd->PreflightDraw()) {
    return CrowdReplacementPreflightResult::kRhiDrawStateRejected;
  }
  return CrowdReplacementPreflightResult::kSucceeded;
}

const char *
CrowdReplacementPreflightResultName(CrowdReplacementPreflightResult result) {
  switch (result) {
  case CrowdReplacementPreflightResult::kSucceeded:
    return "succeeded_exact_frame";
  case CrowdReplacementPreflightResult::kWrongBackend:
    return "wrong_backend";
  case CrowdReplacementPreflightResult::kMissingContext:
    return "missing_context";
  case CrowdReplacementPreflightResult::kInvalidCandidate:
    return "invalid_candidate";
  case CrowdReplacementPreflightResult::kDeviceMismatch:
    return "device_mismatch";
  case CrowdReplacementPreflightResult::kFrameMismatch:
    return "frame_mismatch";
  case CrowdReplacementPreflightResult::kMissingPipelineResources:
    return "missing_pipeline_resources";
  case CrowdReplacementPreflightResult::kMissingDynamicBuffers:
    return "missing_dynamic_buffers";
  case CrowdReplacementPreflightResult::kPreparedDrawMissing:
    return "prepared_draw_missing";
  case CrowdReplacementPreflightResult::kPreparedDrawIdentityMismatch:
    return "prepared_draw_identity_mismatch";
  case CrowdReplacementPreflightResult::kRhiDrawStateRejected:
    return "rhi_draw_state_rejected";
  }
  return "unknown";
}

bool DrawPreflightedCrowdReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context,
    const CrowdReplacementCandidate &candidate) {
  if (!candidate.valid() || context.cmd == nullptr ||
      context.device != g_resources.device ||
      g_resources.prepared_frame != candidate.frame ||
      g_resources.palette_buffer == nullptr ||
      g_resources.constant_buffer == nullptr ||
      g_resources.borrowed_pipeline == nullptr ||
      g_resources.borrowed_pipeline_depth_format !=
          context.depth_attachment_format) {
    return false;
  }
  const auto prepared = std::find_if(
      g_resources.prepared_draws.begin(), g_resources.prepared_draws.end(),
      [&](const PreparedDraw &draw) {
        return draw.source_draw_index == candidate.draw_index;
      });
  if (prepared == g_resources.prepared_draws.end() ||
      prepared->primitive_type != candidate.primitive_type ||
      prepared->index_count != candidate.submitted_index_count ||
      prepared->vertex_buffer == nullptr || prepared->index_buffer == nullptr ||
      prepared->texture_view == nullptr) {
    return false;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.borrowed_pipeline);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  cmd->SetConstantBuffer(0, g_resources.constant_buffer,
                         prepared->constant_offset);
  cmd->SetBufferSrv(1, prepared->vertex_buffer, 0);
  cmd->SetBufferSrv(2, g_resources.palette_buffer, prepared->palette_offset);
  cmd->SetTexture(3, prepared->texture_view);
  cmd->SetIndexBuffer(prepared->index_buffer, 0, prepared->index_bytes);
  return cmd->DrawIndexedChecked(prepared->index_count, 0, 0);
}

void ShutdownCrowdObserverRenderer() { ReleaseResources(); }

} // namespace tabletennis::native
