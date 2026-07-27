// Native observer port of the trace-verified
// BD4B1DF972B828B7 / C6CEFDA3753CF2BA crowd family.
//
// vf95 and vf92 remain in the title's original 8-in-32 guest byte order.
// The vertex shader decodes them directly so the comparison path never
// invents a replacement vertex layout.

cbuffer CrowdConstants : register(b0) {
  float4 crowd_instance_transform[4];  // c32-c35
  float4 crowd_view_projection[4];     // c36-c39
  float4 crowd_light_position_inverse_radius[3];  // c136-c138
  float4 crowd_light_color_intensity[3];          // c139-c141
  float4 crowd_ambient;                           // c145
  float4 crowd_decode_constants;                  // c255
  // x: vf95 base byte, y: vf92 base byte, z: vf92 record count,
  // w: vf95 stride (36 in the proven family).
  uint4 crowd_buffer_layout;
};

#if defined(TABLETENNIS_VULKAN)
StructuredBuffer<uint> crowd_vertex_words : register(t1, space0);
// Keep a different element type from vf95. Glslang otherwise merges the two
// storage-buffer block types and SPIRV-Cross may emit an invalid Metal
// argument-buffer declaration.
StructuredBuffer<uint2> crowd_palette_word_pairs : register(t2, space0);
Texture3D crowd_texture : register(t0, space1);
SamplerState crowd_sampler : register(s3, space0);
#else
StructuredBuffer<uint> crowd_vertex_words : register(t0);
StructuredBuffer<uint2> crowd_palette_word_pairs : register(t1);
Texture3D crowd_texture : register(t2);
SamplerState crowd_sampler : register(s0);
#endif

static const uint kCrowdVertexStride = 36u;
static const uint kCrowdPaletteRecordStride = 28u;

struct CrowdBoneBasis {
  float3 column0;
  float3 column1;
  float3 column2;
  float3 translation;
};

struct CrowdVaryings {
  float4 position : SV_Position;
  float3 texture_coordinate : TEXCOORD0;
  float3 lighting : TEXCOORD1;
};

uint CrowdByteSwap32(uint value) {
  return ((value & 0x000000FFu) << 24u) |
         ((value & 0x0000FF00u) << 8u) |
         ((value & 0x00FF0000u) >> 8u) |
         ((value & 0xFF000000u) >> 24u);
}

uint CrowdLoadVertexWord(uint byte_address) {
  return CrowdByteSwap32(crowd_vertex_words[byte_address >> 2u]);
}

float CrowdLoadVertexFloat(uint byte_address) {
  return asfloat(CrowdLoadVertexWord(byte_address));
}

uint CrowdLoadPaletteWord(uint byte_address) {
  const uint word_index = byte_address >> 2u;
  const uint2 pair = crowd_palette_word_pairs[word_index >> 1u];
  return CrowdByteSwap32((word_index & 1u) != 0u ? pair.y : pair.x);
}

float CrowdLoadPaletteFloat(uint byte_address) {
  return asfloat(CrowdLoadPaletteWord(byte_address));
}

float CrowdSigned10(uint packed, uint shift) {
  const uint field = (packed >> shift) & 0x3FFu;
  return float(int(field << 22u) >> 22);
}

float3 CrowdPackedSigned101010(uint packed) {
  return float3(
      CrowdSigned10(packed, 0u),
      CrowdSigned10(packed, 10u),
      CrowdSigned10(packed, 20u));
}

float3 CrowdSafeNormalize(float3 value) {
  const float length_squared = dot(value, value);
  return length_squared > 0.0f
             ? value * rsqrt(length_squared)
             : float3(0.0f, 0.0f, 1.0f);
}

CrowdBoneBasis CrowdLoadBone(uint record_index) {
  const uint base =
      crowd_buffer_layout.y + record_index * kCrowdPaletteRecordStride;
  const float4 quaternion = float4(
      CrowdLoadPaletteFloat(base + 0u),
      CrowdLoadPaletteFloat(base + 4u),
      CrowdLoadPaletteFloat(base + 8u),
      CrowdLoadPaletteFloat(base + 12u));

  const float x = quaternion.x;
  const float y = quaternion.y;
  const float z = quaternion.z;
  const float w = quaternion.w;
  const float r9x = 2.0f * (x * y - z * w);
  const float r9y = 2.0f * (x * z - y * w);
  const float r9z = 1.0f - 2.0f * (y * y + z * z);
  const float r2x = 2.0f * (x * y + z * w);
  const float r2y = 2.0f * (y * z + x * w);
  const float r2w = 1.0f - 2.0f * (x * x + z * z);

  const float r0x = r2x * r2y - r9y * r2w;
  const float r0z = r9y * r9x - r9z * r2y;
  const float r0y = -r2x * r9x + r9z * r2w;

  CrowdBoneBasis bone;
  bone.column0 = float3(r9z, r2x, r9y);
  bone.column1 = float3(r9x, r2w, r2y);
  bone.column2 = float3(r0x, r0z, r0y);
  bone.translation = float3(
      CrowdLoadPaletteFloat(base + 16u),
      CrowdLoadPaletteFloat(base + 20u),
      CrowdLoadPaletteFloat(base + 24u));
  return bone;
}

float3 CrowdTransformDirection(CrowdBoneBasis basis, float3 value) {
  return basis.column0 * value.x +
         basis.column1 * value.y +
         basis.column2 * value.z;
}

float3 CrowdApplyInstanceDirection(float3 value) {
  return crowd_instance_transform[0].xyz * value.x +
         crowd_instance_transform[1].xyz * value.y +
         crowd_instance_transform[2].xyz * value.z;
}

float3 CrowdApplyInstancePosition(float3 value) {
  return CrowdApplyInstanceDirection(value) +
         crowd_instance_transform[3].xyz;
}

float4 CrowdApplyViewProjection(float3 value) {
  // This is the simplified form of instructions 45-47 after resolving their
  // temporary-register swizzles.
  return value.x * crowd_view_projection[0] +
         value.y * crowd_view_projection[1] +
         value.z * crowd_view_projection[2] +
         crowd_view_projection[3];
}

float3 CrowdBuildLighting(float3 world_position, float3 world_normal) {
  float3 lighting = crowd_ambient.xyz;
  [unroll]
  for (uint light_index = 0u; light_index < 3u; ++light_index) {
    const float4 position_inverse_radius =
        crowd_light_position_inverse_radius[light_index];
    const float4 color_intensity =
        crowd_light_color_intensity[light_index];
    const float3 delta = position_inverse_radius.xyz - world_position;
    const float3 radius_space = delta * position_inverse_radius.w;
    const float attenuation =
        color_intensity.w *
        (1.0f - saturate(dot(radius_space, radius_space)));
    const float diffuse =
        max(dot(world_normal, CrowdSafeNormalize(delta)), 0.0f);
    lighting += color_intensity.xyz * diffuse * attenuation;
  }
  return lighting;
}

CrowdVaryings vs_main(uint vertex_id : SV_VertexID) {
  const uint stride =
      crowd_buffer_layout.w != 0u
          ? crowd_buffer_layout.w
          : kCrowdVertexStride;
  const uint base = crowd_buffer_layout.x + vertex_id * stride;
  const float3 local_position = float3(
      CrowdLoadVertexFloat(base + 0u),
      CrowdLoadVertexFloat(base + 4u),
      CrowdLoadVertexFloat(base + 8u));
  // The Xenos vfetch writes FMT_8_8_8_8 component Z, which is guest word
  // bits 16..23 after the 8-in-32 endian conversion.
  const uint palette_record =
      (CrowdLoadVertexWord(base + 16u) >> 16u) & 0xFFu;
  const float3 local_normal =
      CrowdPackedSigned101010(CrowdLoadVertexWord(base + 20u));
  const float2 texture_coordinate = float2(
      CrowdLoadVertexFloat(base + 24u),
      CrowdLoadVertexFloat(base + 28u));

  CrowdVaryings output;
  if (palette_record >= crowd_buffer_layout.z) {
    output.position = float4(2.0f, 2.0f, 2.0f, 1.0f);
    output.texture_coordinate = 0.0f;
    output.lighting = 0.0f;
    return output;
  }

  const CrowdBoneBasis basis = CrowdLoadBone(palette_record);
  const float3 world_position = CrowdApplyInstancePosition(
      CrowdTransformDirection(basis, local_position) + basis.translation);
  const float3 world_normal = CrowdSafeNormalize(
      CrowdApplyInstanceDirection(
          CrowdTransformDirection(basis, local_normal)));

  output.position = CrowdApplyViewProjection(world_position);
  output.texture_coordinate =
      float3(texture_coordinate, crowd_instance_transform[0].w);
  output.lighting = CrowdBuildLighting(world_position, world_normal);
  return output;
}

float4 ps_main(CrowdVaryings input) : SV_Target {
  // The proven fetch uses linear XY filtering but point filtering through the
  // volume. Native static samplers expose one filter for all dimensions, so
  // pin Z to the selected texel center before the anisotropic XY sample.
  static const float kCrowdVolumeDepth = 8.0f;
  const float layer =
      floor(frac(input.texture_coordinate.z) * kCrowdVolumeDepth);
  const float layer_center = (layer + 0.5f) / kCrowdVolumeDepth;
  const float3 texel =
      crowd_texture.Sample(
          crowd_sampler,
          float3(input.texture_coordinate.xy, layer_center)).rgb;
  return float4(saturate(texel * input.lighting), 1.0f);
}
