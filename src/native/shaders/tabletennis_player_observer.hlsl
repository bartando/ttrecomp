#include "tabletennis_player_common.hlsli"

cbuffer PlayerConstants : register(b0) {
  float4 player_world_to_clip[4];
  float4 player_camera_position;
  float4 player_light_position_radius[3];
  float4 player_light_color_intensity[3];
  float4 player_light_mode[3];
  float4 player_pixel_constants[23];
  float4 player_pixel_control_254;
  float4 player_pixel_control_255;
  // x: vf95 base byte, y: vf92 base byte, z: vf92 record count,
  // w: vf95 stride (44 in the proven family).
  uint4 player_buffer_layout;
};

#if defined(TABLETENNIS_VULKAN)
// NRHI places constant/raw buffers and immutable samplers in set 0, then
// gives the texture table its own set 1. Bindings follow layout-param order.
// Keep the two storage-buffer element types distinct. Glslang otherwise
// merges two ByteAddressBuffer block types, and SPIRV-Cross generates an MSL
// argument-buffer member whose variable hides the shared block type before
// the second member uses it.
StructuredBuffer<uint> player_vertex_words : register(t1, space0);
StructuredBuffer<uint2> player_palette_word_pairs : register(t2, space0);
Texture2D player_texture0 : register(t0, space1);
Texture2D player_texture1 : register(t1, space1);
Texture2D player_texture2 : register(t2, space1);
SamplerState player_sampler0 : register(s3, space0);
SamplerState player_sampler1 : register(s4, space0);
SamplerState player_sampler2 : register(s5, space0);
#else
StructuredBuffer<uint> player_vertex_words : register(t0);
StructuredBuffer<uint2> player_palette_word_pairs : register(t1);
Texture2D player_texture0 : register(t2);
Texture2D player_texture1 : register(t3);
Texture2D player_texture2 : register(t4);
SamplerState player_sampler0 : register(s0);
SamplerState player_sampler1 : register(s1);
SamplerState player_sampler2 : register(s2);
#endif

#include "tabletennis_player_skinning.hlsli"
#include "tabletennis_player_material.hlsli"
#include "tabletennis_player_coverage.hlsli"

PlayerVaryings vs_main(uint vertex_id : SV_VertexID) {
  return PlayerSkinVertex(vertex_id);
}

PlayerPrepassOutput ps_prepass(PlayerVaryings input) {
  PlayerPrepassOutput output;
  output.color = PlayerShadeMaterial(input);
  output.coverage =
      PlayerXenos4xCoverageMask(output.color.a, input.position.xy);
  // Match the translator's explicit zero-mask kill before writing SampleMask.
  if (output.coverage == 0u) {
    discard;
  }
  return output;
}

float4 ps_color(PlayerVaryings input) : SV_Target {
  const float4 shaded = PlayerShadeMaterial(input);
  // Exact RB_ALPHA_REF used by all six trace-verified blended-color draws.
  // The guest comparison is GREATER, so equality is rejected too.
  if (shaded.a <= asfloat(0x3D808081u)) {
    discard;
  }
  return shaded;
}
