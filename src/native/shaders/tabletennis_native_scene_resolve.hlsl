// Exact four-sample box resolve for the shared RGBA8 native-scene target.
// NRHI has no ResolveSubresource command, so this deliberately mirrors the
// Skate 3 native renderer's shader-resolve path.

#if defined(TABLETENNIS_VULKAN)
Texture2DMS<float4> native_scene_msaa_color : register(t0, space1);
#else
Texture2DMS<float4> native_scene_msaa_color : register(t0);
#endif

float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
  const float2 corner =
      float2((vertex_id << 1u) & 2u, vertex_id & 2u);
  return float4(
      corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
      0.0f, 1.0f);
}

float4 ps_main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  float4 sum = 0.0f;
  [unroll]
  for (int sample_index = 0; sample_index < 4; ++sample_index) {
    sum += native_scene_msaa_color.Load(pixel, sample_index);
  }
  return sum * 0.25f;
}
