#include "native/tabletennis_model_material_tokens.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_scene_owner_observer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(
    tabletennis_native_model_material_tokens_log_interval, 0, "Table Tennis",
    "Frames between generic model/material value-token coverage reports "
    "(0 disables capture).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

// sub_820F19C8 reads these exact fields before dispatching the selected
// grmShaderFx virtual slot at vtable+0x0C.
constexpr size_t kModelMaterialIndicesOffset = 0x0C;
constexpr size_t kModelGeometryCountOffset = 0x16;
constexpr size_t kShaderGroupArrayOffset = 0x08;
constexpr size_t kShaderCategoryFlagsOffset = 0x04;
constexpr size_t kSelectedShaderSlotOffset = 0x0C;
constexpr uint32_t kGlobalShaderOverride = 0x82606350;
constexpr uint32_t kMaxPlausibleGeometryCount = 2048;
constexpr uint32_t kMaxGeometryTokensPerSubmission = 256;
constexpr uint32_t kMaxTrackedUniqueValues = 256;

struct SubmissionCapture {
  uint32_t geometry_count = 0;
  uint32_t scanned_geometry_count = 0;
  uint32_t category_rejected_count = 0;
  uint32_t dropped_geometry_count = 0;
  uint32_t guest_read_failure_count = 0;
  bool readable = false;
  bool global_shader_override = false;
  uint32_t token_count = 0;
  std::array<ModelMaterialValueToken, kMaxGeometryTokensPerSubmission> tokens{};
};

struct UniqueValues {
  uint32_t count = 0;
  std::array<uint32_t, kMaxTrackedUniqueValues> values{};

  void Add(uint32_t value) {
    if (value == 0 || std::find(values.begin(), values.begin() + count,
                                value) != values.begin() + count) {
      return;
    }
    if (count < values.size()) {
      values[count++] = value;
    }
  }
};

struct FrameAccumulator {
  ModelMaterialValueTokenFrame frame{};
  UniqueValues models{};
  UniqueValues shaders{};
  UniqueValues shader_vtables{};
  UniqueValues slot_targets{};
};

std::mutex g_token_mutex;
FrameAccumulator g_building;
ModelMaterialValueTokenFrame g_published;
uint64_t g_frame_sequence = 0;
bool g_was_enabled = false;

bool CheckedGuestOffset(uint32_t address, size_t offset, size_t size,
                        uint32_t &result) {
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

bool TryCopyGuest(uint8_t *guest_base, uint32_t address, size_t offset,
                  void *destination, size_t size) {
  uint32_t guest_address = 0;
  uint8_t *base = guest_base;
  return guest_base != nullptr &&
         CheckedGuestOffset(address, offset, size, guest_address) &&
         GuestTryCopy(destination, REX_RAW_ADDR(guest_address), size);
}

uint32_t LoadBeU32(const std::byte *bytes) {
  uint32_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return std::byteswap(value);
}

uint16_t LoadBeU16(const std::byte *bytes) {
  uint16_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return std::byteswap(value);
}

bool TryReadBeU32(uint8_t *guest_base, uint32_t address, size_t offset,
                  uint32_t &value) {
  std::array<std::byte, sizeof(uint32_t)> bytes{};
  if (!TryCopyGuest(guest_base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  value = LoadBeU32(bytes.data());
  return true;
}

bool TryReadBeU16(uint8_t *guest_base, uint32_t address, size_t offset,
                  uint16_t &value) {
  std::array<std::byte, sizeof(uint16_t)> bytes{};
  if (!TryCopyGuest(guest_base, address, offset, bytes.data(), bytes.size())) {
    return false;
  }
  value = LoadBeU16(bytes.data());
  return true;
}

uint32_t ShaderRenderCategory(uint16_t flags, bool global_override) {
  // rlwinm r10,r4,27,27,31 in sub_820F19C8 is (flags >> 5) & 0x1F.
  // The global override's low flag bit forces category zero.
  if (global_override && (flags & 1u) != 0) {
    return 0;
  }
  return (flags >> 5) & 0x1Fu;
}

SubmissionCapture CaptureSubmission(uint8_t *guest_base, uint32_t model,
                                    uint32_t shader_group,
                                    uint32_t render_category, uint32_t lod,
                                    const SceneOwnerToken &owner) {
  SubmissionCapture capture;

  uint32_t material_indices = 0;
  uint16_t geometry_count = 0;
  if (!TryReadBeU32(guest_base, model, kModelMaterialIndicesOffset,
                    material_indices) ||
      !TryReadBeU16(guest_base, model, kModelGeometryCountOffset,
                    geometry_count)) {
    ++capture.guest_read_failure_count;
    return capture;
  }
  capture.geometry_count = geometry_count;
  if (geometry_count > kMaxPlausibleGeometryCount || material_indices == 0) {
    ++capture.guest_read_failure_count;
    capture.dropped_geometry_count = geometry_count;
    return capture;
  }

  uint32_t shader = 0;
  if (!TryReadBeU32(guest_base, kGlobalShaderOverride, 0, shader)) {
    ++capture.guest_read_failure_count;
    capture.dropped_geometry_count = geometry_count;
    return capture;
  }
  capture.global_shader_override = shader != 0;

  uint32_t shader_array = 0;
  if (!capture.global_shader_override &&
      (!TryReadBeU32(guest_base, shader_group, kShaderGroupArrayOffset,
                     shader_array) ||
       shader_array == 0)) {
    ++capture.guest_read_failure_count;
    capture.dropped_geometry_count = geometry_count;
    return capture;
  }

  capture.readable = true;
  capture.scanned_geometry_count =
      std::min<uint32_t>(geometry_count, capture.tokens.size());
  capture.dropped_geometry_count =
      geometry_count - capture.scanned_geometry_count;

  for (uint32_t geometry_index = 0;
       geometry_index < capture.scanned_geometry_count; ++geometry_index) {
    uint32_t selected_shader = shader;
    if (!capture.global_shader_override) {
      uint16_t material_index = 0;
      if (!TryReadBeU16(guest_base, material_indices,
                        geometry_index * sizeof(uint16_t), material_index) ||
          !TryReadBeU32(guest_base, shader_array,
                        static_cast<size_t>(material_index) * sizeof(uint32_t),
                        selected_shader)) {
        ++capture.guest_read_failure_count;
        continue;
      }
    }
    if (selected_shader == 0) {
      ++capture.guest_read_failure_count;
      continue;
    }

    uint16_t shader_flags = 0;
    if (!TryReadBeU16(guest_base, selected_shader, kShaderCategoryFlagsOffset,
                      shader_flags)) {
      ++capture.guest_read_failure_count;
      continue;
    }
    if (ShaderRenderCategory(shader_flags, capture.global_shader_override) !=
        render_category) {
      ++capture.category_rejected_count;
      continue;
    }

    ModelMaterialValueToken token;
    token.owner = owner;
    token.model = model;
    token.geometry_index = geometry_index;
    token.shader = selected_shader;
    token.render_category = render_category;
    token.lod = lod;
    token.global_shader_override = capture.global_shader_override;

    if (!TryReadBeU32(guest_base, selected_shader, 0, token.shader_vtable)) {
      ++capture.guest_read_failure_count;
    } else if (token.shader_vtable != 0 &&
               TryReadBeU32(guest_base, token.shader_vtable,
                            kSelectedShaderSlotOffset,
                            token.shader_slot_target)) {
      token.shader_slot_target_valid = token.shader_slot_target != 0;
    } else {
      ++capture.guest_read_failure_count;
    }

    capture.tokens[capture.token_count++] = token;
  }
  return capture;
}

void AppendSubmission(const SubmissionCapture &submission) {
  std::lock_guard lock(g_token_mutex);
  ModelMaterialValueTokenFrame &frame = g_building.frame;
  ++frame.submission_count;
  frame.readable_submission_count += submission.readable;
  frame.geometry_count += submission.geometry_count;
  frame.scanned_geometry_count += submission.scanned_geometry_count;
  frame.category_rejected_count += submission.category_rejected_count;
  frame.global_override_submission_count += submission.global_shader_override;
  frame.dropped_geometry_count += submission.dropped_geometry_count;
  frame.guest_read_failure_count += submission.guest_read_failure_count;

  for (uint32_t index = 0; index < submission.token_count; ++index) {
    const ModelMaterialValueToken &token = submission.tokens[index];
    g_building.models.Add(token.model);
    g_building.shaders.Add(token.shader);
    g_building.shader_vtables.Add(token.shader_vtable);
    if (token.shader_slot_target_valid) {
      g_building.slot_targets.Add(token.shader_slot_target);
      ++frame.valid_slot_target_count;
    }

    if (frame.token_count < frame.tokens.size()) {
      frame.tokens[frame.token_count++] = token;
    } else {
      ++frame.dropped_token_count;
    }
  }
}

void LogFrame(const ModelMaterialValueTokenFrame &frame) {
  uint32_t first_shader_vtable = 0;
  uint32_t first_slot_target = 0;
  for (uint32_t index = 0; index < frame.token_count; ++index) {
    const ModelMaterialValueToken &token = frame.tokens[index];
    if (first_shader_vtable == 0) {
      first_shader_vtable = token.shader_vtable;
    }
    if (first_slot_target == 0 && token.shader_slot_target_valid) {
      first_slot_target = token.shader_slot_target;
    }
    if (first_shader_vtable != 0 && first_slot_target != 0) {
      break;
    }
  }
  REXLOG_INFO(
      "Table Tennis model/material tokens: frame={} submissions={} "
      "readable={} geometry={} scanned={} selected_tokens={} "
      "category_rejected={} global_override_submissions={} "
      "valid_slot_targets={} unique_models={} unique_shaders={} "
      "unique_shader_vtables={} first_vtable={:08X} "
      "unique_slot_targets={} first_slot={:08X} "
      "dropped_geometry={} dropped_tokens={} read_failures={} "
      "observer_only=true",
      frame.sequence, frame.submission_count, frame.readable_submission_count,
      frame.geometry_count, frame.scanned_geometry_count, frame.token_count,
      frame.category_rejected_count, frame.global_override_submission_count,
      frame.valid_slot_target_count, frame.unique_model_count,
      frame.unique_shader_count, frame.unique_shader_vtable_count,
      first_shader_vtable, frame.unique_slot_target_count,
      first_slot_target, frame.dropped_geometry_count,
      frame.dropped_token_count, frame.guest_read_failure_count);
}

} // namespace

void ObserveModelMaterialValueTokens(uint8_t *guest_base, uint32_t model,
                                     uint32_t shader_group,
                                     uint32_t render_category, uint32_t lod) {
  const bool log_enabled =
      REXCVAR_GET(tabletennis_native_model_material_tokens_log_interval) != 0;
  const SceneOwnerToken owner = CurrentSceneOwnerToken();
  if ((!log_enabled && !owner.valid) || guest_base == nullptr || model == 0 ||
      shader_group == 0) {
    return;
  }

  const SubmissionCapture submission = CaptureSubmission(
      guest_base, model, shader_group, render_category, lod, owner);
  ObserveSceneOwnerModelTokens(owner, submission.token_count);
  if (log_enabled) {
    AppendSubmission(submission);
  }
}

void ModelMaterialValueTokensFrameEnd() {
  const uint32_t interval =
      REXCVAR_GET(tabletennis_native_model_material_tokens_log_interval);
  const bool enabled = interval != 0;

  std::lock_guard lock(g_token_mutex);
  ++g_frame_sequence;
  if (!enabled) {
    g_building = {};
    if (g_was_enabled) {
      g_published = {};
      g_published.sequence = g_frame_sequence;
    }
    g_was_enabled = false;
    return;
  }

  g_building.frame.sequence = g_frame_sequence;
  g_building.frame.unique_model_count = g_building.models.count;
  g_building.frame.unique_shader_count = g_building.shaders.count;
  g_building.frame.unique_shader_vtable_count = g_building.shader_vtables.count;
  g_building.frame.unique_slot_target_count = g_building.slot_targets.count;
  g_published = g_building.frame;
  g_building = {};
  g_was_enabled = true;

  if (g_published.sequence % interval == 0) {
    LogFrame(g_published);
  }
}

ModelMaterialValueTokenFrame LatestModelMaterialValueTokenFrame() {
  std::lock_guard lock(g_token_mutex);
  return g_published;
}

} // namespace tabletennis::native
