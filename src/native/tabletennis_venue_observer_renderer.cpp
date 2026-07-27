#include "native/tabletennis_venue_observer_renderer.h"

#include "native/shaders/tabletennis_venue_observer_spirv.h"
#include "native/tabletennis_native_scene_compositor.h"
#include "native/tabletennis_native_scene_pass.h"
#include "native/tabletennis_texture_snapshot.h"
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

REXCVAR_DEFINE_DOUBLE(
    tabletennis_native_venue_observer_opacity, 0.72, "Table Tennis",
    "Opacity of the exact captured venue material observer overlay.")
    .range(0.05, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    tabletennis_native_venue_observer_display_gamma, 2.0, "Table Tennis",
    "Display gamma used only by the end-of-frame venue comparison overlay.")
    .range(1.0, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(tabletennis_native_venue_observer_checkerboard, false,
                    "Table Tennis",
                    "Show alternating native and untouched guest tiles for "
                    "material comparison.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;
namespace xenos = rex::graphics::xenos;

struct VenueVertex {
  std::array<float, 3> position;
  std::array<float, 2> texcoord0;
  std::array<float, 2> texcoord1;
  std::array<float, 4> color;
};

static_assert(sizeof(VenueVertex) == sizeof(float) * 11);

constexpr std::array<nrhi::InputElementDesc, 4> kVenueVertexInputs = {{
    {"POSITION", 0, 0, nrhi::Format::kR32G32B32_FLOAT,
     offsetof(VenueVertex, position)},
    {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT,
     offsetof(VenueVertex, texcoord0)},
    {"TEXCOORD", 1, 2, nrhi::Format::kR32G32_FLOAT,
     offsetof(VenueVertex, texcoord1)},
    {"COLOR", 0, 3, nrhi::Format::kR32G32B32A32_FLOAT,
     offsetof(VenueVertex, color)},
}};

struct DrawRange {
  uint32_t start_index = 0;
  int32_t base_vertex = 0;
  uint32_t index_count = 0;
  uint32_t primitive_type = 0;
};

struct GpuTexture {
  std::shared_ptr<const TextureSnapshot> snapshot;
  nrhi::Texture *texture = nullptr;
  nrhi::TextureView *view = nullptr;
};

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::BindingLayout *layout = nullptr;
  nrhi::Shader *vertex_shader = nullptr;
  nrhi::Shader *observer_pixel_shader = nullptr;
  nrhi::Shader *replacement_pixel_shader = nullptr;
  nrhi::Pipeline *observer_pipeline = nullptr;
  nrhi::Pipeline *replacement_pipeline = nullptr;
  nrhi::Pipeline *native_scene_pipeline = nullptr;
  nrhi::Format observer_pipeline_format = nrhi::Format::kUnknown;
  nrhi::Format replacement_pipeline_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_color_format = nrhi::Format::kUnknown;
  nrhi::Format native_scene_depth_format = nrhi::Format::kUnknown;
  uint32_t native_scene_sample_count = 0;
  nrhi::Buffer *vertex_buffer = nullptr;
  nrhi::Buffer *index_buffer = nullptr;
  uint32_t vertex_bytes = 0;
  uint32_t index_bytes = 0;
  std::vector<std::shared_ptr<const VenueMeshSnapshot>> uploaded_meshes;
  std::vector<DrawRange> draw_ranges;
  std::vector<GpuTexture> textures;
  std::shared_ptr<const VenueFullFamilyFrame> native_scene_frame;
  bool failed = false;
  bool announced_draw = false;
  bool announced_replacement = false;
};

Resources g_resources;
Resources g_full_family_resources;
std::shared_ptr<const VenueReplacementCandidate> g_pending_candidate;
std::array<uint64_t, 7> g_last_matched_candidate_generations{};

constexpr uint64_t kVenueVertexShaderHash = 0x0E9982BE6B1E99A1ull;
constexpr uint64_t kVenuePixelShaderHash = 0x328FA02B07C392DCull;
constexpr std::array<uint32_t, 7> kVenueTracePrefix = {64,  51, 72, 22,
                                                       226, 94, 694};
constexpr uint64_t kReplacementTelemetryPeriodFrames = 120;

enum class ReplacementReject : size_t {
  kCandidateUnavailable,
  kStaleGeneration,
  kCandidateIdentity,
  kResources,
  kGeometry,
  kRender,
  kCount,
};

struct ReplacementTelemetry {
  uint32_t replacement_count = 0;
  uint64_t trace_frames = 0;
  uint64_t selected = 0;
  uint64_t matched = 0;
  uint64_t served = 0;
  std::array<uint64_t, static_cast<size_t>(ReplacementReject::kCount)>
      rejects{};
  uint32_t matched_mask = 0;
  uint32_t served_mask = 0;
  uint32_t lifetime_served_mask = 0;
  bool announced_coverage = false;
};

ReplacementTelemetry g_replacement_telemetry;

uint32_t ReplacementExpectedMask(uint32_t replacement_count) {
  const uint32_t selected_count =
      std::min<uint32_t>(replacement_count, kVenueTracePrefix.size());
  const uint32_t first_index =
      static_cast<uint32_t>(kVenueTracePrefix.size()) - selected_count;
  return 0x7Fu & ~((1u << first_index) - 1u);
}

void ResetReplacementTelemetry(uint32_t replacement_count) {
  g_replacement_telemetry = {};
  g_replacement_telemetry.replacement_count = replacement_count;
}

void LogReplacementTelemetry() {
  ReplacementTelemetry &telemetry = g_replacement_telemetry;
  const uint64_t fallback = telemetry.selected >= telemetry.served
                                ? telemetry.selected - telemetry.served
                                : 0;
  const uint64_t render_rejects =
      telemetry.rejects[static_cast<size_t>(ReplacementReject::kRender)];
  const uint64_t matched_not_served = telemetry.matched >= telemetry.served
                                          ? telemetry.matched - telemetry.served
                                          : 0;
  const uint64_t scope_fallbacks = matched_not_served >= render_rejects
                                       ? matched_not_served - render_rejects
                                       : 0;
  REXLOG_INFO(
      "Table Tennis venue replacement telemetry: frames={} selected={} "
      "matched={} served={} fallback={} masks(matched={:02X},served={:02X}) "
      "rejects(candidate={},stale={},identity={},resources={},geometry={},"
      "render={},scope={})",
      telemetry.trace_frames, telemetry.selected, telemetry.matched,
      telemetry.served, fallback, telemetry.matched_mask, telemetry.served_mask,
      telemetry.rejects[static_cast<size_t>(
          ReplacementReject::kCandidateUnavailable)],
      telemetry
          .rejects[static_cast<size_t>(ReplacementReject::kStaleGeneration)],
      telemetry
          .rejects[static_cast<size_t>(ReplacementReject::kCandidateIdentity)],
      telemetry.rejects[static_cast<size_t>(ReplacementReject::kResources)],
      telemetry.rejects[static_cast<size_t>(ReplacementReject::kGeometry)],
      render_rejects, scope_fallbacks);

  const uint32_t replacement_count = telemetry.replacement_count;
  const uint32_t lifetime_served_mask = telemetry.lifetime_served_mask;
  const bool announced_coverage = telemetry.announced_coverage;
  telemetry = {};
  telemetry.replacement_count = replacement_count;
  telemetry.lifetime_served_mask = lifetime_served_mask;
  telemetry.announced_coverage = announced_coverage;
}

void BeginReplacementTraceFrame(uint32_t replacement_count,
                                size_t prefix_index) {
  if (g_replacement_telemetry.replacement_count != replacement_count) {
    ResetReplacementTelemetry(replacement_count);
  }
  if (prefix_index != 0) {
    return;
  }
  if (g_replacement_telemetry.trace_frames >=
      kReplacementTelemetryPeriodFrames) {
    LogReplacementTelemetry();
  }
  ++g_replacement_telemetry.trace_frames;
}

void RecordReplacementReject(ReplacementReject reason) {
  ++g_replacement_telemetry.rejects[static_cast<size_t>(reason)];
}

void RecordReplacementServed(size_t prefix_index) {
  ReplacementTelemetry &telemetry = g_replacement_telemetry;
  ++telemetry.served;
  telemetry.served_mask |= 1u << prefix_index;
  telemetry.lifetime_served_mask |= 1u << prefix_index;
  const uint32_t expected_mask =
      ReplacementExpectedMask(telemetry.replacement_count);
  if (!telemetry.announced_coverage && expected_mask != 0 &&
      (telemetry.lifetime_served_mask & expected_mask) == expected_mask) {
    telemetry.announced_coverage = true;
    REXLOG_INFO("Table Tennis venue replacement: confirmed all {} configured "
                "trace indices served (mask={:02X})",
                telemetry.replacement_count, expected_mask);
  }
}

void ReleaseResourceSet(Resources &resources) {
  if (resources.device != nullptr) {
    resources.device->DestroyDeferred(resources.observer_pipeline);
    resources.device->DestroyDeferred(resources.replacement_pipeline);
    resources.device->DestroyDeferred(resources.native_scene_pipeline);
    resources.device->DestroyDeferred(resources.vertex_shader);
    resources.device->DestroyDeferred(resources.observer_pixel_shader);
    resources.device->DestroyDeferred(resources.replacement_pixel_shader);
    resources.device->DestroyDeferred(resources.vertex_buffer);
    resources.device->DestroyDeferred(resources.index_buffer);
    for (GpuTexture &texture : resources.textures) {
      resources.device->DestroyDeferred(texture.view);
      resources.device->DestroyDeferred(texture.texture);
    }
  }
  // Binding layouts follow the device lifetime.
  resources = {};
}

void ReleaseResources() {
  ReleaseResourceSet(g_resources);
  ReleaseResourceSet(g_full_family_resources);
  g_pending_candidate.reset();
  g_last_matched_candidate_generations = {};
  g_replacement_telemetry = {};
}

bool EnsureDevice(
    Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (context.device == nullptr) {
    return false;
  }
  if (resources.device != nullptr && resources.device != context.device) {
    if (&resources == &g_resources) {
      ReleaseResources();
    } else {
      ReleaseResourceSet(resources);
    }
  }
  resources.device = context.device;
  return !resources.failed;
}

bool EnsurePipeline(
    Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    bool replacement) {
  if (!EnsureDevice(resources, context) || context.guest_output == nullptr) {
    return false;
  }
  nrhi::Device *const device = context.device;
  if (resources.layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 2;
    layout.params[0] = {nrhi::BindingParamKind::kConstants, 0, 20,
                        nrhi::Visibility::kAll};
    layout.params[1] = {nrhi::BindingParamKind::kTextureTable, 0, 2,
                        nrhi::Visibility::kPixel};
    layout.static_sampler_count = 2;
    layout.static_samplers[0] = {0, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kWrap, 2};
    layout.static_samplers[1] = {1, nrhi::Filter::kAnisotropic,
                                 nrhi::AddressMode::kWrap, 2};
    layout.allow_input_layout = true;
    resources.layout = device->CreateBindingLayout(layout);
    if (resources.layout == nullptr) {
      REXLOG_ERROR(
          "Table Tennis venue observer: binding layout creation failed");
      resources.failed = true;
      return false;
    }
  }

  if (resources.vertex_shader == nullptr ||
      resources.observer_pixel_shader == nullptr ||
      resources.replacement_pixel_shader == nullptr) {
    nrhi::ShaderDesc vertex_desc;
    vertex_desc.stage = nrhi::ShaderStage::kVertex;
    vertex_desc.name = "tabletennis_venue_observer.hlsl";
    vertex_desc.hlsl_source = venue_observer_shader::kHlsl;
    vertex_desc.entry_point = "vs_main";
    vertex_desc.spirv = venue_observer_shader::kVertexSpirv;
    vertex_desc.spirv_size_bytes = venue_observer_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc observer_pixel_desc;
    observer_pixel_desc.stage = nrhi::ShaderStage::kPixel;
    observer_pixel_desc.name = "tabletennis_venue_observer.hlsl";
    observer_pixel_desc.hlsl_source = venue_observer_shader::kHlsl;
    observer_pixel_desc.entry_point = "ps_main";
    observer_pixel_desc.spirv = venue_observer_shader::kPixelSpirv;
    observer_pixel_desc.spirv_size_bytes =
        venue_observer_shader::kPixelSpirvBytes;
    nrhi::ShaderDesc replacement_pixel_desc = observer_pixel_desc;
    replacement_pixel_desc.entry_point = "ps_replace";
    replacement_pixel_desc.spirv =
        venue_observer_shader::kReplacementPixelSpirv;
    replacement_pixel_desc.spirv_size_bytes =
        venue_observer_shader::kReplacementPixelSpirvBytes;
    resources.vertex_shader = device->CreateShader(vertex_desc);
    resources.observer_pixel_shader = device->CreateShader(observer_pixel_desc);
    resources.replacement_pixel_shader =
        device->CreateShader(replacement_pixel_desc);
    if (resources.vertex_shader == nullptr ||
        resources.observer_pixel_shader == nullptr ||
        resources.replacement_pixel_shader == nullptr) {
      REXLOG_ERROR("Table Tennis venue observer: shader creation failed");
      resources.failed = true;
      return false;
    }
  }

  const nrhi::Format output_format = context.guest_output->format();
  nrhi::Pipeline *&target_pipeline = replacement
                                         ? resources.replacement_pipeline
                                         : resources.observer_pipeline;
  nrhi::Format &target_format = replacement
                                    ? resources.replacement_pipeline_format
                                    : resources.observer_pipeline_format;
  if (target_pipeline != nullptr && target_format == output_format) {
    return true;
  }
  device->DestroyDeferred(target_pipeline);
  target_pipeline = nullptr;

  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = resources.layout;
  pipeline.vs = resources.vertex_shader;
  pipeline.ps = replacement ? resources.replacement_pixel_shader
                            : resources.observer_pixel_shader;
  pipeline.input_elements = kVenueVertexInputs.data();
  pipeline.input_element_count =
      static_cast<uint32_t>(kVenueVertexInputs.size());
  pipeline.vertex_stride = sizeof(VenueVertex);
  pipeline.cull = replacement ? nrhi::CullMode::kBack : nrhi::CullMode::kNone;
  pipeline.depth_clip = true;
  pipeline.depth.test_enable = replacement;
  pipeline.depth.write_enable = replacement;
  pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
  pipeline.blend.enable = !replacement;
  pipeline.blend.write_mask = replacement ? 0x7 : 0xF;
  if (!replacement) {
    pipeline.blend.src = nrhi::BlendFactor::kSrcAlpha;
    pipeline.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    pipeline.blend.op = nrhi::BlendOp::kAdd;
    pipeline.blend.src_alpha = nrhi::BlendFactor::kOne;
    pipeline.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    pipeline.blend.op_alpha = nrhi::BlendOp::kAdd;
  }
  pipeline.rtv_format = output_format;
  pipeline.sample_count = 1;
  target_pipeline = device->CreateGraphicsPipeline(pipeline);
  if (target_pipeline == nullptr) {
    REXLOG_ERROR("Table Tennis venue {} pipeline creation failed",
                 replacement ? "replacement" : "observer");
    resources.failed = true;
    return false;
  }
  target_format = output_format;
  return true;
}

bool EnsureNativeScenePipeline(
    Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      !EnsurePipeline(resources, context, false)) {
    return false;
  }

  const nrhi::Format color_format = targets.color->format();
  const nrhi::Format depth_format = targets.depth->format();
  if (resources.native_scene_pipeline != nullptr &&
      resources.native_scene_color_format == color_format &&
      resources.native_scene_depth_format == depth_format &&
      resources.native_scene_sample_count == targets.sample_count) {
    return true;
  }
  context.device->DestroyDeferred(resources.native_scene_pipeline);
  resources.native_scene_pipeline = nullptr;

  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = resources.layout;
  pipeline.vs = resources.vertex_shader;
  pipeline.ps = resources.replacement_pixel_shader;
  pipeline.input_elements = kVenueVertexInputs.data();
  pipeline.input_element_count =
      static_cast<uint32_t>(kVenueVertexInputs.size());
  pipeline.vertex_stride = sizeof(VenueVertex);
  pipeline.cull = nrhi::CullMode::kBack;
  pipeline.depth_clip = true;
  pipeline.depth.test_enable = true;
  pipeline.depth.write_enable = true;
  pipeline.depth.func = nrhi::CompareFunc::kLessEqual;
  pipeline.blend.enable = false;
  // PS328's verified in-order path writes scene RGB and leaves the output
  // alpha channel alone.
  pipeline.blend.write_mask = 0x7;
  pipeline.rtv_format = color_format;
  pipeline.dsv_format = depth_format;
  pipeline.sample_count = targets.sample_count;
  resources.native_scene_pipeline =
      context.device->CreateGraphicsPipeline(pipeline);
  if (resources.native_scene_pipeline == nullptr) {
    REXLOG_ERROR(
        "Table Tennis PS328 native scene: shared-pass pipeline creation "
        "failed");
    return false;
  }
  resources.native_scene_color_format = color_format;
  resources.native_scene_depth_format = depth_format;
  resources.native_scene_sample_count = targets.sample_count;
  return true;
}

nrhi::Buffer *CreateGeometryBuffer(nrhi::Device *device, size_t size) {
  nrhi::BufferDesc description;
  description.size = size;
  description.heap = nrhi::HeapKind::kUpload;
  description.bind_class = nrhi::BufferBindClass::kVertexIndex;
  return device->CreateBuffer(description);
}

const VenueDrawSnapshot &VenueDrawAt(const VenueFrameSnapshot &frame,
                                     size_t index) {
  return frame.draws[index];
}

const VenueDrawSnapshot &VenueDrawAt(const VenueFullFamilyFrame &frame,
                                     size_t index) {
  return frame.draws[index].captured;
}

template <typename Frame>
bool SameGeometry(const Resources &resources, const Frame &frame) {
  if (frame.draws.size() != resources.uploaded_meshes.size()) {
    return false;
  }
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    if (VenueDrawAt(frame, index).mesh != resources.uploaded_meshes[index]) {
      return false;
    }
  }
  return true;
}

template <typename Frame>
bool EnsureGeometry(
    Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const Frame &frame) {
  if (resources.vertex_buffer != nullptr && resources.index_buffer != nullptr &&
      SameGeometry(resources, frame)) {
    return true;
  }

  size_t vertex_count = 0;
  size_t index_count = 0;
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    const VenueDrawSnapshot &draw = VenueDrawAt(frame, index);
    if (draw.mesh == nullptr || !draw.mesh->valid()) {
      return false;
    }
    vertex_count += draw.mesh->positions.size();
    index_count += draw.mesh->indices.size();
  }
  if (vertex_count == 0 || index_count == 0 ||
      vertex_count >
          std::numeric_limits<uint32_t>::max() / sizeof(VenueVertex) ||
      index_count > std::numeric_limits<uint32_t>::max() / sizeof(uint16_t) ||
      vertex_count > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return false;
  }

  const size_t vertex_bytes = vertex_count * sizeof(VenueVertex);
  const size_t index_bytes = index_count * sizeof(uint16_t);
  nrhi::Buffer *const vertex_buffer =
      CreateGeometryBuffer(context.device, vertex_bytes);
  nrhi::Buffer *const index_buffer =
      CreateGeometryBuffer(context.device, index_bytes);
  if (vertex_buffer == nullptr || index_buffer == nullptr) {
    context.device->DestroyDeferred(vertex_buffer);
    context.device->DestroyDeferred(index_buffer);
    REXLOG_ERROR("Table Tennis venue observer: buffer creation failed");
    return false;
  }

  auto *const vertices =
      static_cast<VenueVertex *>(context.device->Map(vertex_buffer));
  auto *const indices =
      static_cast<uint16_t *>(context.device->Map(index_buffer));
  if (vertices == nullptr || indices == nullptr) {
    if (vertices != nullptr) {
      context.device->Unmap(vertex_buffer);
    }
    if (indices != nullptr) {
      context.device->Unmap(index_buffer);
    }
    context.device->DestroyDeferred(vertex_buffer);
    context.device->DestroyDeferred(index_buffer);
    REXLOG_ERROR("Table Tennis venue observer: buffer map failed");
    return false;
  }

  size_t vertex_offset = 0;
  size_t index_offset = 0;
  std::vector<DrawRange> ranges;
  std::vector<std::shared_ptr<const VenueMeshSnapshot>> meshes;
  ranges.reserve(frame.draws.size());
  meshes.reserve(frame.draws.size());
  for (size_t index = 0; index < frame.draws.size(); ++index) {
    const VenueDrawSnapshot &draw = VenueDrawAt(frame, index);
    const VenueMeshSnapshot &mesh = *draw.mesh;
    for (size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
      vertices[vertex_offset + vertex] = {
          .position = mesh.positions[vertex],
          .texcoord0 = mesh.texcoords0[vertex],
          .texcoord1 = mesh.texcoords1[vertex],
          .color = mesh.colors[vertex],
      };
    }
    std::memcpy(indices + index_offset, mesh.indices.data(),
                mesh.indices.size() * sizeof(uint16_t));
    ranges.push_back({
        .start_index = static_cast<uint32_t>(index_offset),
        .base_vertex = static_cast<int32_t>(vertex_offset),
        .index_count = static_cast<uint32_t>(mesh.indices.size()),
        .primitive_type = mesh.primitive_type,
    });
    meshes.push_back(draw.mesh);
    vertex_offset += mesh.positions.size();
    index_offset += mesh.indices.size();
  }
  context.device->Unmap(vertex_buffer);
  context.device->Unmap(index_buffer);

  context.device->DestroyDeferred(resources.vertex_buffer);
  context.device->DestroyDeferred(resources.index_buffer);
  resources.vertex_buffer = vertex_buffer;
  resources.index_buffer = index_buffer;
  resources.vertex_bytes = static_cast<uint32_t>(vertex_bytes);
  resources.index_bytes = static_cast<uint32_t>(index_bytes);
  resources.uploaded_meshes = std::move(meshes);
  resources.draw_ranges = std::move(ranges);
  resources.announced_draw = false;
  REXLOG_INFO("Table Tennis venue observer: uploaded exact vertex payload "
              "meshes={} vertices={} indices={}",
              frame.draws.size(), vertex_count, index_count);
  return true;
}

nrhi::Format HostTextureFormat(const TextureSnapshot &texture) {
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

void ComposeTextureSwizzle(uint32_t fetch_swizzle, nrhi::Swizzle output[4]) {
  for (uint32_t channel = 0; channel < 4; ++channel) {
    output[channel] =
        static_cast<nrhi::Swizzle>((fetch_swizzle >> (channel * 3)) & 7u);
  }
}

nrhi::TextureView *
EnsureTexture(Resources &resources,
              const rex::graphics::NativeGuestOutputRenderContext &context,
              const std::shared_ptr<const TextureSnapshot> &snapshot) {
  if (snapshot == nullptr || !snapshot->valid()) {
    return nullptr;
  }
  const auto found = std::find_if(
      resources.textures.begin(), resources.textures.end(),
      [&](const GpuTexture &texture) { return texture.snapshot == snapshot; });
  if (found != resources.textures.end()) {
    return found->view;
  }

  const nrhi::Format format = HostTextureFormat(*snapshot);
  if (format == nrhi::Format::kUnknown) {
    REXLOG_ERROR("Table Tennis venue observer: unsupported texture format {}",
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
  const uint32_t upload_row_pitch =
      (snapshot->row_pitch_bytes + nrhi::kRowPitchAlignment - 1) &
      ~(nrhi::kRowPitchAlignment - 1);
  const size_t upload_size = static_cast<size_t>(upload_row_pitch) * block_rows;

  nrhi::TextureDesc texture_desc;
  texture_desc.width = host_width;
  texture_desc.height = host_height;
  texture_desc.format = format;
  texture_desc.initial_state = nrhi::ResourceState::kCopyDest;
  nrhi::Texture *const texture = context.device->CreateTexture(texture_desc);
  nrhi::BufferDesc upload_desc;
  upload_desc.size = upload_size;
  upload_desc.heap = nrhi::HeapKind::kUpload;
  upload_desc.bind_class = nrhi::BufferBindClass::kCopySrc;
  nrhi::Buffer *const upload = context.device->CreateBuffer(upload_desc);
  if (texture == nullptr || upload == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR(
        "Table Tennis venue observer: texture resource creation failed");
    return nullptr;
  }

  uint8_t *const mapped = static_cast<uint8_t *>(context.device->Map(upload));
  if (mapped == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR("Table Tennis venue observer: texture map failed");
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
  ComposeTextureSwizzle(snapshot->fetch_swizzle, view_desc.swizzle);
  nrhi::TextureView *const view =
      context.device->CreateTextureView(texture, view_desc);
  if (view == nullptr) {
    context.device->DestroyDeferred(texture);
    context.device->DestroyDeferred(upload);
    REXLOG_ERROR("Table Tennis venue observer: texture view failed");
    return nullptr;
  }

  context.cmd->CopyBufferToTexture(texture, 0, 0, upload, 0, upload_row_pitch,
                                   host_width, host_height, 1);
  context.cmd->Barrier(texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.device->DestroyDeferred(upload);
  resources.textures.push_back({
      .snapshot = snapshot,
      .texture = texture,
      .view = view,
  });
  REXLOG_INFO("Table Tennis venue observer: uploaded captured texture "
              "{}x{} format={} payload={:016X}",
              snapshot->width, snapshot->height, snapshot->format,
              snapshot->payload_fingerprint);
  return view;
}

nrhi::TextureView *
FindPreparedTexture(const Resources &resources,
                    const std::shared_ptr<const TextureSnapshot> &snapshot) {
  const auto found = std::find_if(
      resources.textures.begin(), resources.textures.end(),
      [&](const GpuTexture &texture) { return texture.snapshot == snapshot; });
  return found != resources.textures.end() ? found->view : nullptr;
}

template <typename Frame>
bool EnsurePayloadResources(
    Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Frame> &frame) {
  if (frame == nullptr || !frame->valid() || context.cmd == nullptr ||
      !EnsureGeometry(resources, context, *frame) ||
      frame->draws.size() != resources.draw_ranges.size()) {
    return false;
  }
  for (size_t index = 0; index < frame->draws.size(); ++index) {
    const VenueDrawSnapshot &draw = VenueDrawAt(*frame, index);
    if (EnsureTexture(resources, context, draw.material.textures[0]) ==
            nullptr ||
        EnsureTexture(resources, context, draw.material.textures[1]) ==
            nullptr) {
      return false;
    }
  }
  return true;
}

template <typename Frame>
bool EnsurePreparedResources(
    Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Frame> &frame, bool replacement) {
  return EnsurePipeline(resources, context, replacement) &&
         EnsurePayloadResources(resources, context, frame);
}

template <typename Frame>
bool PreparedPayloadResourcesMatch(
    const Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Frame> &frame) {
  if (frame == nullptr || !frame->valid() || context.cmd == nullptr ||
      resources.device != context.device || resources.layout == nullptr ||
      resources.vertex_buffer == nullptr || resources.index_buffer == nullptr ||
      frame->draws.size() != resources.draw_ranges.size() ||
      !SameGeometry(resources, *frame)) {
    return false;
  }
  for (size_t index = 0; index < frame->draws.size(); ++index) {
    const VenueDrawSnapshot &draw = VenueDrawAt(*frame, index);
    if (FindPreparedTexture(resources, draw.material.textures[0]) == nullptr ||
        FindPreparedTexture(resources, draw.material.textures[1]) == nullptr) {
      return false;
    }
  }
  return true;
}

template <typename Frame>
bool PreparedResourcesMatch(
    const Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Frame> &frame) {
  return resources.observer_pipeline != nullptr &&
         PreparedPayloadResourcesMatch(resources, context, frame);
}

template <typename Frame>
uint32_t RenderPreparedVenueObserver(
    Resources &resources,
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const Frame> &frame, bool full_family) {
  if (!PreparedResourcesMatch(resources, context, frame)) {
    return 0;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(resources.layout);
  cmd->SetPipeline(resources.observer_pipeline);
  cmd->SetVertexBuffer(resources.vertex_buffer, 0, resources.vertex_bytes,
                       sizeof(VenueVertex));
  cmd->SetIndexBuffer(resources.index_buffer, 0, resources.index_bytes);

  const float opacity = static_cast<float>(
      REXCVAR_GET(tabletennis_native_venue_observer_opacity));
  const float display_gamma = static_cast<float>(
      REXCVAR_GET(tabletennis_native_venue_observer_display_gamma));
  const float checkerboard =
      REXCVAR_GET(tabletennis_native_venue_observer_checkerboard) ? 1.0f : 0.0f;
  uint32_t draw_count = 0;
  for (size_t index = 0; index < frame->draws.size(); ++index) {
    const VenueDrawSnapshot &draw = VenueDrawAt(*frame, index);
    const DrawRange &range = resources.draw_ranges[index];
    if (range.primitive_type == 0x04) {
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    } else if (range.primitive_type == 0x06) {
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
    } else {
      continue;
    }

    std::array<float, 20> constants{};
    std::copy(draw.world_view_projection.begin(),
              draw.world_view_projection.end(), constants.begin());
    constants[16] =
        draw.material.pixel_constant_20[2] * draw.material.pixel_constant_46[0];
    constants[17] = opacity;
    constants[18] = 1.0f / display_gamma;
    constants[19] = checkerboard;
    nrhi::TextureView *const base =
        FindPreparedTexture(resources, draw.material.textures[0]);
    nrhi::TextureView *const detail =
        FindPreparedTexture(resources, draw.material.textures[1]);
    if (base == nullptr || detail == nullptr) {
      return draw_count;
    }
    cmd->SetRootConstants(0, constants.size(), constants.data(), 0);
    cmd->SetTexturePair(1, base, detail);
    cmd->DrawIndexed(range.index_count, range.start_index, range.base_vertex);
    ++draw_count;
  }

  if (draw_count != 0 && !resources.announced_draw) {
    resources.announced_draw = true;
    if (full_family) {
      REXLOG_INFO("Table Tennis full PS328 venue overlay: drew {} ordered "
                  "captured title draws over untouched guest output "
                  "(duplicates preserved, observer-only)",
                  draw_count);
    } else {
      REXLOG_INFO("Table Tennis venue observer: drew {} real meshes with "
                  "captured textures and exact PS328 material equation "
                  "(guest still authoritative)",
                  draw_count);
    }
  }
  return draw_count;
}

} // namespace

uint32_t RenderVenueObserver(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFrameSnapshot> &frame) {
  if (!EnsurePreparedResources(g_resources, context, frame, false)) {
    return 0;
  }
  return RenderPreparedVenueObserver(g_resources, context, frame, false);
}

bool PrepareVenueFullFamilyOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame) {
  return VenueFullFamilyOverlayEnabled() && frame != nullptr &&
         frame->copy_failures == 0 && frame->dropped_draws == 0 &&
         EnsurePreparedResources(g_full_family_resources, context, frame,
                                 false);
}

uint32_t RenderVenueFullFamilyOverlay(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame) {
  if (!VenueFullFamilyOverlayEnabled() || frame == nullptr ||
      frame->copy_failures != 0 || frame->dropped_draws != 0) {
    return 0;
  }
  return RenderPreparedVenueObserver(g_full_family_resources, context, frame,
                                     true);
}

bool PrepareVenueFullFamilyNativeScene(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame) {
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      frame == nullptr || !frame->valid() ||
      !EnsureNativeScenePipeline(g_full_family_resources, context, targets) ||
      !EnsurePayloadResources(g_full_family_resources, context, frame)) {
    return false;
  }
  // Exact shared ownership is the frame token for later command recording.
  // A geometrically compatible stale frame is never good enough here.
  g_full_family_resources.native_scene_frame = frame;
  return true;
}

VenueNativeSceneRecordResult RecordPreparedVenueNativeSceneDraw(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const std::shared_ptr<const VenueFullFamilyFrame> &frame,
    const NativeSceneDrawRef &draw_ref) {
  if (ValidateNativeScenePassTargets(context, targets) !=
      NativeScenePassTargetValidation::kValid) {
    return VenueNativeSceneRecordResult::kInvalidTarget;
  }
  if (draw_ref.family != NativeSceneDrawFamily::kVenuePs328) {
    return VenueNativeSceneRecordResult::kWrongFamily;
  }
  if (frame == nullptr || !frame->valid()) {
    return VenueNativeSceneRecordResult::kInvalidFrame;
  }

  Resources &resources = g_full_family_resources;
  if (resources.native_scene_frame != frame ||
      resources.native_scene_pipeline == nullptr ||
      resources.native_scene_color_format != targets.color->format() ||
      resources.native_scene_depth_format != targets.depth->format() ||
      resources.native_scene_sample_count != targets.sample_count ||
      !PreparedPayloadResourcesMatch(resources, context, frame)) {
    return VenueNativeSceneRecordResult::kResourcesNotPrepared;
  }
  if (draw_ref.family_draw_index >= frame->draws.size() ||
      draw_ref.family_draw_index >= resources.draw_ranges.size()) {
    return VenueNativeSceneRecordResult::kDrawIndexOutOfRange;
  }

  const VenueFullFamilyDrawSnapshot &family_draw =
      frame->draws[draw_ref.family_draw_index];
  const VenueDrawSnapshot &draw = family_draw.captured;
  const DrawRange &range = resources.draw_ranges[draw_ref.family_draw_index];
  if (draw_ref.ordinal == 0 || family_draw.source.ordinal != draw_ref.ordinal ||
      draw.ordinal != draw_ref.ordinal) {
    return VenueNativeSceneRecordResult::kOrdinalMismatch;
  }
  nrhi::PrimitiveTopology topology;
  if (range.primitive_type == 0x04) {
    topology = nrhi::PrimitiveTopology::kTriangleList;
  } else if (range.primitive_type == 0x06) {
    topology = nrhi::PrimitiveTopology::kTriangleStrip;
  } else {
    return VenueNativeSceneRecordResult::kUnsupportedPrimitive;
  }

  nrhi::TextureView *const base =
      FindPreparedTexture(resources, draw.material.textures[0]);
  nrhi::TextureView *const detail =
      FindPreparedTexture(resources, draw.material.textures[1]);
  if (base == nullptr || detail == nullptr) {
    return VenueNativeSceneRecordResult::kMissingTexture;
  }

  std::array<float, 20> constants{};
  std::copy(draw.world_view_projection.begin(),
            draw.world_view_projection.end(), constants.begin());
  constants[16] =
      draw.material.pixel_constant_20[2] * draw.material.pixel_constant_46[0];
  // ps_replace ignores the observer-only tail, but initialize it to the
  // identity path so a shader refactor cannot inherit overlay controls.
  constants[17] = 1.0f;
  constants[18] = 1.0f;

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(resources.layout);
  cmd->SetPipeline(resources.native_scene_pipeline);
  cmd->SetPrimitiveTopology(topology);
  cmd->SetRootConstants(0, constants.size(), constants.data(), 0);
  cmd->SetTexturePair(1, base, detail);
  cmd->SetVertexBuffer(resources.vertex_buffer, 0, resources.vertex_bytes,
                       sizeof(VenueVertex));
  cmd->SetIndexBuffer(resources.index_buffer, 0, resources.index_bytes);
  if (!cmd->DrawIndexedChecked(range.index_count, range.start_index,
                               range.base_vertex)) {
    return VenueNativeSceneRecordResult::kRhiDrawStateRejected;
  }
  return VenueNativeSceneRecordResult::kRecorded;
}

const char *
VenueNativeSceneRecordResultName(VenueNativeSceneRecordResult result) {
  switch (result) {
  case VenueNativeSceneRecordResult::kRecorded:
    return "recorded";
  case VenueNativeSceneRecordResult::kInvalidTarget:
    return "invalid_target";
  case VenueNativeSceneRecordResult::kWrongFamily:
    return "wrong_family";
  case VenueNativeSceneRecordResult::kInvalidFrame:
    return "invalid_frame";
  case VenueNativeSceneRecordResult::kResourcesNotPrepared:
    return "resources_not_prepared";
  case VenueNativeSceneRecordResult::kDrawIndexOutOfRange:
    return "draw_index_out_of_range";
  case VenueNativeSceneRecordResult::kOrdinalMismatch:
    return "ordinal_mismatch";
  case VenueNativeSceneRecordResult::kUnsupportedPrimitive:
    return "unsupported_primitive";
  case VenueNativeSceneRecordResult::kMissingTexture:
    return "missing_texture";
  case VenueNativeSceneRecordResult::kRhiDrawStateRejected:
    return "rhi_draw_state_rejected";
  }
  return "unknown";
}

bool PrepareVenueReplacement(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const std::shared_ptr<const VenueFrameSnapshot> &frame) {
  return EnsurePreparedResources(g_resources, context, frame, true);
}

bool MatchVenueReplacement(const rex::graphics::NativeGuestDrawContext &context,
                           void *) {
  const uint32_t replacement_count = VenueFamilyReplacementDrawCount();
  if (replacement_count == 0) {
    g_pending_candidate.reset();
    if (g_replacement_telemetry.replacement_count != 0) {
      ResetReplacementTelemetry(0);
    }
    return false;
  }
  if (context.backend != rex::graphics::NativeGuestOutputBackend::kVulkan ||
      !context.indexed ||
      context.vertex_shader_hash != kVenueVertexShaderHash ||
      context.pixel_shader_hash != kVenuePixelShaderHash) {
    return false;
  }
  const auto prefix =
      std::find(kVenueTracePrefix.begin(), kVenueTracePrefix.end(),
                context.vertex_or_index_count);
  if (prefix == kVenueTracePrefix.end()) {
    return false;
  }
  const size_t index = static_cast<size_t>(prefix - kVenueTracePrefix.begin());
  BeginReplacementTraceFrame(replacement_count, index);
  const size_t first_replacement =
      kVenueTracePrefix.size() -
      std::min<size_t>(replacement_count, kVenueTracePrefix.size());
  if (index < first_replacement) {
    return false;
  }
  ++g_replacement_telemetry.selected;
  const std::shared_ptr<const VenueReplacementCandidate> candidate =
      LatestVenueReplacementCandidate(static_cast<uint32_t>(index));
  if (candidate == nullptr || !candidate->valid()) {
    RecordReplacementReject(ReplacementReject::kCandidateUnavailable);
    return false;
  }
  if (candidate->generation == g_last_matched_candidate_generations[index]) {
    RecordReplacementReject(ReplacementReject::kStaleGeneration);
    return false;
  }
  if (candidate->prefix_index != index) {
    RecordReplacementReject(ReplacementReject::kCandidateIdentity);
    return false;
  }
  if (index >= g_resources.draw_ranges.size() ||
      index >= g_resources.uploaded_meshes.size() ||
      g_resources.device == nullptr ||
      g_resources.replacement_pipeline == nullptr ||
      g_resources.vertex_buffer == nullptr ||
      g_resources.index_buffer == nullptr) {
    RecordReplacementReject(ReplacementReject::kResources);
    return false;
  }
  const VenueDrawSnapshot &draw = candidate->draw;
  const DrawRange &range = g_resources.draw_ranges[index];
  if (draw.mesh == nullptr || g_resources.uploaded_meshes[index] != draw.mesh ||
      range.index_count != context.vertex_or_index_count ||
      range.primitive_type != context.primitive_type) {
    RecordReplacementReject(ReplacementReject::kGeometry);
    return false;
  }
  if (FindPreparedTexture(g_resources, draw.material.textures[0]) == nullptr ||
      FindPreparedTexture(g_resources, draw.material.textures[1]) == nullptr) {
    RecordReplacementReject(ReplacementReject::kResources);
    return false;
  }
  // Consume exactly one fully verified title-side token. If the backend
  // cannot open the borrowed scope, the original guest draw remains enabled;
  // never retarget a later draw with a coincidental index count.
  g_pending_candidate = candidate;
  g_last_matched_candidate_generations[index] = candidate->generation;
  ++g_replacement_telemetry.matched;
  g_replacement_telemetry.matched_mask |= 1u << index;
  return true;
}

bool RenderVenueReplacement(
    const rex::graphics::NativeGuestDrawContext &context, void *) {
  const std::shared_ptr<const VenueReplacementCandidate> candidate =
      std::move(g_pending_candidate);
  if (candidate == nullptr || !candidate->valid()) {
    RecordReplacementReject(ReplacementReject::kRender);
    return false;
  }
  const size_t index = candidate->prefix_index;
  if (context.cmd == nullptr || context.device != g_resources.device ||
      index >= g_resources.draw_ranges.size()) {
    RecordReplacementReject(ReplacementReject::kRender);
    return false;
  }

  const VenueDrawSnapshot &draw = candidate->draw;
  const DrawRange &range = g_resources.draw_ranges[index];
  nrhi::TextureView *const base =
      FindPreparedTexture(g_resources, draw.material.textures[0]);
  nrhi::TextureView *const detail =
      FindPreparedTexture(g_resources, draw.material.textures[1]);
  if (base == nullptr || detail == nullptr) {
    RecordReplacementReject(ReplacementReject::kRender);
    return false;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.replacement_pipeline);
  cmd->SetVertexBuffer(g_resources.vertex_buffer, 0, g_resources.vertex_bytes,
                       sizeof(VenueVertex));
  cmd->SetIndexBuffer(g_resources.index_buffer, 0, g_resources.index_bytes);
  if (range.primitive_type == 0x04) {
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  } else if (range.primitive_type == 0x06) {
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  } else {
    RecordReplacementReject(ReplacementReject::kRender);
    return false;
  }

  std::array<float, 20> constants{};
  std::copy(draw.world_view_projection.begin(),
            draw.world_view_projection.end(), constants.begin());
  constants[16] =
      draw.material.pixel_constant_20[2] * draw.material.pixel_constant_46[0];
  constants[17] = 1.0f;
  constants[18] = 1.0f;
  constants[19] = 0.0f;
  cmd->SetRootConstants(0, constants.size(), constants.data(), 0);
  cmd->SetTexturePair(1, base, detail);
  cmd->DrawIndexed(range.index_count, range.start_index, range.base_vertex);
  RecordReplacementServed(index);

  if (!g_resources.announced_replacement) {
    g_resources.announced_replacement = true;
    REXLOG_INFO("Table Tennis venue replacement: served trace draw {} "
                "in-order (indices={}, pitch={}, render_pass_key={:08X})",
                index, range.index_count, context.surface_pitch,
                context.render_pass_key);
  }
  return true;
}

void ShutdownVenueObserverRenderer() { ReleaseResources(); }

} // namespace tabletennis::native
