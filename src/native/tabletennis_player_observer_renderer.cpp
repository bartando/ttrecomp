#include "native/tabletennis_player_observer_renderer.h"

#include "native/shaders/tabletennis_player_observer_spirv.h"
#include "native/tabletennis_native_scene_compositor.h"
#include "native/tabletennis_native_scene_pass.h"
#include "native/tabletennis_player_replacement_candidates.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_texture_snapshot.h"

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
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_player_observer_overlay, false, "Table Tennis",
    "Draw the trace-verified native skinned-player pass over untouched guest "
    "output. Observer-only; never suppresses or replaces a guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    tabletennis_native_player_observer_geometry_group, -1, "Table Tennis",
    "Render only the Nth unique immutable player-mesh payload group from the "
    "fully verified player frame, including all of its prepass and color "
    "draws. -1 renders the complete frame.")
    .range(-1, 255)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kPlayerVertexStride = 44;
constexpr uint32_t kConstantSliceBytes = 3 * nrhi::kBufferOffsetAlignment;
constexpr size_t kMaximumStaticBufferCacheEntries = 512;
constexpr size_t kMaximumTextureCacheEntries = 256;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr std::array<uint32_t, 3> kPrepassDescriptors = {0x40106638, 0x4010D638,
                                                         0x401145B8};
constexpr std::array<uint32_t, 3> kColorPassDescriptors = {
    0x4010664C, 0x4010D64C, 0x401145CC};

struct alignas(16) PlayerConstantsGpu {
  std::array<float, 16> world_to_clip{};
  std::array<float, 4> camera_position{};
  std::array<float, 12> light_position_radius{};
  std::array<float, 12> light_color_intensity{};
  std::array<float, 12> light_mode{};
  std::array<float, 92> pixel_constants{};
  std::array<float, 4> pixel_control_254{};
  std::array<float, 4> pixel_control_255{};
  std::array<uint32_t, 4> buffer_layout{};
};

static_assert(sizeof(PlayerConstantsGpu) == 40 * sizeof(float) * 4);
static_assert(sizeof(PlayerConstantsGpu) <= kConstantSliceBytes);

struct GpuVertexPayload {
  std::shared_ptr<const PlayerSkinVertexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
  uint32_t size_bytes = 0;
};

struct GpuIndexPayload {
  std::shared_ptr<const PlayerSkinIndexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
  uint32_t size_bytes = 0;
};

struct GpuTexture {
  std::shared_ptr<const TextureSnapshot> snapshot;
  uint32_t view_swizzle = 0;
  nrhi::Texture *texture = nullptr;
  nrhi::TextureView *view = nullptr;
};

struct PreparedDraw {
  nrhi::Buffer *vertex_buffer = nullptr;
  nrhi::Buffer *index_buffer = nullptr;
  size_t source_draw_index = 0;
  uint32_t index_bytes = 0;
  uint32_t index_count = 0;
  uint32_t primitive_type = 0;
  bool prepass = false;
  uint64_t palette_offset = 0;
  uint64_t constant_offset = 0;
  std::array<nrhi::TextureView *, 3> texture_views{};
};

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::BindingLayout *layout = nullptr;
  nrhi::Shader *vertex_shader = nullptr;
  nrhi::Shader *prepass_pixel_shader = nullptr;
  nrhi::Shader *color_pixel_shader = nullptr;
  nrhi::Pipeline *prepass_pipeline = nullptr;
  nrhi::Pipeline *color_pipeline = nullptr;
  nrhi::Pipeline *native_scene_prepass_pipeline = nullptr;
  nrhi::Pipeline *native_scene_color_pipeline = nullptr;
  nrhi::Format pipeline_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_color_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_depth_format = nrhi::Format::kUnknown;
  uint32_t native_scene_sample_count = 0;
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
  std::shared_ptr<const PlayerSkinFrameSnapshot> prepared_frame;
  std::shared_ptr<const PlayerSkinFrameSnapshot> native_scene_frame;
  std::vector<PreparedDraw> prepared_draws;
  int32_t prepared_geometry_group = -1;
  uint64_t prepared_signature = 0;
  uint64_t announced_signature = 0;
  int32_t announced_invalid_geometry_group =
      std::numeric_limits<int32_t>::min();
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
    g_resources.device->DestroyDeferred(g_resources.prepass_pixel_shader);
    g_resources.device->DestroyDeferred(g_resources.color_pixel_shader);
    g_resources.device->DestroyDeferred(g_resources.prepass_pipeline);
    g_resources.device->DestroyDeferred(g_resources.color_pipeline);
    g_resources.device->DestroyDeferred(
        g_resources.native_scene_prepass_pipeline);
    g_resources.device->DestroyDeferred(
        g_resources.native_scene_color_pipeline);
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

bool EnsurePipelineResources(
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
    layout.params[3] = {nrhi::BindingParamKind::kTextureTable, 2, 3,
                        nrhi::Visibility::kPixel};
    layout.static_sampler_count = 3;
    layout.static_samplers[0] = {0, nrhi::Filter::kLinear,
                                 nrhi::AddressMode::kWrap, 1};
    layout.static_samplers[1] = {1, nrhi::Filter::kLinear,
                                 nrhi::AddressMode::kWrap, 1};
    layout.static_samplers[2] = {2, nrhi::Filter::kLinear,
                                 nrhi::AddressMode::kWrap, 1};
    layout.allow_input_layout = false;
    g_resources.layout = device->CreateBindingLayout(layout);
    if (g_resources.layout == nullptr) {
      REXLOG_ERROR(
          "Table Tennis player observer: binding layout creation failed");
      g_resources.failed = true;
      return false;
    }
  }

  if (g_resources.vertex_shader == nullptr ||
      g_resources.prepass_pixel_shader == nullptr ||
      g_resources.color_pixel_shader == nullptr) {
    nrhi::ShaderDesc vertex_desc;
    vertex_desc.stage = nrhi::ShaderStage::kVertex;
    vertex_desc.name = "tabletennis_player_observer.hlsl";
    vertex_desc.hlsl_source = player_observer_shader::kHlsl;
    vertex_desc.entry_point = "vs_main";
    vertex_desc.spirv = player_observer_shader::kVertexSpirv;
    vertex_desc.spirv_size_bytes = player_observer_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc prepass_pixel_desc;
    prepass_pixel_desc.stage = nrhi::ShaderStage::kPixel;
    prepass_pixel_desc.name = "tabletennis_player_observer.hlsl";
    prepass_pixel_desc.hlsl_source = player_observer_shader::kHlsl;
    prepass_pixel_desc.entry_point = "ps_prepass";
    prepass_pixel_desc.spirv = player_observer_shader::kPrepassPixelSpirv;
    prepass_pixel_desc.spirv_size_bytes =
        player_observer_shader::kPrepassPixelSpirvBytes;
    nrhi::ShaderDesc color_pixel_desc = prepass_pixel_desc;
    color_pixel_desc.entry_point = "ps_color";
    color_pixel_desc.spirv = player_observer_shader::kColorPixelSpirv;
    color_pixel_desc.spirv_size_bytes =
        player_observer_shader::kColorPixelSpirvBytes;
    g_resources.vertex_shader = device->CreateShader(vertex_desc);
    g_resources.prepass_pixel_shader = device->CreateShader(prepass_pixel_desc);
    g_resources.color_pixel_shader = device->CreateShader(color_pixel_desc);
    if (g_resources.vertex_shader == nullptr ||
        g_resources.prepass_pixel_shader == nullptr ||
        g_resources.color_pixel_shader == nullptr) {
      REXLOG_ERROR("Table Tennis player observer: shader creation failed");
      g_resources.failed = true;
      return false;
    }
  }

  return true;
}

bool EnsurePipeline(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (!EnsurePipelineResources(context)) {
    return false;
  }
  nrhi::Device *const device = context.device;
  const nrhi::Format output_format = context.guest_output->format();
  if (g_resources.prepass_pipeline == nullptr ||
      g_resources.color_pipeline == nullptr ||
      g_resources.pipeline_format != output_format) {
    device->DestroyDeferred(g_resources.prepass_pipeline);
    device->DestroyDeferred(g_resources.color_pipeline);
    g_resources.prepass_pipeline = nullptr;
    g_resources.color_pipeline = nullptr;

    nrhi::GraphicsPipelineDesc pipeline;
    pipeline.layout = g_resources.layout;
    pipeline.vs = g_resources.vertex_shader;
    pipeline.input_elements = nullptr;
    pipeline.input_element_count = 0;
    pipeline.vertex_stride = 0;
    pipeline.cull = nrhi::CullMode::kNone;
    pipeline.depth_clip = true;
    pipeline.depth.test_enable = true;
    pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
    pipeline.rtv_format = output_format;
    pipeline.dsv_format = nrhi::Format::kD32_FLOAT;
    pipeline.sample_count = 1;

    pipeline.ps = g_resources.prepass_pixel_shader;
    pipeline.depth.write_enable = true;
    pipeline.blend.enable = false;
    // ps_prepass emits the exact Xenos mask through SV_Coverage. Enabling
    // implementation-defined fixed-function A2C would AND a second mask with
    // it and break the traced 3,1,0,2 dither contract.
    pipeline.blend.alpha_to_coverage = false;
    pipeline.blend.write_mask = 0x8;
    g_resources.prepass_pipeline = device->CreateGraphicsPipeline(pipeline);

    pipeline.ps = g_resources.color_pixel_shader;
    pipeline.depth.write_enable = false;
    pipeline.blend.enable = true;
    pipeline.blend.write_mask = 0xF;
    pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
    pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    pipeline.blend.op = nrhi::BlendOp::kAdd;
    pipeline.blend.src_alpha = nrhi::BlendFactor::kSrcAlpha;
    pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    pipeline.blend.op_alpha = nrhi::BlendOp::kAdd;
    g_resources.color_pipeline = device->CreateGraphicsPipeline(pipeline);
    if (g_resources.prepass_pipeline == nullptr ||
        g_resources.color_pipeline == nullptr) {
      REXLOG_ERROR(
          "Table Tennis player observer: phase pipeline creation failed");
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
          "Table Tennis player observer: private depth creation failed");
      return false;
    }
    device->DestroyDeferred(g_resources.depth);
    g_resources.depth = depth;
    g_resources.depth_width = context.guest_output_width;
    g_resources.depth_height = context.guest_output_height;
  }
  return true;
}

bool EnsureNativeScenePipelines(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      !EnsurePipelineResources(context)) {
    return false;
  }

  const nrhi::Format color_format = targets.color->format();
  const nrhi::Format depth_format = targets.depth->format();
  if (g_resources.native_scene_prepass_pipeline != nullptr &&
      g_resources.native_scene_color_pipeline != nullptr &&
      g_resources.native_scene_color_format == color_format &&
      g_resources.native_scene_depth_format == depth_format &&
      g_resources.native_scene_sample_count == targets.sample_count) {
    return true;
  }
  context.device->DestroyDeferred(g_resources.native_scene_prepass_pipeline);
  context.device->DestroyDeferred(g_resources.native_scene_color_pipeline);
  g_resources.native_scene_prepass_pipeline = nullptr;
  g_resources.native_scene_color_pipeline = nullptr;

  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = g_resources.layout;
  pipeline.vs = g_resources.vertex_shader;
  pipeline.input_elements = nullptr;
  pipeline.input_element_count = 0;
  pipeline.vertex_stride = 0;
  // PA_SU_SC_MODE_CNTL proves front and back culling are both disabled.
  pipeline.cull = nrhi::CullMode::kNone;
  pipeline.depth_clip = true;
  pipeline.depth.test_enable = true;
  pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
  pipeline.rtv_format = color_format;
  pipeline.dsv_format = depth_format;
  pipeline.sample_count = targets.sample_count;

  pipeline.ps = g_resources.prepass_pixel_shader;
  pipeline.depth.write_enable = true;
  pipeline.blend.enable = false;
  pipeline.blend.alpha_to_coverage = false;
  pipeline.blend.write_mask = 0x8;
  g_resources.native_scene_prepass_pipeline =
      context.device->CreateGraphicsPipeline(pipeline);

  pipeline.ps = g_resources.color_pixel_shader;
  pipeline.depth.write_enable = false;
  pipeline.blend.enable = true;
  pipeline.blend.alpha_to_coverage = false;
  pipeline.blend.write_mask = 0xF;
  pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op = nrhi::BlendOp::kAdd;
  pipeline.blend.src_alpha = nrhi::BlendFactor::kSrcAlpha;
  pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op_alpha = nrhi::BlendOp::kAdd;
  g_resources.native_scene_color_pipeline =
      context.device->CreateGraphicsPipeline(pipeline);

  if (g_resources.native_scene_prepass_pipeline == nullptr ||
      g_resources.native_scene_color_pipeline == nullptr) {
    context.device->DestroyDeferred(g_resources.native_scene_prepass_pipeline);
    context.device->DestroyDeferred(g_resources.native_scene_color_pipeline);
    g_resources.native_scene_prepass_pipeline = nullptr;
    g_resources.native_scene_color_pipeline = nullptr;
    REXLOG_ERROR(
        "Table Tennis CA9 native scene: shared phase pipeline creation "
        "failed");
    return false;
  }
  g_resources.native_scene_color_format = color_format;
  g_resources.native_scene_depth_format = depth_format;
  g_resources.native_scene_sample_count = targets.sample_count;
  return true;
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

GpuVertexPayload *EnsureVertexPayload(
    const std::shared_ptr<const PlayerSkinVertexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      snapshot->stride != kPlayerVertexStride || snapshot->raw_bytes.empty() ||
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
    REXLOG_ERROR("Table Tennis player observer: vertex cache exhausted");
    return nullptr;
  }
  nrhi::Buffer *const buffer = CreateAndFillBuffer(
      g_resources.device, snapshot->raw_bytes.data(),
      snapshot->raw_bytes.size(), nrhi::BufferBindClass::kFull);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis player observer: vf95 BufferSrv upload failed");
    return nullptr;
  }
  g_resources.vertices.push_back({
      .snapshot = snapshot,
      .buffer = buffer,
      .size_bytes = static_cast<uint32_t>(snapshot->raw_bytes.size()),
  });
  return &g_resources.vertices.back();
}

GpuVertexPayload *FindVertexPayload(
    const std::shared_ptr<const PlayerSkinVertexPayload> &snapshot) {
  const auto found =
      std::find_if(g_resources.vertices.begin(), g_resources.vertices.end(),
                   [&](const GpuVertexPayload &vertex) {
                     return vertex.snapshot == snapshot;
                   });
  return found != g_resources.vertices.end() ? &*found : nullptr;
}

GpuIndexPayload *EnsureIndexPayload(
    const std::shared_ptr<const PlayerSkinIndexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      snapshot->element_size != sizeof(uint16_t) || snapshot->indices.empty() ||
      snapshot->indices.size() >
          std::numeric_limits<uint32_t>::max() / sizeof(uint16_t) ||
      std::any_of(snapshot->indices.begin(), snapshot->indices.end(),
                  [](uint32_t index) {
                    return index > std::numeric_limits<uint16_t>::max();
                  })) {
    return nullptr;
  }
  const auto found = std::find_if(
      g_resources.indices.begin(), g_resources.indices.end(),
      [&](const GpuIndexPayload &index) { return index.snapshot == snapshot; });
  if (found != g_resources.indices.end()) {
    return &*found;
  }
  if (g_resources.indices.size() >= kMaximumStaticBufferCacheEntries) {
    REXLOG_ERROR("Table Tennis player observer: index cache exhausted");
    return nullptr;
  }

  std::vector<uint16_t> decoded(snapshot->indices.size());
  std::transform(snapshot->indices.begin(), snapshot->indices.end(),
                 decoded.begin(),
                 [](uint32_t index) { return static_cast<uint16_t>(index); });
  const uint32_t size_bytes =
      static_cast<uint32_t>(decoded.size() * sizeof(uint16_t));
  nrhi::Buffer *const buffer =
      CreateAndFillBuffer(g_resources.device, decoded.data(), size_bytes,
                          nrhi::BufferBindClass::kVertexIndex);
  if (buffer == nullptr) {
    REXLOG_ERROR(
        "Table Tennis player observer: decoded 16-bit index upload failed");
    return nullptr;
  }
  g_resources.indices.push_back({
      .snapshot = snapshot,
      .buffer = buffer,
      .size_bytes = size_bytes,
  });
  return &g_resources.indices.back();
}

GpuIndexPayload *FindIndexPayload(
    const std::shared_ptr<const PlayerSkinIndexPayload> &snapshot) {
  const auto found = std::find_if(
      g_resources.indices.begin(), g_resources.indices.end(),
      [&](const GpuIndexPayload &index) { return index.snapshot == snapshot; });
  return found != g_resources.indices.end() ? &*found : nullptr;
}

nrhi::Format HostTextureFormat(const TextureSnapshot &texture) {
  switch (rex::graphics::GetBaseFormat(
      static_cast<xenos::TextureFormat>(texture.format))) {
  case xenos::TextureFormat::k_8_8_8_8:
    return nrhi::Format::kR8G8B8A8_UNORM;
  case xenos::TextureFormat::k_DXT1:
    return nrhi::Format::kBC1_UNORM;
  case xenos::TextureFormat::k_DXT2_3:
    return nrhi::Format::kBC2_UNORM;
  case xenos::TextureFormat::k_DXT4_5:
    return nrhi::Format::kBC3_UNORM;
  case xenos::TextureFormat::k_DXT3A:
  case xenos::TextureFormat::k_DXT5A:
    return nrhi::Format::kBC4_UNORM;
  case xenos::TextureFormat::k_DXN:
    return nrhi::Format::kBC5_UNORM;
  default:
    return nrhi::Format::kUnknown;
  }
}

bool ComposeTextureSwizzle(uint32_t fetch_swizzle, nrhi::Swizzle output[4]) {
  for (uint32_t channel = 0; channel < 4; ++channel) {
    const uint32_t component = (fetch_swizzle >> (channel * 3)) & 7u;
    if (component > static_cast<uint32_t>(nrhi::Swizzle::kOne)) {
      return false;
    }
    output[channel] = static_cast<nrhi::Swizzle>(component);
  }
  return true;
}

nrhi::TextureView *
EnsureTexture(const rex::graphics::NativeGuestOutputRenderContext &context,
              const std::shared_ptr<const TextureSnapshot> &snapshot,
              uint32_t view_swizzle) {
  if (snapshot == nullptr || !snapshot->valid() ||
      snapshot->fetch_swizzle != view_swizzle) {
    return nullptr;
  }
  const auto found =
      std::find_if(g_resources.textures.begin(), g_resources.textures.end(),
                   [&](const GpuTexture &texture) {
                     return texture.snapshot == snapshot &&
                            texture.view_swizzle == view_swizzle;
                   });
  if (found != g_resources.textures.end()) {
    return found->view;
  }
  if (g_resources.textures.size() >= kMaximumTextureCacheEntries) {
    REXLOG_ERROR("Table Tennis player observer: texture cache exhausted");
    return nullptr;
  }

  const nrhi::Format format = HostTextureFormat(*snapshot);
  if (format == nrhi::Format::kUnknown) {
    REXLOG_ERROR("Table Tennis player observer: unsupported texture format {}",
                 snapshot->format);
    return nullptr;
  }
  const uint32_t host_width =
      ((snapshot->width + snapshot->block_width - 1) / snapshot->block_width) *
      snapshot->block_width;
  const uint32_t host_height =
      ((snapshot->height + snapshot->block_height - 1) /
       snapshot->block_height) *
      snapshot->block_height;
  const uint32_t block_rows = host_height / snapshot->block_height;
  const uint32_t upload_row_pitch = static_cast<uint32_t>(
      AlignUp(snapshot->row_pitch_bytes, nrhi::kRowPitchAlignment));
  const uint64_t upload_size =
      static_cast<uint64_t>(upload_row_pitch) * block_rows;

  nrhi::TextureDesc texture_desc;
  texture_desc.width = host_width;
  texture_desc.height = host_height;
  texture_desc.format = format;
  texture_desc.initial_state = nrhi::ResourceState::kCopyDest;
  nrhi::Texture *const texture = context.device->CreateTexture(texture_desc);
  nrhi::Buffer *const upload = CreateUploadBuffer(
      context.device, upload_size, nrhi::BufferBindClass::kCopySrc);
  if (texture == nullptr || upload == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR(
        "Table Tennis player observer: texture resource creation failed");
    return nullptr;
  }

  uint8_t *const mapped = static_cast<uint8_t *>(context.device->Map(upload));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR("Table Tennis player observer: texture map failed");
    return nullptr;
  }
  for (uint32_t row = 0; row < block_rows; ++row) {
    uint8_t *const destination =
        mapped + static_cast<size_t>(row) * upload_row_pitch;
    const uint8_t *const source =
        snapshot->linear_blocks.data() +
        static_cast<size_t>(row) * snapshot->row_pitch_bytes;
    std::memcpy(destination, source, snapshot->row_pitch_bytes);
    std::memset(destination + snapshot->row_pitch_bytes, 0,
                upload_row_pitch - snapshot->row_pitch_bytes);
  }
  context.device->Unmap(upload);

  nrhi::TextureViewDesc view_desc;
  if (!ComposeTextureSwizzle(view_swizzle, view_desc.swizzle)) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  nrhi::TextureView *const view =
      context.device->CreateTextureView(texture, view_desc);
  if (view == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR("Table Tennis player observer: texture view creation failed");
    return nullptr;
  }

  context.cmd->CopyBufferToTexture(texture, 0, 0, upload, 0, upload_row_pitch,
                                   host_width, host_height, 1);
  context.cmd->Barrier(texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.device->DestroyDeferred(upload);
  g_resources.textures.push_back({
      .snapshot = snapshot,
      .view_swizzle = view_swizzle,
      .texture = texture,
      .view = view,
  });
  REXLOG_INFO("Table Tennis player observer: uploaded texture {}x{} format={} "
              "swizzle={:03X} payload={:016X}",
              snapshot->width, snapshot->height, snapshot->format, view_swizzle,
              snapshot->payload_fingerprint);
  return view;
}

nrhi::TextureView *
FindTexture(const std::shared_ptr<const TextureSnapshot> &snapshot,
            uint32_t view_swizzle) {
  const auto found =
      std::find_if(g_resources.textures.begin(), g_resources.textures.end(),
                   [&](const GpuTexture &texture) {
                     return texture.snapshot == snapshot &&
                            texture.view_swizzle == view_swizzle;
                   });
  return found != g_resources.textures.end() ? found->view : nullptr;
}

bool MaterialReady(const PlayerSkinMaterialSnapshot &material) {
  if (!material.valid || !material.pixel_control_constants_valid ||
      !material.texture_view_swizzles_valid ||
      !AllFinite(material.vertex_constants_12_15) ||
      !AllFinite(material.vertex_constant_19) ||
      !AllFinite(material.vertex_constants_46_54) ||
      !AllFinite(material.vertex_constant_255) ||
      !AllFinite(material.pixel_constants_46_68) ||
      !AllFinite(material.pixel_constant_254) ||
      !AllFinite(material.pixel_constant_255)) {
    return false;
  }
  for (size_t slot = 0; slot < material.textures.size(); ++slot) {
    if (material.textures[slot] == nullptr ||
        !material.textures[slot]->valid() ||
        material.textures[slot]->fetch_swizzle !=
            material.texture_view_swizzles[slot]) {
      return false;
    }
  }
  return true;
}

bool IsPrepassDescriptor(uint32_t descriptor) {
  return std::ranges::find(kPrepassDescriptors, descriptor) !=
         kPrepassDescriptors.end();
}

bool IsColorPassDescriptor(uint32_t descriptor) {
  return std::ranges::find(kColorPassDescriptors, descriptor) !=
         kColorPassDescriptors.end();
}

struct DrawSelection {
  std::vector<size_t> indices;
  PlayerSkinMeshIdentity key{};
  bool group_found = false;
  bool phase_complete = false;
};

DrawSelection SelectDraws(const PlayerSkinFrameSnapshot &frame,
                          int32_t geometry_group) {
  DrawSelection selection;
  if (geometry_group < 0) {
    selection.indices.reserve(frame.draws.size());
    for (size_t index = 0; index < frame.draws.size(); ++index) {
      selection.indices.push_back(index);
    }
    selection.group_found = !selection.indices.empty();
    selection.phase_complete = selection.group_found;
    return selection;
  }

  std::vector<PlayerSkinMeshIdentity> groups;
  groups.reserve(frame.draws.size());
  for (const PlayerSkinDrawSnapshot &draw : frame.draws) {
    const PlayerSkinMeshIdentity key = PlayerSkinMeshIdentityForDraw(draw);
    if (std::ranges::find(groups, key) != groups.end()) {
      continue;
    }
    if (groups.size() == static_cast<size_t>(geometry_group)) {
      selection.key = key;
      selection.group_found = true;
      break;
    }
    groups.push_back(key);
  }
  if (!selection.group_found) {
    return selection;
  }

  bool has_prepass = false;
  bool has_color_pass = false;
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    const PlayerSkinDrawSnapshot &draw = frame.draws[index];
    if (PlayerSkinMeshIdentityForDraw(draw) != selection.key) {
      continue;
    }
    selection.indices.push_back(index);
    has_prepass |= IsPrepassDescriptor(draw.pass_descriptor);
    has_color_pass |= IsColorPassDescriptor(draw.pass_descriptor);
  }
  selection.phase_complete = has_prepass && has_color_pass;
  if (!selection.phase_complete) {
    selection.indices.clear();
  }
  return selection;
}

bool DrawReady(const PlayerSkinDrawSnapshot &draw) {
  return draw.valid && draw.vertices != nullptr && draw.vertices->valid() &&
         draw.vertices->stride == kPlayerVertexStride &&
         draw.indices != nullptr && draw.indices->valid() &&
         draw.indices->element_size == sizeof(uint16_t) &&
         draw.palette != nullptr && draw.palette->valid() &&
         draw.submitted_index_count == draw.indices->submitted_index_count &&
         draw.primitive_type == 0x06 &&
         (IsPrepassDescriptor(draw.pass_descriptor) ||
          IsColorPassDescriptor(draw.pass_descriptor)) &&
         MaterialReady(draw.material);
}

bool FrameReady(const PlayerSkinFrameSnapshot &frame) {
  return frame.valid() && std::all_of(frame.draws.begin(), frame.draws.end(),
                                      [](const PlayerSkinDrawSnapshot &draw) {
                                        return DrawReady(draw);
                                      });
}

bool PhaseDescriptorsMatch(uint32_t prepass, uint32_t color) {
  for (size_t index = 0; index < kPrepassDescriptors.size(); ++index) {
    if (prepass == kPrepassDescriptors[index] &&
        color == kColorPassDescriptors[index]) {
      return true;
    }
  }
  return false;
}

bool SameImmutablePhasePayloads(const PlayerSkinDrawSnapshot &left,
                                const PlayerSkinDrawSnapshot &right) {
  if (left.palette == nullptr || right.palette == nullptr ||
      left.palette->fetch.physical_address !=
          right.palette->fetch.physical_address ||
      left.palette->fetch.size != right.palette->fetch.size ||
      left.palette->payload_fingerprint != right.palette->payload_fingerprint ||
      left.material.texture_view_swizzles !=
          right.material.texture_view_swizzles) {
    return false;
  }
  for (size_t slot = 0; slot < left.material.textures.size(); ++slot) {
    const auto &left_texture = left.material.textures[slot];
    const auto &right_texture = right.material.textures[slot];
    if (left_texture == nullptr || right_texture == nullptr ||
        left_texture->fetch_words != right_texture->fetch_words ||
        left_texture->payload_fingerprint !=
            right_texture->payload_fingerprint) {
      return false;
    }
  }
  return true;
}

bool NativeScenePhasePlanReady(const PlayerSkinFrameSnapshot &frame) {
  if (!FrameReady(frame) || frame.draws.empty()) {
    return false;
  }

  struct PhaseGroup {
    PlayerSkinMeshIdentity mesh{};
    std::vector<size_t> indices;
  };
  std::vector<PhaseGroup> groups;
  groups.reserve(frame.draws.size() / 2);
  uint32_t previous_ordinal = 0;
  for (size_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
    const PlayerSkinDrawSnapshot &draw = frame.draws[draw_index];
    if (draw.ordinal <= previous_ordinal) {
      return false;
    }
    previous_ordinal = draw.ordinal;
    const PlayerSkinMeshIdentity mesh = PlayerSkinMeshIdentityForDraw(draw);
    if (!mesh.valid()) {
      return false;
    }
    const auto group = std::ranges::find(groups, mesh, &PhaseGroup::mesh);
    if (group == groups.end()) {
      groups.push_back({.mesh = mesh, .indices = {draw_index}});
    } else {
      group->indices.push_back(draw_index);
    }
  }

  for (const PhaseGroup &group : groups) {
    if (group.indices.size() != 2) {
      return false;
    }
    const PlayerSkinDrawSnapshot &prepass = frame.draws[group.indices[0]];
    const PlayerSkinDrawSnapshot &color = frame.draws[group.indices[1]];
    if (!IsPrepassDescriptor(prepass.pass_descriptor) ||
        !IsColorPassDescriptor(color.pass_descriptor) ||
        !PhaseDescriptorsMatch(prepass.pass_descriptor,
                               color.pass_descriptor) ||
        prepass.ordinal >= color.ordinal ||
        !SameImmutablePhasePayloads(prepass, color)) {
      return false;
    }
  }
  return !groups.empty();
}

uint64_t GeometrySignature(const PlayerSkinFrameSnapshot &frame,
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
    const PlayerSkinDrawSnapshot &draw =
        frame.draws[prepared.source_draw_index];
    append(draw.player);
    append(draw.vertices->fetch.physical_address);
    append(draw.vertices->fetch.size);
    append(draw.pass_descriptor);
    append(draw.primitive_type);
    append(draw.submitted_index_count);
    append(draw.vertices->payload_fingerprint);
    append(draw.indices->physical_address);
    append(draw.indices->payload_fingerprint);
  }
  return signature;
}

PlayerConstantsGpu ConstantsForDraw(const PlayerSkinDrawSnapshot &draw) {
  PlayerConstantsGpu constants;
  constants.world_to_clip = draw.material.vertex_constants_12_15;
  constants.camera_position = draw.material.vertex_constant_19;
  std::copy_n(draw.material.vertex_constants_46_54.begin(), 12,
              constants.light_position_radius.begin());
  std::copy_n(draw.material.vertex_constants_46_54.begin() + 12, 12,
              constants.light_color_intensity.begin());
  std::copy_n(draw.material.vertex_constants_46_54.begin() + 24, 12,
              constants.light_mode.begin());
  constants.pixel_constants = draw.material.pixel_constants_46_68;
  constants.pixel_control_254 = draw.material.pixel_constant_254;
  constants.pixel_control_255 = draw.material.pixel_constant_255;
  constants.buffer_layout = {0, 0, draw.palette->record_count,
                             draw.vertices->stride};
  return constants;
}

struct PaletteSlice {
  std::shared_ptr<const PlayerSkinPalettePayload> snapshot;
  uint64_t offset = 0;
};

bool PrepareDynamicFrame(
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame,
    int32_t geometry_group, std::vector<PreparedDraw> prepared_draws) {
  std::vector<PaletteSlice> palettes;
  palettes.reserve(prepared_draws.size());
  uint64_t palette_bytes = 0;
  for (const PreparedDraw &prepared : prepared_draws) {
    const PlayerSkinDrawSnapshot &draw =
        frame->draws[prepared.source_draw_index];
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
    REXLOG_ERROR(
        "Table Tennis player observer: per-frame buffer creation failed");
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
    REXLOG_ERROR("Table Tennis player observer: per-frame buffer map failed");
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
    const PlayerSkinDrawSnapshot &draw =
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
    const PlayerConstantsGpu constants = ConstantsForDraw(draw);
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
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
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
    const PlayerSkinDrawSnapshot &draw = frame->draws[source_draw_index];
    GpuVertexPayload *const vertex = FindVertexPayload(draw.vertices);
    GpuIndexPayload *const index = FindIndexPayload(draw.indices);
    if (vertex == nullptr || index == nullptr) {
      return false;
    }
    PreparedDraw prepared;
    prepared.vertex_buffer = vertex->buffer;
    prepared.index_buffer = index->buffer;
    prepared.source_draw_index = source_draw_index;
    prepared.index_bytes = index->size_bytes;
    prepared.index_count = draw.submitted_index_count;
    prepared.primitive_type = draw.primitive_type;
    prepared.prepass = IsPrepassDescriptor(draw.pass_descriptor);
    for (size_t slot = 0; slot < prepared.texture_views.size(); ++slot) {
      prepared.texture_views[slot] =
          FindTexture(draw.material.textures[slot],
                      draw.material.texture_view_swizzles[slot]);
      if (prepared.texture_views[slot] == nullptr) {
        return false;
      }
    }
    prepared_draws.push_back(prepared);
  }

  // Upload-heap buffers are persistently host-visible/coherent. Creating and
  // filling these current-frame palette/constant buffers records no Vulkan
  // commands, so it is legal inside a borrowed scope. Static texture copies
  // and barriers remain strictly in the earlier output-postprocess warmup.
  return PrepareDynamicFrame(frame, -1, std::move(prepared_draws));
}

bool PreparedFrameMatches(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame,
    int32_t geometry_group) {
  return frame != nullptr && g_resources.prepared_frame == frame &&
         g_resources.prepared_geometry_group == geometry_group &&
         g_resources.device == context.device &&
         g_resources.layout != nullptr &&
         g_resources.prepass_pipeline != nullptr &&
         g_resources.color_pipeline != nullptr &&
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
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      frame == nullptr || g_resources.native_scene_frame != frame ||
      g_resources.prepared_frame != frame ||
      g_resources.prepared_geometry_group != -1 ||
      g_resources.device != context.device || g_resources.layout == nullptr ||
      g_resources.native_scene_prepass_pipeline == nullptr ||
      g_resources.native_scene_color_pipeline == nullptr ||
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
    const PlayerSkinDrawSnapshot &draw = frame->draws[index];
    if (prepared.source_draw_index != index ||
        prepared.vertex_buffer == nullptr || prepared.index_buffer == nullptr ||
        std::ranges::any_of(
            prepared.texture_views,
            [](nrhi::TextureView *view) { return view == nullptr; }) ||
        prepared.index_count != draw.submitted_index_count ||
        prepared.primitive_type != draw.primitive_type ||
        prepared.prepass != IsPrepassDescriptor(draw.pass_descriptor)) {
      return false;
    }
  }
  return true;
}

} // namespace

bool PlayerObserverOverlayEnabled() {
  return REXCVAR_GET(tabletennis_native_player_observer_overlay);
}

static bool PreparePlayerResources(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame,
    int32_t geometry_group,
    const NativeScenePassTargets *native_scene_targets = nullptr) {
  if (frame == nullptr || !FrameReady(*frame) || context.cmd == nullptr ||
      !EnsureDevice(context) ||
      (native_scene_targets != nullptr &&
       (ValidateNativeScenePassTargets(context, *native_scene_targets) !=
            NativeScenePassTargetValidation::kValid ||
        !NativeScenePhasePlanReady(*frame)))) {
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
      if (selection.group_found) {
        REXLOG_WARN(
            "Table Tennis player observer: geometry group {} exists but "
            "does not contain both a traced prepass and color pass; refusing "
            "a partial overlay",
            geometry_group);
      } else {
        REXLOG_WARN(
            "Table Tennis player observer: geometry group {} is outside the "
            "current fully verified frame",
            geometry_group);
      }
    }
    return false;
  }
  g_resources.announced_invalid_geometry_group =
      std::numeric_limits<int32_t>::min();

  std::vector<PreparedDraw> prepared_draws;
  prepared_draws.reserve(selection.indices.size());
  for (size_t source_draw_index : selection.indices) {
    const PlayerSkinDrawSnapshot &draw = frame->draws[source_draw_index];
    GpuVertexPayload *const vertex = EnsureVertexPayload(draw.vertices);
    GpuIndexPayload *const index = EnsureIndexPayload(draw.indices);
    if (vertex == nullptr || index == nullptr) {
      return false;
    }
    PreparedDraw prepared;
    prepared.vertex_buffer = vertex->buffer;
    prepared.index_buffer = index->buffer;
    prepared.source_draw_index = source_draw_index;
    prepared.index_bytes = index->size_bytes;
    prepared.index_count = draw.submitted_index_count;
    prepared.primitive_type = draw.primitive_type;
    prepared.prepass = IsPrepassDescriptor(draw.pass_descriptor);
    for (size_t slot = 0; slot < prepared.texture_views.size(); ++slot) {
      prepared.texture_views[slot] =
          EnsureTexture(context, draw.material.textures[slot],
                        draw.material.texture_view_swizzles[slot]);
      if (prepared.texture_views[slot] == nullptr) {
        return false;
      }
    }
    prepared_draws.push_back(prepared);
  }
  if (native_scene_targets != nullptr
          ? !EnsureNativeScenePipelines(context, *native_scene_targets)
          : !EnsurePipeline(context)) {
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

bool PreparePlayerObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
  if (!PlayerObserverOverlayEnabled() && !PlayerReplacementPrewarmEnabled()) {
    return false;
  }
  const int32_t geometry_group =
      PlayerReplacementPrewarmEnabled()
          ? -1
          : REXCVAR_GET(tabletennis_native_player_observer_geometry_group);
  return PreparePlayerResources(context, frame, geometry_group);
}

bool PreparePlayerNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
  return ValidateNativeScenePassTargets(context, targets) ==
             NativeScenePassTargetValidation::kValid &&
         frame != nullptr && NativeScenePhasePlanReady(*frame) &&
         PreparePlayerResources(context, frame, -1, &targets);
}

PlayerNativeSceneRecordResult RecordPreparedPlayerNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame,
    const NativeSceneDrawRef &draw_ref) {
  if (ValidateNativeScenePassTargets(context, targets) !=
      NativeScenePassTargetValidation::kValid) {
    return PlayerNativeSceneRecordResult::kInvalidTarget;
  }
  if (draw_ref.family != NativeSceneDrawFamily::kPlayerCa9) {
    return PlayerNativeSceneRecordResult::kWrongFamily;
  }
  if (frame == nullptr || !FrameReady(*frame)) {
    return PlayerNativeSceneRecordResult::kInvalidFrame;
  }
  if (!NativeScenePhasePlanReady(*frame)) {
    return PlayerNativeSceneRecordResult::kIncompletePhasePlan;
  }
  if (!PreparedNativeSceneFrameMatches(context, targets, frame)) {
    return PlayerNativeSceneRecordResult::kResourcesNotPrepared;
  }
  if (draw_ref.family_draw_index >= frame->draws.size()) {
    return PlayerNativeSceneRecordResult::kDrawIndexOutOfRange;
  }

  const PlayerSkinDrawSnapshot &draw = frame->draws[draw_ref.family_draw_index];
  if (draw_ref.ordinal == 0 || draw.ordinal != draw_ref.ordinal) {
    return PlayerNativeSceneRecordResult::kOrdinalMismatch;
  }
  const auto prepared = std::find_if(
      g_resources.prepared_draws.begin(), g_resources.prepared_draws.end(),
      [&](const PreparedDraw &candidate) {
        return candidate.source_draw_index == draw_ref.family_draw_index;
      });
  if (prepared == g_resources.prepared_draws.end()) {
    return PlayerNativeSceneRecordResult::kPreparedDrawMissing;
  }
  const bool prepass = IsPrepassDescriptor(draw.pass_descriptor);
  if ((!prepass && !IsColorPassDescriptor(draw.pass_descriptor)) ||
      prepared->prepass != prepass ||
      prepared->primitive_type != draw.primitive_type ||
      prepared->index_count != draw.submitted_index_count ||
      prepared->vertex_buffer == nullptr || prepared->index_buffer == nullptr ||
      std::ranges::any_of(prepared->texture_views, [](nrhi::TextureView *view) {
        return view == nullptr;
      })) {
    return PlayerNativeSceneRecordResult::kPreparedDrawIdentityMismatch;
  }

  nrhi::PrimitiveTopology topology;
  if (prepared->primitive_type == 0x04) {
    topology = nrhi::PrimitiveTopology::kTriangleList;
  } else if (prepared->primitive_type == 0x06) {
    topology = nrhi::PrimitiveTopology::kTriangleStrip;
  } else {
    return PlayerNativeSceneRecordResult::kUnsupportedPrimitive;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(prepass ? g_resources.native_scene_prepass_pipeline
                           : g_resources.native_scene_color_pipeline);
  cmd->SetPrimitiveTopology(topology);
  cmd->SetConstantBuffer(0, g_resources.constant_buffer,
                         prepared->constant_offset);
  cmd->SetBufferSrv(1, prepared->vertex_buffer, 0);
  cmd->SetBufferSrv(2, g_resources.palette_buffer, prepared->palette_offset);
  cmd->SetTextures(3, prepared->texture_views.data(),
                   static_cast<uint32_t>(prepared->texture_views.size()));
  cmd->SetIndexBuffer(prepared->index_buffer, 0, prepared->index_bytes);
  if (!cmd->DrawIndexedChecked(prepared->index_count, 0, 0)) {
    return PlayerNativeSceneRecordResult::kRhiDrawStateRejected;
  }
  return PlayerNativeSceneRecordResult::kRecorded;
}

const char *
PlayerNativeSceneRecordResultName(PlayerNativeSceneRecordResult result) {
  switch (result) {
  case PlayerNativeSceneRecordResult::kRecorded:
    return "recorded";
  case PlayerNativeSceneRecordResult::kInvalidTarget:
    return "invalid_target";
  case PlayerNativeSceneRecordResult::kWrongFamily:
    return "wrong_family";
  case PlayerNativeSceneRecordResult::kInvalidFrame:
    return "invalid_frame";
  case PlayerNativeSceneRecordResult::kIncompletePhasePlan:
    return "incomplete_phase_plan";
  case PlayerNativeSceneRecordResult::kResourcesNotPrepared:
    return "resources_not_prepared";
  case PlayerNativeSceneRecordResult::kDrawIndexOutOfRange:
    return "draw_index_out_of_range";
  case PlayerNativeSceneRecordResult::kOrdinalMismatch:
    return "ordinal_mismatch";
  case PlayerNativeSceneRecordResult::kPreparedDrawMissing:
    return "prepared_draw_missing";
  case PlayerNativeSceneRecordResult::kPreparedDrawIdentityMismatch:
    return "prepared_draw_identity_mismatch";
  case PlayerNativeSceneRecordResult::kUnsupportedPrimitive:
    return "unsupported_primitive";
  case PlayerNativeSceneRecordResult::kRhiDrawStateRejected:
    return "rhi_draw_state_rejected";
  }
  return "unknown";
}

PlayerReplacementPreflightResult PreflightPlayerReplacementCandidate(
    const rex::graphics::NativeGuestDrawContext &context,
    const PlayerReplacementCandidate &candidate) {
  if (context.backend != rex::graphics::NativeGuestOutputBackend::kVulkan) {
    return PlayerReplacementPreflightResult::kWrongBackend;
  }
  if (context.device == nullptr || context.cmd == nullptr) {
    return PlayerReplacementPreflightResult::kMissingContext;
  }
  if (candidate.frame == nullptr ||
      candidate.draw_index >= candidate.frame->draws.size()) {
    return PlayerReplacementPreflightResult::kInvalidCandidate;
  }
  if (g_resources.device != context.device) {
    return PlayerReplacementPreflightResult::kDeviceMismatch;
  }
  if (g_resources.layout == nullptr ||
      g_resources.prepass_pipeline == nullptr ||
      g_resources.color_pipeline == nullptr) {
    return PlayerReplacementPreflightResult::kMissingPipelineResources;
  }
  if (g_resources.palette_buffer == nullptr ||
      g_resources.constant_buffer == nullptr) {
    return PlayerReplacementPreflightResult::kMissingDynamicBuffers;
  }

  if (g_resources.prepared_frame != candidate.frame) {
    // The output callback warmed every immutable texture/mesh from frame
    // N-1. Populate only host-visible dynamics for exact frame N here,
    // without copies, barriers or any other borrowed-scope-forbidden work.
    (void)PrepareExactReplacementFrameFromWarmCaches(candidate.frame);
  }
  const bool exact_prepared_frame =
      g_resources.prepared_frame == candidate.frame;
  auto prepared = g_resources.prepared_draws.end();
  if (exact_prepared_frame) {
    prepared = std::find_if(
        g_resources.prepared_draws.begin(), g_resources.prepared_draws.end(),
        [&](const PreparedDraw &draw) {
          return draw.source_draw_index == candidate.draw_index;
        });
  } else if (g_resources.prepared_frame != nullptr) {
    // Output post-processing happens after this frame's backend draws, so
    // observer prewarm normally has frame N-1 resources while it is probing
    // frame N. That is sufficient to warm the exact current borrowed-scope
    // PSO and descriptor shapes, but never sufficient to serve a draw.
    // Require an immutable mesh/material-compatible tuple and keep actual
    // replacement on a separate exact-current-frame path.
    prepared = std::find_if(
        g_resources.prepared_draws.begin(), g_resources.prepared_draws.end(),
        [&](const PreparedDraw &prepared_draw) {
          if (prepared_draw.source_draw_index >=
              g_resources.prepared_frame->draws.size()) {
            return false;
          }
          const PlayerSkinDrawSnapshot &draw =
              g_resources.prepared_frame
                  ->draws[prepared_draw.source_draw_index];
          if (PlayerSkinMeshIdentityForDraw(draw) != candidate.mesh ||
              prepared_draw.index_count != candidate.submitted_index_count ||
              prepared_draw.primitive_type != candidate.primitive_type ||
              prepared_draw.prepass !=
                  (candidate.phase ==
                   PlayerReplacementPhase::kDepthAlphaPrepass)) {
            return false;
          }
          for (size_t slot = 0; slot < candidate.texture_fingerprints.size();
               ++slot) {
            if (draw.material.textures[slot] == nullptr ||
                draw.material.textures[slot]->payload_fingerprint !=
                    candidate.texture_fingerprints[slot]) {
              return false;
            }
          }
          return true;
        });
  }
  if (prepared == g_resources.prepared_draws.end()) {
    return exact_prepared_frame
               ? PlayerReplacementPreflightResult::kPreparedDrawMissing
               : PlayerReplacementPreflightResult::kFrameMismatch;
  }
  if (prepared->index_count != candidate.submitted_index_count ||
      prepared->primitive_type != candidate.primitive_type ||
      prepared->prepass !=
          (candidate.phase == PlayerReplacementPhase::kDepthAlphaPrepass)) {
    return PlayerReplacementPreflightResult::kPreparedDrawIdentityMismatch;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(prepared->prepass ? g_resources.prepass_pipeline
                                     : g_resources.color_pipeline);
  if (prepared->primitive_type == 0x04) {
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  } else if (prepared->primitive_type == 0x06) {
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  } else {
    return PlayerReplacementPreflightResult::kUnsupportedPrimitive;
  }
  cmd->SetConstantBuffer(0, g_resources.constant_buffer,
                         prepared->constant_offset);
  cmd->SetBufferSrv(1, prepared->vertex_buffer, 0);
  cmd->SetBufferSrv(2, g_resources.palette_buffer, prepared->palette_offset);
  cmd->SetTextures(3, prepared->texture_views.data(),
                   static_cast<uint32_t>(prepared->texture_views.size()));
  if (!cmd->PreflightDraw()) {
    return PlayerReplacementPreflightResult::kRhiDrawStateRejected;
  }
  return exact_prepared_frame ? PlayerReplacementPreflightResult::kSucceeded
                              : PlayerReplacementPreflightResult::
                                    kSucceededCompatiblePreparedFrame;
}

const char *
PlayerReplacementPreflightResultName(PlayerReplacementPreflightResult result) {
  switch (result) {
  case PlayerReplacementPreflightResult::kSucceeded:
    return "succeeded_exact_frame";
  case PlayerReplacementPreflightResult::kSucceededCompatiblePreparedFrame:
    return "succeeded_compatible_prepared_frame";
  case PlayerReplacementPreflightResult::kWrongBackend:
    return "wrong_backend";
  case PlayerReplacementPreflightResult::kMissingContext:
    return "missing_context";
  case PlayerReplacementPreflightResult::kInvalidCandidate:
    return "invalid_candidate";
  case PlayerReplacementPreflightResult::kDeviceMismatch:
    return "device_mismatch";
  case PlayerReplacementPreflightResult::kFrameMismatch:
    return "frame_mismatch";
  case PlayerReplacementPreflightResult::kMissingPipelineResources:
    return "missing_pipeline_resources";
  case PlayerReplacementPreflightResult::kMissingDynamicBuffers:
    return "missing_dynamic_buffers";
  case PlayerReplacementPreflightResult::kPreparedDrawMissing:
    return "prepared_draw_missing";
  case PlayerReplacementPreflightResult::kPreparedDrawIdentityMismatch:
    return "prepared_draw_identity_mismatch";
  case PlayerReplacementPreflightResult::kUnsupportedPrimitive:
    return "unsupported_primitive";
  case PlayerReplacementPreflightResult::kRhiDrawStateRejected:
    return "rhi_draw_state_rejected";
  }
  return "unknown";
}

bool PlayerReplacementPreflightSucceeded(
    PlayerReplacementPreflightResult result) {
  return result == PlayerReplacementPreflightResult::kSucceeded ||
         result == PlayerReplacementPreflightResult::
                       kSucceededCompatiblePreparedFrame;
}

uint32_t RenderPlayerObserverOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const PlayerSkinFrameSnapshot> &frame) {
  const int32_t geometry_group =
      REXCVAR_GET(tabletennis_native_player_observer_geometry_group);
  const int32_t prepared_geometry_group = g_resources.prepared_geometry_group;
  if (!PlayerObserverOverlayEnabled() ||
      !PreparedFrameMatches(context, frame, prepared_geometry_group) ||
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
  cmd->SetRenderTargets(context.guest_output, g_resources.depth);
  cmd->ClearDepth(g_resources.depth, 1.0f);
  uint32_t draw_count = 0;
  for (const PreparedDraw &draw : g_resources.prepared_draws) {
    if (filter_prepared_draws &&
        std::find(overlay_selection.indices.begin(),
                  overlay_selection.indices.end(),
                  draw.source_draw_index) == overlay_selection.indices.end()) {
      continue;
    }
    cmd->SetPipeline(draw.prepass ? g_resources.prepass_pipeline
                                  : g_resources.color_pipeline);
    if (draw.primitive_type == 0x04) {
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    } else if (draw.primitive_type == 0x06) {
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
    } else {
      continue;
    }
    cmd->SetConstantBuffer(0, g_resources.constant_buffer,
                           draw.constant_offset);
    cmd->SetBufferSrv(1, draw.vertex_buffer, 0);
    cmd->SetBufferSrv(2, g_resources.palette_buffer, draw.palette_offset);
    cmd->SetTextures(3, draw.texture_views.data(),
                     static_cast<uint32_t>(draw.texture_views.size()));
    cmd->SetIndexBuffer(draw.index_buffer, 0, draw.index_bytes);
    cmd->DrawIndexed(draw.index_count, 0, 0);
    ++draw_count;
  }

  if (draw_count != 0 &&
      g_resources.announced_signature != g_resources.prepared_signature) {
    g_resources.announced_signature = g_resources.prepared_signature;
    if (geometry_group < 0) {
      REXLOG_INFO(
          "Table Tennis player observer: drew all {} captured skinned-player "
          "draws in guest order with traced prepass/color phases over "
          "untouched output (observer-only; private D32 depth, single-sample "
          "A2C unavailable)",
          draw_count);
    } else {
      const PlayerSkinDrawSnapshot &first =
          frame->draws[g_resources.prepared_draws.front().source_draw_index];
      REXLOG_INFO("Table Tennis player observer: drew geometry group {} as {} "
                  "captured draws in guest order over untouched output "
                  "(player={:08X} vf95={:08X}/{:016X} "
                  "ib={:08X}/{:016X} indices={} phase_complete=true "
                  "observer_only=true)",
                  geometry_group, draw_count, first.player,
                  first.vertices->fetch.physical_address,
                  first.vertices->payload_fingerprint,
                  first.indices->physical_address,
                  first.indices->payload_fingerprint,
                  first.indices->submitted_index_count);
    }
  }
  return draw_count;
}

void ShutdownPlayerObserverRenderer() { ReleaseResources(); }

} // namespace tabletennis::native
