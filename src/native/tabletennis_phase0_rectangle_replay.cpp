#include "native/tabletennis_phase0_rectangle_replay.h"

#include <map>
#include <mutex>

#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kRectangleVertexShaderHash = 0x0A6D1DD7767FDF27ull;
constexpr uint64_t kRectanglePixelShaderHash = 0x2E372EA28CC404B7ull;
constexpr uint64_t kRectanglePayloadFingerprint = 0x48F89B1D4D072A58ull;
constexpr size_t kMaximumPendingPayloads = 3;

std::mutex g_mutex;
std::map<uint64_t, MainCoverageVertexColorRectangleProof> g_payloads;
std::shared_ptr<const Phase0RectangleReplaySnapshot> g_latest;
bool g_ready_announced = false;
bool g_join_announced = false;

bool ExactTokenContract(
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &token) {
  return token.valid &&
         token.backend ==
             rex::graphics::NativeGuestOutputBackend::kVulkan &&
         token.backend_frame_sequence != 0 &&
         token.vertex_shader_hash == kRectangleVertexShaderHash &&
         token.pixel_shader_hash == kRectanglePixelShaderHash &&
         token.guest_primitive_type == 8 &&
         token.guest_vertex_or_index_count == 3 &&
         token.host_vertex_or_index_count == 4 && token.indexed &&
         !token.guest_index_base_valid && token.color_attachment_count == 1 &&
         token.color_attachment_formats[0] ==
             nrhi::Format::kR8G8B8A8_UNORM &&
         token.depth_attachment_format == token.stencil_attachment_format &&
         (token.depth_attachment_format == nrhi::Format::kD24_UNORM_S8_UINT ||
          token.depth_attachment_format == nrhi::Format::kD32_FLOAT_S8_UINT) &&
         token.sample_count == 4 && token.sample_mask != 0;
}

TranslatedShaderArtifactKey MakeArtifactKey(uint64_t hash,
                                            uint64_t modification,
                                            TranslatedShaderStage stage) {
  return {
      .shader_hash = hash,
      .modification = modification,
      .stage = stage,
  };
}

} // namespace

bool Phase0RectangleReplaySnapshot::valid() const {
  if (backend_frame_sequence == 0) {
    return false;
  }
  if (payload.valid() &&
      payload.payload_fingerprint != kRectanglePayloadFingerprint) {
    return false;
  }
  if (replay_token != nullptr &&
      (!ExactTokenContract(*replay_token) ||
       replay_token->backend_frame_sequence != backend_frame_sequence)) {
    return false;
  }
  if (vertex_shader != nullptr &&
      (replay_token == nullptr || !vertex_shader->valid() ||
       vertex_shader->key !=
           MakeArtifactKey(kRectangleVertexShaderHash,
                           replay_token->vertex_shader_modification,
                           TranslatedShaderStage::kVertex) ||
       vertex_shader->used_texture_fetch_mask != 0)) {
    return false;
  }
  if (pixel_shader != nullptr &&
      (replay_token == nullptr || !pixel_shader->valid() ||
       pixel_shader->key !=
           MakeArtifactKey(kRectanglePixelShaderHash,
                           replay_token->pixel_shader_modification,
                           TranslatedShaderStage::kPixel) ||
       pixel_shader->used_texture_fetch_mask != 0)) {
    return false;
  }
  if (target_contract.valid() &&
      (replay_token == nullptr ||
       target_contract.depth_stencil_format !=
           replay_token->depth_attachment_format ||
       target_contract.sample_count != replay_token->sample_count ||
       target_contract.sample_mask != replay_token->sample_mask)) {
    return false;
  }
  return true;
}

bool Phase0RectangleReplaySnapshot::deferred_replay_safe() const {
  return replay_token != nullptr &&
         replay_token->resources_stable_for_deferred_replay;
}

void ObservePhase0RectangleDraw(
    const rex::graphics::NativeGuestDrawContext &context) {
  if (!TranslatedShaderArtifactStoreEnabled()) {
    return;
  }
  MainCoverageVertexColorRectangleProof payload =
      CaptureMainVertexColorRectangleProof(context);
  if (!payload.valid() ||
      payload.payload_fingerprint != kRectanglePayloadFingerprint ||
      context.backend_frame_sequence == 0) {
    return;
  }

  {
    std::lock_guard lock(g_mutex);
    g_payloads[context.backend_frame_sequence] = std::move(payload);
    while (g_payloads.size() > kMaximumPendingPayloads) {
      g_payloads.erase(g_payloads.begin());
    }
  }

  // The backend token is emitted immediately before the selective draw-state
  // observer reaches this payload tap. Re-run the same-frame join now that
  // both halves exist instead of waiting for a token from the next frame.
  const auto token =
      FindTranslatedRectangleReplayToken(context.backend_frame_sequence);
  if (token != nullptr) {
    ObservePhase0RectangleReplayToken(*token);
  }
}

void ObservePhase0RectangleReplayToken(
    const rex::graphics::NativeGuestTranslatedReplayTokenContext &context) {
  if (!TranslatedShaderArtifactStoreEnabled() ||
      !ExactTokenContract(context)) {
    return;
  }

  auto snapshot = std::make_shared<Phase0RectangleReplaySnapshot>();
  snapshot->backend_frame_sequence = context.backend_frame_sequence;
  snapshot->replay_token =
      FindTranslatedRectangleReplayToken(context.backend_frame_sequence);
  if (snapshot->replay_token != nullptr) {
    snapshot->missing = static_cast<Phase0RectangleReplayMissing>(
        static_cast<uint32_t>(snapshot->missing) &
        ~static_cast<uint32_t>(
            Phase0RectangleReplayMissing::kCurrentFrameToken));
  }

  const auto vertex_key =
      MakeArtifactKey(context.vertex_shader_hash,
                      context.vertex_shader_modification,
                      TranslatedShaderStage::kVertex);
  const auto pixel_key =
      MakeArtifactKey(context.pixel_shader_hash,
                      context.pixel_shader_modification,
                      TranslatedShaderStage::kPixel);
  snapshot->vertex_shader = FindTranslatedShaderArtifact(vertex_key);
  snapshot->pixel_shader = FindTranslatedShaderArtifact(pixel_key);
  if (snapshot->vertex_shader != nullptr) {
    snapshot->missing = static_cast<Phase0RectangleReplayMissing>(
        static_cast<uint32_t>(snapshot->missing) &
        ~static_cast<uint32_t>(
            Phase0RectangleReplayMissing::kTranslatedVertexArtifact));
  }
  if (snapshot->pixel_shader != nullptr) {
    snapshot->missing = static_cast<Phase0RectangleReplayMissing>(
        static_cast<uint32_t>(snapshot->missing) &
        ~static_cast<uint32_t>(
            Phase0RectangleReplayMissing::kTranslatedPixelArtifact));
  }

  bool announce_ready = false;
  {
    std::lock_guard lock(g_mutex);
    const auto payload = g_payloads.find(context.backend_frame_sequence);
    if (payload != g_payloads.end()) {
      snapshot->payload = payload->second;
      snapshot->missing = static_cast<Phase0RectangleReplayMissing>(
          static_cast<uint32_t>(snapshot->missing) &
          ~static_cast<uint32_t>(
              Phase0RectangleReplayMissing::kCapturedPayload));
    }
    snapshot->target_contract = {
        .depth_stencil_format = context.depth_attachment_format,
        .sample_count = context.sample_count,
        .sample_mask = context.sample_mask,
    };
    if (snapshot->target_contract.valid()) {
      snapshot->missing = static_cast<Phase0RectangleReplayMissing>(
          static_cast<uint32_t>(snapshot->missing) &
          ~static_cast<uint32_t>(
              Phase0RectangleReplayMissing::kExactMainTargetContract));
    }
    // Target preparation evidence is callback-scoped and is never inherited
    // by a later token snapshot.
    snapshot->private_targets_prepared_in_output_context = false;
    g_latest = snapshot;
    announce_ready =
        snapshot->observer_inputs_ready() && !g_ready_announced;
    g_ready_announced |= snapshot->observer_inputs_ready();
  }

  if (announce_ready) {
    REXLOG_INFO(
        "Table Tennis phase-0 rectangle observer join ready frame={} "
        "deferred_replay_safe={} replay_ready={} observer_only=true "
        "private_target=true",
        context.backend_frame_sequence, snapshot->deferred_replay_safe(),
        snapshot->replay_ready());
  }
  bool announce_join = false;
  {
    std::lock_guard lock(g_mutex);
    announce_join = !g_join_announced;
    g_join_announced = true;
  }
  if (announce_join) {
    REXLOG_INFO(
        "Table Tennis phase-0 rectangle join frame={} payload={} "
        "vs_artifact={} ps_artifact={} target_contract={} "
        "depth_format={} samples={} sample_mask={:016X} "
        "private_targets={} missing={:08X} observer_only=true",
        context.backend_frame_sequence, snapshot->payload.valid(),
        snapshot->vertex_shader != nullptr, snapshot->pixel_shader != nullptr,
        snapshot->target_contract.valid(),
        static_cast<uint32_t>(context.depth_attachment_format),
        context.sample_count, context.sample_mask,
        (static_cast<uint32_t>(snapshot->missing) &
         static_cast<uint32_t>(
             Phase0RectangleReplayMissing::kPrivateTargetsPrepared)) == 0,
        static_cast<uint32_t>(snapshot->missing));
  }
}

ExactMainTargetsResult PreparePhase0RectanglePrivateTargets(
    const rex::graphics::NativeGuestOutputRenderContext &context,
    ExactMainPreparedTargets &targets_out) {
  targets_out = {};
  std::shared_ptr<Phase0RectangleReplaySnapshot> updated;
  ExactMainTargetContract contract;
  {
    std::lock_guard lock(g_mutex);
    if (g_latest == nullptr || !g_latest->target_contract.valid()) {
      return ExactMainTargetsResult::kInvalidContract;
    }
    contract = g_latest->target_contract;
  }

  ExactMainPreparedTargets prepared;
  const ExactMainTargetsResult result =
      PrepareExactMainTargets(context, contract, prepared);
  if (result != ExactMainTargetsResult::kSucceeded) {
    return result;
  }
  if (!prepared.valid(context)) {
    return ExactMainTargetsResult::kInvalidTargets;
  }
  targets_out = prepared;

  bool announce_ready = false;
  {
    std::lock_guard lock(g_mutex);
    if (g_latest != nullptr && g_latest->target_contract.valid() &&
        g_latest->target_contract.depth_stencil_format ==
            contract.depth_stencil_format &&
        g_latest->target_contract.sample_count == contract.sample_count &&
        g_latest->target_contract.sample_mask == contract.sample_mask) {
      updated =
          std::make_shared<Phase0RectangleReplaySnapshot>(*g_latest);
      updated->private_targets_prepared_in_output_context = true;
      updated->missing = static_cast<Phase0RectangleReplayMissing>(
          static_cast<uint32_t>(updated->missing) &
          ~static_cast<uint32_t>(
              Phase0RectangleReplayMissing::kPrivateTargetsPrepared));
      g_latest = updated;
      announce_ready =
          updated->observer_inputs_ready() && !g_ready_announced;
      g_ready_announced |= updated->observer_inputs_ready();
    }
  }
  if (announce_ready) {
    REXLOG_INFO(
        "Table Tennis phase-0 rectangle observer join ready frame={} "
        "deferred_replay_safe={} replay_ready={} observer_only=true "
        "private_target=true",
        updated->backend_frame_sequence, updated->deferred_replay_safe(),
        updated->replay_ready());
  }
  return result;
}

std::shared_ptr<const Phase0RectangleReplaySnapshot>
LatestPhase0RectangleReplaySnapshot() {
  std::lock_guard lock(g_mutex);
  return g_latest;
}

void ShutdownPhase0RectangleReplay() {
  std::lock_guard lock(g_mutex);
  g_payloads.clear();
  g_latest.reset();
  g_ready_announced = false;
  g_join_announced = false;
}

} // namespace tabletennis::native
