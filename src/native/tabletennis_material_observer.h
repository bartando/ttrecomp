#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tabletennis::native {

struct MaterialDrawObservation {
  uint32_t shader = 0;
  uint32_t shader_vtable = 0;
  uint32_t shader_group = 0;
  uint32_t model = 0;
  uint32_t geometry_index = 0;
  uint32_t lod = 0;
  bool alternate_pass = false;
};

struct MaterialDeviceCommand {
  uint32_t subobject_offset = 0;
  uint32_t argument = 0;
  uint32_t callback = 0;
};

struct MaterialSamplerStateCommand {
  uint16_t argument_or_index = 0;
  uint16_t subobject_offset = 0;
  uint32_t value = 0;
  uint32_t callback = 0;
};

struct MaterialTextureFetch {
  uint32_t offset = 0;
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t dimension = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t pitch_pixels = 0;
  uint32_t base_address = 0;
  uint32_t mip_address = 0;
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  bool tiled = false;
  bool packed_mips = false;
  bool valid = false;
};

struct MaterialTextureParameterObservation {
  MaterialDrawObservation draw{};
  uint32_t owner_shader = 0;
  uint32_t fx_runtime = 0;
  uint32_t encoded_handle = 0;
  uint32_t supplied_resource = 0;
  uint32_t effective_resource = 0;
  uint32_t resource_vtable = 0;
  uint32_t gpu_binding = 0;
  std::array<uint32_t, 13> gpu_binding_words{};
  std::array<uint32_t, 6> texture_fetch_words{};
  uint32_t gpu_binding_word_count = 0;
  uint32_t texture_fetch_offset = 0;
  bool gpu_binding_words_stable = false;
  MaterialTextureFetch decoded_fetch{};
  uint32_t guest_read_failures = 0;
  bool used_fallback = false;
  bool valid = false;
};

struct MaterialPassObservation {
  static constexpr size_t kMaxDeviceCommands = 32;
  static constexpr size_t kMaxSamplerStateCommands = 32;

  MaterialDrawObservation draw{};
  uint32_t runtime_state = 0;
  uint32_t graphics_device = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t vertex_shader_reference = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader_reference = 0;
  uint32_t pixel_shader = 0;
  uint32_t device_command_list = 0;
  uint32_t device_command_count = 0;
  uint32_t captured_device_command_count = 0;
  uint32_t sampler_state_list = 0;
  uint32_t sampler_state_count = 0;
  uint32_t captured_sampler_state_count = 0;
  uint32_t guest_read_failures = 0;
  bool valid = false;
  std::array<MaterialDeviceCommand, kMaxDeviceCommands> device_commands{};
  std::array<MaterialSamplerStateCommand, kMaxSamplerStateCommands>
      sampler_state_commands{};
};

struct MaterialFrameObservation {
  static constexpr size_t kMaxUniqueDraws = 64;
  static constexpr size_t kMaxUniquePasses = 64;
  static constexpr size_t kMaxUniqueTextureParameters = 64;

  uint64_t sequence = 0;
  uint32_t draw_count = 0;
  uint32_t unique_draw_count = 0;
  uint32_t dropped_unique_draws = 0;
  uint32_t pass_apply_count = 0;
  uint32_t unique_pass_count = 0;
  uint32_t dropped_unique_passes = 0;
  uint32_t texture_parameter_bind_count = 0;
  uint32_t unique_texture_parameter_count = 0;
  uint32_t dropped_unique_texture_parameters = 0;
  uint32_t rejected_vtables = 0;
  uint32_t guest_read_failures = 0;
  std::array<MaterialDrawObservation, kMaxUniqueDraws> unique_draws{};
  std::array<MaterialPassObservation, kMaxUniquePasses> unique_passes{};
  std::array<MaterialTextureParameterObservation,
             kMaxUniqueTextureParameters>
      unique_texture_parameters{};
};

// Bracket rage::grmShaderFx::DrawModelGeometry at 0x820EFB30. The surrounding
// model-draw scope must already prove that the model belongs to lvlTable.
// Every call is pushed, including non-table calls, so nesting cannot inherit
// an outer material association.
void BeginTableMaterialDraw(uint8_t* guest_base, uint32_t shader,
                            uint32_t model, uint32_t geometry_index,
                            uint32_t lod, bool alternate_pass);
void EndTableMaterialDraw();

// Observe rage::grmShaderFx pass application at 0x82158C48. This is where the
// title selects the real VS/PS pair and executes the pass's material command
// lists. The original pass remains authoritative and always runs.
void ObserveTableMaterialPass(uint8_t* guest_base, uint32_t runtime_state,
                              uint32_t pass_descriptor);

// True only while DrawIndexedPrimitive is inside the structurally recovered
// two-texture visible table/net pass (five device commands, twelve sampler
// commands). This lets the camera observer retain that pass's exact WVP
// instead of whichever valid perspective pass happened to run last.
bool CurrentTableVisibleMaterialPass();

// Observe the type-6 Fx parameter setter at 0x8215A830. Resource bindings are
// created during material build/refresh, before DrawModelGeometry, so they are
// cached by owner shader and joined only after the table draw proves ownership.
void ObserveTableTextureParameterBind(uint8_t* guest_base,
                                      uint32_t fx_runtime,
                                      uint32_t encoded_handle,
                                      uint32_t resource);

// Observe the return value of the two concrete +0x50 resource unwrappers used
// by the proven material: grcTextureReference and grcTextureXenon.
void ObserveTextureResourceUnwrap(uint8_t* guest_base, uint32_t resource,
                                  uint32_t gpu_binding);

void MaterialObserverFrameEnd();
MaterialFrameObservation LatestMaterialFrameObservation();

}  // namespace tabletennis::native
