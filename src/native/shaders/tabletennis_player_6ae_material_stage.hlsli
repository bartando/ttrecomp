// Exact observer port of 6AE43640A86B33D8 instructions 19-59.
//
// This is deliberately an intermediate material-stage probe. It evaluates
// the guest's normal reconstruction and six-lobe diffuse-lighting sum, but
// it does not pretend that instructions 60-185 (shadow tests, specular,
// texture tf0 modulation and the final composite) have already been proven.

float4 Player6AESampleTf0(float2 uv) {
  return player_6ae_texture_tf0.Sample(player_6ae_wrap_sampler, uv);
}

float4 Player6AESampleTf1(float2 uv) {
  return player_6ae_texture_tf1.Sample(player_6ae_wrap_sampler, uv);
}

float4 Player6AESampleTf2(float2 uv) {
  return player_6ae_texture_tf2.Sample(
      player_6ae_linear_clamp_sampler, uv);
}

float4 Player6AESampleTf3(float2 uv) {
  return player_6ae_texture_tf3.Sample(player_6ae_point_clamp_sampler, uv);
}

float4 Player6AESampleTf4(float2 uv) {
  return player_6ae_texture_tf4.Sample(player_6ae_point_clamp_sampler, uv);
}

float4 Player6AESampleTf5(float2 uv) {
  return player_6ae_texture_tf5.Sample(player_6ae_wrap_sampler, uv);
}

float4 Player6AEC(uint index) {
  if (index == 19u) {
    return player_6ae_pixel_constant_19;
  }
  if (index >= 21u && index <= 27u) {
    return player_6ae_pixel_constants_21_27[index - 21u];
  }
  if (index >= 46u && index <= 70u) {
    return player_6ae_pixel_constants_46_70[index - 46u];
  }
  return index == 254u
             ? player_6ae_pixel_constants_254_255[0]
             : player_6ae_pixel_constants_254_255[1];
}

float3 Player6AESafeNormalizeMaterial(float3 value) {
  const float length_squared = dot(value, value);
  return length_squared > 0.0f
             ? value * rsqrt(length_squared)
             : float3(0.0f, 0.0f, 0.0f);
}

struct Player6AEMaterialStage {
  float3 normal;
  float3 diffuse;
  float material_mask;
  float sampled_mask;
};

Player6AEMaterialStage Player6AEEvaluateMaterialStage(
    Player6AEBBB580Varyings input) {
  const float4 c254 = Player6AEC(254u);
  const float4 c255 = Player6AEC(255u);

  // Guest registers r0-r6 enter the pixel shader as interpolators o0-o6.
  float4 r0 = input.interpolator0;
  float4 r1 = input.interpolator1;
  float4 r2 = input.interpolator2;
  float4 r3 = input.interpolator3;
  float4 r4 = input.interpolator4;
  float4 r5 = input.interpolator5;
  float4 r6 = input.interpolator6;
  float4 r7 = 0.0f;
  float4 r8 = 0.0f;
  float4 r9 = 0.0f;
  float4 r10 = 0.0f;

  // 19-25: material mask and tangent-space normal inputs. The writes and
  // preserved lanes intentionally mirror the microcode masks.
  const float4 tf1 = Player6AESampleTf1(r0.xy);
  r8.y = tf1.y;
  r8.w = tf1.w;
  r7.xyz = r1.xyz * float3(c255.z, c255.z, c255.w);
  r0.w = c254.x + r7.y;
  r8.xy = r7.xz * r0.ww;
  r9 = r8 + c255.xxyy;
  r1.x = Player6AESampleTf2(r0.xy).x;
  r10.x = Player6AESampleTf5(r9.xy).x;

  // 26-50: reconstruct the perturbed normal in the skinned tangent frame and
  // evaluate the two groups of clamped directional-light weights.
  const float4 c64 = Player6AEC(64u);
  const float4 c65 = Player6AEC(65u);
  const float4 c66 = Player6AEC(66u);
  const float4 c67 = Player6AEC(67u);
  const float4 c68 = Player6AEC(68u);
  const float4 c69 = Player6AEC(69u);
  const float4 c70 = Player6AEC(70u);
  r7.x = rcp(c70.x);
  r7.y = rcp(c69.w + c254.x);
  r7.z = rcp(c67.w + c254.x);
  r7.w = rcp(c65.w + c254.x);
  r8.xy = r9.wz + r8.wz;
  r10.yz = r8.yx * r8.yx;
  r9.xy = c254.xx - r10.xz;
  r0.w = r9.y - r10.y;
  r3.xyz = -r8.x * r3.xyz;
  r0.w = sqrt(abs(r0.w));
  r2.xyz = r0.w * r2.zxy + r3.zxy;
  r2.xyz = -r8.y * r4.xzy + r2.yxz;
  r0.w = rsqrt(abs(dot(r2.xzy, r2.xzy)));
  r4.x = c70.x - r1.y;
  r2.yzw = r2.zyx * r0.www;
  r4.y = c69.w - r2.z;
  r4.w = c65.w - r2.w;
  r4.z = c67.w - r2.y;

  float4 r3_weights = 0.0f;
  r3_weights.x = rcp(c68.w + c254.x);
  r3_weights.y = rcp(c66.w + c254.x);
  r3_weights.w = rcp(c64.w + c254.x);
  r8.x = c68.w + r2.z;
  r8.y = c66.w + r2.y;
  r8.z = r2.w + c64.w;
  r3_weights.z = c254.x - r9.x * Player6AEC(52u).x;
  r8.xyz = saturate(r8.xyz * r3_weights.xyw);
  r4 = saturate(r4 * r7);

  // 51-59: exact six-term diffuse accumulator, including the trace's
  // component routing through r3.xy_w.
  r0.w = max(r4.x, c70.y);
  r7.xyz = r4.zzz * c67.xyz;
  float4 diffuse_accumulator = 0.0f;
  diffuse_accumulator.xyw = r8.yyy * c66.xyz;
  diffuse_accumulator.xyw =
      r8.zzz * c64.xyz + diffuse_accumulator.xyw;
  diffuse_accumulator.xyw =
      r8.xxx * c68.xyz + diffuse_accumulator.xyw;
  diffuse_accumulator.xyw =
      r4.www * c65.xyz + diffuse_accumulator.xyw;
  diffuse_accumulator.xyw =
      r7.xyz * r0.www + diffuse_accumulator.xyw;
  diffuse_accumulator.xyw =
      r4.yyy * c69.xyz + diffuse_accumulator.xyw;

  Player6AEMaterialStage output;
  output.normal = r2.wyz;
  output.diffuse =
      diffuse_accumulator.xyw * Player6AEC(57u).xxx;
  output.material_mask = r4.x;
  output.sampled_mask = r10.x;
  return output;
}
