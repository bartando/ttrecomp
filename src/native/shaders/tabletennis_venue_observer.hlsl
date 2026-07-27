cbuffer VenueConstants : register(b0) {
  float4 world_view_projection_row0;
  float4 world_view_projection_row1;
  float4 world_view_projection_row2;
  float4 world_view_projection_row3;
  // x: c20.z * c46.x, y: observer opacity, z: display gamma reciprocal,
  // w: checkerboard comparison enabled.
  float4 material_options;
};

#if defined(TABLETENNIS_VULKAN)
Texture2D base_texture : register(t0, space1);
Texture2D detail_texture : register(t1, space1);
SamplerState base_sampler : register(s1, space0);
SamplerState detail_sampler : register(s2, space0);
#else
Texture2D base_texture : register(t0);
Texture2D detail_texture : register(t1);
SamplerState base_sampler : register(s0);
SamplerState detail_sampler : register(s1);
#endif

struct VertexInput {
  float3 position : POSITION;
  float2 texcoord0 : TEXCOORD0;
  float2 texcoord1 : TEXCOORD1;
  float4 color : COLOR0;
};

struct VertexOutput {
  float4 position : SV_Position;
  float2 texcoord0 : TEXCOORD0;
  float2 texcoord1 : TEXCOORD1;
  float4 color : COLOR0;
};

VertexOutput vs_main(VertexInput input) {
  VertexOutput output;
  output.position =
      input.position.x * world_view_projection_row0 +
      input.position.y * world_view_projection_row1 +
      input.position.z * world_view_projection_row2 +
      world_view_projection_row3;
  output.texcoord0 = input.texcoord0;
  output.texcoord1 = input.texcoord1;
  output.color = input.color;
  return output;
}

float4 ps_main(VertexOutput input) : SV_Target {
  if (material_options.w > 0.5f) {
    const uint2 tile = uint2(input.position.xy) >> 3;
    if (((tile.x ^ tile.y) & 1u) != 0u) {
      discard;
    }
  }
  const float4 base =
      base_texture.Sample(base_sampler, input.texcoord0);
  const float3 detail =
      detail_texture.Sample(detail_sampler, input.texcoord1).rgb;
  float3 color =
      base.rgb * detail * input.color.rgb * material_options.x;
  // The observer runs after the guest gamma-ramp pass. The real in-order
  // replacement renders to the linear scene target and must omit this.
  color = pow(saturate(color), material_options.z);
  return float4(
      color, base.a * input.color.a * material_options.y);
}

// Exact in-order material path. This runs on the guest scene target before
// its gamma/post stack, so observer opacity, checkerboard and display-gamma
// correction must not leak into it.
float4 ps_replace(VertexOutput input) : SV_Target {
  const float4 base =
      base_texture.Sample(base_sampler, input.texcoord0);
  const float3 detail =
      detail_texture.Sample(detail_sampler, input.texcoord1).rgb;
  return float4(
      base.rgb * detail * input.color.rgb * material_options.x,
      base.a * input.color.a);
}
