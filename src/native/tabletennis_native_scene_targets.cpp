#include "native/tabletennis_native_scene_targets.h"

#include "native/shaders/tabletennis_native_scene_resolve_spirv.h"
#include "native/tabletennis_offscreen_target_owner.h"

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr OffscreenTargetDesc kCustomSceneTargetDesc = {
    .color_format = nrhi::Format::kR8G8B8A8_UNORM,
    .depth_format = nrhi::Format::kD32_FLOAT,
    .sample_count = 4,
};

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::BindingLayout *resolve_layout = nullptr;
  nrhi::Shader *resolve_vertex_shader = nullptr;
  nrhi::Shader *resolve_pixel_shader = nullptr;
  nrhi::Pipeline *resolve_pipeline = nullptr;
  nrhi::Format presenter_format = nrhi::Format::kUnknown;
};

Resources g_resources;
OffscreenTargetOwner g_target_owner;

void ReleaseResources() {
  if (g_resources.device != nullptr) {
    g_resources.device->DestroyDeferred(g_resources.resolve_pipeline);
    g_resources.device->DestroyDeferred(g_resources.resolve_vertex_shader);
    g_resources.device->DestroyDeferred(g_resources.resolve_pixel_shader);
  }
  // Binding layouts follow the device lifetime.
  g_resources = {};
  g_target_owner.Shutdown();
}

bool EnsureDevice(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (context.device == nullptr || context.cmd == nullptr ||
      context.guest_output == nullptr) {
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
  context.cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                       nrhi::ResourceState::kGuestOutput);
  context.cmd->FlushBarriers();
  g_target_owner.RestoreAfterResolve(context);
}

} // namespace

NativeSceneRenderTargetsResult PrepareNativeSceneRenderTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    NativeScenePassTargets &targets_out) {
  targets_out = {};
  if (!EnsureDevice(context)) {
    return NativeSceneRenderTargetsResult::kInvalidContext;
  }
  if (!EnsureResolveResources(context)) {
    return NativeSceneRenderTargetsResult::kResolveResourcesFailed;
  }
  const OffscreenTargetOwnerResult owner_result =
      g_target_owner.Prepare(context, kCustomSceneTargetDesc, targets_out);
  if (owner_result == OffscreenTargetOwnerResult::kPassAlreadyOpen) {
    return NativeSceneRenderTargetsResult::kPassAlreadyOpen;
  }
  if (owner_result == OffscreenTargetOwnerResult::kSampleCountUnsupported) {
    return NativeSceneRenderTargetsResult::kFourSampleUnsupported;
  }
  if (owner_result == OffscreenTargetOwnerResult::kAttachmentCreationFailed) {
    return NativeSceneRenderTargetsResult::kAttachmentCreationFailed;
  }
  if (owner_result != OffscreenTargetOwnerResult::kSucceeded) {
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }
  if (ValidateNativeScenePassTargets(context, targets_out) !=
      NativeScenePassTargetValidation::kValid) {
    targets_out = {};
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }
  return NativeSceneRenderTargetsResult::kSucceeded;
}

NativeSceneRenderTargetsResult BeginNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets,
    const NativeScenePassClearValues &clear_values) {
  const OffscreenTargetOwnerResult result = g_target_owner.Begin(
      context, targets, clear_values.color, clear_values.depth);
  switch (result) {
  case OffscreenTargetOwnerResult::kSucceeded:
    return NativeSceneRenderTargetsResult::kSucceeded;
  case OffscreenTargetOwnerResult::kInvalidContext:
    return NativeSceneRenderTargetsResult::kInvalidContext;
  case OffscreenTargetOwnerResult::kPassAlreadyOpen:
    return NativeSceneRenderTargetsResult::kPassAlreadyOpen;
  case OffscreenTargetOwnerResult::kInvalidClearValues:
    return NativeSceneRenderTargetsResult::kInvalidClearValues;
  default:
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }
}

NativeSceneRenderTargetsResult AbortNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  const OffscreenTargetOwnerResult result = g_target_owner.Abort(context);
  if (result == OffscreenTargetOwnerResult::kSucceeded) {
    return NativeSceneRenderTargetsResult::kSucceeded;
  }
  if (result == OffscreenTargetOwnerResult::kInvalidContext) {
    return NativeSceneRenderTargetsResult::kInvalidContext;
  }
  if (result == OffscreenTargetOwnerResult::kPassNotOpen) {
    return NativeSceneRenderTargetsResult::kPassNotOpen;
  }
  return NativeSceneRenderTargetsResult::kInvalidTargets;
}

NativeSceneRenderTargetsResult ResolveNativeSceneRenderPass(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const NativeScenePassTargets &targets) {
  if (!EnsureDevice(context)) {
    return NativeSceneRenderTargetsResult::kInvalidContext;
  }
  if (ValidateNativeScenePassTargets(context, targets) !=
          NativeScenePassTargetValidation::kValid ||
      g_resources.resolve_layout == nullptr ||
      g_resources.resolve_pipeline == nullptr) {
    g_target_owner.Abort(context);
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }

  const OffscreenTargetOwnerResult resolve_result =
      g_target_owner.PrepareForResolve(context, targets);
  if (resolve_result == OffscreenTargetOwnerResult::kPassNotOpen) {
    return NativeSceneRenderTargetsResult::kPassNotOpen;
  }
  if (resolve_result != OffscreenTargetOwnerResult::kSucceeded ||
      g_target_owner.resolve_source() == nullptr) {
    g_target_owner.Abort(context);
    return NativeSceneRenderTargetsResult::kInvalidTargets;
  }
  context.cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
                       nrhi::ResourceState::kRenderTarget);
  context.cmd->FlushBarriers();
  context.cmd->SetRenderTargets(context.guest_output, nullptr);
  SetFullOutputArea(context.cmd, targets.width, targets.height);
  context.cmd->SetBindingLayout(g_resources.resolve_layout);
  context.cmd->SetPipeline(g_resources.resolve_pipeline);
  context.cmd->SetTexture(0, g_target_owner.resolve_source());
  context.cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  // Vulkan resolves pipeline/descriptor state lazily; D3D12 verifies its
  // eagerly recorded texture-table binding. Neither backend reaches Draw
  // unless the complete resolve state is ready.
  if (!context.cmd->PreflightDraw()) {
    RestoreSteadyStates(context);
    return NativeSceneRenderTargetsResult::kResolvePreflightFailed;
  }
  context.cmd->Draw(3, 0);
  RestoreSteadyStates(context);
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
