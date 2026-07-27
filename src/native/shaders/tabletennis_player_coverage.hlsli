// Exact native-attachment alpha-to-coverage contract for the CA9 player
// prepass. This mirrors the non-interlock Vulkan path in
// SpirvShaderTranslator::CompletePixelShader_WriteToOutput.

struct PlayerPrepassOutput {
  float4 color : SV_Target;
  uint coverage : SV_Coverage;
};

uint PlayerXenos4xCoverageMask(float alpha, float2 fragment_position) {
  const uint2 pixel = uint2(fragment_position);
  const uint offset_index =
      (pixel.x & 1u) | ((pixel.y & 1u) << 1u);
  // RB_COLORCONTROL 0x87000015 contains offsets 3, 1, 0, 2 in bits
  // 24:31. Keep the packed representation so the extraction is identical to
  // the guest translator instead of turning the offsets into an invented
  // spatial table.
  const uint offset =
      (0x87u >> (offset_index << 1u)) & 3u;
  const float threshold_offset = float(offset) * (1.0f / 16.0f);

  uint coverage = 0u;
  // Guest TL, BL, TR, BR samples map to Vulkan TL, TR, BL, BR indices.
  coverage |= alpha >= (0.75f - threshold_offset) ? (1u << 0u) : 0u;
  coverage |= alpha >= (0.25f - threshold_offset) ? (1u << 2u) : 0u;
  coverage |= alpha >= (0.50f - threshold_offset) ? (1u << 1u) : 0u;
  coverage |= alpha >= (1.00f - threshold_offset) ? (1u << 3u) : 0u;
  return coverage;
}
