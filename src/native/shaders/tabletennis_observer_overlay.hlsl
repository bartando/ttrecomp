cbuffer OverlayConstants : register(b0) {
  float4 view_projection_row0;
  float4 view_projection_row1;
  float4 view_projection_row2;
  float4 view_projection_row3;
  float4 overlay_color;
  float4 overlay_options;
};

// NRHI's Vulkan descriptor plan separates texture tables into set 1 while
// keeping constants and immutable samplers in set 0. D3D12 uses the original
// register spaces, so the offline SPIR-V build selects the Vulkan mapping
// explicitly instead of relying on compiler auto-mapping.
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
      input.position.x * view_projection_row0 +
      input.position.y * view_projection_row1 +
      input.position.z * view_projection_row2 +
      view_projection_row3;
  output.texcoord0 = input.texcoord0;
  output.texcoord1 = input.texcoord1;
  output.color = input.color;
  return output;
}

float4 ps_main(VertexOutput input,
               bool is_front_face : SV_IsFrontFace) : SV_Target {
  if (overlay_options.x > 0.5f) {
    return float4(1.0f, 0.0f, 1.0f, overlay_color.a);
  }

  float2 detail_uv = input.texcoord1;
  if (!is_front_face) {
    detail_uv.x = 1.0f - detail_uv.x;
  }

  float4 base =
      base_texture.Sample(base_sampler, input.texcoord0);
  float4 detail =
      detail_texture.Sample(detail_sampler, detail_uv);
  float3 color =
      lerp(base.rgb, detail.rgb, detail.a) * input.color.rgb;
  float alpha =
      base.a * (2.0f - base.a) * overlay_color.a;
  return float4(color, alpha);
}
