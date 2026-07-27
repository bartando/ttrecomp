// Exact byte layout observed for vf95.
static const uint kPlayerVertexStride = 44u;
static const uint kPlayerPaletteRecordStride = 28u;

struct PlayerDecodedVertex {
  float3 position;
  float4 weights;
  uint4 bone_indices;
  float3 normal;
  float2 auxiliary_texcoord;
  float2 main_texcoord;
  float3 tangent;
};

struct PlayerBoneBasis {
  float3 column0;
  float3 column1;
  float3 column2;
  float3 translation;
};

uint PlayerLoadVertexWord(uint byte_address) {
  return PlayerByteSwap32(player_vertex_words[byte_address >> 2u]);
}

float PlayerLoadVertexFloat(uint byte_address) {
  return asfloat(PlayerLoadVertexWord(byte_address));
}

uint PlayerLoadPaletteWord(uint byte_address) {
  const uint word_index = byte_address >> 2u;
  const uint2 pair = player_palette_word_pairs[word_index >> 1u];
  return PlayerByteSwap32(
      (word_index & 1u) != 0u ? pair.y : pair.x);
}

float PlayerLoadPaletteFloat(uint byte_address) {
  return asfloat(PlayerLoadPaletteWord(byte_address));
}

PlayerDecodedVertex PlayerDecodeVertex(uint vertex_id) {
  const uint vertex_stride =
      player_buffer_layout.w != 0u
          ? player_buffer_layout.w
          : kPlayerVertexStride;
  const uint base =
      player_buffer_layout.x + vertex_id * vertex_stride;

  PlayerDecodedVertex vertex;
  vertex.position = float3(
      PlayerLoadVertexFloat(base + 0u),
      PlayerLoadVertexFloat(base + 4u),
      PlayerLoadVertexFloat(base + 8u));

  const uint packed_weights = PlayerLoadVertexWord(base + 12u);
  const uint packed_indices = PlayerLoadVertexWord(base + 16u);
  // vfetch r7.zyxw / r1.zyxw. The live trace proves that these four
  // weight bytes sum to 255 for populated vertices.
  vertex.weights =
      float4(
          (packed_weights >> 16u) & 0xFFu,
          (packed_weights >> 8u) & 0xFFu,
          packed_weights & 0xFFu,
          (packed_weights >> 24u) & 0xFFu) /
      255.0f;
  vertex.bone_indices = uint4(
      (packed_indices >> 16u) & 0xFFu,
      (packed_indices >> 8u) & 0xFFu,
      packed_indices & 0xFFu,
      (packed_indices >> 24u) & 0xFFu);

  vertex.normal =
      PlayerPackedSigned101010(PlayerLoadVertexWord(base + 20u));
  vertex.auxiliary_texcoord = float2(
      PlayerLoadVertexFloat(base + 24u),
      PlayerLoadVertexFloat(base + 28u));
  vertex.main_texcoord = float2(
      PlayerLoadVertexFloat(base + 32u),
      PlayerLoadVertexFloat(base + 36u));
  vertex.tangent =
      PlayerPackedSigned101010(PlayerLoadVertexWord(base + 40u));
  return vertex;
}

PlayerBoneBasis PlayerLoadBone(uint bone_index) {
  // Diagnostic assumption: the native vf92 binding begins at the exact live
  // fetch base, so the byte influence is a direct record index. The Xenos
  // shader proves index * 28 addressing; capture/serving code must still
  // attest the bound base and record count before this asset is ever served.
  const uint base =
      player_buffer_layout.y + bone_index * kPlayerPaletteRecordStride;
  const float4 quaternion = float4(
      PlayerLoadPaletteFloat(base + 0u),
      PlayerLoadPaletteFloat(base + 4u),
      PlayerLoadPaletteFloat(base + 8u),
      PlayerLoadPaletteFloat(base + 12u));
  const float3 translation = float3(
      PlayerLoadPaletteFloat(base + 16u),
      PlayerLoadPaletteFloat(base + 20u),
      PlayerLoadPaletteFloat(base + 24u));

  const float x = quaternion.x;
  const float y = quaternion.y;
  const float z = quaternion.z;
  const float w = quaternion.w;

  // This follows the Xenos ALU sequence rather than replacing it with a
  // generic matrix helper. For the unit quaternions in vf92 it is the usual
  // rotation matrix. The third column is reconstructed from the first two in
  // the same order as instructions 34-40 of CA9BBF96B0928616.
  const float r9x = 2.0f * (x * y - z * w);
  const float r9y = 2.0f * (x * z - y * w);
  const float r9z = 1.0f - 2.0f * (y * y + z * z);
  const float r2x = 2.0f * (x * y + z * w);
  const float r2y = 2.0f * (y * z + x * w);
  const float r2w = 1.0f - 2.0f * (x * x + z * z);

  const float r0x = r2x * r2y - r9y * r2w;
  const float r0z = r9y * r9x - r9z * r2y;
  const float r0y = -r2x * r9x + r9z * r2w;

  PlayerBoneBasis bone;
  bone.column0 = float3(r9z, r2x, r9y);
  bone.column1 = float3(r9x, r2w, r2y);
  bone.column2 = float3(r0x, r0z, r0y);
  bone.translation = translation;
  return bone;
}

PlayerBoneBasis PlayerBlendBones(
    float4 weights, uint4 bone_indices) {
  PlayerBoneBasis blended;
  blended.column0 = 0.0f;
  blended.column1 = 0.0f;
  blended.column2 = 0.0f;
  blended.translation = 0.0f;

  [unroll]
  for (uint influence = 0u; influence < 4u; ++influence) {
    const float weight = weights[influence];
    const uint bone_index = bone_indices[influence];
    // The original always fetches influence zero and predicates the other
    // three on non-zero weights. Avoiding all zero-weight reads is equivalent
    // for valid captured vertices and keeps observer buffers robust.
    if (weight <= 0.0f ||
        bone_index >= player_buffer_layout.z) {
      continue;
    }
    const PlayerBoneBasis bone = PlayerLoadBone(bone_index);
    blended.column0 += bone.column0 * weight;
    blended.column1 += bone.column1 * weight;
    blended.column2 += bone.column2 * weight;
    blended.translation += bone.translation * weight;
  }
  return blended;
}

float3 PlayerTransformDirection(
    PlayerBoneBasis skin, float3 value) {
  return skin.column0 * value.x +
         skin.column1 * value.y +
         skin.column2 * value.z;
}

float4 PlayerTransformPosition(float3 world_position) {
  // Exact c12-c15 swizzle used by the guest shader.
  float4 mixed =
      world_position.y * player_world_to_clip[1].wzyx +
      world_position.x * player_world_to_clip[0].xzwy +
      world_position.z * player_world_to_clip[2].yzxw;
  return mixed.zxyw + player_world_to_clip[3];
}

float4 PlayerBuildLight(
    float3 world_position, uint light_index) {
  const float4 position_radius =
      player_light_position_radius[light_index];
  const float4 color_intensity =
      player_light_color_intensity[light_index];
  const float4 mode = player_light_mode[light_index];
  if (mode.x != 0.0f) {
    return float4(-position_radius.xyz, color_intensity.w);
  }

  const float3 delta = position_radius.xyz - world_position;
  const float inverse_radius = rcp(position_radius.w);
  const float3 radius_space = delta * inverse_radius;
  const float attenuation =
      color_intensity.w *
      (1.0f - saturate(dot(radius_space, radius_space)));
  return float4(PlayerSafeNormalize(delta), attenuation);
}

PlayerVaryings PlayerSkinVertex(uint vertex_id) {
  const PlayerDecodedVertex vertex = PlayerDecodeVertex(vertex_id);
  const PlayerBoneBasis skin =
      PlayerBlendBones(vertex.weights, vertex.bone_indices);
  const float3 world_position =
      PlayerTransformDirection(skin, vertex.position) +
      skin.translation;

  PlayerVaryings output;
  output.position = PlayerTransformPosition(world_position);
  output.main_texcoord = vertex.main_texcoord;
  output.auxiliary_texcoord = vertex.auxiliary_texcoord;
  output.light0 = PlayerBuildLight(world_position, 0u);
  output.light1 = PlayerBuildLight(world_position, 1u);
  output.light2 = PlayerBuildLight(world_position, 2u);
  output.world_position = world_position;
  output.camera_position = player_camera_position.xyz;
  output.world_normal = PlayerSafeNormalize(
      PlayerTransformDirection(skin, vertex.normal));
  output.world_tangent = PlayerSafeNormalize(
      PlayerTransformDirection(skin, vertex.tangent));
  return output;
}
