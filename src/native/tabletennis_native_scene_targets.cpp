#include "native/tabletennis_native_scene_targets.h"

#include "native/shaders/tabletennis_native_scene_resolve_spirv.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint32_t kSceneSampleCount = 4;

enum class PassState : uint8_t {
  kUnavailable,
  kReady,
  kOpen,
};

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::Texture *color = nullptr;
  nrhi::Texture *depth = nullptr;
  nrhi::TextureView *resolve_source = nullptr;
  nrhi::BindingLayout *resolve_layout = nullptr;
  nrhi::Shader *resolve_vertex_shader = nullptr;
  nrhi::Shader *resolve_pixel_shader = nullptr;
  nrhi::Pipeline *resolve_pipeline = nullptr;
  nrhi::Format presenter_format = nrhi::Format::kUnknown;
  uint32_t width = 0;
  uint32_t height = 0;
  PassState state = PassState::kUnavailable;
};

Resources g_resources;

void ReleaseResources() {
  if (g_resources.device != nullptr) {
    g_resources.device->DestroyDeferred(g_resources.resolve_pipeline);
    g_resources.device->DestroyDeferred(g_resources.resolve_vertex_shader);
    g_resources.device->DestroyDeferred(g_resources.resolve_pixel_shader);
    g_resources.device->DestroyDeferred(g_resources.resolve_source);
    g_resources.device->DestroyDeferred(g_resources.color);
    g_resources.device->DestroyDeferred(g_resources.depth);
  }
  // Binding layouts follow the device lifetime.
  g_resources = {};
}

bool EnsureDevice(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (context.device == nullptr || context.cmd == nullptr ||
      context.guest_output == nullptr || context.guest_output_width == 0 ||
      context.guest_output_height == 0 ||
      context.guest_output_width >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      context.guest_output_height >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
    return false;
  }
  if (g_resources.device != nullptr && g_resources.device != context.device) {
    // A changed context device means the old command processor has already
    // torn its RHI wrapper down. Never dereference that stale pointer; normal
    // title shutdown releases resources explicitly while the device is live.
    g_resources = {};
  }
  g_resources.device = context.device;
  return true;
}

bool EnsureResolveResources(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  nrhi::Device *const device = context.device;
  if (g_resources.resolve_layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 1;
    layout.params[0] = {nrhi::BindingParamKind::kTextureTable, 0, 1,
                        nrhi::Visibility::kPixel};
    layout.allow_input_layout = false;
    g_resources.resolve_layout = device->CreateBindingLayout(layout);
    if (g_resources.resolve_layout == nullptr) {
      return false;
    }
  }

  if (g_resources.resolve_vertex_shader == nullptr ||
      g_resources.resolve_pixel_shader == nullptr) {
    device->DestroyDeferred(g_resources.resolve_vertex_shader);
    device->DestroyDeferred(g_resources.resolve_pixel_shader);
    g_resources.resolve_vertex_shader = nullptr;
    g_resources.resolve_pixel_shader = nullptr;
    nrhi::ShaderDesc vertex;
    vertex.stage = nrhi::ShaderStage::kVertex;
    vertex.name = "tabletennis_native_scene_resolve.hlsl";
    vertex.hlsl_source = native_scene_resolve_shader::kHlsl;
    vertex.entry_point = "vs_main";
    vertex.spirv = native_scene_resolve_shader::kVertexSpirv;
    vertex.spirv_size_bytes = native_scene_resolve_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc pixel = vertex;
    pixel.stage = nrhi::ShaderStage::kPixel;
    pixel.entry_point = "ps_main";
    pixel.spirv = native_scene_resolve_shader::kPixelSpirv;
    pixel.spirv_size_bytes = native_scene_resolve_shader::kPixelSpirvBytes;
    nrhi::Shader *const vertex_shader = device->CreateShader(vertex);
    nrhi::Shader *const pixel_shader = device->CreateShader(pixel);
    if (vertex_shader == nullptr || pixel_shader == nullptr) {
      device->DestroyDeferred(vertex_shader);
      device->DestroyDeferred(pixel_shader);
      return false;
    }
    g_resources.resolve_vertex_shader = vertex_shader;
    g_resources.resolve_pixel_shader = pixel_shader;
  }

  const nrhi::Format presenter_format = context.guest_output->format();
  if (g_resources.resolve_pipeline != nullptr &&
      g_resources.presenter_format == presenter_format) {
    return true;
  }
  device->DestroyDeferred(g_resources.resolve_pipeline);
  g_resources.resolve_pipeline = nullptr;

  nrhi::GraphicsPipelineDesc pipeline;
  pipeline.layout = g_resources.resolve_layout;
  pipeline.vs = g_resources.resolve_vertex_shader;
  pipeline.ps = g_resources.resolve_pixel_shader;
  pipeline.cull = nrhi::CullMode::kNone;
  pipeline.depth_clip = false;
  pipeline.rtv_format = presenter_format;
  pipeline.sample_count = 1;
  g_resources.resolve_pipeline = device->CreateGraphicsPipeline(pipeline);
  if (g_resources.resolve_pipeline == nullptr) {
    return false;
  }
  g_resources.presenter_format = presenter_format;
  return true;
}

bool EnsureAttachments(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (g_resources.color != nullptr && g_resources.depth != nullptr &&
      g_resources.resolve_source != nullptr &&
      g_resources.width == context.guest_output_width &&
      g_resources.height == context.guest_output_height) {
    return true;
  }

  nrhi::TextureDesc color;
  color.width = context.guest_output_width;
  color.height = context.guest_output_height;
  color.sample_count = kSceneSampleCount;
  color.format = nrhi::Format::kR8G8B8A8_UNORM;
  color.usage = nrhi::kTextureUsageRenderTarget;
  color.initial_state = nrhi::ResourceState::kRenderTarget;
  nrhi::Texture *const new_color = context.device->CreateTexture(color);

  nrhi::TextureDesc depth;
  depth.width = context.guest_output_width;
  depth.height = context.guest_output_height;
  depth.sample_count = kSceneSampleCount;
  depth.format = nrhi::Format::kD32_FLOAT;
  depth.usage = nrhi::kTextureUsageDepthStencil;
  depth.initial_state = nrhi::ResourceState::kDepthWrite;
  depth.clear_depth = 1.0f;
  nrhi::Texture *const new_depth = context.device->CreateTexture(depth);
  if (new_color == nullptr || new_depth == nullptr) {
    context.device->DestroyDeferred(new_color);
    context.device->DestroyDeferred(new_depth);
    return false;
  }

  nrhi::TextureViewDesc resolve_view;
  resolve_view.dimension = nrhi::ViewDimension::k2DMS;
  resolve_view.mip_levels = 1;
  nrhi::TextureView *const new_resolve_source =
      context.device->CreateTextureView(new_color, resolve_view);
  if (new_resolve_source == nullptr) {
    context.device->DestroyDeferred(new_color);
    context.device->DestroyDeferred(new_depth);
    return false;
  }

  context.device->DestroyDeferred(g_resources.resolve_source);
  context.device->DestroyDeferred(g_resources.color);
  context.device->DestroyDeferred(g_resources.depth);
  g_resources.resolve_source = new_resolve_source;
  g_resources.color = new_color;
  g_resources.depth = new_depth;
  g_resources.width = context.guest_output_width;
  g_resources.height = context.guest_output_height;
  return true;
}

NativeScenePassTargets CurrentTargets() {
  return {
      .color = g_resources.color,
      .depth = g_resources.depth,
      .width = g_resources.width,
      .height = g_resources.height,
      .sample_count = kSceneSampleCount,
  };
}

bool ExactOwnedTargets(const NativeScenePassTargets &targets) {
  return targets.color == g_resources.color &&
         targets.depth == g_resources.depth &&
         targets.width == g_resources.width &&
         targets.height == g_resources.height &&
         targets.sample_count == kSceneSampleCount;
}

bool ValidClearValues(const NativeScenePassClearValues &clear_values) {
  return std::ranges::all_of(
             clear_values.color,
             [](float value) { return std::isfinite(value); }) &&
         std::isfinite(clear_values.depth) && clear_values.depth >= 0.0f &&
         clear_values.depth <= 1.0f;
}

void SetFullOutputArea(nrhi::Cmd *cmd, uint32_t width, uint32_t height) {
  nrhi::Viewport viewport;
  viewport.width = static_cast<float>(width);
  viewport.height = static_cast<float>(height);
  cmd->SetViewport(viewport);
  nrhi::Rect scissor;
  scissor.right = static_cast<int32_t>(width);
  scissor.bottom = static_cast<int32_t>(height);
  cmd->SetScissor(scissor);
}

void RestoreSteadyStates(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  context.cmd->Barrier(g_resources.color,
                       nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kRenderTarget);
  context.cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                       nrhi::ResourceState::kGuestOutput);
  context.cmd->FlushBarriers();
}

void DiscardOpenPass(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  // Leaving attachment state forces Vulkan to consume any pending clear and
  // close an open render pass. Both images then return to the states expected
  // by the next Begin call. The presenter is deliberately untouched.
  context.cmd->Barrier(g_resources.color, nrhi::ResourceState::kRenderTarget,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->Barrier(g_resources.depth, nrhi::ResourceState::kDepthWrite,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();
  context.cmd->Barrier(g_resources.color,
                       nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kRenderTarget);
  context.cmd->Barrier(g_resources.depth,
                       nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kDepthWrite);
  context.cmd->FlushBarriers();
  g_resources.state = PassState::kReady;
}

} // namespace

NativeSceneRenderTargetsResult PrepareNativeSceneRenderTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    NativeScenePassTargets &targets_out) {
  targets_out = {};
  if (!EnsureDevice(context) || context.guest_output->sample_count() != 1) {
    return NativeSceneRenderTargetsResult::kInvalidContext;
  }
  if (g_resources.state == PassState::kOpen) {
    return NativeSceneRenderTargetsResult::kPassAlreadyOpen;
  }
  if (context.device->GetSupportedSampleCount(nrhi::Format::kR8G8B8A8_UNORM,
                                              kSceneSampleCount) !=
          kSceneSampleCount ||
      context.device->GetSupportedSampleCount(
          nrhi::Format::kD32_FLOAT, kSceneSampleCount) != kSceneSampleCount) {
    return NativeSceneRenderTargetsResult::kFourSampleUnsupported;
  }
  if (!EnsureResolveResources(context)) {
    return NativeSceneRenderTargetsResult::kResolveResourcesFailed;
  }
  if (!EnsureAttachments(context)) {
    return NativeSceneRenderTargetsResult::kAttachmentCreationFailed;
  }

  targets_out = CurrentTargets();
  if (ValidateNativeScenePassTargets(context, targets_out) !=
      NativeScenePassTargetValidation::kValid) {
    targets_out = {};
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }
  g_resources.state = PassState::kReady;
  return NativeSceneRenderTargetsResult::kSucceeded;
}

NativeSceneRenderTargetsResult BeginNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const NativeScenePassClearValues &clear_values) {
  if (!EnsureDevice(context)) {
    return NativeSceneRenderTargetsResult::kInvalidContext;
  }
  if (g_resources.state == PassState::kOpen) {
    return NativeSceneRenderTargetsResult::kPassAlreadyOpen;
  }
  if (g_resources.state != PassState::kReady || !ExactOwnedTargets(targets) ||
      ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid) {
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }
  if (!ValidClearValues(clear_values)) {
    return NativeSceneRenderTargetsResult::kInvalidClearValues;
  }

  SetFullOutputArea(context.cmd, targets.width, targets.height);
  context.cmd->SetRenderTargets(targets.color, targets.depth);
  context.cmd->ClearRenderTarget(targets.color, clear_values.color.data());
  context.cmd->ClearDepth(targets.depth, clear_values.depth);
  g_resources.state = PassState::kOpen;
  return NativeSceneRenderTargetsResult::kSucceeded;
}

NativeSceneRenderTargetsResult AbortNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (!EnsureDevice(context)) {
    return NativeSceneRenderTargetsResult::kInvalidContext;
  }
  if (g_resources.state != PassState::kOpen) {
    return NativeSceneRenderTargetsResult::kPassNotOpen;
  }
  if (g_resources.color == nullptr || g_resources.depth == nullptr) {
    g_resources.state = PassState::kUnavailable;
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }
  DiscardOpenPass(context);
  return NativeSceneRenderTargetsResult::kSucceeded;
}

NativeSceneRenderTargetsResult ResolveNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (!EnsureDevice(context)) {
    return NativeSceneRenderTargetsResult::kInvalidContext;
  }
  if (g_resources.state != PassState::kOpen) {
    return NativeSceneRenderTargetsResult::kPassNotOpen;
  }
  if (!ExactOwnedTargets(targets) ||
      ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      g_resources.resolve_source == nullptr ||
      g_resources.resolve_layout == nullptr ||
      g_resources.resolve_pipeline == nullptr) {
    DiscardOpenPass(context);
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }

  context.cmd->Barrier(g_resources.color, nrhi::ResourceState::kRenderTarget,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
                       nrhi::ResourceState::kRenderTarget);
  context.cmd->FlushBarriers();
  context.cmd->SetRenderTargets(context.guest_output, nullptr);
  SetFullOutputArea(context.cmd, targets.width, targets.height);
  context.cmd->SetBindingLayout(g_resources.resolve_layout);
  context.cmd->SetPipeline(g_resources.resolve_pipeline);
  context.cmd->SetTexture(0, g_resources.resolve_source);
  context.cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  // Vulkan resolves pipeline/descriptor state lazily; D3D12 verifies its
  // eagerly recorded texture-table binding. Neither backend reaches Draw
  // unless the complete resolve state is ready.
  if (!context.cmd->PreflightDraw()) {
    RestoreSteadyStates(context);
    g_resources.state = PassState::kReady;
    return NativeSceneRenderTargetsResult::kResolvePreflightFailed;
  }
  context.cmd->Draw(3, 0);
  RestoreSteadyStates(context);
  g_resources.state = PassState::kReady;
  return NativeSceneRenderTargetsResult::kSucceeded;
}

const char *
NativeSceneRenderTargetsResultName(NativeSceneRenderTargetsResult result) {
  switch (result) {
  case NativeSceneRenderTargetsResult::kSucceeded:
    return "succeeded";
  case NativeSceneRenderTargetsResult::kInvalidContext:
    return "invalid_context";
  case NativeSceneRenderTargetsResult::kPassAlreadyOpen:
    return "pass_already_open";
  case NativeSceneRenderTargetsResult::kPassNotOpen:
    return "pass_not_open";
  case NativeSceneRenderTargetsResult::kInvalidTargets:
    return "invalid_targets";
  case NativeSceneRenderTargetsResult::kInvalidClearValues:
    return "invalid_clear_values";
  case NativeSceneRenderTargetsResult::kFourSampleUnsupported:
    return "four_sample_unsupported";
  case NativeSceneRenderTargetsResult::kAttachmentCreationFailed:
    return "attachment_creation_failed";
  case NativeSceneRenderTargetsResult::kResolveResourcesFailed:
    return "resolve_resources_failed";
  case NativeSceneRenderTargetsResult::kResolvePreflightFailed:
    return "resolve_preflight_failed";
  }
  return "unknown";
}

void ShutdownNativeSceneRenderTargets() { ReleaseResources(); }

} // namespace tabletennis::native
