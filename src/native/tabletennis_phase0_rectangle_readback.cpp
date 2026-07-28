#include "native/tabletennis_phase0_rectangle_readback.h"

#include "native/shaders/tabletennis_native_scene_resolve_spirv.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(
    tabletennis_native_private_replay_dump_directory, "", "Table Tennis",
    "Write one completed guarded private replay as an RGB PPM in this "
    "directory. Empty disables the diagnostic.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint32_t kMaximumDiagnosticWidth = 3840;
constexpr uint32_t kMaximumDiagnosticHeight = 2160;

struct Resources {
  nrhi::Device *device = nullptr;
  nrhi::BindingLayout *layout = nullptr;
  nrhi::Shader *vertex_shader = nullptr;
  nrhi::Shader *pixel_shader = nullptr;
  nrhi::Pipeline *pipeline = nullptr;
  nrhi::Texture *resolved = nullptr;
  nrhi::Buffer *readback = nullptr;
  uint8_t *mapped = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_pitch = 0;
  uint64_t pending_submission = 0;
  uint64_t pending_backend_frame_sequence = 0;
  PrivateReplayDiagnosticKind pending_kind =
      PrivateReplayDiagnosticKind::kRectangle;
};

Resources g_resources;
std::mutex g_result_mutex;
Phase0RectangleReadbackResult g_latest_result;
bool g_private_replay_dump_attempted = false;

uint32_t AlignRowPitch(uint32_t value) {
  return (value + nrhi::kRowPitchAlignment - 1) &
         ~(nrhi::kRowPitchAlignment - 1);
}

void ReleaseResources() {
  if (g_resources.device != nullptr) {
    if (g_resources.mapped != nullptr && g_resources.readback != nullptr) {
      g_resources.device->Unmap(g_resources.readback);
    }
    g_resources.device->DestroyDeferred(g_resources.readback);
    g_resources.device->DestroyDeferred(g_resources.resolved);
    g_resources.device->DestroyDeferred(g_resources.pipeline);
    g_resources.device->DestroyDeferred(g_resources.vertex_shader);
    g_resources.device->DestroyDeferred(g_resources.pixel_shader);
  }
  g_resources = {};
}

bool EnsureResources(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    uint32_t width, uint32_t height) {
  if (context.device == nullptr || context.cmd == nullptr || width == 0 ||
      height == 0 || width > kMaximumDiagnosticWidth ||
      height > kMaximumDiagnosticHeight) {
    return false;
  }
  if (g_resources.device != nullptr && g_resources.device != context.device) {
    g_resources = {};
  }
  g_resources.device = context.device;

  if (g_resources.layout == nullptr) {
    nrhi::BindingLayoutDesc layout;
    layout.param_count = 1;
    layout.params[0] = {nrhi::BindingParamKind::kTextureTable, 0, 1,
                        nrhi::Visibility::kPixel};
    layout.allow_input_layout = false;
    g_resources.layout = context.device->CreateBindingLayout(layout);
    if (g_resources.layout == nullptr) {
      return false;
    }
  }
  if (g_resources.vertex_shader == nullptr ||
      g_resources.pixel_shader == nullptr) {
    context.device->DestroyDeferred(g_resources.vertex_shader);
    context.device->DestroyDeferred(g_resources.pixel_shader);
    g_resources.vertex_shader = nullptr;
    g_resources.pixel_shader = nullptr;
    nrhi::ShaderDesc vertex;
    vertex.stage = nrhi::ShaderStage::kVertex;
    vertex.name = "tabletennis_phase0_rectangle_resolve.hlsl";
    vertex.hlsl_source = native_scene_resolve_shader::kHlsl;
    vertex.entry_point = "vs_main";
    vertex.spirv = native_scene_resolve_shader::kVertexSpirv;
    vertex.spirv_size_bytes = native_scene_resolve_shader::kVertexSpirvBytes;
    nrhi::ShaderDesc pixel = vertex;
    pixel.stage = nrhi::ShaderStage::kPixel;
    pixel.entry_point = "ps_main";
    pixel.spirv = native_scene_resolve_shader::kPixelSpirv;
    pixel.spirv_size_bytes = native_scene_resolve_shader::kPixelSpirvBytes;
    nrhi::Shader *const vertex_shader = context.device->CreateShader(vertex);
    nrhi::Shader *const pixel_shader = context.device->CreateShader(pixel);
    if (vertex_shader == nullptr || pixel_shader == nullptr) {
      context.device->DestroyDeferred(vertex_shader);
      context.device->DestroyDeferred(pixel_shader);
      return false;
    }
    g_resources.vertex_shader = vertex_shader;
    g_resources.pixel_shader = pixel_shader;
  }
  if (g_resources.pipeline == nullptr) {
    nrhi::GraphicsPipelineDesc pipeline;
    pipeline.layout = g_resources.layout;
    pipeline.vs = g_resources.vertex_shader;
    pipeline.ps = g_resources.pixel_shader;
    pipeline.cull = nrhi::CullMode::kNone;
    pipeline.depth_clip = false;
    pipeline.rtv_format = nrhi::Format::kR8G8B8A8_UNORM;
    pipeline.sample_count = 1;
    g_resources.pipeline = context.device->CreateGraphicsPipeline(pipeline);
    if (g_resources.pipeline == nullptr) {
      return false;
    }
  }

  if (g_resources.resolved != nullptr && g_resources.readback != nullptr &&
      g_resources.mapped != nullptr && g_resources.width == width &&
      g_resources.height == height) {
    return true;
  }
  if (g_resources.pending_submission != 0) {
    return false;
  }

  if (g_resources.mapped != nullptr && g_resources.readback != nullptr) {
    context.device->Unmap(g_resources.readback);
  }
  context.device->DestroyDeferred(g_resources.readback);
  context.device->DestroyDeferred(g_resources.resolved);
  g_resources.readback = nullptr;
  g_resources.resolved = nullptr;
  g_resources.mapped = nullptr;

  nrhi::TextureDesc resolved;
  resolved.width = width;
  resolved.height = height;
  resolved.sample_count = 1;
  resolved.format = nrhi::Format::kR8G8B8A8_UNORM;
  resolved.usage =
      nrhi::kTextureUsageRenderTarget | nrhi::kTextureUsageCopySource;
  resolved.initial_state = nrhi::ResourceState::kRenderTarget;
  g_resources.resolved = context.device->CreateTexture(resolved);

  const uint32_t row_pitch = AlignRowPitch(width * 4);
  nrhi::BufferDesc readback;
  readback.size = uint64_t(row_pitch) * height;
  readback.heap = nrhi::HeapKind::kReadback;
  g_resources.readback = context.device->CreateBuffer(readback);
  if (g_resources.resolved == nullptr || g_resources.readback == nullptr) {
    return false;
  }
  g_resources.mapped =
      static_cast<uint8_t *>(context.device->Map(g_resources.readback));
  if (g_resources.mapped == nullptr) {
    return false;
  }
  g_resources.width = width;
  g_resources.height = height;
  g_resources.row_pitch = row_pitch;
  return true;
}

Phase0RectangleReadbackResult AnalyzeMappedPixels() {
  Phase0RectangleReadbackResult result;
  result.backend_frame_sequence = g_resources.pending_backend_frame_sequence;
  result.kind = g_resources.pending_kind;
  result.width = g_resources.width;
  result.height = g_resources.height;
  result.pixel_hash = kFnvOffsetBasis;
  uint32_t min_x = std::numeric_limits<uint32_t>::max();
  uint32_t min_y = std::numeric_limits<uint32_t>::max();
  uint32_t max_x = 0;
  uint32_t max_y = 0;
  bool found = false;
  for (uint32_t y = 0; y < g_resources.height; ++y) {
    const uint8_t *row =
        g_resources.mapped + uint64_t(y) * g_resources.row_pitch;
    for (uint32_t x = 0; x < g_resources.width; ++x) {
      const uint8_t *pixel = row + x * 4;
      for (uint32_t channel = 0; channel < 4; ++channel) {
        result.pixel_hash ^= pixel[channel];
        result.pixel_hash *= kFnvPrime;
      }
      if ((pixel[0] | pixel[1] | pixel[2] | pixel[3]) == 0) {
        continue;
      }
      ++result.nonzero_pixel_count;
      min_x = std::min(min_x, x);
      min_y = std::min(min_y, y);
      max_x = std::max(max_x, x);
      max_y = std::max(max_y, y);
      if (!found) {
        std::copy_n(pixel, 4, result.first_nonzero_rgba.begin());
        found = true;
      }
    }
  }
  if (found) {
    result.nonzero_bounds = {min_x, min_y, max_x + 1, max_y + 1};
  }
  result.valid = true;
  return result;
}

void DumpGuardedReplayPpm(const Phase0RectangleReadbackResult &result) {
  if (g_private_replay_dump_attempted ||
      result.kind != PrivateReplayDiagnosticKind::kGuardedVenueBatch ||
      !result.nonempty()) {
    return;
  }
  const std::string directory =
      REXCVAR_GET(tabletennis_native_private_replay_dump_directory);
  if (directory.empty()) {
    return;
  }
  g_private_replay_dump_attempted = true;

  const std::filesystem::path output_path =
      rex::to_path(directory) /
      ("tabletennis_private_frame_" +
       std::to_string(result.backend_frame_sequence) + ".ppm");
  if (!rex::filesystem::CreateParentFolder(output_path)) {
    REXLOG_WARN("Table Tennis private replay PPM parent creation failed path={}",
                rex::path_to_utf8(output_path));
    return;
  }
  FILE *file = rex::filesystem::OpenFile(output_path, "wb");
  if (file == nullptr) {
    REXLOG_WARN("Table Tennis private replay PPM open failed path={}",
                rex::path_to_utf8(output_path));
    return;
  }

  const std::string header =
      "P6\n" + std::to_string(result.width) + " " +
      std::to_string(result.height) + "\n255\n";
  bool succeeded =
      std::fwrite(header.data(), 1, header.size(), file) == header.size();
  std::vector<uint8_t> rgb_row(size_t(result.width) * 3);
  for (uint32_t y = 0; succeeded && y < result.height; ++y) {
    const uint8_t *source =
        g_resources.mapped + uint64_t(y) * g_resources.row_pitch;
    for (uint32_t x = 0; x < result.width; ++x) {
      rgb_row[size_t(x) * 3 + 0] = source[size_t(x) * 4 + 0];
      rgb_row[size_t(x) * 3 + 1] = source[size_t(x) * 4 + 1];
      rgb_row[size_t(x) * 3 + 2] = source[size_t(x) * 4 + 2];
    }
    succeeded =
        std::fwrite(rgb_row.data(), 1, rgb_row.size(), file) ==
        rgb_row.size();
  }
  succeeded = std::fclose(file) == 0 && succeeded;
  if (!succeeded) {
    REXLOG_WARN("Table Tennis private replay PPM write failed path={}",
                rex::path_to_utf8(output_path));
    return;
  }
  REXLOG_INFO(
      "Table Tennis private replay PPM written path={} frame={} "
      "size={}x{} alpha_ignored=true one_shot=true",
      rex::path_to_utf8(output_path), result.backend_frame_sequence,
      result.width, result.height);
}

} // namespace

void PollPhase0RectangleReadback(
    const rex::graphics::NativeGuestOutputRenderContext &context) {
  if (g_resources.pending_submission == 0 || context.device == nullptr ||
      context.device != g_resources.device ||
      context.device->CompletedSubmission() < g_resources.pending_submission) {
    return;
  }
  const uint64_t byte_count =
      uint64_t(g_resources.row_pitch) * g_resources.height;
  context.device->InvalidateForRead(g_resources.readback, 0, byte_count);
  const Phase0RectangleReadbackResult result = AnalyzeMappedPixels();
  DumpGuardedReplayPpm(result);
  g_resources.pending_submission = 0;
  g_resources.pending_backend_frame_sequence = 0;
  {
    std::lock_guard lock(g_result_mutex);
    g_latest_result = result;
  }
  const char *kind_name = "unknown";
  switch (result.kind) {
  case PrivateReplayDiagnosticKind::kRectangle:
    kind_name = "rectangle";
    break;
  case PrivateReplayDiagnosticKind::kPs328:
    kind_name = "ps328";
    break;
  case PrivateReplayDiagnosticKind::kPs328Batch:
    kind_name = "ps328_batch";
    break;
  case PrivateReplayDiagnosticKind::kGuardedVenueBatch:
    kind_name = "guarded_venue_batch";
    break;
  }
  REXLOG_INFO(
      "Table Tennis private translated replay readback kind={} frame={} "
      "hash={:016X} "
      "size={}x{} nonzero_pixels={} bounds={},{},{},{} "
      "first_rgba={:02X}{:02X}{:02X}{:02X} nonempty={} "
      "private_only=true guest_untouched=true",
      kind_name, result.backend_frame_sequence, result.pixel_hash, result.width,
      result.height, result.nonzero_pixel_count, result.nonzero_bounds[0],
      result.nonzero_bounds[1], result.nonzero_bounds[2],
      result.nonzero_bounds[3], result.first_nonzero_rgba[0],
      result.first_nonzero_rgba[1], result.first_nonzero_rgba[2],
      result.first_nonzero_rgba[3], result.nonempty());
}

bool QueuePhase0RectangleReadback(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    const ExactMainPreparedTargets &targets, uint64_t backend_frame_sequence,
    PrivateReplayDiagnosticKind kind) {
  if (backend_frame_sequence == 0 || !targets.valid(context) ||
      g_resources.pending_submission != 0 ||
      !EnsureResources(context, targets.attachments.width,
                       targets.attachments.height)) {
    return false;
  }

  nrhi::TextureView *source = nullptr;
  if (PrepareExactMainTargetsForPrivateResolve(context, targets, source) !=
          ExactMainTargetsResult::kSucceeded ||
      source == nullptr) {
    return false;
  }

  nrhi::Cmd *const cmd = context.cmd;
  cmd->SetRenderTargets(g_resources.resolved, nullptr);
  nrhi::Viewport viewport;
  viewport.width = static_cast<float>(g_resources.width);
  viewport.height = static_cast<float>(g_resources.height);
  cmd->SetViewport(viewport);
  nrhi::Rect scissor;
  scissor.right = static_cast<int32_t>(g_resources.width);
  scissor.bottom = static_cast<int32_t>(g_resources.height);
  cmd->SetScissor(scissor);
  cmd->SetBindingLayout(g_resources.layout);
  cmd->SetPipeline(g_resources.pipeline);
  cmd->SetTexture(0, source);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  if (!cmd->PreflightDraw()) {
    RestoreExactMainTargetsAfterPrivateResolve(context);
    return false;
  }
  cmd->Draw(3, 0);
  cmd->Barrier(g_resources.resolved, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kCopySource);
  cmd->FlushBarriers();
  cmd->CopyTextureToBuffer(g_resources.readback, 0, g_resources.row_pitch,
                           g_resources.resolved, 0, g_resources.width,
                           g_resources.height);
  cmd->Barrier(g_resources.resolved, nrhi::ResourceState::kCopySource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  RestoreExactMainTargetsAfterPrivateResolve(context);
  g_resources.pending_submission = context.device->CurrentSubmission();
  g_resources.pending_backend_frame_sequence = backend_frame_sequence;
  g_resources.pending_kind = kind;
  return true;
}

Phase0RectangleReadbackResult LatestPhase0RectangleReadbackResult() {
  std::lock_guard lock(g_result_mutex);
  return g_latest_result;
}

void ShutdownPhase0RectangleReadback() {
  ReleaseResources();
  g_private_replay_dump_attempted = false;
  std::lock_guard lock(g_result_mutex);
  g_latest_result = {};
}

} // namespace tabletennis::native
