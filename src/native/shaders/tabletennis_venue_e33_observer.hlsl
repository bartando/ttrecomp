// Native observer port of the trace-verified 37F2AEC8A23E44E0 vertex shader
// and E33DEAA20A98FCEF pixel shader. The captured stride-32 vertex bytes stay
// in Xenos 8-in-32 endian order and are decoded directly by vertex ID.

cbuffer VenueE33Constants : register(b0) {
  float4 venue_e33_local_to_world[4];  // VS c0-c3
  float4 venue_e33_direction_basis[3]; // VS c4-c6
  float4 venue_e33_clip_transform[4];  // VS c12-c15

  float4 venue_e33_c19;
  float4 venue_e33_c20;
  float4 venue_e33_c46;
  float4 venue_e33_c47;
  float4 venue_e33_c48;
  float4 venue_e33_c254;
  float4 venue_e33_c255;

  // x: trace-proven vertex stride (32), y/z/w: reserved.
  uint4 venue_e33_buffer_layout;
  // x: observer opacity, y: reciprocal display gamma.
  float4 venue_e33_observer_options;
};

#if defined(TABLETENNIS_VULKAN)
StructuredBuffer<uint> venue_e33_vertex_words : register(t1, space0);
Texture2D venue_e33_texture0 : register(t0, space1);
Texture2D venue_e33_texture1 : register(t1, space1);
TextureCube venue_e33_texture2 : register(t2, space1);
SamplerState venue_e33_wrap_sampler : register(s2, space0);
SamplerState venue_e33_cube_sampler : register(s3, space0);
#else
StructuredBuffer<uint> venue_e33_vertex_words : register(t0);
Texture2D venue_e33_texture0 : register(t1);
Texture2D venue_e33_texture1 : register(t2);
TextureCube venue_e33_texture2 : register(t3);
SamplerState venue_e33_wrap_sampler : register(s0);
SamplerState venue_e33_cube_sampler : register(s1);
#endif

struct VenueE33Varyings {
  float4 position : SV_Position;
  float2 texture_coordinates : TEXCOORD0;
  float3 world_normal : TEXCOORD1;
  float3 world_position : TEXCOORD2;
  float4 color : COLOR0;
};

uint VenueE33ByteSwap32(uint value) {
  return ((value & 0x000000FFu) << 24u) |
         ((value & 0x0000FF00u) << 8u) |
         ((value & 0x00FF0000u) >> 8u) |
         ((value & 0xFF000000u) >> 24u);
}

uint VenueE33LoadWord(uint byte_address) {
  return VenueE33ByteSwap32(venue_e33_vertex_words[byte_address >> 2u]);
}

float VenueE33LoadFloat(uint byte_address) {
  return asfloat(VenueE33LoadWord(byte_address));
}

float VenueE33Signed10(uint packed, uint shift) {
  const uint field = (packed >> shift) & 0x3FFu;
  return float(int(field << 22u) >> 22);
}

float3 VenueE33DecodeSigned101010(uint packed) {
  return float3(
      VenueE33Signed10(packed, 0u),
      VenueE33Signed10(packed, 10u),
      VenueE33Signed10(packed, 20u));
}

float3 VenueE33SafeNormalize(float3 value) {
  const float length_squared = dot(value, value);
  return length_squared > 0.0f
             ? value * rsqrt(length_squared)
             : float3(0.0f, 0.0f, 1.0f);
}

float3 VenueE33TransformPosition(float3 position) {
  return position.x * venue_e33_local_to_world[0].xyz +
         position.y * venue_e33_local_to_world[1].xyz +
         position.z * venue_e33_local_to_world[2].xyz +
         venue_e33_local_to_world[3].xyz;
}

float3 VenueE33TransformDirection(float3 direction) {
  return direction.x * venue_e33_direction_basis[0].xyz +
         direction.y * venue_e33_direction_basis[1].xyz +
         direction.z * venue_e33_direction_basis[2].xyz;
}

float4 VenueE33TransformClip(float3 position) {
  return position.x * venue_e33_clip_transform[0] +
         position.y * venue_e33_clip_transform[1] +
         position.z * venue_e33_clip_transform[2] +
         venue_e33_clip_transform[3];
}

VenueE33Varyings vs_main(uint vertex_id : SV_VertexID) {
  const uint base = vertex_id * venue_e33_buffer_layout.x;
  const float3 position = float3(
      VenueE33LoadFloat(base + 0u),
      VenueE33LoadFloat(base + 4u),
      VenueE33LoadFloat(base + 8u));
  const float3 normal =
      VenueE33DecodeSigned101010(VenueE33LoadWord(base + 12u));
  const uint packed_color = VenueE33LoadWord(base + 16u);

  VenueE33Varyings output;
  output.position = VenueE33TransformClip(position);
  output.texture_coordinates = float2(
      VenueE33LoadFloat(base + 20u),
      VenueE33LoadFloat(base + 24u));
  output.world_normal =
      VenueE33SafeNormalize(VenueE33TransformDirection(normal));
  output.world_position = VenueE33TransformPosition(position);
  // FMT_8_8_8_8 after the guest shader's .zyxw destination swizzle.
  output.color = float4(
      float((packed_color >> 16u) & 0xFFu),
      float((packed_color >> 8u) & 0xFFu),
      float(packed_color & 0xFFu),
      float((packed_color >> 24u) & 0xFFu)) / 255.0f;
  return output;
}

float4 ps_main(VenueE33Varyings input) : SV_Target {
  // Instructions 4-25: reproduce the guest's reflected ray / sphere
  // intersection before the ordinary cube fetch. The scalar co-issue
  // instructions are expanded explicitly here.
  const float radial_term =
      dot(input.world_position.xz, input.world_position.xz) +
      venue_e33_c254.z;
  const float sphere_radius_squared =
      venue_e33_c20.z * venue_e33_c20.z;
  const float3 view_direction = VenueE33SafeNormalize(
      input.world_position - venue_e33_c19.xyz);
  const float reflection_dot =
      dot(input.world_normal, -view_direction);
  const float3 reflection_direction =
      (reflection_dot + reflection_dot) * input.world_normal +
      view_direction;
  const float doubled_position_dot =
      2.0f * dot(reflection_direction, input.world_position);
  const float sphere_term =
      dot(input.world_position, input.world_position) -
      venue_e33_c48.x * venue_e33_c48.x;
  const float discriminant =
      doubled_position_dot * doubled_position_dot -
      sphere_term * venue_e33_c254.w;
  const float distance =
      (-doubled_position_dot + sqrt(abs(discriminant))) *
      venue_e33_c254.y;
  const float3 environment_direction =
      distance * reflection_direction + input.world_position;

  const float4 base = venue_e33_texture0.Sample(
      venue_e33_wrap_sampler, input.texture_coordinates);
  const float3 detail = venue_e33_texture1.Sample(
      venue_e33_wrap_sampler, input.texture_coordinates).rgb;
  const float4 environment = venue_e33_texture2.Sample(
      venue_e33_cube_sampler, environment_direction);

  // Exact instructions 29-35, including the non-intuitive register swizzles.
  const float3 environment_delta =
      environment.zyx - base.zyx;
  float3 detail_environment =
      (detail * venue_e33_c46.xyz).zxy * environment.a;
  const float3 reflected_base =
      environment_delta.yxz * venue_e33_c47.x + base.yzx;
  float3 linear_color =
      reflected_base.xzy * input.color.yxz +
      detail_environment.zyx;
  linear_color = linear_color.yxz *
      (venue_e33_c255.x + sphere_radius_squared);

  const float3 display_color = pow(
      saturate(linear_color),
      venue_e33_observer_options.yyy);
  return float4(
      display_color,
      base.a * input.color.a * venue_e33_observer_options.x);
}
