#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tabletennis::native {

// Value-only semantic ownership captured from synchronous title render
// scopes. Guest pointers are identities only and are never dereferenced after
// the scope which produced the token.
enum class SceneOwnerKind : uint8_t {
  kNone,
  kTable,
  kBall,
  kPaddle,
};

enum class SceneOwnerRole : uint8_t {
  kNone,
  kTableRenderable,
  kBallTransformSource,
  kBallNodeRenderable,
  kBallSpinRenderable,
  kBallSplashRenderable,
  kPaddleRenderable,
};

struct SceneOwnerToken {
  SceneOwnerKind kind = SceneOwnerKind::kNone;
  SceneOwnerRole role = SceneOwnerRole::kNone;
  uint32_t owner = 0;
  uint32_t renderable = 0;
  uint32_t renderable_vtable = 0;
  bool valid = false;
};

struct SceneOwnerIdentityStats {
  SceneOwnerToken token{};
  uint32_t queue_count = 0;
  uint32_t render_scope_count = 0;
  uint32_t catalog_draw_count = 0;
  uint32_t model_token_count = 0;
};

struct SceneOwnerObserverFrame {
  static constexpr size_t kMaxIdentities = 24;

  uint64_t sequence = 0;
  uint32_t table_render_scope_count = 0;
  uint32_t ball_submission_count = 0;
  uint32_t ball_transform_count = 0;
  uint32_t ball_render_scope_count = 0;
  uint32_t paddle_submission_count = 0;
  uint32_t paddle_render_scope_count = 0;
  uint32_t tagged_queue_count = 0;
  uint32_t unmatched_queue_count = 0;
  uint32_t tagged_catalog_draw_count = 0;
  uint32_t tagged_model_token_count = 0;
  uint32_t guest_read_failure_count = 0;
  uint32_t identity_count = 0;
  uint32_t dropped_identity_count = 0;
  std::array<SceneOwnerIdentityStats, kMaxIdentities> identities{};
};

bool SceneOwnerObserverEnabled();

// Proven render callbacks. lvlTable and gmBallNode embed their renderable at
// owner+0x60; pongPaddle is itself the renderable.
void BeginSceneOwnerTableRenderScope(uint8_t *guest_base,
                                     uint32_t renderable);
void BeginSceneOwnerBallNodeRenderScope(uint8_t *guest_base,
                                        uint32_t renderable);
void BeginSceneOwnerPaddleRenderScope(uint8_t *guest_base, uint32_t paddle);

// Ball effects are separately queued renderables. Their exact ball owner is
// learned at sub_82281C10 and resolved only for the same title frame.
void BeginSceneOwnerMappedBallRenderScope(uint8_t *guest_base,
                                          uint32_t renderable);
void EndSceneOwnerRenderScope();

// Bracket the title submission helpers before their originals. The nested
// pongDrawBucket::AddEntry hook confirms the exact queued identity.
void BeginSceneOwnerBallSubmission(uint8_t *guest_base, uint32_t ball);
void BeginSceneOwnerPaddleSubmission(uint8_t *guest_base, uint32_t paddle);
void EndSceneOwnerSubmission();
void ObserveSceneOwnerDrawBucketEntry(uint32_t renderable, uint32_t vtable);
void ObserveSceneOwnerBallTransform(uint32_t ball);

// Synchronous joins used by the generic scene/model observer streams.
SceneOwnerToken CurrentSceneOwnerToken();
void ObserveSceneOwnerCatalogDraw(const SceneOwnerToken &token);
void ObserveSceneOwnerModelTokens(const SceneOwnerToken &token,
                                  uint32_t token_count);

void SceneOwnerObserverFrameEnd();
SceneOwnerObserverFrame LatestSceneOwnerObserverFrame();

} // namespace tabletennis::native
