#include "native/tabletennis_translated_shader_artifact_store.h"

#include "native/tabletennis_phase0_rectangle_replay.h"
#include "native/tabletennis_ps328_tile_invariance.h"

#include <map>
#include <mutex>
#include <set>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_translated_shader_artifacts, false, "Table Tennis",
    "Capture only explicitly requested exact translated shader artifacts for "
    "generic logical-draw replay (observer-only; requires restart).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace tabletennis::native {
namespace {

std::mutex g_mutex;
std::set<TranslatedShaderArtifactKey> g_requests;
std::map<TranslatedShaderArtifactKey,
         std::shared_ptr<const TranslatedShaderArtifact>>
    g_artifacts;
bool g_installed = false;
std::shared_ptr<const rex::graphics::NativeGuestTranslatedReplayTokenContext>
    g_rectangle_replay_token;
bool g_rectangle_replay_token_announced = false;

constexpr uint64_t kRectangleVertexShaderHash = 0x0A6D1DD7767FDF27ull;
constexpr uint64_t kRectanglePixelShaderHash = 0x2E372EA28CC404B7ull;

TranslatedShaderArtifactKey MakeKey(uint64_t shader_hash, uint64_t modification,
                                    bool is_vertex_shader) {
  return {
      .shader_hash = shader_hash,
      .modification = modification,
      .stage = is_vertex_shader ? TranslatedShaderStage::kVertex
                                : TranslatedShaderStage::kPixel,
  };
}

bool FilterArtifact(uint64_t shader_hash, uint64_t modification,
                    bool is_vertex_shader, void *) {
  const TranslatedShaderArtifactKey key =
      MakeKey(shader_hash, modification, is_vertex_shader);
  std::lock_guard lock(g_mutex);
  return g_requests.contains(key) && !g_artifacts.contains(key);
}

void ObserveArtifact(
    const rex::graphics::NativeGuestShaderArtifactContext &context, void *) {
  const TranslatedShaderArtifactKey key = MakeKey(
      context.shader_hash, context.modification, context.is_vertex_shader);
  {
    std::lock_guard lock(g_mutex);
    if (!g_requests.contains(key) || g_artifacts.contains(key)) {
      return;
    }
  }

  auto artifact = std::make_shared<TranslatedShaderArtifact>();
  artifact->key = key;
  artifact->backend = context.backend;
  artifact->spirv = context.spirv;
  artifact->used_texture_fetch_mask = context.used_texture_fetch_mask;
  artifact->texture_bindings = context.texture_bindings;
  artifact->sampler_bindings = context.sampler_bindings;
  artifact->descriptor_set_shared_memory_and_edram =
      context.descriptor_set_shared_memory_and_edram;
  artifact->descriptor_set_constants = context.descriptor_set_constants;
  artifact->descriptor_set_textures_vertex =
      context.descriptor_set_textures_vertex;
  artifact->descriptor_set_textures_pixel =
      context.descriptor_set_textures_pixel;
  artifact->descriptor_set_count = context.descriptor_set_count;
  artifact->shared_memory_binding = context.shared_memory_binding;
  artifact->edram_binding = context.edram_binding;
  artifact->constant_buffer_system_binding =
      context.constant_buffer_system_binding;
  artifact->constant_buffer_float_vertex_binding =
      context.constant_buffer_float_vertex_binding;
  artifact->constant_buffer_float_pixel_binding =
      context.constant_buffer_float_pixel_binding;
  artifact->constant_buffer_bool_loop_binding =
      context.constant_buffer_bool_loop_binding;
  artifact->constant_buffer_fetch_binding =
      context.constant_buffer_fetch_binding;
  artifact->constant_buffer_count = context.constant_buffer_count;
  if (!artifact->valid()) {
    return;
  }

  {
    std::lock_guard lock(g_mutex);
    if (!g_requests.contains(key)) {
      return;
    }
    g_artifacts.emplace(key, artifact);
  }
  REXLOG_INFO("Table Tennis replay artifact: captured hash={:016X} "
              "modification={:016X} "
              "stage={} words={} observer_only=true",
              key.shader_hash, key.modification,
              key.stage == TranslatedShaderStage::kVertex ? "VS" : "PS",
              artifact->spirv.size());
}

bool FilterReplayToken(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                       void *) {
  return (vertex_shader_hash == kRectangleVertexShaderHash &&
          pixel_shader_hash == kRectanglePixelShaderHash) ||
         FilterPs328TileInvarianceToken(vertex_shader_hash, pixel_shader_hash);
}

void ObserveReplayToken(
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &context,
    void *) {
  if (FilterPs328TileInvarianceToken(context.vertex_shader_hash,
                                     context.pixel_shader_hash)) {
    if (Ps328TitleCaptureRetired()) {
      // Keep the filter installed so RexGlue still produces guarded
      // current-frame replay tokens. Only the now-redundant PS328 title proof
      // publication and artifact-request churn are retired.
      return;
    }
    {
      std::lock_guard lock(g_mutex);
      g_requests.insert({
          .shader_hash = context.vertex_shader_hash,
          .modification = context.vertex_shader_modification,
          .stage = TranslatedShaderStage::kVertex,
      });
      g_requests.insert({
          .shader_hash = context.pixel_shader_hash,
          .modification = context.pixel_shader_modification,
          .stage = TranslatedShaderStage::kPixel,
      });
    }
    ObservePs328TileInvarianceToken(context);
    return;
  }
  if (!context.valid ||
      context.vertex_shader_hash != kRectangleVertexShaderHash ||
      context.pixel_shader_hash != kRectanglePixelShaderHash) {
    return;
  }

  auto token =
      std::make_shared<rex::graphics::NativeGuestTranslatedReplayTokenContext>(
          context);
  bool announce = false;
  {
    std::lock_guard lock(g_mutex);
    g_rectangle_replay_token = std::move(token);
    announce = !g_rectangle_replay_token_announced;
    g_rectangle_replay_token_announced = true;
    g_requests.insert({
        .shader_hash = context.vertex_shader_hash,
        .modification = context.vertex_shader_modification,
        .stage = TranslatedShaderStage::kVertex,
    });
    g_requests.insert({
        .shader_hash = context.pixel_shader_hash,
        .modification = context.pixel_shader_modification,
        .stage = TranslatedShaderStage::kPixel,
    });
  }
  ObservePhase0RectangleReplayToken(context);
  if (announce) {
    REXLOG_INFO(
        "Table Tennis replay token: captured rectangle frame={} token={} "
        "vs_mod={:016X} ps_mod={:016X} host_primitive={} host_count={} "
        "observer_only=true",
        context.backend_frame_sequence, context.opaque_token,
        context.vertex_shader_modification, context.pixel_shader_modification,
        context.host_primitive_type, context.host_vertex_or_index_count);
  }
}

} // namespace

bool TranslatedShaderArtifact::valid() const {
  constexpr uint32_t kSpirvMagic = 0x07230203;
  return key.valid() &&
         backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         spirv.size() >= 5 && spirv.front() == kSpirvMagic &&
         descriptor_set_count != 0 && constant_buffer_count != 0;
}

bool TranslatedShaderArtifactStoreEnabled() {
  return REXCVAR_GET(tabletennis_native_translated_shader_artifacts);
}

void InstallTranslatedShaderArtifactStore() {
  if (!TranslatedShaderArtifactStoreEnabled()) {
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    if (g_installed) {
      return;
    }
    g_installed = true;
  }
  rex::graphics::SetNativeGuestShaderArtifactObserver(&ObserveArtifact,
                                                      &FilterArtifact, nullptr);
  rex::graphics::SetNativeGuestTranslatedReplayTokenObserver(
      &ObserveReplayToken, &FilterReplayToken, nullptr);
  REXLOG_INFO("Table Tennis translated shader artifact store installed "
              "(exact-key + rectangle replay-token observer-only)");
}

void ShutdownTranslatedShaderArtifactStore() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_installed) {
      return;
    }
    g_installed = false;
    g_requests.clear();
    g_artifacts.clear();
    g_rectangle_replay_token.reset();
    g_rectangle_replay_token_announced = false;
  }
  ResetPs328TileInvariance();
  rex::graphics::SetNativeGuestTranslatedReplayTokenObserver(nullptr, nullptr,
                                                             nullptr);
  rex::graphics::SetNativeGuestShaderArtifactObserver(nullptr, nullptr,
                                                      nullptr);
}

void RequestTranslatedShaderArtifact(TranslatedShaderArtifactKey key) {
  if (!key.valid()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_requests.insert(key);
}

std::shared_ptr<const TranslatedShaderArtifact>
FindTranslatedShaderArtifact(TranslatedShaderArtifactKey key) {
  if (!key.valid()) {
    return nullptr;
  }
  std::lock_guard lock(g_mutex);
  const auto found = g_artifacts.find(key);
  return found != g_artifacts.end() ? found->second : nullptr;
}

std::shared_ptr<const rex::graphics::NativeGuestTranslatedReplayTokenContext>
FindTranslatedRectangleReplayToken(uint64_t backend_frame_sequence) {
  if (backend_frame_sequence == 0) {
    return nullptr;
  }
  std::lock_guard lock(g_mutex);
  if (g_rectangle_replay_token == nullptr || !g_rectangle_replay_token->valid ||
      g_rectangle_replay_token->backend_frame_sequence !=
          backend_frame_sequence) {
    return nullptr;
  }
  return g_rectangle_replay_token;
}

} // namespace tabletennis::native
