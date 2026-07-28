#include "native/tabletennis_venue_14d_renderer.h"

#include "native/shaders/tabletennis_venue_14d_observer_spirv.h"
#include "native/tabletennis_native_scene_compositor.h"
#include "native/tabletennis_native_scene_pass.h"
#include "native/tabletennis_texture_snapshot.h"
#include "native/tabletennis_venue_14d_observer.h"

#include <algorithm>
#include <array>
#include <bit>
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
    tabletennis_native_venue_14d_renderer, false, "Table Tennis",
    "Overlay the real, independently verified 14D venue family using its "
    "captured geometry, constants, five textures and ported guest shader. "
    "Observer-only; never suppresses or replaces a guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(tabletennis_native_venue_14d_opacity, 0.58,
                      "Table Tennis",
                      "Opacity of the real 14D observer overlay.")
    .range(0.05, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    tabletennis_native_venue_14d_display_gamma, 2.0, "Table Tennis",
    "Display gamma used only by the end-of-frame 14D comparison overlay.")
    .range(1.0, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
namespace xenos = rex::graphics::xenos;

constexpr uint64_t kVertexShader40 = 0x4EAEC701E97DCDADull;
constexpr uint64_t kVertexShader48 = 0x08D6210341AD63F6ull;
constexpr uint64_t kPixelShader = 0x14D6B61CBC3D853Cull;
constexpr uint32_t kVertexStride40 = 40;
constexpr uint32_t kVertexStride48 = 48;
constexpr uint32_t kTangentOffset40 = 36;
constexpr uint32_t kTangentOffset48 = 44;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kNativeSceneDepthControl = 0x00700736;
constexpr uint32_t kNativeSceneColorMask = 0x00000007;
constexpr uint32_t kNativeSceneColorControl = 0x87000005;
constexpr uint32_t kNativeSceneOpaqueBlendControl = 0x00010001;
constexpr uint32_t kNativeSceneAlphaBlendControl = 0x07060706;
constexpr uint32_t kNativeSceneRasterizerMode = 0x00018002;
constexpr size_t kMaximumStaticPayloads = 256;
constexpr size_t kMaximumTextures = 256;

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

struct alignas(16) DrawConstants {
  std::array<uint32_t, 4 * 4> local_to_world{};
  std::array<uint32_t, 3 * 4> direction_basis{};
  std::array<uint32_t, 4 * 4> clip_transform{};
  std::array<uint32_t, 4> c19{};
  std::array<uint32_t, 4> c20{};
  std::array<uint32_t, 4> c46{};
  std::array<uint32_t, 4> c47{};
  std::array<uint32_t, 4> c48{};
  std::array<uint32_t, 4> c49{};
  std::array<uint32_t, 4> c50{};
  std::array<uint32_t, 4> c254{};
  std::array<uint32_t, 4> c255{};
  std::array<uint32_t, 4> buffer_layout{};
  std::array<uint32_t, 4> observer_options{};
};

static_assert(sizeof(DrawConstants) == 22 * sizeof(uint32_t) * 4);
constexpr uint64_t kConstantStride =
    AlignUp(sizeof(DrawConstants), nrhi::kBufferOffsetAlignment);

struct GpuVertexPayload {
  std::shared_ptr<const Venue14DVertexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
};

struct GpuIndexPayload {
  std::shared_ptr<const Venue14DIndexPayload> snapshot;
  nrhi::Buffer *buffer = nullptr;
  uint32_t bytes = 0;
};

struct GpuTexture {
  std::shared_ptr<const TextureSnapshot> snapshot;
  nrhi::Texture *texture = nullptr;
  nrhi::TextureView *view = nullptr;
};

struct PreparedDraw {
  nrhi::Buffer *vertex_buffer = nullptr;
  nrhi::Buffer *index_buffer = nullptr;
  size_t source_draw_index = 0;
  uint32_t index_bytes = 0;
  uint32_t index_count = 0;
  uint64_t constant_offset = 0;
  std::array<nrhi::TextureView *, 5> textures{};
};

struct PreparedFrameResources {
  nrhi::Buffer *constant_buffer = nullptr;
  uint64_t constant_buffer_bytes = 0;
  std::shared_ptr<const Venue14DFrameSnapshot> frame;
  std::vector<PreparedDraw> draws;
};

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::BindingLayout *layout = nullptr;
  nrhi::Shader *vertex_shader = nullptr;
  nrhi::Shader *pixel_shader = nullptr;
  nrhi::Pipeline *pipeline = nullptr;
  nrhi::Pipeline *native_scene_pipeline = nullptr;
  nrhi::Pipeline *native_scene_blended_pipeline = nullptr;
  nrhi::Format pipeline_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_color_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_depth_format = nrhi::Format::kUnknown;
  uint32_t native_scene_sample_count = 0;
  std::vector<GpuVertexPayload> vertices;
  std::vector<GpuIndexPayload> indices;
  std::vector<GpuTexture> textures;
  PreparedFrameResources overlay;
  PreparedFrameResources native_scene;
  bool failed = false;
  bool announced_draw = false;
  bool announced_full_mips = false;
  bool announced_sampler_rejection = false;
  bool announced_native_scene_contract_rejection = false;
  bool announced_native_scene_data_rejection = false;
};

Resources g_resources;

void ReleaseResources() {
  if (g_resources.device != nullptr) {
    g_resources.device->DestroyDeferred(g_resources.pipeline);
    g_resources.device->DestroyDeferred(g_resources.native_scene_pipeline);
    g_resources.device->DestroyDeferred(
        g_resources.native_scene_blended_pipeline);
    g_resources.device->DestroyDeferred(g_resources.vertex_shader);
    g_resources.device->DestroyDeferred(g_resources.pixel_shader);
    g_resources.device->DestroyDeferred(g_resources.overlay.constant_buffer);
    g_resources.device->DestroyDeferred(
        g_resources.native_scene.constant_buffer);
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

nrhi::Buffer *CreateAndFillBuffer(nrhi::Device *device, const void *source,
                                  uint64_t size,
                                  nrhi::BufferBindClass bind_class) {
  if (device == nullptr || source == nullptr || size == 0) {
    return nullptr;
  }
  nrhi::BufferDesc desc;
  desc.size = size;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = bind_class;
  nrhi::Buffer *const buffer = device->CreateBuffer(desc);
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

bool ExactVertexLayout(const Venue14DDrawSnapshot &draw, uint32_t &stride,
                       uint32_t &tangent_offset) {
  if (!draw.valid() || draw.backend.pixel_shader_hash != kPixelShader ||
      draw.title->vertices == nullptr || draw.title->vertices->endian != 2) {
    return false;
  }
  if (draw.backend.vertex_shader_hash == kVertexShader40 &&
      draw.title->vertices->stride == kVertexStride40) {
    stride = kVertexStride40;
    tangent_offset = kTangentOffset40;
    return true;
  }
  if (draw.backend.vertex_shader_hash == kVertexShader48 &&
      draw.title->vertices->stride == kVertexStride48) {
    stride = kVertexStride48;
    tangent_offset = kTangentOffset48;
    return true;
  }
  return false;
}

GpuVertexPayload *
EnsureVertex(const std::shared_ptr<const Venue14DVertexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid()) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(g_resources.vertices,
                                          [&](const GpuVertexPayload &vertex) {
                                            return vertex.snapshot == snapshot;
                                          });
  if (found != g_resources.vertices.end()) {
    return &*found;
  }
  if (g_resources.vertices.size() >= kMaximumStaticPayloads) {
    REXLOG_ERROR("Table Tennis 14D observer: vertex cache exhausted");
    return nullptr;
  }
  nrhi::Buffer *const buffer = CreateAndFillBuffer(
      g_resources.device, snapshot->raw_bytes.data(),
      snapshot->raw_bytes.size(), nrhi::BufferBindClass::kFull);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis 14D observer: raw vf95 upload failed");
    return nullptr;
  }
  g_resources.vertices.push_back({snapshot, buffer});
  return &g_resources.vertices.back();
}

GpuIndexPayload *
EnsureIndex(const std::shared_ptr<const Venue14DIndexPayload> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      snapshot->indices.size() >
          std::numeric_limits<uint32_t>::max() / sizeof(uint16_t)) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      g_resources.indices,
      [&](const GpuIndexPayload &index) { return index.snapshot == snapshot; });
  if (found != g_resources.indices.end()) {
    return &*found;
  }
  if (g_resources.indices.size() >= kMaximumStaticPayloads) {
    REXLOG_ERROR("Table Tennis 14D observer: index cache exhausted");
    return nullptr;
  }
  const uint32_t bytes =
      static_cast<uint32_t>(snapshot->indices.size() * sizeof(uint16_t));
  nrhi::Buffer *const buffer =
      CreateAndFillBuffer(g_resources.device, snapshot->indices.data(), bytes,
                          nrhi::BufferBindClass::kVertexIndex);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis 14D observer: decoded index upload failed");
    return nullptr;
  }
  g_resources.indices.push_back({snapshot, buffer, bytes});
  return &g_resources.indices.back();
}

nrhi::Format HostTextureFormat(const TextureSnapshot &snapshot) {
  switch (rex::graphics::GetBaseFormat(
      static_cast<xenos::TextureFormat>(snapshot.format))) {
  case xenos::TextureFormat::k_DXT1:
    return nrhi::Format::kBC1_UNORM;
  case xenos::TextureFormat::k_DXT4_5:
    return nrhi::Format::kBC3_UNORM;
  default:
    return nrhi::Format::kUnknown;
  }
}

bool ComposeTextureSwizzle(uint32_t guest_swizzle, nrhi::Swizzle output[4]) {
  for (uint32_t channel = 0; channel < 4; ++channel) {
    uint32_t component = (guest_swizzle >> (channel * 3)) & 7u;
    if (component >= 4) {
      // Match the guest texture cache's invalid 6/7 sanitization.
      component &= 5u;
    }
    if (component > static_cast<uint32_t>(nrhi::Swizzle::kOne)) {
      return false;
    }
    output[channel] = static_cast<nrhi::Swizzle>(component);
  }
  return true;
}

bool ExactSamplerFetch(const std::array<uint32_t, 6> &fetch_words,
                       const TextureSnapshot &texture, uint32_t slot) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.dword_0 = fetch_words[0];
  fetch.dword_1 = fetch_words[1];
  fetch.dword_2 = fetch_words[2];
  fetch.dword_3 = fetch_words[3];
  fetch.dword_4 = fetch_words[4];
  fetch.dword_5 = fetch_words[5];

  const bool cube = slot == 4;
  const xenos::ClampMode expected_clamp =
      cube ? xenos::ClampMode::kClampToEdge : xenos::ClampMode::kRepeat;
  const xenos::DataDimension expected_dimension =
      cube ? xenos::DataDimension::kCube : xenos::DataDimension::k2DOrStacked;
  const xenos::TextureFormat base_format =
      rex::graphics::GetBaseFormat(fetch.format);
  const bool supported_format =
      base_format == xenos::TextureFormat::k_DXT1 ||
      base_format == xenos::TextureFormat::k_DXT4_5;

  // The plain tfetch instructions in the verified 14D shader have no
  // instruction-level filter, LOD, gradient or bias overrides, so these are
  // the effective fetch-constant states. RexGlue's guest sampler path forces
  // min/mag/mip linear when anisotropy is enabled; the two immutable NRHI
  // samplers below intentionally reproduce that normalized backend behavior.
  // Texture compression is descriptor-owned, not slot-owned: live 14D draws
  // bind both DXT1 and DXT5 resources at slot 2. The immutable snapshot carries
  // the exact same six fetch words and HostTextureFormat validates the matching
  // resource representation, so accepting either proven format does not relax
  // sampler behavior.
  return fetch.type == xenos::FetchConstantType::kTexture &&
         supported_format &&
         fetch.dimension == expected_dimension && !fetch.stacked &&
         fetch.clamp_x == expected_clamp && fetch.clamp_y == expected_clamp &&
         fetch.clamp_z == expected_clamp &&
         fetch.mag_filter == xenos::TextureFilter::kLinear &&
         fetch.min_filter == xenos::TextureFilter::kLinear &&
         fetch.mip_filter == xenos::TextureFilter::kPoint &&
         fetch.aniso_filter == xenos::AnisoFilter::kMax_2_1 &&
         fetch.mag_aniso_walk == 1 && fetch.min_aniso_walk == 1 &&
         fetch.lod_bias == 0 && fetch.grad_exp_adjust_h == 0 &&
         fetch.grad_exp_adjust_v == 0 && fetch.exp_adjust == 0 &&
         fetch.mip_min_level == 0 &&
         fetch.mip_max_level == texture.descriptor_mip_max_level &&
         fetch.border_color == xenos::BorderColor::k_ABGR_Black &&
         fetch.border_size == 0 && fetch.aniso_bias == 0 &&
         fetch.tri_clamp == 3 && fetch.force_bc_w_to_max == 0 &&
         fetch.num_format == 0 && fetch.swizzle == 0x688 &&
         ((fetch.dword_0 >> 2) & 0xFFu) == 0 &&
         texture.fetch_words == fetch_words &&
         texture.layer_count == (cube ? 6u : 1u);
}

GpuTexture *
EnsureTexture(const rex::graphics::NativeGuestOutputRenderContext &context,
              const std::shared_ptr<const TextureSnapshot> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      !snapshot->full_mip_chain() ||
      (snapshot->layer_count != 1 && snapshot->layer_count != 6)) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      g_resources.textures,
      [&](const GpuTexture &texture) { return texture.snapshot == snapshot; });
  if (found != g_resources.textures.end()) {
    return &*found;
  }
  if (g_resources.textures.size() >= kMaximumTextures) {
    REXLOG_ERROR("Table Tennis 14D observer: texture cache exhausted");
    return nullptr;
  }

  const nrhi::Format format = HostTextureFormat(*snapshot);
  if (format == nrhi::Format::kUnknown) {
    REXLOG_ERROR("Table Tennis 14D observer: unsupported texture format {}",
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
  // All 14D fetches are block-aligned powers of two. Failing closed here
  // prevents the host resource's mip dimensions from silently diverging from
  // a non-block-aligned guest descriptor.
  if (host_width != snapshot->width || host_height != snapshot->height ||
      snapshot->mips.size() > std::numeric_limits<uint32_t>::max()) {
    return nullptr;
  }

  struct UploadRegion {
    const TextureMipSnapshot *mip = nullptr;
    uint32_t layer = 0;
    uint32_t row_pitch = 0;
    uint32_t footprint_width = 0;
    uint32_t footprint_height = 0;
    uint64_t offset = 0;
  };
  constexpr uint64_t kTexturePlacementAlignment = 512;
  std::vector<UploadRegion> upload_regions;
  upload_regions.reserve(snapshot->mips.size() * snapshot->layer_count);
  uint64_t upload_bytes = 0;
  for (const TextureMipSnapshot &mip : snapshot->mips) {
    const uint32_t expected_width = std::max(snapshot->width >> mip.level, 1u);
    const uint32_t expected_height =
        std::max(snapshot->height >> mip.level, 1u);
    if (mip.width != expected_width || mip.height != expected_height ||
        mip.row_pitch_bytes != mip.width_blocks * snapshot->bytes_per_block ||
        mip.layer_stride_bytes !=
            static_cast<size_t>(mip.row_pitch_bytes) * mip.height_blocks) {
      return nullptr;
    }
    const uint32_t upload_row_pitch = static_cast<uint32_t>(
        AlignUp(mip.row_pitch_bytes, nrhi::kRowPitchAlignment));
    const uint64_t upload_face_bytes =
        static_cast<uint64_t>(upload_row_pitch) * mip.height_blocks;
    for (uint32_t layer = 0; layer < snapshot->layer_count; ++layer) {
      upload_bytes = AlignUp(upload_bytes, kTexturePlacementAlignment);
      if (upload_face_bytes >
          std::numeric_limits<uint64_t>::max() - upload_bytes) {
        return nullptr;
      }
      upload_regions.push_back({
          .mip = &mip,
          .layer = layer,
          .row_pitch = upload_row_pitch,
          .footprint_width = mip.width_blocks * snapshot->block_width,
          .footprint_height = mip.height_blocks * snapshot->block_height,
          .offset = upload_bytes,
      });
      upload_bytes += upload_face_bytes;
    }
  }
  if (upload_bytes == 0 || upload_bytes > std::numeric_limits<size_t>::max()) {
    return nullptr;
  }

  nrhi::TextureDesc texture_desc;
  texture_desc.kind = snapshot->layer_count == 6 ? nrhi::TextureKind::kCube
                                                 : nrhi::TextureKind::k2D;
  texture_desc.width = host_width;
  texture_desc.height = host_height;
  texture_desc.mip_levels = static_cast<uint32_t>(snapshot->mips.size());
  texture_desc.format = format;
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
    REXLOG_ERROR("Table Tennis 14D observer: texture resource creation failed");
    return nullptr;
  }

  uint8_t *const mapped = static_cast<uint8_t *>(context.device->Map(upload));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  std::memset(mapped, 0, static_cast<size_t>(upload_bytes));
  for (const UploadRegion &region : upload_regions) {
    const TextureMipSnapshot &mip = *region.mip;
    const uint8_t *const source = snapshot->linear_blocks.data() +
                                  mip.linear_offset +
                                  mip.layer_stride_bytes * region.layer;
    for (uint32_t row = 0; row < mip.height_blocks; ++row) {
      std::memcpy(mapped + region.offset +
                      static_cast<uint64_t>(row) * region.row_pitch,
                  source + static_cast<size_t>(row) * mip.row_pitch_bytes,
                  mip.row_pitch_bytes);
    }
  }
  context.device->Unmap(upload);

  nrhi::TextureViewDesc view_desc;
  view_desc.dimension = snapshot->layer_count == 6 ? nrhi::ViewDimension::kCube
                                                   : nrhi::ViewDimension::k2D;
  view_desc.base_mip = 0;
  view_desc.mip_levels = static_cast<uint32_t>(snapshot->mips.size());
  if (!ComposeTextureSwizzle(snapshot->fetch_swizzle, view_desc.swizzle)) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  nrhi::TextureView *const view =
      context.device->CreateTextureView(texture, view_desc);
  if (view == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  for (const UploadRegion &region : upload_regions) {
    context.cmd->CopyBufferToTexture(
        texture, region.mip->level, region.layer, upload, region.offset,
        region.row_pitch, region.footprint_width, region.footprint_height, 1);
  }
  context.cmd->Barrier(texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.device->DestroyDeferred(upload);
  g_resources.textures.push_back({snapshot, texture, view});
  return &g_resources.textures.back();
}

bool EnsurePipelineResources(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (!EnsureDevice(context) || context.guest_output == nullptr) {
    return false;
  }
  nrhi::Device *const device = context.device;
  if (g_resources.layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 3;
    layout.params[0] = {nrhi::BindingParamKind::kConstantBuffer, 0, 1,
                        nrhi::Visibility::kAll};
    layout.params[1] = {nrhi::BindingParamKind::kBufferSrv, 0, 1,
                        nrhi::Visibility::kVertex};
    layout.params[2] = {nrhi::BindingParamKind::kTextureTable, 1, 5,
                        nrhi::Visibility::kPixel};
    layout.static_sampler_count = 2;
    layout.static_samplers[0] = {0, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kWrap, 2};
    layout.static_samplers[1] = {1, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kClamp, 2};
    layout.allow_input_layout = false;
    g_resources.layout = device->CreateBindingLayout(layout);
    if (g_resources.layout == nullptr) {
      REXLOG_ERROR("Table Tennis 14D observer: binding layout creation failed");
      g_resources.failed = true;
      return false;
    }
  }
  if (g_resources.vertex_shader == nullptr ||
      g_resources.pixel_shader == nullptr) {
    nrhi::ShaderDesc vertex_desc;
    vertex_desc.stage = nrhi::ShaderStage::kVertex;
    vertex_desc.name = "tabletennis_venue_14d_observer.hlsl";
    vertex_desc.hlsl_source = venue_14d_observer_shader::kHlsl;
    vertex_desc.entry_point = "vs_main";
    vertex_desc.spirv = venue_14d_observer_shader::kVertexSpirv;
    vertex_desc.spirv_size_bytes = venue_14d_observer_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc pixel_desc;
    pixel_desc.stage = nrhi::ShaderStage::kPixel;
    pixel_desc.name = "tabletennis_venue_14d_observer.hlsl";
    pixel_desc.hlsl_source = venue_14d_observer_shader::kHlsl;
    pixel_desc.entry_point = "ps_main";
    pixel_desc.spirv = venue_14d_observer_shader::kPixelSpirv;
    pixel_desc.spirv_size_bytes = venue_14d_observer_shader::kPixelSpirvBytes;
    g_resources.vertex_shader = device->CreateShader(vertex_desc);
    g_resources.pixel_shader = device->CreateShader(pixel_desc);
    if (g_resources.vertex_shader == nullptr ||
        g_resources.pixel_shader == nullptr) {
      REXLOG_ERROR("Table Tennis 14D observer: shader creation failed");
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
  if (g_resources.pipeline != nullptr &&
      g_resources.pipeline_format == output_format) {
    return true;
  }
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
  pipeline.depth.test_enable = false;
  pipeline.depth.write_enable = false;
  pipeline.blend.enable = true;
  pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op = nrhi::BlendOp::kAdd;
  pipeline.blend.src_alpha = nrhi::BlendFactor::kOne;
  pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op_alpha = nrhi::BlendOp::kAdd;
  pipeline.blend.write_mask = 0xF;
  pipeline.rtv_format = output_format;
  pipeline.sample_count = 1;
  g_resources.pipeline = device->CreateGraphicsPipeline(pipeline);
  if (g_resources.pipeline == nullptr) {
    REXLOG_ERROR(
        "Table Tennis 14D observer: graphics pipeline creation failed");
    g_resources.failed = true;
    return false;
  }
  g_resources.pipeline_format = output_format;
  return true;
}

bool EnsureNativeScenePipeline(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      !EnsurePipelineResources(context)) {
    return false;
  }

  const nrhi::Format color_format = targets.color->format();
  const nrhi::Format depth_format = targets.depth->format();
  if (g_resources.native_scene_pipeline != nullptr &&
      g_resources.native_scene_blended_pipeline != nullptr &&
      g_resources.native_scene_color_format == color_format &&
      g_resources.native_scene_depth_format == depth_format &&
      g_resources.native_scene_sample_count == targets.sample_count) {
    return true;
  }
  context.device->DestroyDeferred(g_resources.native_scene_pipeline);
  context.device->DestroyDeferred(g_resources.native_scene_blended_pipeline);
  g_resources.native_scene_pipeline = nullptr;
  g_resources.native_scene_blended_pipeline = nullptr;

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
  pipeline.blend.enable = false;
  pipeline.blend.alpha_to_coverage = false;
  // The verified backend contract writes RGB and preserves target alpha.
  pipeline.blend.write_mask = 0x7;
  pipeline.rtv_format = color_format;
  pipeline.dsv_format = depth_format;
  pipeline.sample_count = targets.sample_count;
  g_resources.native_scene_pipeline =
      context.device->CreateGraphicsPipeline(pipeline);
  pipeline.blend.enable = true;
  pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op = nrhi::BlendOp::kAdd;
  pipeline.blend.src_alpha = nrhi::BlendFactor::kSrcAlpha;
  pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  pipeline.blend.op_alpha = nrhi::BlendOp::kAdd;
  g_resources.native_scene_blended_pipeline =
      context.device->CreateGraphicsPipeline(pipeline);
  if (g_resources.native_scene_pipeline == nullptr ||
      g_resources.native_scene_blended_pipeline == nullptr) {
    REXLOG_ERROR(
        "Table Tennis 14D native scene: opaque/blended shared-pass pipeline "
        "creation failed");
    return false;
  }
  g_resources.native_scene_color_format = color_format;
  g_resources.native_scene_depth_format = depth_format;
  g_resources.native_scene_sample_count = targets.sample_count;
  return true;
}

void CopyConstant(
    const std::array<uint32_t, Venue14DMaterialSnapshot::kConstantWordCount>
        &source,
    size_t register_index, std::array<uint32_t, 4> &destination) {
  std::copy_n(source.begin() + register_index * 4, 4, destination.begin());
}

DrawConstants BuildConstants(const Venue14DDrawSnapshot &draw, uint32_t stride,
                             uint32_t tangent_offset, float opacity,
                             float reciprocal_display_gamma) {
  DrawConstants constants;
  const auto &vertex = draw.title->material.vertex_constant_words;
  const auto &pixel = draw.title->material.pixel_constant_words;
  std::copy_n(vertex.begin(), constants.local_to_world.size(),
              constants.local_to_world.begin());
  std::copy_n(vertex.begin() + 4 * 4, constants.direction_basis.size(),
              constants.direction_basis.begin());
  std::copy_n(vertex.begin() + 12 * 4, constants.clip_transform.size(),
              constants.clip_transform.begin());
  CopyConstant(pixel, 19, constants.c19);
  CopyConstant(pixel, 20, constants.c20);
  CopyConstant(pixel, 46, constants.c46);
  CopyConstant(pixel, 47, constants.c47);
  CopyConstant(pixel, 48, constants.c48);
  CopyConstant(pixel, 49, constants.c49);
  CopyConstant(pixel, 50, constants.c50);
  CopyConstant(pixel, 254, constants.c254);
  CopyConstant(pixel, 255, constants.c255);
  constants.buffer_layout[0] = stride;
  constants.buffer_layout[1] = tangent_offset;
  constants.observer_options[0] = std::bit_cast<uint32_t>(opacity);
  constants.observer_options[1] =
      std::bit_cast<uint32_t>(reciprocal_display_gamma);
  return constants;
}

bool NativeSceneBackendContractReady(const Venue14DDrawSnapshot &draw) {
  return draw.valid() &&
         draw.backend.normalized_depth_control == kNativeSceneDepthControl &&
         draw.backend.normalized_color_mask == kNativeSceneColorMask &&
         draw.backend.color_control == kNativeSceneColorControl &&
         (draw.backend.blend_control_0 ==
              kNativeSceneOpaqueBlendControl ||
          draw.backend.blend_control_0 ==
              kNativeSceneAlphaBlendControl) &&
         draw.backend.rasterizer_mode_control_valid &&
         draw.backend.rasterizer_mode_control ==
             kNativeSceneRasterizerMode &&
         draw.backend.color_attachment_count == 1 &&
         draw.backend.color_attachment_formats[0] ==
             static_cast<uint32_t>(nrhi::Format::kR8G8B8A8_UNORM) &&
         draw.backend.sample_count == 4 &&
         draw.backend.sample_mask == std::numeric_limits<uint64_t>::max() &&
         !draw.backend.primitive_restart_enabled;
}

bool NativeSceneFrameReady(const Venue14DFrameSnapshot &frame) {
  return frame.valid() &&
         std::ranges::all_of(frame.draws, NativeSceneBackendContractReady);
}

void LogNativeSceneContractRejection(const Venue14DFrameSnapshot &frame) {
  if (g_resources.announced_native_scene_contract_rejection) {
    return;
  }
  g_resources.announced_native_scene_contract_rejection = true;
  const auto rejected = std::ranges::find_if(
      frame.draws, [](const Venue14DDrawSnapshot &draw) {
        return !NativeSceneBackendContractReady(draw);
      });
  if (rejected == frame.draws.end()) {
    REXLOG_INFO(
        "Table Tennis 14D native scene: frame={} rejected invalid frame "
        "snapshot before preparation",
        frame.sequence);
    return;
  }
  const size_t draw_index =
      static_cast<size_t>(rejected - frame.draws.begin());
  REXLOG_INFO(
      "Table Tennis 14D native scene: frame={} draw={} ordinal={} backend "
      "contract rejected valid={} depth={:08X}/{:08X} "
      "mask={:08X}/{:08X} color={:08X}/{:08X} "
      "blend={:08X}/[{:08X}|{:08X}] "
      "raster={:08X}/{:08X}/{} attachments={} color0={} samples={} "
      "sample_mask={:016X} restart={}",
      frame.sequence, draw_index,
      rejected->title != nullptr ? rejected->title->ordinal : 0,
      rejected->valid(), rejected->backend.normalized_depth_control,
      kNativeSceneDepthControl, rejected->backend.normalized_color_mask,
      kNativeSceneColorMask, rejected->backend.color_control,
      kNativeSceneColorControl, rejected->backend.blend_control_0,
      kNativeSceneOpaqueBlendControl, kNativeSceneAlphaBlendControl,
      rejected->backend.rasterizer_mode_control,
      kNativeSceneRasterizerMode,
      rejected->backend.rasterizer_mode_control_valid,
      rejected->backend.color_attachment_count,
      rejected->backend.color_attachment_formats[0],
      rejected->backend.sample_count, rejected->backend.sample_mask,
      rejected->backend.primitive_restart_enabled);
}

bool PreparedFrameMatches(
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame,
    const PreparedFrameResources &prepared) {
  if (frame == nullptr || prepared.frame != frame ||
      prepared.constant_buffer == nullptr ||
      prepared.constant_buffer_bytes != kConstantStride * frame->draws.size() ||
      prepared.draws.size() != frame->draws.size()) {
    return false;
  }
  for (size_t index = 0; index < prepared.draws.size(); ++index) {
    const PreparedDraw &prepared_draw = prepared.draws[index];
    const Venue14DDrawSnapshot &draw = frame->draws[index];
    if (prepared_draw.source_draw_index != index ||
        prepared_draw.vertex_buffer == nullptr ||
        prepared_draw.index_buffer == nullptr ||
        prepared_draw.index_count !=
            draw.title->identity.submitted_index_count ||
        std::ranges::find(prepared_draw.textures, nullptr) !=
            prepared_draw.textures.end()) {
      return false;
    }
  }
  return true;
}

bool EnsurePreparedFrameData(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame, float opacity,
    float reciprocal_display_gamma, PreparedFrameResources &destination) {
  if (frame == nullptr || !frame->valid() || context.cmd == nullptr ||
      !EnsureDevice(context)) {
    return false;
  }
  if (PreparedFrameMatches(frame, destination)) {
    return true;
  }

  const uint64_t constant_bytes = kConstantStride * frame->draws.size();
  if (constant_bytes == 0 ||
      constant_bytes > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  nrhi::BufferDesc constant_desc;
  constant_desc.size = constant_bytes;
  constant_desc.heap = nrhi::HeapKind::kUpload;
  constant_desc.bind_class = nrhi::BufferBindClass::kFull;
  nrhi::Buffer *const constant_buffer =
      context.device->CreateBuffer(constant_desc);
  if (constant_buffer == nullptr) {
    return false;
  }
  uint8_t *const mapped =
      static_cast<uint8_t *>(context.device->Map(constant_buffer));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(constant_buffer);
    return false;
  }
  std::memset(mapped, 0, static_cast<size_t>(constant_bytes));

  std::vector<PreparedDraw> prepared;
  prepared.reserve(frame->draws.size());
  bool valid = true;
  for (size_t index = 0; index < frame->draws.size(); ++index) {
    const Venue14DDrawSnapshot &draw = frame->draws[index];
    uint32_t stride = 0;
    uint32_t tangent_offset = 0;
    if (!ExactVertexLayout(draw, stride, tangent_offset) ||
        draw.title->indices == nullptr ||
        draw.title->identity.primitive_type != kTriangleStripPrimitive) {
      valid = false;
      break;
    }
    GpuVertexPayload *const vertex = EnsureVertex(draw.title->vertices);
    GpuIndexPayload *const indices = EnsureIndex(draw.title->indices);
    PreparedDraw prepared_draw;
    prepared_draw.vertex_buffer = vertex != nullptr ? vertex->buffer : nullptr;
    prepared_draw.index_buffer = indices != nullptr ? indices->buffer : nullptr;
    prepared_draw.source_draw_index = index;
    prepared_draw.index_bytes = indices != nullptr ? indices->bytes : 0;
    prepared_draw.index_count = draw.title->identity.submitted_index_count;
    prepared_draw.constant_offset = index * kConstantStride;
    for (size_t slot = 0; slot < prepared_draw.textures.size(); ++slot) {
      const auto &texture_snapshot = draw.title->material.textures[slot];
      if (texture_snapshot == nullptr ||
          !ExactSamplerFetch(draw.title->material.texture_fetches[slot],
                             *texture_snapshot, static_cast<uint32_t>(slot))) {
        if (!g_resources.announced_sampler_rejection) {
          g_resources.announced_sampler_rejection = true;
          xenos::xe_gpu_texture_fetch_t fetch{};
          fetch.dword_0 = draw.title->material.texture_fetches[slot][0];
          fetch.dword_1 = draw.title->material.texture_fetches[slot][1];
          fetch.dword_2 = draw.title->material.texture_fetches[slot][2];
          fetch.dword_3 = draw.title->material.texture_fetches[slot][3];
          fetch.dword_4 = draw.title->material.texture_fetches[slot][4];
          fetch.dword_5 = draw.title->material.texture_fetches[slot][5];
          REXLOG_ERROR(
              "Table Tennis 14D observer: rejected unproven sampler state "
              "at draw={} ordinal={} slot={} "
              "words={:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X} "
              "type={} format={} dimension={} stacked={} "
              "clamp={}/{}/{} filter={}/{}/{} aniso={} walk={}/{} "
              "lod_bias={} grad={}/{} exp={} levels={}/{} border={}/{} "
              "aniso_bias={} tri_clamp={} bc_w={} num={} swizzle={:03X} "
              "descriptor_mip_max={} fetch_copy_match={} "
              "(observer remains disabled for frame)",
              index, draw.title->ordinal, slot, fetch.dword_0, fetch.dword_1,
              fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5,
              static_cast<uint32_t>(fetch.type),
              static_cast<uint32_t>(fetch.format),
              static_cast<uint32_t>(fetch.dimension),
              static_cast<uint32_t>(fetch.stacked),
              static_cast<uint32_t>(fetch.clamp_x),
              static_cast<uint32_t>(fetch.clamp_y),
              static_cast<uint32_t>(fetch.clamp_z),
              static_cast<uint32_t>(fetch.mag_filter),
              static_cast<uint32_t>(fetch.min_filter),
              static_cast<uint32_t>(fetch.mip_filter),
              static_cast<uint32_t>(fetch.aniso_filter),
              static_cast<uint32_t>(fetch.mag_aniso_walk),
              static_cast<uint32_t>(fetch.min_aniso_walk),
              static_cast<uint32_t>(fetch.lod_bias),
              static_cast<uint32_t>(fetch.grad_exp_adjust_h),
              static_cast<uint32_t>(fetch.grad_exp_adjust_v),
              static_cast<uint32_t>(fetch.exp_adjust),
              static_cast<uint32_t>(fetch.mip_min_level),
              static_cast<uint32_t>(fetch.mip_max_level),
              static_cast<uint32_t>(fetch.border_color),
              static_cast<uint32_t>(fetch.border_size),
              static_cast<uint32_t>(fetch.aniso_bias),
              static_cast<uint32_t>(fetch.tri_clamp),
              static_cast<uint32_t>(fetch.force_bc_w_to_max),
              static_cast<uint32_t>(fetch.num_format),
              static_cast<uint32_t>(fetch.swizzle),
              texture_snapshot != nullptr
                  ? texture_snapshot->descriptor_mip_max_level
                  : 0,
              texture_snapshot != nullptr &&
                  texture_snapshot->fetch_words ==
                      draw.title->material.texture_fetches[slot]);
        }
        valid = false;
        break;
      }
      GpuTexture *const texture = EnsureTexture(context, texture_snapshot);
      prepared_draw.textures[slot] =
          texture != nullptr ? texture->view : nullptr;
    }
    if (!valid) {
      break;
    }
    if (prepared_draw.vertex_buffer == nullptr ||
        prepared_draw.index_buffer == nullptr ||
        std::ranges::find(prepared_draw.textures, nullptr) !=
            prepared_draw.textures.end()) {
      valid = false;
      break;
    }
    if (draw.title->material.textures[4]->layer_count != 6) {
      valid = false;
      break;
    }
    for (size_t slot = 0; slot < 4; ++slot) {
      if (draw.title->material.textures[slot]->layer_count != 1) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      break;
    }
    const DrawConstants constants = BuildConstants(
        draw, stride, tangent_offset, opacity, reciprocal_display_gamma);
    std::memcpy(mapped + prepared_draw.constant_offset, &constants,
                sizeof(constants));
    prepared.push_back(prepared_draw);
  }
  context.device->Unmap(constant_buffer);
  if (!valid || prepared.size() != frame->draws.size()) {
    context.device->DestroyDeferred(constant_buffer);
    return false;
  }

  context.device->DestroyDeferred(destination.constant_buffer);
  destination.constant_buffer = constant_buffer;
  destination.constant_buffer_bytes = constant_bytes;
  destination.frame = frame;
  destination.draws = std::move(prepared);
  if (!g_resources.announced_full_mips) {
    g_resources.announced_full_mips = true;
    REXLOG_INFO("Table Tennis 14D observer: prepared {} exact ordered draws; "
                "five complete descriptor-selected texture mip chains per "
                "draw with proven repeat/clamp sampler states (observer-only)",
                frame->draws.size());
  }
  return true;
}

bool EnsurePreparedFrame(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame) {
  const bool frame_changed = g_resources.overlay.frame != frame;
  const bool prepared =
      EnsurePipeline(context) &&
      EnsurePreparedFrameData(
          context, frame,
          static_cast<float>(REXCVAR_GET(tabletennis_native_venue_14d_opacity)),
          1.0f / static_cast<float>(
                     REXCVAR_GET(tabletennis_native_venue_14d_display_gamma)),
          g_resources.overlay);
  if (prepared && frame_changed) {
    g_resources.announced_draw = false;
  }
  return prepared;
}

bool PreparedNativeSceneFrameMatches(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame) {
  return ValidateNativeScenePassTargets(context, targets) ==
             NativeScenePassTargetValidation::kValid &&
         frame != nullptr && NativeSceneFrameReady(*frame) &&
         g_resources.device == context.device &&
         g_resources.layout != nullptr &&
         g_resources.native_scene_pipeline != nullptr &&
         g_resources.native_scene_blended_pipeline != nullptr &&
         g_resources.native_scene_color_format == targets.color->format() &&
         g_resources.native_scene_depth_format == targets.depth->format() &&
         g_resources.native_scene_sample_count == targets.sample_count &&
         PreparedFrameMatches(frame, g_resources.native_scene);
}

} // namespace

bool Venue14DRendererEnabled() {
  return REXCVAR_GET(tabletennis_native_venue_14d_renderer);
}

bool PrepareVenue14DObserver(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame) {
  return Venue14DRendererEnabled() && EnsurePreparedFrame(context, frame);
}

uint32_t RenderVenue14DObserver(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame) {
  if (!Venue14DRendererEnabled() || !EnsurePreparedFrame(context, frame) ||
      !PreparedFrameMatches(frame, g_resources.overlay)) {
    return 0;
  }

  const PreparedFrameResources &prepared = g_resources.overlay;
  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.pipeline);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  uint32_t draw_count = 0;
  for (const PreparedDraw &draw : prepared.draws) {
    cmd->SetConstantBuffer(0, prepared.constant_buffer, draw.constant_offset);
    cmd->SetBufferSrv(1, draw.vertex_buffer, 0);
    cmd->SetTextures(2, draw.textures.data(),
                     static_cast<uint32_t>(draw.textures.size()));
    cmd->SetIndexBuffer(draw.index_buffer, 0, draw.index_bytes);
    cmd->DrawIndexed(draw.index_count, 0, 0);
    ++draw_count;
  }

  if (draw_count != 0 && !g_resources.announced_draw) {
    g_resources.announced_draw = true;
    REXLOG_INFO("Table Tennis 14D observer: drew {} real title meshes in "
                "backend-verified order with the ported five-texture material "
                "(guest frame untouched, no replacement)",
                draw_count);
  }
  return draw_count;
}

bool PrepareVenue14DNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      frame == nullptr) {
    return false;
  }
  if (!NativeSceneFrameReady(*frame)) {
    LogNativeSceneContractRejection(*frame);
    return false;
  }
  if (!EnsureNativeScenePipeline(context, targets)) {
    return false;
  }
  if (!EnsurePreparedFrameData(context, frame, 1.0f, 1.0f,
                               g_resources.native_scene)) {
    if (!g_resources.announced_native_scene_data_rejection) {
      g_resources.announced_native_scene_data_rejection = true;
      REXLOG_INFO(
          "Table Tennis 14D native scene: frame={} immutable GPU payload "
          "preparation failed after backend contract validation",
          frame->sequence);
    }
    return false;
  }
  return true;
}

Venue14DNativeSceneRecordResult RecordPreparedVenue14DNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const Venue14DFrameSnapshot> &frame,
    const NativeSceneDrawRef &draw_ref) {
  if (ValidateNativeScenePassTargets(context, targets) !=
      NativeScenePassTargetValidation::kValid) {
    return Venue14DNativeSceneRecordResult::kInvalidTarget;
  }
  if (draw_ref.family != NativeSceneDrawFamily::kVenue14D) {
    return Venue14DNativeSceneRecordResult::kWrongFamily;
  }
  if (frame == nullptr || !frame->valid()) {
    return Venue14DNativeSceneRecordResult::kInvalidFrame;
  }
  if (!NativeSceneFrameReady(*frame)) {
    return Venue14DNativeSceneRecordResult::kUnsupportedBackendContract;
  }
  if (!PreparedNativeSceneFrameMatches(context, targets, frame)) {
    return Venue14DNativeSceneRecordResult::kResourcesNotPrepared;
  }
  if (draw_ref.family_draw_index >= frame->draws.size()) {
    return Venue14DNativeSceneRecordResult::kDrawIndexOutOfRange;
  }

  const Venue14DDrawSnapshot &draw = frame->draws[draw_ref.family_draw_index];
  if (draw.title == nullptr || draw_ref.ordinal == 0 ||
      draw.title->ordinal != draw_ref.ordinal) {
    return Venue14DNativeSceneRecordResult::kOrdinalMismatch;
  }
  const PreparedFrameResources &prepared_frame = g_resources.native_scene;
  const auto prepared = std::ranges::find(
      prepared_frame.draws, static_cast<size_t>(draw_ref.family_draw_index),
      &PreparedDraw::source_draw_index);
  if (prepared == prepared_frame.draws.end()) {
    return Venue14DNativeSceneRecordResult::kPreparedDrawMissing;
  }
  if (prepared->vertex_buffer == nullptr || prepared->index_buffer == nullptr ||
      prepared->index_count != draw.title->identity.submitted_index_count ||
      prepared->source_draw_index != draw_ref.family_draw_index ||
      std::ranges::find(prepared->textures, nullptr) !=
          prepared->textures.end()) {
    return Venue14DNativeSceneRecordResult::kPreparedDrawIdentityMismatch;
  }
  if (draw.title->identity.primitive_type != kTriangleStripPrimitive) {
    return Venue14DNativeSceneRecordResult::kUnsupportedPrimitive;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(
      draw.backend.blend_control_0 == kNativeSceneAlphaBlendControl
          ? g_resources.native_scene_blended_pipeline
          : g_resources.native_scene_pipeline);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  cmd->SetConstantBuffer(0, prepared_frame.constant_buffer,
                         prepared->constant_offset);
  cmd->SetBufferSrv(1, prepared->vertex_buffer, 0);
  cmd->SetTextures(2, prepared->textures.data(),
                   static_cast<uint32_t>(prepared->textures.size()));
  cmd->SetIndexBuffer(prepared->index_buffer, 0, prepared->index_bytes);
  if (!cmd->DrawIndexedChecked(prepared->index_count, 0, 0)) {
    return Venue14DNativeSceneRecordResult::kRhiDrawStateRejected;
  }
  return Venue14DNativeSceneRecordResult::kRecorded;
}

const char *
Venue14DNativeSceneRecordResultName(Venue14DNativeSceneRecordResult result) {
  switch (result) {
  case Venue14DNativeSceneRecordResult::kRecorded:
    return "recorded";
  case Venue14DNativeSceneRecordResult::kInvalidTarget:
    return "invalid_target";
  case Venue14DNativeSceneRecordResult::kWrongFamily:
    return "wrong_family";
  case Venue14DNativeSceneRecordResult::kInvalidFrame:
    return "invalid_frame";
  case Venue14DNativeSceneRecordResult::kUnsupportedBackendContract:
    return "unsupported_backend_contract";
  case Venue14DNativeSceneRecordResult::kResourcesNotPrepared:
    return "resources_not_prepared";
  case Venue14DNativeSceneRecordResult::kDrawIndexOutOfRange:
    return "draw_index_out_of_range";
  case Venue14DNativeSceneRecordResult::kOrdinalMismatch:
    return "ordinal_mismatch";
  case Venue14DNativeSceneRecordResult::kPreparedDrawMissing:
    return "prepared_draw_missing";
  case Venue14DNativeSceneRecordResult::kPreparedDrawIdentityMismatch:
    return "prepared_draw_identity_mismatch";
  case Venue14DNativeSceneRecordResult::kUnsupportedPrimitive:
    return "unsupported_primitive";
  case Venue14DNativeSceneRecordResult::kRhiDrawStateRejected:
    return "rhi_draw_state_rejected";
  }
  return "unknown";
}

void ShutdownVenue14DRenderer() { ReleaseResources(); }

} // namespace tabletennis::native
