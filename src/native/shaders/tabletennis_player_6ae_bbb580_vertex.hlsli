// Faithful output port for BBB580AA5620D2A6 instructions 114-161.
//
// The geometry probe originally emitted only o0.xy because its diagnostic
// pixel shader needed just the base UV. The real 6AE pixel program consumes
// all seven interpolators. Keep this helper independent of the probe pixel
// shader so the vertex contract can be verified before the material port is
// allowed anywhere near serving.

struct Player6AEBBB580Varyings {
  float4 position : SV_Position;
  float4 interpolator0 : TEXCOORD0;
  float4 interpolator1 : TEXCOORD1;
  float4 interpolator2 : TEXCOORD2;
  float4 interpolator3 : TEXCOORD3;
  float4 interpolator4 : TEXCOORD4;
  float4 interpolator5 : TEXCOORD5;
  float4 interpolator6 : TEXCOORD6;
};

float Player6AEBBB580Signed10(uint packed, uint shift) {
  const uint field = (packed >> shift) & 0x3FFu;
  return float(int(field << 22u) >> 22);
}

float Player6AEBBB580Signed2(uint packed) {
  const uint field = (packed >> 30u) & 0x3u;
  return float(int(field << 30u) >> 30);
}

float4 Player6AEBBB580DecodeSigned1010102(uint packed) {
  // Both BBB580 vfetches specify Signed=true, NumFormat=integer. There is no
  // normalized conversion: the 10-bit fields remain in [-512, 511], and the
  // handedness field remains in [-2, 1].
  return float4(
      Player6AEBBB580Signed10(packed, 0u),
      Player6AEBBB580Signed10(packed, 10u),
      Player6AEBBB580Signed10(packed, 20u),
      Player6AEBBB580Signed2(packed));
}

float3 Player6AEBBB580SafeNormalize(float3 value) {
  const float length_squared = dot(value, value);
  return length_squared > 0.0f
             ? value * rsqrt(length_squared)
             : float3(0.0f, 0.0f, 0.0f);
}

float3 Player6AEBBB580TransformPackedDirection(
    Player6AEBoneBasis basis, float3 value) {
  // Instructions 114-119 build the direction in yzx world order. This is
  // the same skin basis used by the position path, followed by its explicit
  // register swizzle.
  return Player6AETransformDirection(basis, value).yzx;
}

float4 Player6AEBBB580AffineProjection(
    float3 world_yzx, uint first_constant) {
  const uint first_row = first_constant - 29u;
  const float4 row0 =
      player_6ae_vertex_constants_29_36[first_row + 0u];
  const float4 row1 =
      player_6ae_vertex_constants_29_36[first_row + 1u];
  const float4 row2 =
      player_6ae_vertex_constants_29_36[first_row + 2u];
  const float4 row3 =
      player_6ae_vertex_constants_29_36[first_row + 3u];
  return float4(
      dot(world_yzx, row0.xyz) + row0.w,
      dot(world_yzx, row1.xyz) + row1.w,
      dot(world_yzx, row2.xyz) + row2.w,
      dot(world_yzx, row3.xyz) + row3.w);
}

Player6AEBBB580Varyings Player6AEBBB580BuildVaryings(
    float3 world_position,
    float2 texture_coordinate,
    float4 packed_direction0,
    float4 packed_direction1,
    Player6AEBoneBasis skin) {
  // BBB580 instructions 114-138. r5 (offset 20) becomes o2, while r4
  // (offset 32) becomes o3. o4 is their normalized cross product multiplied
  // by the offset-32 signed 2-bit handedness component.
  const float3 direction0 = Player6AEBBB580SafeNormalize(
      Player6AEBBB580TransformPackedDirection(
          skin, packed_direction0.xyz));
  const float3 direction1 = Player6AEBBB580SafeNormalize(
      Player6AEBBB580TransformPackedDirection(
          skin, packed_direction1.xyz));
  const float3 cross_direction =
      cross(direction1, direction0) * packed_direction1.w;

  const float selector_test =
      player_6ae_vertex_constant_19.z * world_position.x;
  const float material_selector =
      selector_test > 0.0f
          ? player_6ae_vertex_constant_46.x +
                player_6ae_vertex_constant_47.x
          : max(player_6ae_vertex_constant_46.x,
                player_6ae_vertex_constant_46.x);
  const float3 world_yzx = world_position.yzx;

  Player6AEBBB580Varyings output;
  output.position = Player6AETransformPosition(world_position);
  // BBB580 masks o0-o4 to xyz. Zero is the explicit native value for the
  // unexported component; the 6AE material port must not invent data there.
  output.interpolator0 =
      float4(texture_coordinate, material_selector, 0.0f);
  output.interpolator1 = float4(world_yzx, 0.0f);
  output.interpolator2 = float4(direction0, 0.0f);
  output.interpolator3 = float4(direction1, 0.0f);
  output.interpolator4 = float4(cross_direction, 0.0f);
  output.interpolator5 =
      Player6AEBBB580AffineProjection(world_yzx, 29u);
  output.interpolator6 =
      Player6AEBBB580AffineProjection(world_yzx, 33u);
  return output;
}
