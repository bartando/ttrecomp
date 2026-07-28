#include "native/tabletennis_venue_e33_renderer.h"

#include "native/shaders/tabletennis_venue_e33_observer_spirv.h"
#include "native/tabletennis_texture_snapshot.h"
#include "native/tabletennis_venue_e33_observer.h"

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
    tabletennis_native_venue_e33_renderer, false, "Table Tennis",
    "Overlay the real, independently verified E33 venue family using its "
    "captured geometry, constants, three textures and ported guest shader. "
    "Observer-only; never suppresses or replaces a guest draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(tabletennis_native_venue_e33_opacity, 0.58,
                      "Table Tennis",
                      "Opacity of the real E33 observer overlay.")
    .range(0.05, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    tabletennis_native_venue_e33_display_gamma, 2.0, "Table Tennis",
    "Display gamma used only by the end-of-frame E33 comparison overlay.")
    .range(1.0, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kTriangleStripPrimitive = 6;
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
  std::array<uint32_t, 4> c254{};
  std::array<uint32_t, 4> c255{};
  std::array<uint32_t, 4> buffer_layout{};
  std::array<uint32_t, 4> observer_options{};
};

static_assert(sizeof(DrawConstants) == 20 * sizeof(uint32_t) * 4);
constexpr uint64_t kConstantStride =
    AlignUp(sizeof(DrawConstants), nrhi::kBufferOffsetAlignment);

struct GpuVertexPayload {
  std::shared_ptr<const VenueE33VertexPayload> snapshot;
  nrhi::Buffer* buffer = nullptr;
};

struct GpuIndexPayload {
  std::shared_ptr<const VenueE33IndexPayload> snapshot;
  nrhi::Buffer* buffer = nullptr;
  uint32_t bytes = 0;
};

struct GpuTexture {
  std::shared_ptr<const TextureSnapshot> snapshot;
  nrhi::Texture* texture = nullptr;
  nrhi::TextureView* view = nullptr;
};

struct PreparedDraw {
  nrhi::Buffer* vertex_buffer = nullptr;
  nrhi::Buffer* index_buffer = nullptr;
  size_t source_draw_index = 0;
  uint32_t index_bytes = 0;
  uint32_t index_count = 0;
  uint64_t constant_offset = 0;
  std::array<nrhi::TextureView*, VenueE33MaterialSnapshot::kTextureCount>
      textures{};
};

struct PreparedFrameResources {
  nrhi::Buffer* constant_buffer = nullptr;
  uint64_t constant_buffer_bytes = 0;
  std::shared_ptr<const VenueE33FrameSnapshot> frame;
  std::vector<PreparedDraw> draws;
};

struct Resources {
  nrhi::Device* device = nullptr;
  nrhi::BindingLayout* layout = nullptr;
  nrhi::Shader* vertex_shader = nullptr;
  nrhi::Shader* pixel_shader = nullptr;
  nrhi::Pipeline* pipeline = nullptr;
  nrhi::Format pipeline_format = nrhi::Format::kUnknown;
  std::vector<GpuVertexPayload> vertices;
  std::vector<GpuIndexPayload> indices;
  std::vector<GpuTexture> textures;
  PreparedFrameResources overlay;
  bool failed = false;
  bool announced_draw = false;
  bool announced_full_mips = false;
  bool announced_sampler_rejection = false;
};

Resources g_resources;

void ReleaseResources() {
  if (g_resources.device != nullptr) {
    g_resources.device->DestroyDeferred(g_resources.pipeline);
    g_resources.device->DestroyDeferred(g_resources.vertex_shader);
    g_resources.device->DestroyDeferred(g_resources.pixel_shader);
    g_resources.device->DestroyDeferred(g_resources.overlay.constant_buffer);
    for (GpuVertexPayload& vertex : g_resources.vertices) {
      g_resources.device->DestroyDeferred(vertex.buffer);
    }
    for (GpuIndexPayload& index : g_resources.indices) {
      g_resources.device->DestroyDeferred(index.buffer);
    }
    for (GpuTexture& texture : g_resources.textures) {
      g_resources.device->DestroyDeferred(texture.view);
      g_resources.device->DestroyDeferred(texture.texture);
    }
  }
  // Binding layouts follow the device lifetime.
  g_resources = {};
}

bool EnsureDevice(
    const rex::graphics::NativeGuestOutputRenderContext& context) {
  if (context.device == nullptr) {
    return false;
  }
  if (g_resources.device != nullptr && g_resources.device != context.device) {
    ReleaseResources();
  }
  g_resources.device = context.device;
  return !g_resources.failed;
}

nrhi::Buffer* CreateAndFillBuffer(nrhi::Device* device, const void* source,
                                  uint64_t size,
                                  nrhi::BufferBindClass bind_class) {
  if (device == nullptr || source == nullptr || size == 0) {
    return nullptr;
  }
  nrhi::BufferDesc desc;
  desc.size = size;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = bind_class;
  nrhi::Buffer* const buffer = device->CreateBuffer(desc);
  if (buffer == nullptr) {
    return nullptr;
  }
  void* const mapped = device->Map(buffer);
  if (mapped == nullptr) {
    device->DestroyDeferred(buffer);
    return nullptr;
  }
  std::memcpy(mapped, source, static_cast<size_t>(size));
  device->Unmap(buffer);
  return buffer;
}

bool ExactVertexLayout(const VenueE33DrawSnapshot& draw) {
  return draw.valid() &&
         draw.backend.vertex_shader_hash == kVenueE33VertexShaderHash &&
         draw.backend.pixel_shader_hash == kVenueE33PixelShaderHash &&
         draw.title->vertices != nullptr &&
         draw.title->vertices->byte_count ==
             draw.title->vertices->vertex_count * kVenueE33VertexStride &&
         draw.backend_identity.guest_vertex_endian == kVenueE33VertexEndian;
}

GpuVertexPayload* EnsureVertex(
    const std::shared_ptr<const VenueE33VertexPayload>& snapshot) {
  if (snapshot == nullptr || !snapshot->valid()) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(g_resources.vertices,
                                          [&](const GpuVertexPayload& vertex) {
                                            return vertex.snapshot == snapshot;
                                          });
  if (found != g_resources.vertices.end()) {
    return &*found;
  }
  if (g_resources.vertices.size() >= kMaximumStaticPayloads) {
    REXLOG_ERROR("Table Tennis E33 observer: vertex cache exhausted");
    return nullptr;
  }
  nrhi::Buffer* const buffer = CreateAndFillBuffer(
      g_resources.device, snapshot->raw_bytes.data(),
      snapshot->raw_bytes.size(), nrhi::BufferBindClass::kFull);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis E33 observer: raw vf95 upload failed");
    return nullptr;
  }
  g_resources.vertices.push_back({snapshot, buffer});
  return &g_resources.vertices.back();
}

GpuIndexPayload* EnsureIndex(
    const std::shared_ptr<const VenueE33IndexPayload>& snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      snapshot->indices.size() >
          std::numeric_limits<uint32_t>::max() / sizeof(uint16_t)) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      g_resources.indices,
      [&](const GpuIndexPayload& index) { return index.snapshot == snapshot; });
  if (found != g_resources.indices.end()) {
    return &*found;
  }
  if (g_resources.indices.size() >= kMaximumStaticPayloads) {
    REXLOG_ERROR("Table Tennis E33 observer: index cache exhausted");
    return nullptr;
  }
  const uint32_t bytes =
      static_cast<uint32_t>(snapshot->indices.size() * sizeof(uint16_t));
  nrhi::Buffer* const buffer =
      CreateAndFillBuffer(g_resources.device, snapshot->indices.data(), bytes,
                          nrhi::BufferBindClass::kVertexIndex);
  if (buffer == nullptr) {
    REXLOG_ERROR("Table Tennis E33 observer: decoded index upload failed");
    return nullptr;
  }
  g_resources.indices.push_back({snapshot, buffer, bytes});
  return &g_resources.indices.back();
}

nrhi::Format HostTextureFormat(const TextureSnapshot& snapshot) {
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

bool ExactSamplerFetch(const std::array<uint32_t, 6>& fetch_words,
                       const TextureSnapshot& texture, uint32_t slot) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.dword_0 = fetch_words[0];
  fetch.dword_1 = fetch_words[1];
  fetch.dword_2 = fetch_words[2];
  fetch.dword_3 = fetch_words[3];
  fetch.dword_4 = fetch_words[4];
  fetch.dword_5 = fetch_words[5];

  const bool cube = slot == 2;
  const xenos::ClampMode expected_clamp =
      cube ? xenos::ClampMode::kClampToEdge : xenos::ClampMode::kRepeat;
  const xenos::DataDimension expected_dimension =
      cube ? xenos::DataDimension::kCube : xenos::DataDimension::k2DOrStacked;
  const xenos::TextureFormat base_format =
      rex::graphics::GetBaseFormat(fetch.format);
  const bool exact_descriptor_mip_range =
      fetch.mip_min_level == 0 &&
      texture.mips.size() ==
          static_cast<size_t>(texture.descriptor_mip_max_level) + 1;
  const bool exact_shape =
      cube ? base_format == xenos::TextureFormat::k_DXT4_5 &&
                 texture.width == 256 && texture.height == 256 &&
                 texture.layer_count == 6 && exact_descriptor_mip_range
           : base_format == xenos::TextureFormat::k_DXT1 &&
                 texture.width == 128 && texture.height == 128 &&
                 texture.layer_count == 1 && exact_descriptor_mip_range;

  // The plain tfetch instructions in the verified E33 shader have no
  // instruction-level filter, LOD, gradient or bias overrides, so these are
  // the effective fetch-constant states. RexGlue's guest sampler path forces
  // min/mag/mip linear when anisotropy is enabled; the two immutable NRHI
  // samplers below intentionally reproduce that normalized backend behavior.
  // Compression is descriptor-owned. The immutable snapshot carries the exact
  // same six fetch words and HostTextureFormat validates the matching resource.
  return fetch.type == xenos::FetchConstantType::kTexture && exact_shape &&
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

GpuTexture* EnsureTexture(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const TextureSnapshot>& snapshot) {
  if (snapshot == nullptr || !snapshot->valid() ||
      !snapshot->full_mip_chain() ||
      (snapshot->layer_count != 1 && snapshot->layer_count != 6)) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      g_resources.textures,
      [&](const GpuTexture& texture) { return texture.snapshot == snapshot; });
  if (found != g_resources.textures.end()) {
    return &*found;
  }
  if (g_resources.textures.size() >= kMaximumTextures) {
    REXLOG_ERROR("Table Tennis E33 observer: texture cache exhausted");
    return nullptr;
  }

  const nrhi::Format format = HostTextureFormat(*snapshot);
  if (format == nrhi::Format::kUnknown) {
    REXLOG_ERROR("Table Tennis E33 observer: unsupported texture format {}",
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
  // All E33 fetches are block-aligned powers of two. Failing closed here
  // prevents the host resource's mip dimensions from silently diverging from
  // a non-block-aligned guest descriptor.
  if (host_width != snapshot->width || host_height != snapshot->height ||
      snapshot->mips.size() > std::numeric_limits<uint32_t>::max()) {
    return nullptr;
  }

  struct UploadRegion {
    const TextureMipSnapshot* mip = nullptr;
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
  for (const TextureMipSnapshot& mip : snapshot->mips) {
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
  nrhi::Texture* const texture = context.device->CreateTexture(texture_desc);
  nrhi::BufferDesc upload_desc;
  upload_desc.size = upload_bytes;
  upload_desc.heap = nrhi::HeapKind::kUpload;
  upload_desc.bind_class = nrhi::BufferBindClass::kCopySrc;
  nrhi::Buffer* const upload = context.device->CreateBuffer(upload_desc);
  if (texture == nullptr || upload == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR("Table Tennis E33 observer: texture resource creation failed");
    return nullptr;
  }

  uint8_t* const mapped = static_cast<uint8_t*>(context.device->Map(upload));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  std::memset(mapped, 0, static_cast<size_t>(upload_bytes));
  for (const UploadRegion& region : upload_regions) {
    const TextureMipSnapshot& mip = *region.mip;
    const uint8_t* const source = snapshot->linear_blocks.data() +
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
  nrhi::TextureView* const view =
      context.device->CreateTextureView(texture, view_desc);
  if (view == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    return nullptr;
  }
  for (const UploadRegion& region : upload_regions) {
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
    const rex::graphics::NativeGuestOutputRenderContext& context) {
  if (!EnsureDevice(context) || context.guest_output == nullptr) {
    return false;
  }
  nrhi::Device* const device = context.device;
  if (g_resources.layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 3;
    layout.params[0] = {nrhi::BindingParamKind::kConstantBuffer, 0, 1,
                        nrhi::Visibility::kAll};
    layout.params[1] = {nrhi::BindingParamKind::kBufferSrv, 0, 1,
                        nrhi::Visibility::kVertex};
    layout.params[2] = {nrhi::BindingParamKind::kTextureTable, 1,
                        VenueE33MaterialSnapshot::kTextureCount,
                        nrhi::Visibility::kPixel};
    layout.static_sampler_count = 2;
    layout.static_samplers[0] = {0, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kWrap, 2};
    layout.static_samplers[1] = {1, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kClamp, 2};
    layout.allow_input_layout = false;
    g_resources.layout = device->CreateBindingLayout(layout);
    if (g_resources.layout == nullptr) {
      REXLOG_ERROR("Table Tennis E33 observer: binding layout creation failed");
      g_resources.failed = true;
      return false;
    }
  }
  if (g_resources.vertex_shader == nullptr ||
      g_resources.pixel_shader == nullptr) {
    nrhi::ShaderDesc vertex_desc;
    vertex_desc.stage = nrhi::ShaderStage::kVertex;
    vertex_desc.name = "tabletennis_venue_e33_observer.hlsl";
    vertex_desc.hlsl_source = venue_e33_observer_shader::kHlsl;
    vertex_desc.entry_point = "vs_main";
    vertex_desc.spirv = venue_e33_observer_shader::kVertexSpirv;
    vertex_desc.spirv_size_bytes = venue_e33_observer_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc pixel_desc;
    pixel_desc.stage = nrhi::ShaderStage::kPixel;
    pixel_desc.name = "tabletennis_venue_e33_observer.hlsl";
    pixel_desc.hlsl_source = venue_e33_observer_shader::kHlsl;
    pixel_desc.entry_point = "ps_main";
    pixel_desc.spirv = venue_e33_observer_shader::kPixelSpirv;
    pixel_desc.spirv_size_bytes = venue_e33_observer_shader::kPixelSpirvBytes;
    g_resources.vertex_shader = device->CreateShader(vertex_desc);
    g_resources.pixel_shader = device->CreateShader(pixel_desc);
    if (g_resources.vertex_shader == nullptr ||
        g_resources.pixel_shader == nullptr) {
      REXLOG_ERROR("Table Tennis E33 observer: shader creation failed");
      g_resources.failed = true;
      return false;
    }
  }
  return true;
}

bool EnsurePipeline(
    const rex::graphics::NativeGuestOutputRenderContext& context) {
  if (!EnsurePipelineResources(context)) {
    return false;
  }
  nrhi::Device* const device = context.device;
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
        "Table Tennis E33 observer: graphics pipeline creation failed");
    g_resources.failed = true;
    return false;
  }
  g_resources.pipeline_format = output_format;
  return true;
}

void CopyConstant(
    const std::array<uint32_t, VenueE33MaterialSnapshot::kConstantWordCount>&
        source,
    size_t register_index, std::array<uint32_t, 4>& destination) {
  std::copy_n(source.begin() + register_index * 4, 4, destination.begin());
}

DrawConstants BuildConstants(const VenueE33DrawSnapshot& draw, float opacity,
                             float reciprocal_display_gamma) {
  DrawConstants constants;
  const auto& vertex = draw.title->material.vertex_constant_words;
  const auto& pixel = draw.title->material.pixel_constant_words;
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
  CopyConstant(pixel, 254, constants.c254);
  CopyConstant(pixel, 255, constants.c255);
  constants.buffer_layout[0] = kVenueE33VertexStride;
  constants.observer_options[0] = std::bit_cast<uint32_t>(opacity);
  constants.observer_options[1] =
      std::bit_cast<uint32_t>(reciprocal_display_gamma);
  return constants;
}

bool PreparedFrameMatches(
    const std::shared_ptr<const VenueE33FrameSnapshot>& frame,
    const PreparedFrameResources& prepared) {
  if (frame == nullptr || prepared.frame != frame ||
      prepared.constant_buffer == nullptr ||
      prepared.constant_buffer_bytes != kConstantStride * frame->draws.size() ||
      prepared.draws.size() != frame->draws.size()) {
    return false;
  }
  for (size_t index = 0; index < prepared.draws.size(); ++index) {
    const PreparedDraw& prepared_draw = prepared.draws[index];
    const VenueE33DrawSnapshot& draw = frame->draws[index];
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
    const rex::graphics::NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const VenueE33FrameSnapshot>& frame, float opacity,
    float reciprocal_display_gamma, PreparedFrameResources& destination) {
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
  nrhi::Buffer* const constant_buffer =
      context.device->CreateBuffer(constant_desc);
  if (constant_buffer == nullptr) {
    return false;
  }
  uint8_t* const mapped =
      static_cast<uint8_t*>(context.device->Map(constant_buffer));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(constant_buffer);
    return false;
  }
  std::memset(mapped, 0, static_cast<size_t>(constant_bytes));

  std::vector<PreparedDraw> prepared;
  prepared.reserve(frame->draws.size());
  bool valid = true;
  for (size_t index = 0; index < frame->draws.size(); ++index) {
    const VenueE33DrawSnapshot& draw = frame->draws[index];
    if (!ExactVertexLayout(draw) || draw.title->indices == nullptr ||
        draw.title->vertices == nullptr ||
        std::ranges::any_of(draw.title->indices->indices,
                            [&](uint16_t vertex_index) {
                              return vertex_index >=
                                     draw.title->vertices->vertex_count;
                            }) ||
        draw.title->identity.primitive_type != kTriangleStripPrimitive) {
      valid = false;
      break;
    }
    GpuVertexPayload* const vertex = EnsureVertex(draw.title->vertices);
    GpuIndexPayload* const indices = EnsureIndex(draw.title->indices);
    PreparedDraw prepared_draw;
    prepared_draw.vertex_buffer = vertex != nullptr ? vertex->buffer : nullptr;
    prepared_draw.index_buffer = indices != nullptr ? indices->buffer : nullptr;
    prepared_draw.source_draw_index = index;
    prepared_draw.index_bytes = indices != nullptr ? indices->bytes : 0;
    prepared_draw.index_count = draw.title->identity.submitted_index_count;
    prepared_draw.constant_offset = index * kConstantStride;
    for (size_t slot = 0; slot < prepared_draw.textures.size(); ++slot) {
      const auto& texture_snapshot = draw.title->material.textures[slot];
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
              "Table Tennis E33 observer: rejected unproven sampler state "
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
      GpuTexture* const texture = EnsureTexture(context, texture_snapshot);
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
    if (draw.title->material.textures[2]->layer_count != 6) {
      valid = false;
      break;
    }
    for (size_t slot = 0; slot < 2; ++slot) {
      if (draw.title->material.textures[slot]->layer_count != 1) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      break;
    }
    const DrawConstants constants =
        BuildConstants(draw, opacity, reciprocal_display_gamma);
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
    REXLOG_INFO(
        "Table Tennis E33 observer: prepared {} exact ordered draws; "
        "three complete descriptor-selected texture mip chains per "
        "draw with proven repeat/clamp sampler states (observer-only)",
        frame->draws.size());
  }
  return true;
}

bool EnsurePreparedFrame(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const VenueE33FrameSnapshot>& frame) {
  const bool frame_changed = g_resources.overlay.frame != frame;
  const bool prepared =
      EnsurePipeline(context) &&
      EnsurePreparedFrameData(
          context, frame,
          static_cast<float>(REXCVAR_GET(tabletennis_native_venue_e33_opacity)),
          1.0f / static_cast<float>(
                     REXCVAR_GET(tabletennis_native_venue_e33_display_gamma)),
          g_resources.overlay);
  if (prepared && frame_changed) {
    g_resources.announced_draw = false;
  }
  return prepared;
}

}  // namespace

bool VenueE33RendererEnabled() {
  return REXCVAR_GET(tabletennis_native_venue_e33_renderer);
}

bool PrepareVenueE33Observer(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const VenueE33FrameSnapshot>& frame) {
  return VenueE33RendererEnabled() && EnsurePreparedFrame(context, frame);
}

uint32_t RenderVenueE33Observer(
    const rex::graphics::NativeGuestOutputRenderContext& context,
    const std::shared_ptr<const VenueE33FrameSnapshot>& frame) {
  if (!VenueE33RendererEnabled() || !EnsurePreparedFrame(context, frame) ||
      !PreparedFrameMatches(frame, g_resources.overlay)) {
    return 0;
  }

  const PreparedFrameResources& prepared = g_resources.overlay;
  nrhi::Cmd* const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.pipeline);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  uint32_t draw_count = 0;
  for (const PreparedDraw& draw : prepared.draws) {
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
    REXLOG_INFO(
        "Table Tennis E33 observer: drew {} real title meshes in "
        "backend-verified order with the ported three-texture material "
        "(guest frame untouched, no replacement)",
        draw_count);
  }
  return draw_count;
}

void ShutdownVenueE33Renderer() { ReleaseResources(); }

}  // namespace tabletennis::native
