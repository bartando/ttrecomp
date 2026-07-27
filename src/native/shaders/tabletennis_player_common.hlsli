// Native observer contract for the CA9BBF96B0928616 /
// 77AF85E1AF823D02 skinned-player family.
//
// The buffers contain unmodified Xbox guest bytes. The vertex shader performs
// the 8-in-32 endian conversion itself, so an observer can upload the captured
// vf95 and vf92 ranges without creating a second decoded vertex format.

struct PlayerVaryings {
  float4 position : SV_Position;
  float2 main_texcoord : TEXCOORD0;
  float2 auxiliary_texcoord : TEXCOORD1;
  float4 light0 : TEXCOORD2;
  float4 light1 : TEXCOORD3;
  float3 world_position : TEXCOORD4;
  float3 camera_position : TEXCOORD5;
  float3 world_normal : TEXCOORD6;
  float3 world_tangent : TEXCOORD7;
    centroid float4 light2 : TEXCOORD8;
};

uint PlayerByteSwap32(uint value) {
  return ((value & 0x000000FFu) << 24u) |
         ((value & 0x0000FF00u) << 8u) |
         ((value & 0x00FF0000u) >> 8u) |
         ((value & 0xFF000000u) >> 24u);
}

float PlayerGuestFloat(uint value) {
  return asfloat(PlayerByteSwap32(value));
}

float PlayerSigned10(uint packed, uint shift) {
  const uint field = (packed >> shift) & 0x3FFu;
  return float(int(field << 22u) >> 22);
}

float3 PlayerPackedSigned101010(uint packed) {
  return float3(
      PlayerSigned10(packed, 0u),
      PlayerSigned10(packed, 10u),
      PlayerSigned10(packed, 20u));
}

float3 PlayerSafeNormalize(float3 value) {
  const float length_squared = dot(value, value);
  return length_squared > 0.0f
             ? value * rsqrt(length_squared)
             : float3(0.0f, 0.0f, 1.0f);
}
