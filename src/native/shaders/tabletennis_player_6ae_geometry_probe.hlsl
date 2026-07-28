// Observer-only geometry tracer for the BBB580AA5620D2A6 / 6AE43640A86B33D8
// player family. This ports the real 36-byte skinned vertex path and the
// instruction-19..59 normal/diffuse material stage. It is deliberately not
// the final 6AE pixel-material port and must never suppress a guest draw.

cbuffer Player6AEProbeConstants : register(b0) {
  float4 player_6ae_world_to_clip[4];
  float4 player_6ae_vertex_constant_19;
  float4 player_6ae_vertex_constants_29_36[8];
  float4 player_6ae_vertex_constant_46;
  float4 player_6ae_vertex_constant_47;
  float4 player_6ae_vertex_constant_255;
  float4 player_6ae_pixel_constant_19;
  float4 player_6ae_pixel_constants_21_27[7];
  float4 player_6ae_pixel_constants_46_70[25];
  float4 player_6ae_pixel_constants_254_255[2];
  // x: vf95 base byte, y: vf92 base byte, z: vf92 record count,
  // w: exact vf95 stride (36).
  uint4 player_6ae_buffer_layout;
  float4 player_6ae_probe_tint;
};

#if defined(TABLETENNIS_VULKAN)
StructuredBuffer<uint> player_6ae_vertex_words : register(t1, space0);
StructuredBuffer<uint2> player_6ae_palette_word_pairs : register(t2, space0);
Texture2D player_6ae_texture_tf0 : register(t0, space1);
Texture2D player_6ae_texture_tf1 : register(t1, space1);
Texture2D player_6ae_texture_tf2 : register(t2, space1);
Texture2D player_6ae_texture_tf3 : register(t3, space1);
Texture2D player_6ae_texture_tf4 : register(t4, space1);
Texture2D player_6ae_texture_tf5 : register(t5, space1);
SamplerState player_6ae_wrap_sampler : register(s3, space0);
SamplerState player_6ae_point_clamp_sampler : register(s4, space0);
SamplerState player_6ae_linear_clamp_sampler : register(s5, space0);
#else
StructuredBuffer<uint> player_6ae_vertex_words : register(t0);
StructuredBuffer<uint2> player_6ae_palette_word_pairs : register(t1);
Texture2D player_6ae_texture_tf0 : register(t2);
Texture2D player_6ae_texture_tf1 : register(t3);
Texture2D player_6ae_texture_tf2 : register(t4);
Texture2D player_6ae_texture_tf3 : register(t5);
Texture2D player_6ae_texture_tf4 : register(t6);
Texture2D player_6ae_texture_tf5 : register(t7);
SamplerState player_6ae_wrap_sampler : register(s0);
SamplerState player_6ae_point_clamp_sampler : register(s1);
SamplerState player_6ae_linear_clamp_sampler : register(s2);
#endif

static const uint kPlayer6AEVertexStride = 36u;
static const uint kPlayer6AEPaletteStride = 28u;

struct Player6AEBoneBasis {
  float3 column0;
  float3 column1;
  float3 column2;
  float3 translation;
};

uint Player6AEByteSwap32(uint value) {
  return ((value & 0x000000FFu) << 24u) |
         ((value & 0x0000FF00u) << 8u) |
         ((value & 0x00FF0000u) >> 8u) |
         ((value & 0xFF000000u) >> 24u);
}

uint Player6AELoadVertexWord(uint byte_address) {
  return Player6AEByteSwap32(
      player_6ae_vertex_words[byte_address >> 2u]);
}

float Player6AELoadVertexFloat(uint byte_address) {
  return asfloat(Player6AELoadVertexWord(byte_address));
}

uint Player6AELoadPaletteWord(uint byte_address) {
  const uint word_index = byte_address >> 2u;
  const uint2 pair =
      player_6ae_palette_word_pairs[word_index >> 1u];
  return Player6AEByteSwap32(
      (word_index & 1u) != 0u ? pair.y : pair.x);
}

float Player6AELoadPaletteFloat(uint byte_address) {
  return asfloat(Player6AELoadPaletteWord(byte_address));
}

Player6AEBoneBasis Player6AELoadBone(uint bone_index) {
  const uint base =
      player_6ae_buffer_layout.y +
      bone_index * kPlayer6AEPaletteStride;
  const float4 quaternion = float4(
      Player6AELoadPaletteFloat(base + 0u),
      Player6AELoadPaletteFloat(base + 4u),
      Player6AELoadPaletteFloat(base + 8u),
      Player6AELoadPaletteFloat(base + 12u));
  const float3 translation = float3(
      Player6AELoadPaletteFloat(base + 16u),
      Player6AELoadPaletteFloat(base + 20u),
      Player6AELoadPaletteFloat(base + 24u));

  const float x = quaternion.x;
  const float y = quaternion.y;
  const float z = quaternion.z;
  const float w = quaternion.w;
  // Keep the BBB580AA5620D2A6 ALU sequence intact. In particular, the guest
  // reconstructs the third column as the cross product of the first two
  // rather than evaluating the generic closed-form quaternion matrix.
  const float r9x = 2.0f * (x * y - z * w);
  const float r9y = 2.0f * (x * z - y * w);
  const float r9z = 1.0f - 2.0f * (y * y + z * z);
  const float r2x = 2.0f * (x * y + z * w);
  const float r2y = 2.0f * (y * z + x * w);
  const float r2w = 1.0f - 2.0f * (x * x + z * z);
  const float r0x = r2x * r2y - r9y * r2w;
  const float r0z = r9y * r9x - r9z * r2y;
  const float r0y = -r2x * r9x + r9z * r2w;

  Player6AEBoneBasis bone;
  bone.column0 = float3(r9z, r2x, r9y);
  bone.column1 = float3(r9x, r2w, r2y);
  bone.column2 = float3(r0x, r0z, r0y);
  bone.translation = translation;
  return bone;
}

Player6AEBoneBasis Player6AEBlendBones(
    float4 weights, uint4 bone_indices) {
  Player6AEBoneBasis blended;
  blended.column0 = 0.0f;
  blended.column1 = 0.0f;
  blended.column2 = 0.0f;
  blended.translation = 0.0f;

  [unroll]
  for (uint influence = 0u; influence < 4u; ++influence) {
    if (weights[influence] <= 0.0f ||
        bone_indices[influence] >= player_6ae_buffer_layout.z) {
      continue;
    }
    const Player6AEBoneBasis bone =
        Player6AELoadBone(bone_indices[influence]);
    blended.column0 += bone.column0 * weights[influence];
    blended.column1 += bone.column1 * weights[influence];
    blended.column2 += bone.column2 * weights[influence];
    blended.translation += bone.translation * weights[influence];
  }
  return blended;
}

float3 Player6AETransformDirection(
    Player6AEBoneBasis basis, float3 value) {
  return basis.column0 * value.x +
         basis.column1 * value.y +
         basis.column2 * value.z;
}

float4 Player6AETransformPosition(float3 world_position) {
  // BBB580AA5620D2A6 instructions 132-137:
  //   r2 = z * c13.wzyx
  //   r5 = y * c12.xzwy + r2.wyxz
  //   r5 = x * c14.yzxw + r5.wyxz
  //   oPos = r5.zxyw + c15
  // Resolving those temporary swizzles gives this exact row selection.
  return world_position.x * player_6ae_world_to_clip[2] +
         world_position.y * player_6ae_world_to_clip[0] +
         world_position.z * player_6ae_world_to_clip[1] +
         player_6ae_world_to_clip[3];
}

#include "tabletennis_player_6ae_bbb580_vertex.hlsli"
#include "tabletennis_player_6ae_material_stage.hlsli"

Player6AEBBB580Varyings vs_main(uint vertex_id : SV_VertexID) {
  const uint stride =
      player_6ae_buffer_layout.w != 0u
          ? player_6ae_buffer_layout.w
          : kPlayer6AEVertexStride;
  const uint base =
      player_6ae_buffer_layout.x + vertex_id * stride;
  const float3 position = float3(
      Player6AELoadVertexFloat(base + 0u),
      Player6AELoadVertexFloat(base + 4u),
      Player6AELoadVertexFloat(base + 8u));

  const uint packed_weights = Player6AELoadVertexWord(base + 12u);
  const uint packed_indices = Player6AELoadVertexWord(base + 16u);
  // vf95 vfetch uses zyxw byte order. Reading raw bytes directly is
  // equivalent to the guest's normalized weight fetch and c255.y index scale.
  const float4 weights = float4(
      (packed_weights >> 16u) & 0xFFu,
      (packed_weights >> 8u) & 0xFFu,
      packed_weights & 0xFFu,
      (packed_weights >> 24u) & 0xFFu) / 255.0f;
  const uint4 bone_indices = uint4(
      (packed_indices >> 16u) & 0xFFu,
      (packed_indices >> 8u) & 0xFFu,
      packed_indices & 0xFFu,
      (packed_indices >> 24u) & 0xFFu);

  const Player6AEBoneBasis skin =
      Player6AEBlendBones(weights, bone_indices);
  const float3 world_position =
      Player6AETransformDirection(skin, position) +
      skin.translation;
  const float4 packed_direction0 =
      Player6AEBBB580DecodeSigned1010102(
          Player6AELoadVertexWord(base + 20u));
  const float2 texture_coordinate = float2(
      Player6AELoadVertexFloat(base + 24u),
      Player6AELoadVertexFloat(base + 28u));
  const float4 packed_direction1 =
      Player6AEBBB580DecodeSigned1010102(
          Player6AELoadVertexWord(base + 32u));

  return Player6AEBBB580BuildVaryings(
      world_position, texture_coordinate,
      packed_direction0, packed_direction1, skin);
}

float4 ps_main(Player6AEBBB580Varyings input) : SV_Target {
  const Player6AEMaterialStage material =
      Player6AEEvaluateMaterialStage(input);
  // This is the guest's real instruction-59 intermediate, displayed directly
  // as an observer diagnostic. No invented "final material" equation is used.
  return float4(max(material.diffuse, 0.0f),
                saturate(player_6ae_probe_tint.w));
}
