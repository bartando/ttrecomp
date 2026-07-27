#include "native/tabletennis_venue_diagnostic_suppression.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_venue_ps328_diagnostic_suppress_guest, false,
    "Table Tennis",
    "Benchmark only: suppress the exact complete PS328/0E99 Vulkan gameplay "
    "family without replacement rendering. Keep disabled outside controlled "
    "performance measurements.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint64_t kVenueVertexShaderHash = 0x0E9982BE6B1E99A1ull;
constexpr uint64_t kVenuePixelShaderHash = 0x328FA02B07C392DCull;
constexpr uint32_t kGameplayRenderPassKey = 0x0000000E;
constexpr uint32_t kGameplaySurfacePitch = 1280;
constexpr uint32_t kTriangleStripPrimitive = 0x06;
constexpr uint32_t kVenueDepthControl = 0x00700736;
constexpr uint32_t kVenueColorMask = 0x00000007;

uint64_t g_suppressed_draws = 0;
uint64_t g_suppressed_indices = 0;
bool g_announced_suppression = false;

bool HasExactVenueAttachments(
    const rex::graphics::NativeGuestDrawContext& context) {
  const rex::graphics::nrhi::Format depth_format =
      context.depth_attachment_format;
  const bool depth_format_supported =
      depth_format ==
          rex::graphics::nrhi::Format::kD24_UNORM_S8_UINT ||
      depth_format ==
          rex::graphics::nrhi::Format::kD32_FLOAT_S8_UINT;
  return context.borrowed_attachment_contract_valid &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] ==
             rex::graphics::nrhi::Format::kR8G8B8A8_UNORM &&
         depth_format_supported &&
         context.stencil_attachment_format == depth_format &&
         context.sample_count == 4 && context.sample_mask == UINT64_MAX;
}

}  // namespace

bool MatchVenueDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context, void*) {
  return REXCVAR_GET(
             tabletennis_native_venue_ps328_diagnostic_suppress_guest) &&
         context.backend ==
             rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.render_pass_key_valid &&
         context.render_pass_key == kGameplayRenderPassKey &&
         context.surface_pitch == kGameplaySurfacePitch &&
         context.indexed && context.guest_index_base_valid &&
         context.guest_index_base != 0 &&
         context.draw_state_contract_valid &&
         HasExactVenueAttachments(context) &&
         context.vertex_shader_hash == kVenueVertexShaderHash &&
         context.pixel_shader_hash == kVenuePixelShaderHash &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.vertex_or_index_count != 0 &&
         context.normalized_depth_control == kVenueDepthControl &&
         context.normalized_color_mask == kVenueColorMask &&
         !context.primitive_restart_enabled;
}

bool RenderVenueDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context, void*) {
  if (context.cmd == nullptr) {
    return false;
  }

  ++g_suppressed_draws;
  g_suppressed_indices += context.vertex_or_index_count;
  if (!g_announced_suppression) {
    g_announced_suppression = true;
    REXLOG_WARN(
        "Table Tennis PS328 diagnostic: suppressing the exact complete "
        "venue family without replacement; benchmark_only=true "
        "pass={:08X} pitch={} primitive={} depth={:08X} mask={:08X} "
        "color_control={:08X} blend0={:08X}",
        context.render_pass_key, context.surface_pitch,
        context.primitive_type, context.normalized_depth_control,
        context.normalized_color_mask, context.color_control,
        context.blend_control_0);
  }
  if ((g_suppressed_draws & 0xFFFu) == 0) {
    REXLOG_INFO(
        "Table Tennis PS328 diagnostic: suppressed_draws={} "
        "suppressed_indices={} benchmark_only=true",
        g_suppressed_draws, g_suppressed_indices);
  }
  return true;
}

}  // namespace tabletennis::native
