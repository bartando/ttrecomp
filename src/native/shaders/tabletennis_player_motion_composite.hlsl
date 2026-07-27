// Port of the title's 2AC059EB5C7A942F player motion-composite pixel shader.
//
// The vertex ports are deliberately separate: the title uses four vertex
// variants for this pass. This file owns only the shared, trace-proven pixel
// behavior.

cbuffer PlayerMotionCompositeConstants : register(b0) {
  // xy: reciprocal scene dimensions.
  // zw: 0.5 + half a texel, matching title pixel constant c37.
  float4 player_motion_scene_scale;
};

#if defined(TABLETENNIS_VULKAN)
Texture2D<float4> player_motion_scene : register(t0, space1);
SamplerState player_motion_sampler : register(s1, space0);
#else
Texture2D<float4> player_motion_scene : register(t0);
SamplerState player_motion_sampler : register(s0);
#endif

struct PlayerMotionCompositeVaryings {
  // xy: screen-space motion vector, z: interpolated coverage threshold.
  float4 motion_coverage : TEXCOORD0;
  // xy / w: projected scene coordinate before the title's half-screen offset.
  float4 projected_scene : TEXCOORD1;
};

float4 ps_main(PlayerMotionCompositeVaryings input) : SV_Target {
  const float2 motion = input.motion_coverage.xy;
  const float coverage_threshold = input.motion_coverage.z;
  const float2 scene_uv =
      input.projected_scene.xy / input.projected_scene.w +
      player_motion_scene_scale.zw;

  // The title selects the single-tap path only when half the absolute motion
  // is strictly below one texel on both axes.
  const float2 half_motion = abs(motion) * 0.5f;
  if (player_motion_scene_scale.x > half_motion.x &&
      player_motion_scene_scale.y > half_motion.y) {
    const float4 scene =
        player_motion_scene.SampleLevel(player_motion_sampler, scene_uv, 0.0f);
    return float4(
        scene.rgb, scene.a >= coverage_threshold ? 1.0f : 0.0f);
  }

  float3 color_sum = 0.0f;
  uint accepted_count = 0u;
  [unroll]
  for (uint sample_index = 0u; sample_index < 12u; ++sample_index) {
    const float fraction = float(sample_index) * (1.0f / 12.0f);
    const float4 scene = player_motion_scene.SampleLevel(
        player_motion_sampler, scene_uv + motion * fraction, 0.0f);
    if (scene.a >= coverage_threshold) {
      color_sum += scene.rgb;
      ++accepted_count;
    }
  }

  // Valid title silhouettes always accept at least one tap. Keep malformed
  // observer input deterministic instead of manufacturing NaNs.
  if (accepted_count == 0u) {
    return 0.0f;
  }
  return float4(
      color_sum / float(accepted_count), float(accepted_count) / 12.0f);
}
