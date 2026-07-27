#include "native/tabletennis_material_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_draw_constants.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_observer_overlay.h"
#include "native/tabletennis_texture_snapshot.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_native_material_log_interval, 0, "Table Tennis",
    "Frames between observer-only grmShaderFx table-material reports "
    "(0 disables).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

// Confirmed by the RTTI descriptor at 0x825D352C/0x825D3534.
constexpr uint32_t kGrmShaderFxVtable = 0x8202F2DC;
constexpr uint32_t kFallbackFxResource = 0x825EBA20;
constexpr size_t kMaxMaterialScopeDepth = 8;
constexpr size_t kMaxPendingTextureParameters = 1024;

struct MaterialDrawScope {
  bool observed_table_material = false;
  bool visible_material_pass = false;
  MaterialDrawObservation draw{};
};

std::mutex g_material_mutex;
MaterialFrameObservation g_building_frame;
MaterialFrameObservation g_published_frame;
std::array<MaterialPassObservation,
           MaterialFrameObservation::kMaxUniquePasses>
    g_known_passes;
uint32_t g_known_pass_count = 0;
std::array<MaterialTextureParameterObservation,
           kMaxPendingTextureParameters>
    g_pending_texture_parameters;
uint32_t g_pending_texture_parameter_count = 0;
uint64_t g_dropped_pending_texture_parameters = 0;
thread_local std::array<MaterialDrawScope, kMaxMaterialScopeDepth>
    g_material_scope_stack;
thread_local size_t g_material_scope_depth = 0;

uint32_t LoadBeU32(const std::byte* data) {
  uint32_t value;
  std::memcpy(&value, data, sizeof(value));
  return std::byteswap(value);
}

uint16_t LoadBeU16(const std::byte* data) {
  uint16_t value;
  std::memcpy(&value, data, sizeof(value));
  return std::byteswap(value);
}

bool MaterialObservationEnabled() {
  return ObserverOverlayEnabled() ||
         REXCVAR_GET(tabletennis_native_material_log_interval) != 0;
}

bool SameDraw(const MaterialDrawObservation& left,
              const MaterialDrawObservation& right) {
  return left.shader == right.shader && left.model == right.model &&
         left.geometry_index == right.geometry_index &&
         left.lod == right.lod &&
         left.alternate_pass == right.alternate_pass;
}

bool SamePass(const MaterialPassObservation& left,
              const MaterialPassObservation& right) {
  return SameDraw(left.draw, right.draw) &&
         left.pass_descriptor == right.pass_descriptor &&
         left.program_pair == right.program_pair;
}

bool SamePassKey(const MaterialPassObservation& left,
                 const MaterialDrawObservation& draw,
                 uint32_t pass_descriptor) {
  return SameDraw(left.draw, draw) &&
         left.pass_descriptor == pass_descriptor;
}

bool IsVisibleMaterialPass(const MaterialPassObservation& pass) {
  return pass.valid && pass.device_command_count == 5 &&
         pass.sampler_state_count == 12;
}

const MaterialDrawScope* CurrentMaterialScope() {
  if (g_material_scope_depth == 0 ||
      g_material_scope_depth > g_material_scope_stack.size()) {
    return nullptr;
  }
  return &g_material_scope_stack[g_material_scope_depth - 1];
}

MaterialDrawScope* CurrentMaterialScopeMutable() {
  if (g_material_scope_depth == 0 ||
      g_material_scope_depth > g_material_scope_stack.size()) {
    return nullptr;
  }
  return &g_material_scope_stack[g_material_scope_depth - 1];
}

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t& result) {
  if (address == 0) {
    return false;
  }
  const uint64_t start = static_cast<uint64_t>(address) + offset;
  const uint64_t end = start + size;
  if (start > std::numeric_limits<uint32_t>::max() ||
      end > (uint64_t{1} << 32)) {
    return false;
  }
  result = static_cast<uint32_t>(start);
  return true;
}

bool TryReadBeU32(uint8_t* guest_base, uint32_t address, size_t offset,
                  uint32_t& value) {
  uint32_t guest_address;
  if (guest_base == nullptr ||
      !CheckedGuestOffset(address, offset, sizeof(uint32_t), guest_address)) {
    return false;
  }
  std::array<std::byte, sizeof(uint32_t)> bytes;
  const void* host_address =
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address);
  if (!GuestTryCopy(bytes.data(), host_address, bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data());
  return true;
}

bool TryReadBeU16(uint8_t* guest_base, uint32_t address, size_t offset,
                  uint16_t& value) {
  uint32_t guest_address;
  if (guest_base == nullptr ||
      !CheckedGuestOffset(address, offset, sizeof(uint16_t), guest_address)) {
    return false;
  }
  std::array<std::byte, sizeof(uint16_t)> bytes;
  const void* host_address =
      guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address);
  if (!GuestTryCopy(bytes.data(), host_address, bytes.size())) {
    return false;
  }
  value = LoadBeU16(bytes.data());
  return true;
}

void StoreFramePassLocked(const MaterialPassObservation& pass) {
  const auto begin = g_building_frame.unique_passes.begin();
  const auto used_end = begin + g_building_frame.unique_pass_count;
  if (std::find_if(begin, used_end, [&](const auto& existing) {
        return SamePass(existing, pass);
      }) != used_end) {
    return;
  }
  if (g_building_frame.unique_pass_count ==
      g_building_frame.unique_passes.size()) {
    ++g_building_frame.dropped_unique_passes;
    return;
  }
  g_building_frame.unique_passes[g_building_frame.unique_pass_count++] =
      pass;
}

bool SameTextureParameter(
    const MaterialTextureParameterObservation& left,
    const MaterialTextureParameterObservation& right) {
  return left.owner_shader == right.owner_shader &&
         left.fx_runtime == right.fx_runtime &&
         left.encoded_handle == right.encoded_handle &&
         left.effective_resource == right.effective_resource;
}

void StoreFrameTextureParameterLocked(
    const MaterialTextureParameterObservation& texture) {
  const auto begin = g_building_frame.unique_texture_parameters.begin();
  const auto used_end =
      begin + g_building_frame.unique_texture_parameter_count;
  if (std::find_if(begin, used_end, [&](const auto& existing) {
        return SameTextureParameter(existing, texture);
      }) != used_end) {
    return;
  }
  if (g_building_frame.unique_texture_parameter_count ==
      g_building_frame.unique_texture_parameters.size()) {
    ++g_building_frame.dropped_unique_texture_parameters;
    return;
  }
  g_building_frame.unique_texture_parameters
      [g_building_frame.unique_texture_parameter_count++] = texture;
}

MaterialTextureFetch DecodeTextureFetch(
    uint32_t offset, const std::array<uint32_t, 6>& words) {
  MaterialTextureFetch fetch;
  fetch.offset = offset;
  if ((words[0] & 0x3u) != 2 || words[1] == 0) {
    return fetch;
  }

  fetch.pitch_pixels = ((words[0] >> 22) & 0x1FFu) << 5;
  fetch.tiled = (words[0] >> 31) != 0;
  fetch.format = words[1] & 0x3Fu;
  fetch.endianness = (words[1] >> 6) & 0x3u;
  fetch.base_address = ((words[1] >> 12) & 0xFFFFFu) << 12;
  fetch.dimension = (words[5] >> 9) & 0x3u;
  switch (fetch.dimension) {
    case 0:
      fetch.width = (words[2] & 0xFFFFFFu) + 1;
      fetch.height = 1;
      fetch.depth = 1;
      break;
    case 1:
    case 3:
      fetch.width = (words[2] & 0x1FFFu) + 1;
      fetch.height = ((words[2] >> 13) & 0x1FFFu) + 1;
      fetch.depth =
          fetch.dimension == 3 ? 6 : ((words[2] >> 26) & 0x3Fu) + 1;
      break;
    case 2:
      fetch.width = (words[2] & 0x7FFu) + 1;
      fetch.height = ((words[2] >> 11) & 0x7FFu) + 1;
      fetch.depth = ((words[2] >> 22) & 0x3FFu) + 1;
      break;
  }
  fetch.mip_min_level = (words[4] >> 2) & 0xFu;
  fetch.mip_max_level = (words[4] >> 6) & 0xFu;
  fetch.packed_mips = ((words[5] >> 11) & 0x1u) != 0;
  fetch.mip_address = ((words[5] >> 12) & 0xFFFFFu) << 12;
  fetch.valid = fetch.width != 0 && fetch.height != 0 &&
                fetch.pitch_pixels != 0 && fetch.base_address != 0;
  return fetch;
}

}  // namespace

void BeginTableMaterialDraw(uint8_t* guest_base, uint32_t shader,
                            uint32_t model, uint32_t geometry_index,
                            uint32_t lod, bool alternate_pass) {
  MaterialDrawScope scope;
  uint32_t shader_group = 0;
  if (!MaterialObservationEnabled() || guest_base == nullptr ||
      shader == 0 || model == 0 ||
      !CurrentObservedTableModelDraw(model, shader_group)) {
    if (g_material_scope_depth < g_material_scope_stack.size()) {
      g_material_scope_stack[g_material_scope_depth] = scope;
    }
    ++g_material_scope_depth;
    return;
  }

  std::array<std::byte, sizeof(uint32_t)> vtable_bytes;
  const void* shader_host_address =
      guest_base + shader + REX_PHYS_HOST_OFFSET(shader);
  if (!GuestTryCopy(vtable_bytes.data(), shader_host_address,
                    vtable_bytes.size())) {
    std::lock_guard lock(g_material_mutex);
    ++g_building_frame.guest_read_failures;
    if (g_material_scope_depth < g_material_scope_stack.size()) {
      g_material_scope_stack[g_material_scope_depth] = scope;
    }
    ++g_material_scope_depth;
    return;
  }

  MaterialDrawObservation observation;
  observation.shader = shader;
  observation.shader_vtable = LoadBeU32(vtable_bytes.data());
  observation.shader_group = shader_group;
  observation.model = model;
  observation.geometry_index = geometry_index;
  observation.lod = lod;
  observation.alternate_pass = alternate_pass;
  scope.observed_table_material =
      observation.shader_vtable == kGrmShaderFxVtable;
  scope.draw = observation;
  if (g_material_scope_depth < g_material_scope_stack.size()) {
    g_material_scope_stack[g_material_scope_depth] = scope;
  }
  ++g_material_scope_depth;

  std::array<MaterialTextureParameterObservation,
             MaterialFrameObservation::kMaxUniqueTextureParameters>
      joined_texture_parameters{};
  uint32_t joined_texture_parameter_count = 0;
  {
    std::lock_guard lock(g_material_mutex);
    ++g_building_frame.draw_count;
    if (!scope.observed_table_material) {
      ++g_building_frame.rejected_vtables;
      return;
    }

    const auto begin = g_building_frame.unique_draws.begin();
    const auto used_end = begin + g_building_frame.unique_draw_count;
    if (std::find_if(begin, used_end, [&](const auto& existing) {
          return SameDraw(existing, observation);
        }) != used_end) {
      return;
    }
    if (g_building_frame.unique_draw_count ==
        g_building_frame.unique_draws.size()) {
      ++g_building_frame.dropped_unique_draws;
      return;
    }
    g_building_frame
        .unique_draws[g_building_frame.unique_draw_count++] = observation;

    // Type-6 resource parameters were assigned while the material was built
    // or refreshed, before this draw. Joining by the now-proven owner shader
    // keeps unrelated materials out without relying on transient heap
    // addresses.
    for (uint32_t index = 0;
         index < g_pending_texture_parameter_count; ++index) {
      const MaterialTextureParameterObservation& pending =
          g_pending_texture_parameters[index];
      if (pending.owner_shader != observation.shader) {
        continue;
      }
      MaterialTextureParameterObservation joined = pending;
      joined.draw = observation;
      ++g_building_frame.texture_parameter_bind_count;
      StoreFrameTextureParameterLocked(joined);
      if (joined_texture_parameter_count <
          joined_texture_parameters.size()) {
        joined_texture_parameters[joined_texture_parameter_count++] =
            joined;
      }
    }
  }

  // Guest payload reads are fault-guarded and may copy megabytes. Never do
  // that work under the frame-observer mutex.
  if (ObserverOverlayEnabled() || NetBB903ObserverEnabled()) {
    for (uint32_t index = 0; index < joined_texture_parameter_count; ++index) {
      TryCaptureTableTextureSnapshot(guest_base,
                                     joined_texture_parameters[index]);
    }
  }
}

void EndTableMaterialDraw() {
  if (g_material_scope_depth != 0) {
    --g_material_scope_depth;
  }
}

bool CurrentTableVisibleMaterialPass() {
  const MaterialDrawScope* scope = CurrentMaterialScope();
  return scope != nullptr && scope->observed_table_material &&
         scope->visible_material_pass;
}

void ObserveTableMaterialPass(uint8_t* guest_base, uint32_t runtime_state,
                              uint32_t pass_descriptor) {
  const MaterialDrawScope* scope = CurrentMaterialScope();
  if (scope == nullptr || !scope->observed_table_material ||
      guest_base == nullptr || runtime_state == 0 || pass_descriptor == 0) {
    return;
  }

  {
    std::lock_guard lock(g_material_mutex);
    ++g_building_frame.pass_apply_count;
    const auto known_end = g_known_passes.begin() + g_known_pass_count;
    const auto known =
        std::find_if(g_known_passes.begin(), known_end,
                     [&](const auto& existing) {
                       return SamePassKey(existing, scope->draw,
                                          pass_descriptor);
                     });
    if (known != known_end) {
      if (MaterialDrawScope* mutable_scope =
              CurrentMaterialScopeMutable()) {
        mutable_scope->visible_material_pass =
            IsVisibleMaterialPass(*known);
      }
      StoreFramePassLocked(*known);
      return;
    }
  }

  MaterialPassObservation pass;
  pass.draw = scope->draw;
  pass.runtime_state = runtime_state;
  pass.pass_descriptor = pass_descriptor;
  auto read = [&](uint32_t address, size_t offset, uint32_t& value) {
    if (!TryReadBeU32(guest_base, address, offset, value)) {
      ++pass.guest_read_failures;
      return false;
    }
    return true;
  };

  read(runtime_state, 0x2BC, pass.graphics_device);
  read(pass_descriptor, 0x08, pass.program_pair);
  read(pass_descriptor, 0x0C, pass.device_command_list);
  read(pass_descriptor, 0x10, pass.sampler_state_list);
  if (pass.program_pair != 0) {
    read(pass.program_pair, 0x48, pass.vertex_shader_reference);
    read(pass.program_pair, 0x4C, pass.pixel_shader_reference);
  }
  if (pass.vertex_shader_reference != 0) {
    read(pass.vertex_shader_reference, 0, pass.vertex_shader);
  }
  if (pass.pixel_shader_reference != 0) {
    read(pass.pixel_shader_reference, 0, pass.pixel_shader);
  }
  if (pass.device_command_list != 0) {
    read(pass.device_command_list, 0x10, pass.device_command_count);
  }
  if (pass.sampler_state_list != 0) {
    read(pass.sampler_state_list, 0x80, pass.sampler_state_count);
  }

  pass.captured_device_command_count =
      std::min<uint32_t>(pass.device_command_count,
                         pass.device_commands.size());
  for (uint32_t index = 0;
       index < pass.captured_device_command_count; ++index) {
    MaterialDeviceCommand& command = pass.device_commands[index];
    const size_t record_offset = 0x14 + static_cast<size_t>(index) * 8;
    read(pass.device_command_list, record_offset + 0,
         command.subobject_offset);
    read(pass.device_command_list, record_offset + 4, command.argument);
    read(pass.graphics_device,
         static_cast<size_t>(command.subobject_offset) + 0x60,
         command.callback);
  }

  pass.captured_sampler_state_count =
      std::min<uint32_t>(pass.sampler_state_count,
                         pass.sampler_state_commands.size());
  for (uint32_t index = 0;
       index < pass.captured_sampler_state_count; ++index) {
    MaterialSamplerStateCommand& command =
        pass.sampler_state_commands[index];
    const size_t record_offset = 0x84 + static_cast<size_t>(index) * 8;
    if (!TryReadBeU16(guest_base, pass.sampler_state_list,
                      record_offset + 0, command.argument_or_index)) {
      ++pass.guest_read_failures;
    }
    if (!TryReadBeU16(guest_base, pass.sampler_state_list,
                      record_offset + 2, command.subobject_offset)) {
      ++pass.guest_read_failures;
    }
    read(pass.sampler_state_list, record_offset + 4, command.value);
    read(pass.graphics_device,
         static_cast<size_t>(command.subobject_offset) + 0x1E4,
         command.callback);
  }
  pass.valid = pass.guest_read_failures == 0 &&
               pass.graphics_device != 0 && pass.program_pair != 0 &&
               pass.vertex_shader != 0 && pass.pixel_shader != 0;
  if (MaterialDrawScope* mutable_scope =
          CurrentMaterialScopeMutable()) {
    mutable_scope->visible_material_pass =
        IsVisibleMaterialPass(pass);
  }

  std::lock_guard lock(g_material_mutex);
  g_building_frame.guest_read_failures += pass.guest_read_failures;
  if (pass.valid && g_known_pass_count < g_known_passes.size()) {
    g_known_passes[g_known_pass_count++] = pass;
  }
  StoreFramePassLocked(pass);
}

void ObserveTableTextureParameterBind(uint8_t* guest_base,
                                      uint32_t fx_runtime,
                                      uint32_t encoded_handle,
                                      uint32_t resource) {
  if (!MaterialObservationEnabled() || guest_base == nullptr ||
      fx_runtime < 0x10) {
    return;
  }

  MaterialTextureParameterObservation texture;
  texture.owner_shader = fx_runtime - 0x10;
  texture.fx_runtime = fx_runtime;
  texture.encoded_handle = encoded_handle;
  texture.supplied_resource = resource;
  uint32_t owner_vtable = 0;
  if (!TryReadBeU32(guest_base, texture.owner_shader, 0, owner_vtable) ||
      owner_vtable != kGrmShaderFxVtable) {
    return;
  }
  texture.effective_resource = resource;
  if (texture.effective_resource == 0) {
    texture.used_fallback = true;
    if (!TryReadBeU32(guest_base, kFallbackFxResource, 0,
                      texture.effective_resource)) {
      ++texture.guest_read_failures;
    }
  }
  if (texture.effective_resource != 0 &&
      !TryReadBeU32(guest_base, texture.effective_resource, 0,
                    texture.resource_vtable)) {
    ++texture.guest_read_failures;
  }
  texture.valid =
      texture.guest_read_failures == 0 &&
      texture.effective_resource != 0 && texture.resource_vtable != 0;

  std::lock_guard lock(g_material_mutex);
  const auto begin = g_pending_texture_parameters.begin();
  const auto used_end = begin + g_pending_texture_parameter_count;
  const auto existing =
      std::find_if(begin, used_end, [&](const auto& candidate) {
        return candidate.owner_shader == texture.owner_shader &&
               candidate.encoded_handle == texture.encoded_handle;
      });
  if (existing != used_end) {
    *existing = texture;
    return;
  }
  if (g_pending_texture_parameter_count ==
      g_pending_texture_parameters.size()) {
    ++g_dropped_pending_texture_parameters;
    return;
  }
  g_pending_texture_parameters[g_pending_texture_parameter_count++] =
      texture;
}

void ObserveTextureResourceUnwrap(uint8_t* guest_base, uint32_t resource,
                                  uint32_t gpu_binding) {
  if (!MaterialObservationEnabled() || guest_base == nullptr ||
      resource == 0 || gpu_binding == 0) {
    return;
  }

  bool tracked = false;
  {
    std::lock_guard lock(g_material_mutex);
    tracked = std::any_of(
        g_pending_texture_parameters.begin(),
        g_pending_texture_parameters.begin() +
            g_pending_texture_parameter_count,
        [&](const auto& texture) {
          return texture.effective_resource == resource;
        });
  }
  if (!tracked) {
    return;
  }

  std::array<uint32_t, 13> words{};
  std::array<uint32_t, 13> verification_words{};
  uint32_t word_count = 0;
  for (; word_count < words.size(); ++word_count) {
    if (!TryReadBeU32(guest_base, gpu_binding,
                      static_cast<size_t>(word_count) * sizeof(uint32_t),
                      words[word_count])) {
      break;
    }
  }
  uint32_t verification_count = 0;
  for (; verification_count < verification_words.size();
       ++verification_count) {
    if (!TryReadBeU32(
            guest_base, gpu_binding,
            static_cast<size_t>(verification_count) * sizeof(uint32_t),
            verification_words[verification_count])) {
      break;
    }
  }
  const bool stable =
      word_count == words.size() &&
      verification_count == verification_words.size() &&
      words == verification_words;

  uint32_t fetch_offset = 0;
  std::array<uint32_t, 6> fetch_words{};
  if (stable) {
    // Locate the title's embedded Xenos texture fetch block structurally.
    // Fetch type 2 is a texture and dword 1 carries a nonzero base address.
    for (uint32_t index = 0;
         index + fetch_words.size() <= words.size(); ++index) {
      if ((words[index] & 0x3u) != 2 || words[index + 1] == 0) {
        continue;
      }
      fetch_offset = index * sizeof(uint32_t);
      std::copy_n(words.begin() + index, fetch_words.size(),
                  fetch_words.begin());
      break;
    }
  }

  std::lock_guard lock(g_material_mutex);
  for (uint32_t index = 0;
       index < g_pending_texture_parameter_count; ++index) {
    MaterialTextureParameterObservation& texture =
        g_pending_texture_parameters[index];
    if (texture.effective_resource == resource) {
      texture.gpu_binding = gpu_binding;
      texture.gpu_binding_words = words;
      texture.gpu_binding_word_count = word_count;
      texture.gpu_binding_words_stable = stable;
      texture.texture_fetch_offset = fetch_offset;
      texture.texture_fetch_words = fetch_words;
      texture.decoded_fetch = DecodeTextureFetch(fetch_offset, fetch_words);
    }
  }
}

void MaterialObserverFrameEnd() {
  if (!MaterialObservationEnabled()) {
    return;
  }
  std::lock_guard lock(g_material_mutex);
  g_building_frame.sequence = g_published_frame.sequence + 1;
  g_published_frame = g_building_frame;
  g_building_frame = {};

  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_material_log_interval);
  if (interval == 0 || g_published_frame.sequence % interval != 0) {
    return;
  }

  REXLOG_INFO(
      "Table Tennis material observer: frame={} draws={} unique={} "
      "dropped={} pass_applies={} unique_passes={} dropped_passes={} "
      "texture_binds={} unique_textures={} dropped_textures={} "
      "rejected_vtables={} read_failures={} observer_only=true",
      g_published_frame.sequence, g_published_frame.draw_count,
      g_published_frame.unique_draw_count,
      g_published_frame.dropped_unique_draws,
      g_published_frame.pass_apply_count,
      g_published_frame.unique_pass_count,
      g_published_frame.dropped_unique_passes,
      g_published_frame.texture_parameter_bind_count,
      g_published_frame.unique_texture_parameter_count,
      g_published_frame.dropped_unique_texture_parameters,
      g_published_frame.rejected_vtables,
      g_published_frame.guest_read_failures);
  for (uint32_t index = 0;
       index < g_published_frame.unique_draw_count; ++index) {
    const MaterialDrawObservation& draw =
        g_published_frame.unique_draws[index];
    REXLOG_INFO(
        "  material[{}] shader={:08X} vt={:08X} shader_group={:08X} "
        "model={:08X} geometry={} lod={} alternate_pass={}",
        index, draw.shader, draw.shader_vtable, draw.shader_group, draw.model,
        draw.geometry_index, draw.lod, draw.alternate_pass);
  }
  for (uint32_t index = 0;
       index < g_published_frame.unique_pass_count; ++index) {
    const MaterialPassObservation& pass =
        g_published_frame.unique_passes[index];
    REXLOG_INFO(
        "  pass[{}] descriptor={:08X} state={:08X} device={:08X} "
        "program_pair={:08X} vs_ref={:08X} vs={:08X} "
        "ps_ref={:08X} ps={:08X} commands={:08X}/{} "
        "samplers={:08X}/{} valid={} read_failures={}",
        index, pass.pass_descriptor, pass.runtime_state,
        pass.graphics_device, pass.program_pair,
        pass.vertex_shader_reference, pass.vertex_shader,
        pass.pixel_shader_reference, pass.pixel_shader,
        pass.device_command_list, pass.device_command_count,
        pass.sampler_state_list, pass.sampler_state_count, pass.valid,
        pass.guest_read_failures);
    for (uint32_t command_index = 0;
         command_index < pass.captured_device_command_count;
         ++command_index) {
      const MaterialDeviceCommand& command =
          pass.device_commands[command_index];
      REXLOG_INFO(
          "    device_command[{}] subobject={:08X} arg={:08X} "
          "callback={:08X}",
          command_index, command.subobject_offset, command.argument,
          command.callback);
    }
    for (uint32_t command_index = 0;
         command_index < pass.captured_sampler_state_count;
         ++command_index) {
      const MaterialSamplerStateCommand& command =
          pass.sampler_state_commands[command_index];
      REXLOG_INFO(
          "    sampler_state[{}] arg_or_index={} subobject={:04X} "
          "value={:08X} callback={:08X}",
          command_index, command.argument_or_index,
          command.subobject_offset, command.value, command.callback);
      }
  }
  for (uint32_t index = 0;
       index < g_published_frame.unique_texture_parameter_count;
       ++index) {
    const MaterialTextureParameterObservation& texture =
        g_published_frame.unique_texture_parameters[index];
    REXLOG_INFO(
        "  texture[{}] fx={:08X} handle={:08X} supplied={:08X} "
        "owner={:08X} effective={:08X} vt={:08X} gpu={:08X} "
        "fallback={} valid={} read_failures={}",
        index, texture.fx_runtime, texture.encoded_handle,
        texture.supplied_resource, texture.owner_shader,
        texture.effective_resource,
        texture.resource_vtable, texture.gpu_binding,
        texture.used_fallback, texture.valid,
        texture.guest_read_failures);
    REXLOG_INFO(
        "    gpu_words({} stable={})=[{:08X} {:08X} {:08X} {:08X} "
        "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
        "{:08X}]",
        texture.gpu_binding_word_count,
        texture.gpu_binding_words_stable,
        texture.gpu_binding_words[0], texture.gpu_binding_words[1],
        texture.gpu_binding_words[2], texture.gpu_binding_words[3],
        texture.gpu_binding_words[4], texture.gpu_binding_words[5],
        texture.gpu_binding_words[6], texture.gpu_binding_words[7],
        texture.gpu_binding_words[8], texture.gpu_binding_words[9],
        texture.gpu_binding_words[10], texture.gpu_binding_words[11],
        texture.gpu_binding_words[12]);
    REXLOG_INFO(
        "    fetch(+0x{:X})=[{:08X} {:08X} {:08X} {:08X} {:08X} "
        "{:08X}]",
        texture.texture_fetch_offset, texture.texture_fetch_words[0],
        texture.texture_fetch_words[1], texture.texture_fetch_words[2],
        texture.texture_fetch_words[3], texture.texture_fetch_words[4],
        texture.texture_fetch_words[5]);
    const MaterialTextureFetch& fetch = texture.decoded_fetch;
    REXLOG_INFO(
        "    texture_info={}x{}x{} pitch={} format={} endian={} dim={} "
        "base={:08X} mip={:08X} levels={}-{} tiled={} packed={} valid={}",
        fetch.width, fetch.height, fetch.depth, fetch.pitch_pixels,
        fetch.format, fetch.endianness, fetch.dimension,
        fetch.base_address, fetch.mip_address, fetch.mip_min_level,
        fetch.mip_max_level, fetch.tiled, fetch.packed_mips, fetch.valid);
  }
}

MaterialFrameObservation LatestMaterialFrameObservation() {
  std::lock_guard lock(g_material_mutex);
  return g_published_frame;
}

}  // namespace tabletennis::native
