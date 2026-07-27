#pragma once

#include "native/tabletennis_player_2ac_observer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

// CPU-side proof that the exact indexed subset of one captured vf95 payload
// decodes under the vertex contract selected by the live bound shader.
// Geometry is never synthesized: bounds, palette references and the
// fingerprint are derived only from immutable guest bytes.
struct Player2ACDecodedDrawGeometry {
  Player2ACTitleKind kind = Player2ACTitleKind::kUnknown;
  uint32_t ordinal = 0;
  uint32_t primitive_type = 0;
  uint32_t rasterizer_mode_control = 0;
  uint32_t vertex_stride = 0;
  uint32_t source_vertex_count = 0;
  uint32_t submitted_index_count = 0;
  uint32_t referenced_vertex_count = 0;
  uint32_t palette_record_count = 0;
  uint32_t maximum_referenced_palette_record = 0;
  std::array<float, 3> bounds_min{};
  std::array<float, 3> bounds_max{};
  uint64_t decoded_fingerprint = 0;
  bool skinned = false;
  bool valid = false;
};

enum class Player2ACCompositeBlocker : uint8_t {
  kNone = 0,
  kMissingFrame,
  kInvalidFrame,
  kInvalidPayload,
  kGeometryDecodeFailed,
  // The shared pixel port exists. Exact native ports of all four bound title
  // vertex programs do not, and substituting a generic player transform would
  // produce wrong motion vectors and coverage.
  kMissingVertexProgramPorts,
  // 2AC samples the resolved MAIN scene. The current private MAIN pass has no
  // immutable resolved-scene handoff to a COMP transaction yet.
  kMissingResolvedSceneInput,
  // COMP is a distinct one-sample pass, not a draw family inside the shared
  // 4x MAIN pass. It needs its own ordered begin/record/resolve transaction.
  kMissingCompositeTransaction,
};

struct Player2ACCompositeReadiness {
  uint64_t sequence = 0;
  uint32_t draw_count = 0;
  uint32_t total_index_count = 0;
  uint32_t decoded_vertex_reference_count = 0;
  std::array<uint32_t, 4> draws_by_kind{};
  Player2ACCompositeBlocker blocker = Player2ACCompositeBlocker::kMissingFrame;
  bool snapshot_exact = false;
  bool payloads_exact = false;
  bool constants_captured = false;
  bool geometry_decoded = false;
  bool order_preserved = false;
  bool pixel_program_port_available = true;
  bool vertex_program_ports_available = false;
  bool resolved_scene_input_available = false;
  bool composite_transaction_available = false;
  bool gpu_recording_ready = false;
};

// Observer-owned preparation result. A valid plan proves that the real 2AC
// geometry payloads decode in exact draw order. It deliberately does not mean
// the pass is safe to record while the explicit blockers above remain.
struct Player2ACCompositeRenderPlan {
  std::shared_ptr<const Player2ACFrameSnapshot> frame;
  Player2ACCompositeReadiness readiness{};
  std::vector<Player2ACDecodedDrawGeometry> draws;

  bool observer_valid() const {
    return frame != nullptr && readiness.snapshot_exact &&
           readiness.payloads_exact && readiness.constants_captured &&
           readiness.geometry_decoded && readiness.order_preserved &&
           draws.size() == readiness.draw_count;
  }
};

// Decodes and validates only vertices referenced by each captured index
// payload. The function allocates no GPU resources, records no commands and
// can never suppress guest rendering.
Player2ACCompositeRenderPlan BuildPlayer2ACCompositeRenderPlan(
    std::shared_ptr<const Player2ACFrameSnapshot> frame);

// Hot observer driver. Enabling it also arms the underlying 2AC capture, then
// publishes one decoded immutable plan per newly joined frame.
bool Player2ACCompositeObserverEnabled();
void Player2ACCompositeObserverFrameEnd();
std::shared_ptr<const Player2ACCompositeRenderPlan>
LatestPlayer2ACCompositeRenderPlan();

const char *Player2ACCompositeBlockerName(Player2ACCompositeBlocker blocker);

} // namespace tabletennis::native
