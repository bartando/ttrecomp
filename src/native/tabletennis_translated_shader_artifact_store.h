#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/graphics/native_guest_renderer.h>

namespace tabletennis::native {

enum class TranslatedShaderStage : uint8_t {
  kVertex,
  kPixel,
};

struct TranslatedShaderArtifactKey {
  uint64_t shader_hash = 0;
  uint64_t modification = 0;
  TranslatedShaderStage stage = TranslatedShaderStage::kVertex;

  bool valid() const { return shader_hash != 0; }
  auto operator<=>(const TranslatedShaderArtifactKey &) const = default;
};

// Immutable value-owned translated program. No live VkShaderModule, pipeline,
// descriptor set, command list or cache object escapes the backend.
struct TranslatedShaderArtifact {
  TranslatedShaderArtifactKey key{};
  rex::graphics::NativeGuestOutputBackend backend =
      rex::graphics::NativeGuestOutputBackend::kUnknown;
  std::vector<uint32_t> spirv;
  uint32_t used_texture_fetch_mask = 0;
  std::vector<rex::graphics::NativeGuestShaderArtifactContext::TextureBinding>
      texture_bindings;
  std::vector<rex::graphics::NativeGuestShaderArtifactContext::SamplerBinding>
      sampler_bindings;
  uint32_t descriptor_set_shared_memory_and_edram = 0;
  uint32_t descriptor_set_constants = 0;
  uint32_t descriptor_set_textures_vertex = 0;
  uint32_t descriptor_set_textures_pixel = 0;
  uint32_t descriptor_set_count = 0;
  uint32_t shared_memory_binding = 0;
  uint32_t edram_binding = 0;
  uint32_t constant_buffer_system_binding = 0;
  uint32_t constant_buffer_float_vertex_binding = 0;
  uint32_t constant_buffer_float_pixel_binding = 0;
  uint32_t constant_buffer_bool_loop_binding = 0;
  uint32_t constant_buffer_fetch_binding = 0;
  uint32_t constant_buffer_count = 0;

  bool valid() const;
};

bool TranslatedShaderArtifactStoreEnabled();
void InstallTranslatedShaderArtifactStore();
void ShutdownTranslatedShaderArtifactStore();

// The backend prefilter accepts only keys explicitly requested here. This
// prevents broad SPIR-V copying and guarantees modification identity is part
// of selection.
void RequestTranslatedShaderArtifact(TranslatedShaderArtifactKey key);
std::shared_ptr<const TranslatedShaderArtifact>
FindTranslatedShaderArtifact(TranslatedShaderArtifactKey key);

// Phase-zero backend replay proof for the stable vertex-color rectangle. The
// opaque token remains usable only in its backend frame; callers must provide
// that frame when looking it up and may never dereference the token.
std::shared_ptr<
    const rex::graphics::NativeGuestTranslatedReplayTokenContext>
FindTranslatedRectangleReplayToken(uint64_t backend_frame_sequence);

} // namespace tabletennis::native
