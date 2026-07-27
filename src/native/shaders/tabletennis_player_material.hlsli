// Semantic port of Xenos pixel shader 77AF85E1AF823D02.
//
// Register-shaped locals intentionally mirror the guest disassembly. This is
// verbose, but it keeps texture channel use, swizzles and material constants
// reviewable against instructions 11-128 instead of hiding them in an
// invented PBR model.

#define PLAYER_PC(row) player_pixel_constants[(row) - 46]

float4 PlayerShadeMaterial(PlayerVaryings input) {
  float4 r0 = float4(input.main_texcoord, 0.0f, 0.0f);
  float4 r1 = float4(input.auxiliary_texcoord, 0.0f, 0.0f);
  float4 r2 = input.light0;
  float4 r3 = input.light1;
  float4 r4 = float4(input.world_position, 0.0f);
  float4 r5 = float4(input.camera_position, 0.0f);
  float4 r6 = float4(input.world_normal, 0.0f);
  float4 r7 = float4(input.world_tangent, 0.0f);
  float4 r8 = input.light2;
  float4 r9 = 0.0f;
  float4 r10 = 0.0f;
  float4 r11 = 0.0f;
  float4 r12 = 0.0f;
  float4 r13 = 0.0f;
  float4 r14 = 0.0f;
  float4 r15 = 0.0f;
  float previous_scalar = 0.0f;

  const float4 texture0 =
      player_texture0.Sample(player_sampler0, input.main_texcoord);
  const float4 texture1 =
      player_texture1.Sample(player_sampler1, input.auxiliary_texcoord);
  const float4 texture2 =
      player_texture2.Sample(player_sampler2, input.main_texcoord);
  // tfetch destination swizzles: __wy, xw__, __xw.
  r0.z = texture0.w;
  r0.w = texture0.y;
  r12.xy = texture1.xw;
  r12.zw = texture2.xw;

  r8.xy = r12.yw + player_pixel_control_254.zz;
  r0.y = PLAYER_PC(57).x * r8.y;
  r8.w = r0.y + PLAYER_PC(55).x;
  r8.z = PLAYER_PC(56).x + r0.y;
  r0.y = dot(r7.xyz, r7.xyz);
  r5.xyz = r5.xyz - r4.xyz;
  r1.x = dot(r6.xyz, r6.xyz);
  r0.x = dot(r5.xyz, r5.xyz);
  previous_scalar = rsqrt(abs(r1.x));
  r1.xyz = previous_scalar * r6.xyz;
  previous_scalar = rsqrt(abs(r0.y));
  r9.xyz = previous_scalar * r7.xyz;
  previous_scalar = rsqrt(abs(r0.x));
  r5.xyz *= previous_scalar;
  r10.xyz = r5.xyz + r2.xyz;
  r6.xyz = r5.xyz + r3.xyz;
  r1.w = dot(r6.xyz, r6.xyz);
  r8.zw = r8.x * PLAYER_PC(58).xx + r8.zw;
  r0.x = dot(r10.xyz, r10.xyz);
  previous_scalar = rsqrt(abs(r1.w));
  r6.xyz *= previous_scalar;
  previous_scalar = rsqrt(abs(r0.x));
  r11.xyz = r10.xyz * previous_scalar;
  r10.xyz = r1.xyz * r8.z + r9.xyz;
  r9.xyz = r1.xyz * r8.w + r9.xyz;
  r1.w = dot(r9.xyz, r9.xyz);
  r0.x = dot(r10.xyz, r10.xyz);
  previous_scalar = rsqrt(abs(r1.w));
  r9.xyz *= previous_scalar;
  previous_scalar = rsqrt(abs(r0.x));
  r13.xyz = r10.xyz * previous_scalar;
  r10.x = dot(r13.xyz, r11.xyz);
  r10.y = dot(r9.xyz, r6.xyz);
  r10.z = dot(r13.xyz, r6.xyz);
  r6.z = r12.x * r12.z;
  r6.xyw = r10.xzy * r10.xzy;
  r0.x = dot(r9.xyz, r11.xyz);
  r1.w =
      player_pixel_control_255.y - r0.x * r0.x;
  r1.w = sqrt(abs(r1.w));
  r6 = player_pixel_control_255.yyyy - r6.wyxz;
  r8.z = log2(abs(r1.w));
  r1.w = sqrt(abs(r6.x));
  r8.w = log2(abs(r1.w));
  r1.w = sqrt(abs(r6.y));
  r4.w = sqrt(abs(r6.z));
  r9.x = log2(abs(r4.w));
  r9.y = log2(abs(r1.w));
  r11.xyz =
      saturate(r10.xyz + player_pixel_control_255.yyy);
  r11.w = saturate(r6.w * PLAYER_PC(59).x);
  r10 =
      player_pixel_control_255.xxxy - r11.zyxw;
  r6.y = r10.x - r11.z;
  r6.z = r10.y - r11.y;
  r6.w = r10.z - r11.x;
  r12.zw = r9.xy * PLAYER_PC(64).xx;
  r12.xy = r8.zw * PLAYER_PC(63).xx;
  r1.w = exp2(r12.w);
  r9.xyz = r1.w * PLAYER_PC(62).xyz;
  r1.w = exp2(r12.y);
  r13.xyz = r1.w * PLAYER_PC(61).xyz;
  r13.xyz = r13.zxy * r11.y;
  r9.xyz = r9.zxy * r11.z;
  r9.xyz = r9.zxy * r11.z;
  r13.xyz = r13.zxy * r11.y;
  r13.xyz = r13.zxy * r6.z;
  previous_scalar = PLAYER_PC(66).x;
  r9.xyz = r9.zxy * r6.y;
  r1.w = previous_scalar - PLAYER_PC(67).x;
  r9.yzw = r9.zxy * r10.w;
  r9.x = rcp(r1.w);
  r9.yzw =
      r13.yzx * r10.w + r9.wyz;

  r8.z = dot(r1.xyz, r3.xyz);
  r8.w = dot(r1.xyz, r2.xyz);
  r7.xyz *= PLAYER_PC(68).x;
  r7.xyz = r7.zxy * r0.y;
  r7.xyz =
      r7.xzy * r8.y + r1.zyx;
  r0.y = dot(r7.zyx, r7.zyx);
  r8.zw += player_pixel_control_254.ww;
  previous_scalar = rsqrt(abs(r0.y));
  r7.xyz = r7.zyx * previous_scalar;
  r8.x = dot(r7.xyz, r3.xyz);
  r8.y = dot(r7.xyz, r2.xyz);
  r0.y = dot(r5.xyz, r1.xyz);
  r8 = saturate(
      r8.ywxz * player_pixel_control_255.wzwz);

  r5.xyz = r8.z * PLAYER_PC(47).xyz;
  previous_scalar = r0.y;
  r7.yzw = r5.zxy * r3.w;
  r7.x = previous_scalar - PLAYER_PC(67).x;
  r0.y =
      PLAYER_PC(49).w + player_pixel_control_255.y;
  r5.z = rcp(PLAYER_PC(54).x);
  r1.w =
      PLAYER_PC(53).w + player_pixel_control_255.y;
  r5.x = rcp(r0.y);
  r0.y =
      PLAYER_PC(51).w + player_pixel_control_255.y;
  r5.y = rcp(r1.w);
  r5.w = rcp(r0.y);
  r13.z = PLAYER_PC(54).x - r4.y;
  r13.x = PLAYER_PC(49).w - r1.x;
  r13.w = PLAYER_PC(51).w - r1.y;
  r13.y = PLAYER_PC(53).w - r1.z;
  r0.y =
      PLAYER_PC(52).w + player_pixel_control_255.y;
  r1.w =
      PLAYER_PC(50).w + player_pixel_control_255.y;
  r4.x = rcp(r0.y);
  r0.y =
      PLAYER_PC(48).w + player_pixel_control_255.y;
  r4.y = rcp(r1.w);
  r4.z = rcp(r0.y);
  r14.x = PLAYER_PC(52).w + r1.z;
  r14.z = r1.x + PLAYER_PC(48).w;
  r14.y = PLAYER_PC(50).w + r1.y;
  r4.xyz = saturate(r14.xyz * r4.xyz);
  r13 = saturate(r13 * r5);

  r14.xyz = r13.w * PLAYER_PC(51).xyz;
  r6.x = PLAYER_PC(60).z * r0.w;
  r1.xyz = r4.y * PLAYER_PC(50).xyz;
  r15.x = r13.z;
  r1.xyz += r4.z * PLAYER_PC(48).xyz;
  r1.xyz =
      r4.x * PLAYER_PC(52).zxy + r1.zxy;
  r1.xyz =
      r13.x * PLAYER_PC(49).yzx + r1.zxy;
  r4.xyz = r8.y * PLAYER_PC(46).xyz;
  r15.y = PLAYER_PC(54).y;
  r5.xyz = r8.w * PLAYER_PC(47).xyz;
  r0.y = max(r15.x, r15.y);
  r1.xyz = r14.xyz * r0.y + r1.zxy;
  r1.xyz =
      r13.y * PLAYER_PC(53).zxy + r1.zxy;
  r3.xyz = r5.zxy * r3.w;
  r0.y = r8.x;
  const float3 old_r3 = r3.xyz;
  r3.x = r4.y * r2.w + old_r3.z;
  r3.z = r4.z * r2.w + old_r3.x;
  r3.w = r4.x * r2.w + old_r3.y;
  r1.w = PLAYER_PC(46).x * r0.y;
  r3.y = saturate(
      r0.x - player_pixel_control_254.y);
  r0.y =
      -r3.y * player_pixel_control_254.x +
      player_pixel_control_255.x;
  r5.xyz = r3.xzw + r1.zxy;
  r0.x = exp2(r12.z);
  r1.xyz = r0.x * PLAYER_PC(62).xyz;
  r0.x = exp2(r12.x);
  r3.x = r0.x * PLAYER_PC(61).x;
  r3.z = r0.x * PLAYER_PC(61).y;
  r3.w = r0.x * PLAYER_PC(61).z;
  r0.x = PLAYER_PC(46).y * r8.x;
  r4.xyz = r3.wxz * r3.y;
  r3.x = PLAYER_PC(46).z * r8.x;
  r1.xyz = r1.zxy * r11.x;
  previous_scalar = r3.x;
  r1.xyz = r1.zxy * r11.x;
  r3.x = previous_scalar * r2.w;
  r3.yzw = r4.zxy * r3.y;
  previous_scalar = r1.w;
  r8.xyz = r3.wyz * previous_scalar;
  r3.y = previous_scalar * r2.w;
  r1.xyz = r1.zxy * r6.w;
  previous_scalar = r0.x;
  r1.xyz = r1.zxy * r10.w;
  r3.z = previous_scalar * r2.w;
  r1.xyz =
      r8.yzx * r10.w + r1.zxy;
  r4.xyz = r3.zxy * r1.xyz;
  r6.y = PLAYER_PC(60).y * r0.w;
  const float4 old_r7 = r7;
  r1 = old_r7.xwyz * r9;
  r6.z = PLAYER_PC(60).x * r0.w;
  r2.xyz =
      r6.yzx * r5.xzy + r4.xzy;
  const float3 color = r2.yxz + r1.wyz;
  r0.y = log2(abs(r1.x));
  r0.x = PLAYER_PC(65).x * r0.y;
  r0.x = saturate(exp2(r0.x));
  return float4(color, r0.x * r0.z);
}

#undef PLAYER_PC

