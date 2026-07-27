#include "native/tabletennis_crowd_diagnostic_suppression.h"

#include "native/tabletennis_crowd_observer.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_crowd_diagnostic_suppress_guest, false,
    "Table Tennis",
    "Benchmark only: suppress exact C6/BD4 guest crowd draws without "
    "serving an in-order replacement. Keep disabled outside controlled "
    "observer measurements.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

constexpr uint64_t kCrowdVertexShaderHash = 0xBD4B1DF972B828B7ull;
constexpr uint64_t kCrowdPixelShaderHash = 0xC6CEFDA3753CF2BAull;
constexpr uint32_t kTriangleStripPrimitive = 0x06;
bool g_announced_suppression = false;

}  // namespace

bool MatchCrowdDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context, void*) {
  return REXCVAR_GET(
             tabletennis_native_crowd_diagnostic_suppress_guest) &&
         CrowdObserverEnabled() &&
         context.backend ==
             rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.indexed && context.guest_index_base_valid &&
         context.vertex_shader_hash == kCrowdVertexShaderHash &&
         context.pixel_shader_hash == kCrowdPixelShaderHash &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.vertex_or_index_count != 0 &&
         context.guest_index_base != 0;
}

bool RenderCrowdDiagnosticSuppression(
    const rex::graphics::NativeGuestDrawContext& context, void*) {
  if (context.cmd == nullptr) {
    return false;
  }
  if (!g_announced_suppression) {
    g_announced_suppression = true;
    REXLOG_WARN(
        "Table Tennis crowd diagnostic: suppressing exact C6/BD4 guest "
        "draws without in-order serving; benchmark_only=true");
  }
  return true;
}

}  // namespace tabletennis::native
