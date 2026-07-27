// Native observer port of the trace-verified
// 4EAEC701E97DCDAD / 08D6210341AD63F6 vertex shaders and
// 14D6B61CBC3D853C pixel shader.
//
// The title's vf95 bytes stay in their captured 8-in-32 guest order. Decoding
// them here keeps both proven 40-byte and 48-byte variants exact without
// inventing a replacement vertex declaration.

cbuffer Venue14DConstants : register(b0) {
  float4 venue_14d_local_to_world[4];  // c0-c3
  float4 venue_14d_direction_basis[3]; // c4-c6
  float4 venue_14d_clip_transform[4];  // c12-c15

  float4 venue_14d_c19;
  float4 venue_14d_c20;
  float4 venue_14d_c46;
  float4 venue_14d_c47;
  float4 venue_14d_c48;
  float4 venue_14d_c49;
  float4 venue_14d_c50;
  float4 venue_14d_c254;
  float4 venue_14d_c255;

  // x: vf95 stride, y: tangent byte offset, z/w: reserved.
  uint4 venue_14d_buffer_layout;
  // x: observer opacity, y: reciprocal display gamma.
  float4 venue_14d_observer_options;
};

#if defined(TABLETENNIS_VULKAN)
StructuredBuffer<uint> venue_14d_vertex_words : register(t1, space0);
Texture2D venue_14d_texture0 : register(t0, space1);
Texture2D venue_14d_texture1 : register(t1, space1);
Texture2D venue_14d_texture2 : register(t2, space1);
Texture2D venue_14d_texture3 : register(t3, space1);
TextureCube venue_14d_texture4 : register(t4, space1);
SamplerState venue_14d_wrap_sampler : register(s2, space0);
SamplerState venue_14d_cube_sampler : register(s3, space0);
#else
StructuredBuffer<uint> venue_14d_vertex_words : register(t0);
Texture2D venue_14d_texture0 : register(t1);
Texture2D venue_14d_texture1 : register(t2);
Texture2D venue_14d_texture2 : register(t3);
Texture2D venue_14d_texture3 : register(t4);
TextureCube venue_14d_texture4 : register(t5);
SamplerState venue_14d_wrap_sampler : register(s0);
SamplerState venue_14d_cube_sampler : register(s1);
#endif

struct Venue14DVaryings {
  float4 position : SV_Position;
  float4 texture_coordinates : TEXCOORD0;
  float3 world_normal : TEXCOORD1;
  float3 world_position : TEXCOORD2;
  float3 world_tangent : TEXCOORD3;
  float3 world_bitangent : TEXCOORD4;
  float4 color : COLOR0;
};

uint Venue14DByteSwap32(uint value) {
  return ((value & 0x000000FFu) << 24u) |
         ((value & 0x0000FF00u) << 8u) |
         ((value & 0x00FF0000u) >> 8u) |
         ((value & 0xFF000000u) >> 24u);
}

uint Venue14DLoadWord(uint byte_address) {
  return Venue14DByteSwap32(
      venue_14d_vertex_words[byte_address >> 2u]);
}

float Venue14DLoadFloat(uint byte_address) {
  return asfloat(Venue14DLoadWord(byte_address));
}

float Venue14DSigned10(uint packed, uint shift) {
  const uint field = (packed >> shift) & 0x3FFu;
  return float(int(field << 22u) >> 22);
}

float Venue14DSigned2(uint packed) {
  const uint field = (packed >> 30u) & 0x3u;
  return float(int(field << 30u) >> 30);
}

float3 Venue14DDecodeSigned101010(uint packed) {
  return float3(
      Venue14DSigned10(packed, 0u),
      Venue14DSigned10(packed, 10u),
      Venue14DSigned10(packed, 20u));
}

float3 Venue14DSafeNormalize(float3 value) {
  const float length_squared = dot(value, value);
  return length_squared > 0.0f
             ? value * rsqrt(length_squared)
             : float3(0.0f, 0.0f, 1.0f);
}

float3 Venue14DTransformPosition(float3 position) {
  return position.x * venue_14d_local_to_world[0].xyz +
         position.y * venue_14d_local_to_world[1].xyz +
         position.z * venue_14d_local_to_world[2].xyz +
         venue_14d_local_to_world[3].xyz;
}

float3 Venue14DTransformDirection(float3 direction) {
  return direction.x * venue_14d_direction_basis[0].xyz +
         direction.y * venue_14d_direction_basis[1].xyz +
         direction.z * venue_14d_direction_basis[2].xyz;
}

float4 Venue14DTransformClip(float3 position) {
  return position.x * venue_14d_clip_transform[0] +
         position.y * venue_14d_clip_transform[1] +
         position.z * venue_14d_clip_transform[2] +
         venue_14d_clip_transform[3];
}

Venue14DVaryings vs_main(uint vertex_id : SV_VertexID) {
  const uint base = vertex_id * venue_14d_buffer_layout.x;
  const float3 position = float3(
      Venue14DLoadFloat(base + 0u),
      Venue14DLoadFloat(base + 4u),
      Venue14DLoadFloat(base + 8u));
  const float3 normal = Venue14DDecodeSigned101010(
      Venue14DLoadWord(base + 12u));
  const uint packed_color = Venue14DLoadWord(base + 16u);
  const float2 texture_coordinate0 = float2(
      Venue14DLoadFloat(base + 20u),
      Venue14DLoadFloat(base + 24u));
  const float2 texture_coordinate1 = float2(
      Venue14DLoadFloat(base + 28u),
      Venue14DLoadFloat(base + 32u));
  const uint packed_tangent =
      Venue14DLoadWord(base + venue_14d_buffer_layout.y);

  const float3 world_normal = Venue14DSafeNormalize(
      Venue14DTransformDirection(normal));
  const float3 world_tangent = Venue14DSafeNormalize(
      Venue14DTransformDirection(
          Venue14DDecodeSigned101010(packed_tangent)));
  const float tangent_sign = Venue14DSigned2(packed_tangent);

  Venue14DVaryings output;
  output.position = Venue14DTransformClip(position);
  output.texture_coordinates =
      float4(texture_coordinate0, texture_coordinate1);
  output.world_normal = world_normal;
  output.world_position = Venue14DTransformPosition(position);
  output.world_tangent = world_tangent;
  output.world_bitangent =
      cross(world_normal, world_tangent) * tangent_sign;
  // FMT_8_8_8_8 with 8-in-32 endian followed by the guest .zyxw swizzle.
  output.color = float4(
      float((packed_color >> 16u) & 0xFFu),
      float((packed_color >> 8u) & 0xFFu),
      float(packed_color & 0xFFu),
      float((packed_color >> 24u) & 0xFFu)) / 255.0f;
  return output;
}

float4 ps_main(Venue14DVaryings input) : SV_Target {
  // 14D stores its tangent-space XY normal in DXT5 green/alpha.
  const float4 packed_normal = venue_14d_texture2.Sample(
      venue_14d_wrap_sampler, input.texture_coordinates.xy);
  const float2 tangent_xy =
      float2(packed_normal.a, packed_normal.g) * 2.0f -
      venue_14d_c254.xx;
  const float tangent_z = sqrt(abs(
      venue_14d_c254.x -
      dot(tangent_xy, tangent_xy)));
  const float3 surface_normal =
      input.world_normal * tangent_z +
      input.world_tangent * tangent_xy.x * venue_14d_c48.x +
      input.world_bitangent * tangent_xy.y * venue_14d_c48.x;

  // Instructions 22-35 reflect the camera ray, intersect that ray with the
  // c50.x sphere, then use the intersection as the cube lookup direction.
  const float3 camera_ray = Venue14DSafeNormalize(
      input.world_position - venue_14d_c19.xyz);
  const float3 reflection_ray =
      reflect(camera_ray, surface_normal);
  const float ray_position_dot =
      dot(reflection_ray, input.world_position);
  const float doubled_dot = ray_position_dot + ray_position_dot;
  const float sphere_term =
      dot(input.world_position, input.world_position) -
      venue_14d_c50.x * venue_14d_c50.x;
  const float discriminant =
      doubled_dot * doubled_dot -
      venue_14d_c255.w * sphere_term;
  const float distance =
      (-doubled_dot + sqrt(abs(discriminant))) *
      venue_14d_c255.x;
  const float3 environment_direction =
      input.world_position + reflection_ray * distance;

  // The observer binds every descriptor-selected guest mip, so these implicit
  // gradient samples reproduce the title shader's ordinary texture fetches.
  const float4 environment = venue_14d_texture4.Sample(
      venue_14d_cube_sampler, environment_direction);
  const float3 emission = venue_14d_texture3.Sample(
      venue_14d_wrap_sampler, input.texture_coordinates.xy).rgb;
  const float3 detail = venue_14d_texture1.Sample(
      venue_14d_wrap_sampler, input.texture_coordinates.zw).rgb;
  const float4 base = venue_14d_texture0.Sample(
      venue_14d_wrap_sampler, input.texture_coordinates.xy);

  const float3 base_detail =
      base.rgb * detail * venue_14d_c46.x;
  const float3 reflected =
      lerp(base_detail, environment.rgb, venue_14d_c49.x);
  const float3 linear_color =
      (reflected * input.color.rgb +
       emission * venue_14d_c47.xyz * environment.a) *
      venue_14d_c20.z;
  const float3 display_color = pow(
      saturate(linear_color),
      venue_14d_observer_options.yyy);
  return float4(
      display_color,
      base.a * input.color.a * venue_14d_observer_options.x);
}
