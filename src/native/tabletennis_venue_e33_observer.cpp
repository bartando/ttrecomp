#include "native/tabletennis_venue_e33_observer.h"

#include "generated/default/tabletennis_init.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_guest_memory.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_venue_e33_renderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    tabletennis_native_venue_e33_observer, false, "Table Tennis",
    "Learn E33 static-venue identity from an exact same-frame dynamic "
    "three-tile backend hash/order proof, then capture immutable payloads only "
    "for matching learned draws. "
    "Observer-only; never suppresses or replaces a draw.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace tabletennis::native {
namespace {

namespace nrhi = rex::graphics::nrhi;

constexpr uint64_t kVertexShaderHash = kVenueE33VertexShaderHash;
constexpr uint64_t kPixelShaderHash = kVenueE33PixelShaderHash;
constexpr uint32_t kGameplayRenderPassKey = 0x0000000E;
constexpr uint32_t kGameplaySurfacePitch = 1280;
constexpr uint32_t kMainColorEdramBase = 0x400;
constexpr uint32_t kMainDepthEdramBase = 0;
constexpr uint32_t kMainEdramMode = 4;
constexpr uint32_t kTriangleStripPrimitive = 6;
constexpr uint32_t kNormalizedDepthControl = 0x00700736;
constexpr uint32_t kNormalizedColorMask = 0x00000007;
constexpr uint32_t kColorControl = 0x87000005;
constexpr uint32_t kBlendControl0 = 0x00010001;
constexpr size_t kMaximumTitleCandidates = 2048;
constexpr size_t kMaximumRetainedFrames = 8;
constexpr size_t kMaximumQueuedBackendEvents = 4096;
constexpr uint32_t kMaximumRendererContractLogs = 6;
constexpr uint32_t kMaximumSelectedMaterialContractLogs = 6;
constexpr uint32_t kPixelConstantBankOffset = 0x1780;
constexpr uint32_t kConstantRowBytes = 16;
constexpr size_t kMaximumApplyPassCommands = 64;

struct ApplyPassDeviceCommand {
  uint32_t device_subobject_offset = 0;
  uint32_t argument = 0;
};

struct ApplyPassSamplerCommand {
  uint16_t argument = 0;
  uint16_t device_subobject_offset = 0;
  uint32_t value = 0;
};

struct ApplyPassProbe {
  uint64_t learned_generation = 0;
  uint32_t pass_descriptor = 0;
  uint32_t program_pair = 0;
  uint32_t runtime_state = 0;
  uint32_t device = 0;
  uint32_t device_command_list = 0;
  uint32_t sampler_command_list = 0;
  uint32_t declared_device_command_count = 0;
  uint32_t declared_sampler_command_count = 0;
  uint32_t captured_device_command_count = 0;
  uint32_t captured_sampler_command_count = 0;
  std::array<ApplyPassDeviceCommand, kMaximumApplyPassCommands>
      device_commands{};
  std::array<ApplyPassSamplerCommand, kMaximumApplyPassCommands>
      sampler_commands{};
  std::array<uint32_t, 4> c20_before{};
  std::array<uint32_t, 4> c255_before{};
  bool command_lists_valid = false;
  bool constants_before_valid = false;
  bool active = false;
};

struct TitleToken {
  VenueE33TitleCandidate candidate{};
  std::shared_ptr<const VenueE33TitleDrawSnapshot> snapshot;
  uint64_t capture_generation = 0;
  uint32_t capture_generation_mismatches = 0;
  uint32_t guest_read_failures = 0;
  uint32_t payload_copy_failures = 0;
  uint32_t texture_capture_failures = 0;
  uint32_t material_validation_failures = 0;
  uint32_t renderer_full_mip_texture_count = 0;
  uint32_t renderer_texture_shape_match_count = 0;
};

struct BackendEvent {
  uint64_t backend_frame_sequence = 0;
  VenueE33DrawIdentity identity{};
  VenueE33BackendContract contract{};
};

struct BuildingFrame {
  uint64_t sequence = 0;
  uint32_t dropped_candidate_count = 0;
  uint32_t capture_generation_mismatches = 0;
  bool capture_identity_latched = false;
  size_t learned_match_cursor = 0;
  std::shared_ptr<const VenueE33LearnedIdentitySnapshot> capture_identity;
  std::vector<std::shared_ptr<TitleToken>> candidates;
};

struct FrameLedger {
  uint64_t sequence = 0;
  bool finalized = false;
  BuildingFrame title{};
  uint32_t backend_tile_blocks_matched = 0;
  uint32_t backend_sequence_mismatches = 0;
  uint32_t matched_index_count = 0;
  std::vector<size_t> selected_candidate_indices;
  std::vector<VenueE33DrawIdentity> first_block_identities;
  std::vector<VenueE33BackendContract> first_block_contracts;
  std::vector<BackendEvent> backend_events;
};

std::mutex g_observer_mutex;
BuildingFrame g_building_frame;
std::deque<FrameLedger> g_frames;
std::deque<BackendEvent> g_backend_events;
std::shared_ptr<const VenueE33LearnedIdentitySnapshot> g_learned_identity;
std::shared_ptr<const VenueE33FrameSnapshot> g_published_frame;
VenueE33ObserverTelemetry g_telemetry;
uint64_t g_latest_catalog_sequence = 0;
uint64_t g_learning_generation = 0;
uint64_t g_logged_published_generation = 0;
bool g_announced_mismatch = false;
bool g_announced_capture_rejection = false;
uint32_t g_title_gate_sample_logs = 0;
uint32_t g_renderer_contract_logs = 0;
uint32_t g_selected_material_contract_logs = 0;
uint64_t g_apply_pass_probe_claimed_generation = 0;
thread_local ApplyPassProbe g_apply_pass_probe;

struct RawDeclarationSample {
  static constexpr size_t kWordCount = 16;

  std::array<uint32_t, kWordCount> words{};
  bool valid = false;
};

RawDeclarationSample CaptureRawDeclarationSample(uint8_t* base,
                                                 uint32_t declaration) {
  RawDeclarationSample sample;
  if (base == nullptr || declaration == 0) {
    return sample;
  }

  std::array<uint32_t, RawDeclarationSample::kWordCount> raw_words{};
  if (!GuestTryCopy(raw_words.data(), REX_RAW_ADDR(declaration),
                    sizeof(raw_words))) {
    return sample;
  }
  for (size_t index = 0; index < raw_words.size(); ++index) {
    sample.words[index] = std::byteswap(raw_words[index]);
  }
  sample.valid = true;
  return sample;
}

bool ProbeReadBeU16(uint8_t* guest_base, uint32_t address, size_t offset,
                    uint16_t& value) {
  if (guest_base == nullptr || address == 0 ||
      offset > std::numeric_limits<uint32_t>::max() - address) {
    return false;
  }
  uint16_t raw = 0;
  const uint32_t guest_address =
      address + static_cast<uint32_t>(offset);
  if (!GuestTryCopy(
          &raw,
          guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address),
          sizeof(raw))) {
    return false;
  }
  value = std::byteswap(raw);
  return true;
}

bool ProbeReadBeU32(uint8_t* guest_base, uint32_t address, size_t offset,
                    uint32_t& value) {
  if (guest_base == nullptr || address == 0 ||
      offset > std::numeric_limits<uint32_t>::max() - address) {
    return false;
  }
  uint32_t raw = 0;
  const uint32_t guest_address =
      address + static_cast<uint32_t>(offset);
  if (!GuestTryCopy(
          &raw,
          guest_base + guest_address + REX_PHYS_HOST_OFFSET(guest_address),
          sizeof(raw))) {
    return false;
  }
  value = std::byteswap(raw);
  return true;
}

bool ProbeReadConstant(uint8_t* guest_base, uint32_t device,
                       uint32_t register_index,
                       std::array<uint32_t, 4>& value) {
  if (register_index > 255 ||
      register_index >
          (std::numeric_limits<uint32_t>::max() -
           kPixelConstantBankOffset) /
              kConstantRowBytes) {
    return false;
  }
  const uint32_t offset =
      kPixelConstantBankOffset + register_index * kConstantRowBytes;
  for (size_t component = 0; component < value.size(); ++component) {
    if (!ProbeReadBeU32(guest_base, device,
                        offset + component * sizeof(uint32_t),
                        value[component])) {
      return false;
    }
  }
  return true;
}

float ProbeFloat(uint32_t bits) { return std::bit_cast<float>(bits); }

bool SupportedDepthFormat(nrhi::Format format) {
  return format == nrhi::Format::kD24_UNORM_S8_UINT ||
         format == nrhi::Format::kD32_FLOAT_S8_UINT;
}

bool RawTargetStateMatchesDecoded(
    const rex::graphics::NativeGuestDrawContext::RenderTargetState& state) {
  constexpr uint32_t kEdramBaseMask = (1u << 12) - 1;
  constexpr uint32_t kSurfacePitchMask = (1u << 14) - 1;
  constexpr uint32_t kEdramModeMask = (1u << 3) - 1;
  return state.valid &&
         (state.rb_color_info_0 & kEdramBaseMask) == state.color_edram_base &&
         (state.rb_depth_info & kEdramBaseMask) == state.depth_edram_base &&
         (state.rb_surface_info & kSurfacePitchMask) == state.surface_pitch &&
         (state.rb_modecontrol & kEdramModeMask) == state.edram_mode;
}

bool IsExactMainTarget(const rex::graphics::NativeGuestDrawContext& context) {
  const auto& target = context.render_target_state;
  return RawTargetStateMatchesDecoded(target) &&
         target.color_edram_base == kMainColorEdramBase &&
         target.depth_edram_base == kMainDepthEdramBase &&
         target.surface_pitch == kGameplaySurfacePitch &&
         target.edram_mode == kMainEdramMode &&
         context.surface_pitch == target.surface_pitch;
}

bool ExactBackendVertexFetch(
    const rex::graphics::NativeGuestDrawContext& context) {
  return context.primary_vertex_fetch.valid &&
         context.primary_vertex_fetch.physical_address != 0 &&
         context.primary_vertex_fetch.byte_count >= kVenueE33VertexStride &&
         context.primary_vertex_fetch.byte_count % kVenueE33VertexStride == 0 &&
         context.primary_vertex_fetch.endian == kVenueE33VertexEndian;
}

bool IsExactBackendDraw(const rex::graphics::NativeGuestDrawContext& context) {
  return context.backend == rex::graphics::NativeGuestOutputBackend::kVulkan &&
         context.backend_frame_sequence != 0 && context.render_pass_key_valid &&
         context.render_pass_key == kGameplayRenderPassKey &&
         IsExactMainTarget(context) && context.indexed &&
         context.guest_index_base_valid && context.guest_index_base != 0 &&
         ExactBackendVertexFetch(context) &&
         context.draw_state_contract_valid &&
         context.rasterizer_mode_control_valid &&
         context.borrowed_attachment_contract_valid &&
         context.vertex_shader_hash == kVertexShaderHash &&
         context.pixel_shader_hash == kPixelShaderHash &&
         context.primitive_type == kTriangleStripPrimitive &&
         context.guest_vertex_or_index_count != 0 &&
         !context.primitive_restart_enabled &&
         context.normalized_depth_control == kNormalizedDepthControl &&
         context.normalized_color_mask == kNormalizedColorMask &&
         context.color_control == kColorControl &&
         context.blend_control_0 == kBlendControl0 &&
         context.color_attachment_count == 1 &&
         context.color_attachment_formats[0] == nrhi::Format::kR8G8B8A8_UNORM &&
         SupportedDepthFormat(context.depth_attachment_format) &&
         context.stencil_attachment_format == context.depth_attachment_format &&
         context.sample_count == 4 &&
         context.sample_mask == std::numeric_limits<uint64_t>::max();
}

VenueE33BackendContract CaptureBackendContract(
    const rex::graphics::NativeGuestDrawContext& context) {
  VenueE33BackendContract contract;
  contract.vertex_shader_hash = context.vertex_shader_hash;
  contract.pixel_shader_hash = context.pixel_shader_hash;
  contract.render_pass_key = context.render_pass_key;
  contract.surface_pitch = context.surface_pitch;
  contract.normalized_depth_control = context.normalized_depth_control;
  contract.normalized_color_mask = context.normalized_color_mask;
  contract.color_control = context.color_control;
  contract.blend_control_0 = context.blend_control_0;
  contract.rasterizer_mode_control = context.rasterizer_mode_control;
  contract.primitive_restart_index = context.primitive_restart_index;
  contract.rb_color_info_0 = context.render_target_state.rb_color_info_0;
  contract.rb_depth_info = context.render_target_state.rb_depth_info;
  contract.rb_surface_info = context.render_target_state.rb_surface_info;
  contract.rb_modecontrol = context.render_target_state.rb_modecontrol;
  contract.color_edram_base = context.render_target_state.color_edram_base;
  contract.depth_edram_base = context.render_target_state.depth_edram_base;
  contract.edram_mode = context.render_target_state.edram_mode;
  for (size_t attachment = 0;
       attachment < contract.color_attachment_formats.size(); ++attachment) {
    contract.color_attachment_formats[attachment] =
        static_cast<uint32_t>(context.color_attachment_formats[attachment]);
  }
  contract.color_attachment_count = context.color_attachment_count;
  contract.depth_attachment_format =
      static_cast<uint32_t>(context.depth_attachment_format);
  contract.stencil_attachment_format =
      static_cast<uint32_t>(context.stencil_attachment_format);
  contract.sample_count = context.sample_count;
  contract.sample_mask = context.sample_mask;
  contract.primitive_restart_enabled = context.primitive_restart_enabled;
  contract.rasterizer_mode_control_valid =
      context.rasterizer_mode_control_valid;
  contract.render_target_state_valid = context.render_target_state.valid;
  contract.valid = IsExactBackendDraw(context);
  return contract;
}

bool SameBackendContract(const VenueE33BackendContract& left,
                         const VenueE33BackendContract& right) {
  return left.valid && right.valid &&
         left.vertex_shader_hash == right.vertex_shader_hash &&
         left.pixel_shader_hash == right.pixel_shader_hash &&
         left.render_pass_key == right.render_pass_key &&
         left.surface_pitch == right.surface_pitch &&
         left.normalized_depth_control == right.normalized_depth_control &&
         left.normalized_color_mask == right.normalized_color_mask &&
         left.color_control == right.color_control &&
         left.blend_control_0 == right.blend_control_0 &&
         left.rasterizer_mode_control == right.rasterizer_mode_control &&
         left.primitive_restart_index == right.primitive_restart_index &&
         left.rb_color_info_0 == right.rb_color_info_0 &&
         left.rb_depth_info == right.rb_depth_info &&
         left.rb_surface_info == right.rb_surface_info &&
         left.rb_modecontrol == right.rb_modecontrol &&
         left.color_edram_base == right.color_edram_base &&
         left.depth_edram_base == right.depth_edram_base &&
         left.edram_mode == right.edram_mode &&
         left.color_attachment_formats == right.color_attachment_formats &&
         left.color_attachment_count == right.color_attachment_count &&
         left.depth_attachment_format == right.depth_attachment_format &&
         left.stencil_attachment_format == right.stencil_attachment_format &&
         left.sample_count == right.sample_count &&
         left.sample_mask == right.sample_mask &&
         left.primitive_restart_enabled == right.primitive_restart_enabled &&
         left.rasterizer_mode_control_valid ==
             right.rasterizer_mode_control_valid &&
         left.render_target_state_valid == right.render_target_state_valid;
}

void LogSelectedMaterialContract(const VenueE33TitleDrawSnapshot& draw) {
  namespace xenos = rex::graphics::xenos;

  for (uint32_t slot = 0;
       slot < draw.material.textures.size() &&
       g_selected_material_contract_logs < kMaximumSelectedMaterialContractLogs;
       ++slot) {
    const std::array<uint32_t, 6>& words = draw.material.texture_fetches[slot];
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.dword_0 = words[0];
    fetch.dword_1 = words[1];
    fetch.dword_2 = words[2];
    fetch.dword_3 = words[3];
    fetch.dword_4 = words[4];
    fetch.dword_5 = words[5];
    const TextureSnapshot* const texture = draw.material.textures[slot].get();
    const bool cube = slot == 2;
    const xenos::DataDimension expected_dimension =
        cube ? xenos::DataDimension::kCube : xenos::DataDimension::k2DOrStacked;
    const xenos::TextureFormat base_format =
        rex::graphics::GetBaseFormat(fetch.format);
    const bool renderer_shape =
        texture != nullptr && texture->full_mip_chain() &&
        fetch.dimension == expected_dimension && !fetch.stacked &&
        texture->dimension == static_cast<uint32_t>(expected_dimension) &&
        texture->layer_count == (cube ? 6u : 1u) &&
        (base_format == xenos::TextureFormat::k_DXT1 ||
         base_format == xenos::TextureFormat::k_DXT4_5);
    ++g_selected_material_contract_logs;
    REXLOG_INFO(
        "Table Tennis E33 selected material contract: ordinal={} slot={} "
        "dimension={} layers={} format={} base_format={} shape={}x{} tiled={} "
        "descriptor_mips={}-{} captured_mips={} full_mips={} "
        "clamp={}/{}/{} filter={}/{}/{} aniso={}/{}/{} lod_bias={} "
        "swizzle={:03X} border={}/{} tri_clamp={} renderer_shape={} "
        "observer_only=true",
        draw.ordinal, slot, static_cast<uint32_t>(fetch.dimension),
        texture != nullptr ? texture->layer_count : 0,
        static_cast<uint32_t>(fetch.format), static_cast<uint32_t>(base_format),
        texture != nullptr ? texture->width : 0,
        texture != nullptr ? texture->height : 0,
        static_cast<uint32_t>(fetch.tiled),
        texture != nullptr ? texture->descriptor_mip_min_level
                           : static_cast<uint32_t>(fetch.mip_min_level),
        texture != nullptr ? texture->descriptor_mip_max_level
                           : static_cast<uint32_t>(fetch.mip_max_level),
        texture != nullptr ? texture->mips.size() : 0,
        texture != nullptr && texture->full_mip_chain(),
        static_cast<uint32_t>(fetch.clamp_x),
        static_cast<uint32_t>(fetch.clamp_y),
        static_cast<uint32_t>(fetch.clamp_z),
        static_cast<uint32_t>(fetch.mag_filter),
        static_cast<uint32_t>(fetch.min_filter),
        static_cast<uint32_t>(fetch.mip_filter),
        static_cast<uint32_t>(fetch.aniso_filter),
        static_cast<uint32_t>(fetch.mag_aniso_walk),
        static_cast<uint32_t>(fetch.min_aniso_walk),
        static_cast<int32_t>(fetch.lod_bias),
        static_cast<uint32_t>(fetch.swizzle),
        static_cast<uint32_t>(fetch.border_color),
        static_cast<uint32_t>(fetch.border_size),
        static_cast<uint32_t>(fetch.tri_clamp), renderer_shape);
  }
}

FrameLedger* FindPendingFrameLocked(uint64_t sequence) {
  const auto found =
      std::ranges::find_if(g_frames, [sequence](const FrameLedger& frame) {
        return frame.sequence == sequence && !frame.finalized;
      });
  return found == g_frames.end() ? nullptr : &*found;
}

void UpdatePendingCountsLocked() {
  g_telemetry.pending_frames = static_cast<uint32_t>(
      std::ranges::count_if(g_frames, [](const FrameLedger& frame) {
        return !frame.finalized && !frame.title.candidates.empty();
      }));
  g_telemetry.queued_backend_events =
      static_cast<uint32_t>(g_backend_events.size());
}

void RecordMismatchLocked(FrameLedger& frame) {
  ++frame.backend_sequence_mismatches;
  ++g_telemetry.backend_sequence_mismatches;
}

void ResetLearnedIdentityLocked() {
  if (g_learned_identity == nullptr) {
    return;
  }
  g_learned_identity.reset();
  g_published_frame.reset();
  g_logged_published_generation = 0;
  g_telemetry.learned_generation = 0;
  g_telemetry.learned_unique_program_count = 0;
}

void AnnounceAnalysisMismatch(const FrameLedger& frame, const char* reason,
                              size_t event_index, const BackendEvent* expected,
                              const BackendEvent* observed) {
  if (g_announced_mismatch) {
    return;
  }
  g_announced_mismatch = true;
  const BackendEvent empty{};
  const BackendEvent& expected_event = expected != nullptr ? *expected : empty;
  const BackendEvent& observed_event = observed != nullptr ? *observed : empty;
  REXLOG_INFO(
      "Table Tennis E33 venue observer: frame-bucket mismatch "
      "title_frame={} backend_frame={} reason={} event={} "
      "backend_events={} title_candidates={} "
      "expected[primitive={} indices={} ib={:08X} "
      "vb={:08X}/{} endian={}] "
      "observed[primitive={} indices={} ib={:08X} "
      "vb={:08X}/{} endian={}] "
      "observer_only=true guest_suppressed=false",
      frame.sequence, observed_event.backend_frame_sequence, reason,
      event_index, frame.backend_events.size(), frame.title.candidates.size(),
      expected_event.identity.primitive_type,
      expected_event.identity.submitted_index_count,
      expected_event.identity.guest_index_base,
      expected_event.identity.guest_vertex_base,
      expected_event.identity.guest_vertex_bytes,
      expected_event.identity.guest_vertex_endian,
      observed_event.identity.primitive_type,
      observed_event.identity.submitted_index_count,
      observed_event.identity.guest_index_base,
      observed_event.identity.guest_vertex_base,
      observed_event.identity.guest_vertex_bytes,
      observed_event.identity.guest_vertex_endian);
}

uint32_t UniqueProgramCount(
    const std::vector<VenueE33TitleProgramIdentity>& programs) {
  std::vector<VenueE33TitleProgramIdentity> unique;
  for (const VenueE33TitleProgramIdentity& program : programs) {
    if (std::ranges::find(unique, program) == unique.end()) {
      unique.push_back(program);
    }
  }
  return static_cast<uint32_t>(unique.size());
}

bool ExtractOrderedIdentity(
    const FrameLedger& frame,
    std::vector<VenueE33TitleProgramIdentity>& programs,
    std::vector<VenueE33VertexDeclarationIdentity>& declarations) {
  programs.clear();
  declarations.clear();
  programs.reserve(frame.selected_candidate_indices.size());
  declarations.reserve(frame.selected_candidate_indices.size());
  for (size_t candidate_index : frame.selected_candidate_indices) {
    if (candidate_index >= frame.title.candidates.size() ||
        frame.title.candidates[candidate_index] == nullptr) {
      return false;
    }
    const VenueE33TitleCandidate& candidate =
        frame.title.candidates[candidate_index]->candidate;
    if (!candidate.program.valid() || !candidate.vertex_declaration.valid()) {
      return false;
    }
    programs.push_back(candidate.program);
    declarations.push_back(candidate.vertex_declaration);
  }
  return !programs.empty();
}

bool SameLearnedIdentity(
    const VenueE33LearnedIdentitySnapshot& learned,
    const std::vector<VenueE33TitleProgramIdentity>& programs,
    const std::vector<VenueE33VertexDeclarationIdentity>& declarations) {
  return learned.valid() && learned.ordered_draw_programs == programs &&
         learned.ordered_vertex_declarations == declarations;
}

bool MatchesLearnedPosition(const VenueE33TitleCandidate& candidate,
                            const VenueE33LearnedIdentitySnapshot& learned,
                            size_t position) {
  return learned.valid() && position < learned.ordered_draw_programs.size() &&
         position < learned.ordered_vertex_declarations.size() &&
         candidate.program == learned.ordered_draw_programs[position] &&
         candidate.vertex_declaration ==
             learned.ordered_vertex_declarations[position];
}

bool PrepareLearnedIdentityLocked(FrameLedger& frame) {
  std::vector<VenueE33TitleProgramIdentity> programs;
  std::vector<VenueE33VertexDeclarationIdentity> declarations;
  if (!ExtractOrderedIdentity(frame, programs, declarations)) {
    return false;
  }
  if (g_learned_identity != nullptr &&
      SameLearnedIdentity(*g_learned_identity, programs, declarations)) {
    return true;
  }

  auto learned = std::make_shared<VenueE33LearnedIdentitySnapshot>();
  learned->generation = ++g_learning_generation;
  learned->proof_sequence = frame.sequence;
  learned->ordered_draw_programs = std::move(programs);
  learned->ordered_vertex_declarations = std::move(declarations);
  learned->unique_program_count =
      UniqueProgramCount(learned->ordered_draw_programs);
  if (!learned->valid()) {
    return false;
  }
  g_learned_identity = std::move(learned);
  g_published_frame.reset();
  g_logged_published_generation = 0;
  ++g_telemetry.learned_generations;
  g_telemetry.learned_generation = g_learned_identity->generation;
  g_telemetry.learned_unique_program_count =
      g_learned_identity->unique_program_count;

  REXLOG_INFO(
      "Table Tennis E33 venue observer: learned generation={} frame={} "
      "live_draws={} live_indices={} unique_title_programs={} "
      "backend_tiles=3 observer_only=true guest_suppressed=false",
      g_learned_identity->generation, frame.sequence,
      frame.selected_candidate_indices.size(), frame.matched_index_count,
      g_learned_identity->unique_program_count);
  std::vector<VenueE33TitleProgramIdentity> announced;
  for (const VenueE33TitleProgramIdentity& program :
       g_learned_identity->ordered_draw_programs) {
    if (std::ranges::find(announced, program) != announced.end()) {
      continue;
    }
    announced.push_back(program);
    REXLOG_INFO(
        "  E33 title identity pass={:08X} program={:08X} "
        "vs={:08X} ps={:08X} observer_only=true",
        program.pass_descriptor, program.program_pair, program.vertex_shader,
        program.pixel_shader);
  }
  return true;
}

void PublishFrameLocked(FrameLedger& frame) {
  auto published = std::make_shared<VenueE33FrameSnapshot>();
  published->sequence = frame.sequence;
  published->backend_frame_sequence = frame.sequence;
  published->learned_generation =
      g_learned_identity != nullptr ? g_learned_identity->generation : 0;
  published->learned_identity = g_learned_identity;
  published->title_candidate_count =
      static_cast<uint32_t>(frame.title.candidates.size());
  published->matched_draw_count =
      static_cast<uint32_t>(frame.selected_candidate_indices.size());
  published->matched_index_count = frame.matched_index_count;
  published->unmatched_title_candidate_count =
      published->title_candidate_count >= published->matched_draw_count
          ? published->title_candidate_count - published->matched_draw_count
          : 0;
  published->dropped_candidate_count = frame.title.dropped_candidate_count;
  published->capture_generation_mismatches =
      frame.title.capture_generation_mismatches;
  if (frame.title.capture_identity != nullptr &&
      frame.title.capture_identity->generation !=
          published->learned_generation) {
    ++published->capture_generation_mismatches;
  }
  published->backend_event_count =
      static_cast<uint32_t>(frame.backend_events.size());
  published->backend_draws_per_tile = published->matched_draw_count;
  published->backend_indices_per_tile = published->matched_index_count;
  published->backend_tile_blocks_matched = frame.backend_tile_blocks_matched;
  published->backend_sequence_mismatches = frame.backend_sequence_mismatches;
  published->draws.reserve(frame.selected_candidate_indices.size());

  for (size_t index = 0; index < frame.selected_candidate_indices.size();
       ++index) {
    const size_t selected = frame.selected_candidate_indices[index];
    if (selected >= frame.title.candidates.size() ||
        index >= frame.first_block_identities.size() ||
        index >= frame.first_block_contracts.size() ||
        frame.title.candidates[selected] == nullptr) {
      continue;
    }
    const TitleToken& token = *frame.title.candidates[selected];
    published->capture_generation_mismatches +=
        token.capture_generation_mismatches;
    if (frame.title.capture_identity != nullptr &&
        frame.title.capture_identity->generation ==
            published->learned_generation &&
        token.capture_generation != published->learned_generation) {
      ++published->capture_generation_mismatches;
    }
    published->guest_read_failures += token.guest_read_failures;
    published->payload_copy_failures += token.payload_copy_failures;
    published->texture_capture_failures += token.texture_capture_failures;
    published->material_validation_failures +=
        token.material_validation_failures;
    published->draws.push_back({
        .title = token.snapshot,
        .backend_identity = frame.first_block_identities[index],
        .backend = frame.first_block_contracts[index],
    });
  }

  g_published_frame = std::move(published);
  ObserveMainCoverageFamilyFrame(g_published_frame);
  frame.finalized = true;
  ++g_telemetry.finalized_frames;
  g_telemetry.capture_generation_mismatches +=
      g_published_frame->capture_generation_mismatches;
  if (!g_published_frame->valid()) {
    ++g_telemetry.capture_frames_rejected;
    if (!g_announced_capture_rejection) {
      g_announced_capture_rejection = true;
      REXLOG_INFO(
          "Table Tennis E33 venue observer: rejected frame={} "
          "generation={} candidates={} matched={} indices={} dropped={} "
          "reads={} payloads={} textures={} materials={} backend_events={} "
          "tiles={} backend_mismatches={} capture_generation_mismatches={} "
          "observer_only=true guest_suppressed=false",
          g_published_frame->sequence, g_published_frame->learned_generation,
          g_published_frame->title_candidate_count,
          g_published_frame->matched_draw_count,
          g_published_frame->matched_index_count,
          g_published_frame->dropped_candidate_count,
          g_published_frame->guest_read_failures,
          g_published_frame->payload_copy_failures,
          g_published_frame->texture_capture_failures,
          g_published_frame->material_validation_failures,
          g_published_frame->backend_event_count,
          g_published_frame->backend_tile_blocks_matched,
          g_published_frame->backend_sequence_mismatches,
          g_published_frame->capture_generation_mismatches);
    }
    return;
  }

  ++g_telemetry.valid_frames;
  g_telemetry.latest_published_sequence = g_published_frame->sequence;
  for (const VenueE33DrawSnapshot& draw : g_published_frame->draws) {
    if (draw.title != nullptr) {
      LogSelectedMaterialContract(*draw.title);
    }
  }
  if (g_logged_published_generation != g_published_frame->learned_generation) {
    g_logged_published_generation = g_published_frame->learned_generation;
    REXLOG_INFO(
        "Table Tennis E33 venue observer: published immutable frame={} "
        "generation={} live_draws={} live_indices={} unmatched_title={} "
        "backend_events={} textures_per_draw=3 full_constant_banks=true "
        "backend_tiles=3 observer_only=true guest_suppressed=false",
        g_published_frame->sequence, g_published_frame->learned_generation,
        g_published_frame->matched_draw_count,
        g_published_frame->matched_index_count,
        g_published_frame->unmatched_title_candidate_count,
        g_published_frame->backend_event_count);
  }
}

bool TitleMatchesBackend(const std::shared_ptr<TitleToken>& candidate,
                         const BackendEvent& event) {
  return candidate != nullptr && candidate->candidate.eligible &&
         candidate->candidate.identity == event.identity &&
         event.contract.valid &&
         event.contract.vertex_shader_hash == kVertexShaderHash &&
         event.contract.pixel_shader_hash == kPixelShaderHash;
}

void AnalyzeFrameLocked(FrameLedger& frame) {
  ++g_telemetry.backend_frames_analyzed;
  const size_t event_count = frame.backend_events.size();
  if (event_count == 0 ||
      event_count % VenueE33FrameSnapshot::kRequiredTileBlockCount != 0) {
    AnnounceAnalysisMismatch(frame, "event-count-not-three-blocks", 0, nullptr,
                             nullptr);
    RecordMismatchLocked(frame);
    ResetLearnedIdentityLocked();
    PublishFrameLocked(frame);
    return;
  }

  const size_t draws_per_tile =
      event_count / VenueE33FrameSnapshot::kRequiredTileBlockCount;
  if (draws_per_tile == 0 || draws_per_tile > frame.title.candidates.size()) {
    AnnounceAnalysisMismatch(frame, "draw-count-exceeds-title", 0, nullptr,
                             &frame.backend_events.front());
    RecordMismatchLocked(frame);
    ResetLearnedIdentityLocked();
    PublishFrameLocked(frame);
    return;
  }

  for (size_t tile = 1; tile < VenueE33FrameSnapshot::kRequiredTileBlockCount;
       ++tile) {
    for (size_t draw = 0; draw < draws_per_tile; ++draw) {
      const BackendEvent& expected = frame.backend_events[draw];
      const BackendEvent& observed =
          frame.backend_events[tile * draws_per_tile + draw];
      if (expected.backend_frame_sequence != frame.sequence ||
          observed.backend_frame_sequence != frame.sequence ||
          !(expected.identity == observed.identity) ||
          !SameBackendContract(expected.contract, observed.contract)) {
        AnnounceAnalysisMismatch(frame, "tile-block-contract",
                                 tile * draws_per_tile + draw, &expected,
                                 &observed);
        RecordMismatchLocked(frame);
        ResetLearnedIdentityLocked();
        PublishFrameLocked(frame);
        return;
      }
    }
  }

  std::vector<size_t> earliest(draws_per_tile);
  size_t candidate_cursor = 0;
  for (size_t draw = 0; draw < draws_per_tile; ++draw) {
    const BackendEvent& event = frame.backend_events[draw];
    const auto found =
        std::find_if(frame.title.candidates.begin() +
                         static_cast<std::ptrdiff_t>(candidate_cursor),
                     frame.title.candidates.end(),
                     [&](const std::shared_ptr<TitleToken>& candidate) {
                       return TitleMatchesBackend(candidate, event);
                     });
    if (found == frame.title.candidates.end()) {
      AnnounceAnalysisMismatch(frame, "backend-title-join", draw, nullptr,
                               &event);
      RecordMismatchLocked(frame);
      ResetLearnedIdentityLocked();
      PublishFrameLocked(frame);
      return;
    }
    earliest[draw] =
        static_cast<size_t>(found - frame.title.candidates.begin());
    candidate_cursor = earliest[draw] + 1;
  }

  std::vector<size_t> latest(draws_per_tile);
  candidate_cursor = frame.title.candidates.size();
  for (size_t draw = draws_per_tile; draw-- > 0;) {
    const BackendEvent& event = frame.backend_events[draw];
    bool matched = false;
    while (candidate_cursor != 0) {
      --candidate_cursor;
      if (TitleMatchesBackend(frame.title.candidates[candidate_cursor],
                              event)) {
        latest[draw] = candidate_cursor;
        matched = true;
        break;
      }
    }
    if (!matched) {
      AnnounceAnalysisMismatch(frame, "backend-title-reverse-join", draw,
                               nullptr, &event);
      RecordMismatchLocked(frame);
      ResetLearnedIdentityLocked();
      PublishFrameLocked(frame);
      return;
    }
  }
  if (earliest != latest) {
    AnnounceAnalysisMismatch(frame, "backend-title-ambiguous", 0, nullptr,
                             &frame.backend_events.front());
    RecordMismatchLocked(frame);
    ResetLearnedIdentityLocked();
    PublishFrameLocked(frame);
    return;
  }

  frame.selected_candidate_indices = std::move(earliest);
  frame.first_block_identities.reserve(draws_per_tile);
  frame.first_block_contracts.reserve(draws_per_tile);
  for (size_t draw = 0; draw < draws_per_tile; ++draw) {
    const BackendEvent& event = frame.backend_events[draw];
    frame.first_block_identities.push_back(event.identity);
    frame.first_block_contracts.push_back(event.contract);
    frame.matched_index_count += event.identity.submitted_index_count;
  }

  frame.backend_tile_blocks_matched =
      VenueE33FrameSnapshot::kRequiredTileBlockCount;
  g_telemetry.backend_tile_blocks_matched +=
      VenueE33FrameSnapshot::kRequiredTileBlockCount;
  ++g_telemetry.proof_frames;
  if (!PrepareLearnedIdentityLocked(frame)) {
    AnnounceAnalysisMismatch(frame, "title-program-identity", 0, nullptr,
                             &frame.backend_events.front());
    RecordMismatchLocked(frame);
    ResetLearnedIdentityLocked();
  }
  PublishFrameLocked(frame);
}

void ReconcileBackendEventsLocked() {
  auto event = g_backend_events.begin();
  while (event != g_backend_events.end()) {
    FrameLedger* frame = FindPendingFrameLocked(event->backend_frame_sequence);
    if (frame != nullptr) {
      frame->backend_events.push_back(std::move(*event));
      event = g_backend_events.erase(event);
      continue;
    }
    // The catalog sequence becomes current before its FrameLedger is inserted
    // at Swap, so an equal-sequence backend event must remain queued for that
    // imminent exact join. Only strictly older events are stale.
    if (event->backend_frame_sequence < g_latest_catalog_sequence) {
      ++g_telemetry.backend_events_stale;
      ++g_telemetry.backend_events_without_title_frame;
      event = g_backend_events.erase(event);
      continue;
    }
    ++event;
  }
}

void AnalyzeCompletedFramesLocked() {
  const uint64_t completed_before = g_telemetry.latest_backend_frame_sequence;
  for (FrameLedger& frame : g_frames) {
    if (!frame.finalized && frame.sequence < completed_before) {
      AnalyzeFrameLocked(frame);
    }
  }
}

void ExpireFramesLocked() {
  while (g_frames.size() > kMaximumRetainedFrames) {
    if (!g_frames.front().finalized) {
      ++g_telemetry.expired_frames;
    }
    g_frames.pop_front();
  }
}

}  // namespace

bool VenueE33FrameSnapshot::valid() const {
  if (sequence == 0 || backend_frame_sequence != sequence ||
      learned_generation == 0 || learned_identity == nullptr ||
      !learned_identity->valid() ||
      learned_identity->generation != learned_generation ||
      matched_draw_count == 0 || matched_draw_count != backend_draws_per_tile ||
      matched_index_count == 0 ||
      matched_index_count != backend_indices_per_tile ||
      draws.size() != matched_draw_count ||
      title_candidate_count !=
          matched_draw_count + unmatched_title_candidate_count ||
      learned_identity->ordered_draw_programs.size() != matched_draw_count ||
      learned_identity->ordered_vertex_declarations.size() !=
          matched_draw_count ||
      backend_event_count !=
          backend_draws_per_tile * backend_tile_blocks_matched ||
      dropped_candidate_count != 0 || guest_read_failures != 0 ||
      payload_copy_failures != 0 || texture_capture_failures != 0 ||
      material_validation_failures != 0 || capture_generation_mismatches != 0 ||
      backend_tile_blocks_matched != kRequiredTileBlockCount ||
      backend_sequence_mismatches != 0) {
    return false;
  }
  for (size_t index = 0; index < draws.size(); ++index) {
    if (!draws[index].valid() ||
        draws[index].title->program !=
            learned_identity->ordered_draw_programs[index] ||
        draws[index].title->vertex_declaration !=
            learned_identity->ordered_vertex_declarations[index]) {
      return false;
    }
  }
  return true;
}

bool VenueE33ObserverEnabled() {
  return REXCVAR_GET(tabletennis_native_venue_e33_observer) ||
         VenueE33RendererEnabled() || NativeFrameSceneFullCaptureEnabled();
}

void BeginVenueE33ApplyPassProbe(uint8_t* guest_base, uint32_t runtime_state,
                                 uint32_t pass_descriptor) {
  g_apply_pass_probe = {};
  if (!VenueE33ObserverEnabled() || guest_base == nullptr ||
      runtime_state == 0 || pass_descriptor == 0) {
    return;
  }

  std::shared_ptr<const VenueE33LearnedIdentitySnapshot> learned;
  {
    std::lock_guard lock(g_observer_mutex);
    learned = g_learned_identity;
    if (learned == nullptr || !learned->valid() ||
        learned->generation == g_apply_pass_probe_claimed_generation) {
      return;
    }
  }

  const auto learned_program = std::ranges::find(
      learned->ordered_draw_programs, pass_descriptor,
      &VenueE33TitleProgramIdentity::pass_descriptor);
  if (learned_program == learned->ordered_draw_programs.end()) {
    return;
  }

  ApplyPassProbe probe;
  probe.learned_generation = learned->generation;
  probe.pass_descriptor = pass_descriptor;
  probe.program_pair = learned_program->program_pair;
  probe.runtime_state = runtime_state;
  bool valid =
      ProbeReadBeU32(guest_base, pass_descriptor, 0x08, probe.program_pair) &&
      probe.program_pair == learned_program->program_pair &&
      ProbeReadBeU32(guest_base, runtime_state, 0x2BC, probe.device) &&
      ProbeReadBeU32(guest_base, pass_descriptor, 0x0C,
                     probe.device_command_list) &&
      ProbeReadBeU32(guest_base, pass_descriptor, 0x10,
                     probe.sampler_command_list);
  valid =
      valid && probe.device != 0 && probe.device_command_list != 0 &&
      probe.sampler_command_list != 0 &&
      ProbeReadBeU32(guest_base, probe.device_command_list, 0x10,
                     probe.declared_device_command_count) &&
      ProbeReadBeU32(guest_base, probe.sampler_command_list, 0x80,
                     probe.declared_sampler_command_count) &&
      probe.declared_device_command_count <= kMaximumApplyPassCommands &&
      probe.declared_sampler_command_count <= kMaximumApplyPassCommands;
  if (!valid) {
    return;
  }

  probe.captured_device_command_count =
      probe.declared_device_command_count;
  for (size_t index = 0; index < probe.captured_device_command_count;
       ++index) {
    ApplyPassDeviceCommand& command = probe.device_commands[index];
    const size_t offset = 0x14 + index * 8;
    valid =
        ProbeReadBeU32(guest_base, probe.device_command_list, offset,
                       command.device_subobject_offset) &&
        ProbeReadBeU32(guest_base, probe.device_command_list, offset + 4,
                       command.argument);
    if (!valid) {
      return;
    }
  }

  probe.captured_sampler_command_count =
      probe.declared_sampler_command_count;
  for (size_t index = 0; index < probe.captured_sampler_command_count;
       ++index) {
    ApplyPassSamplerCommand& command = probe.sampler_commands[index];
    const size_t offset = 0x84 + index * 8;
    valid =
        ProbeReadBeU16(guest_base, probe.sampler_command_list, offset,
                       command.argument) &&
        ProbeReadBeU16(guest_base, probe.sampler_command_list, offset + 2,
                       command.device_subobject_offset) &&
        ProbeReadBeU32(guest_base, probe.sampler_command_list, offset + 4,
                       command.value);
    if (!valid) {
      return;
    }
  }
  probe.command_lists_valid = true;
  probe.constants_before_valid =
      ProbeReadConstant(guest_base, probe.device, 20, probe.c20_before) &&
      ProbeReadConstant(guest_base, probe.device, 255, probe.c255_before);
  if (!probe.constants_before_valid) {
    return;
  }

  {
    std::lock_guard lock(g_observer_mutex);
    if (g_learned_identity != learned ||
        g_apply_pass_probe_claimed_generation == learned->generation) {
      return;
    }
    g_apply_pass_probe_claimed_generation = learned->generation;
  }
  probe.active = true;
  g_apply_pass_probe = probe;
}

void EndVenueE33ApplyPassProbe(uint8_t* guest_base) {
  if (!g_apply_pass_probe.active) {
    return;
  }
  ApplyPassProbe probe = g_apply_pass_probe;
  g_apply_pass_probe = {};

  std::array<uint32_t, 4> c20_after{};
  std::array<uint32_t, 4> c255_after{};
  const bool constants_after_valid =
      ProbeReadConstant(guest_base, probe.device, 20, c20_after) &&
      ProbeReadConstant(guest_base, probe.device, 255, c255_after);
  if (!constants_after_valid) {
    std::lock_guard lock(g_observer_mutex);
    if (g_apply_pass_probe_claimed_generation ==
        probe.learned_generation) {
      g_apply_pass_probe_claimed_generation = 0;
    }
  }
  const bool c20_changed =
      constants_after_valid && c20_after != probe.c20_before;
  const bool c255_changed =
      constants_after_valid && c255_after != probe.c255_before;

  REXLOG_INFO(
      "Table Tennis E33 ApplyPass boundary: generation={} pass={:08X} "
      "program={:08X} runtime={:08X} device={:08X} "
      "device_list={:08X}/{} sampler_list={:08X}/{} "
      "before_valid={} after_valid={} c20_changed={} c255_changed={} "
      "observer_only=true guest_mutated=false",
      probe.learned_generation, probe.pass_descriptor, probe.program_pair,
      probe.runtime_state, probe.device, probe.device_command_list,
      probe.declared_device_command_count, probe.sampler_command_list,
      probe.declared_sampler_command_count, probe.constants_before_valid,
      constants_after_valid, c20_changed, c255_changed);
  REXLOG_INFO(
      "  E33 ApplyPass c20 before=[{:.9g},{:.9g},{:.9g},{:.9g}] "
      "bits={:08X}/{:08X}/{:08X}/{:08X} "
      "after=[{:.9g},{:.9g},{:.9g},{:.9g}] "
      "bits={:08X}/{:08X}/{:08X}/{:08X}",
      ProbeFloat(probe.c20_before[0]), ProbeFloat(probe.c20_before[1]),
      ProbeFloat(probe.c20_before[2]), ProbeFloat(probe.c20_before[3]),
      probe.c20_before[0], probe.c20_before[1], probe.c20_before[2],
      probe.c20_before[3], ProbeFloat(c20_after[0]),
      ProbeFloat(c20_after[1]), ProbeFloat(c20_after[2]),
      ProbeFloat(c20_after[3]), c20_after[0], c20_after[1], c20_after[2],
      c20_after[3]);
  REXLOG_INFO(
      "  E33 ApplyPass c255 before=[{:.9g},{:.9g},{:.9g},{:.9g}] "
      "bits={:08X}/{:08X}/{:08X}/{:08X} "
      "after=[{:.9g},{:.9g},{:.9g},{:.9g}] "
      "bits={:08X}/{:08X}/{:08X}/{:08X}",
      ProbeFloat(probe.c255_before[0]), ProbeFloat(probe.c255_before[1]),
      ProbeFloat(probe.c255_before[2]), ProbeFloat(probe.c255_before[3]),
      probe.c255_before[0], probe.c255_before[1], probe.c255_before[2],
      probe.c255_before[3], ProbeFloat(c255_after[0]),
      ProbeFloat(c255_after[1]), ProbeFloat(c255_after[2]),
      ProbeFloat(c255_after[3]), c255_after[0], c255_after[1], c255_after[2],
      c255_after[3]);
  for (size_t index = 0; index < probe.captured_device_command_count;
       ++index) {
    const ApplyPassDeviceCommand& command = probe.device_commands[index];
    REXLOG_INFO(
        "  E33 ApplyPass device_cmd[{}] subobject_offset={:04X} "
        "argument={:08X}",
        index, command.device_subobject_offset, command.argument);
  }
  for (size_t index = 0; index < probe.captured_sampler_command_count;
       ++index) {
    const ApplyPassSamplerCommand& command = probe.sampler_commands[index];
    REXLOG_INFO(
        "  E33 ApplyPass sampler_cmd[{}] subobject_offset={:04X} "
        "argument={:04X} value={:08X}",
        index, command.device_subobject_offset, command.argument,
        command.value);
  }
}

void ObserveVenueE33TitleDraw(uint8_t* guest_base,
                              const SceneCatalogDrawOccurrence& draw) {
  if (!VenueE33ObserverEnabled()) {
    return;
  }
  {
    std::lock_guard lock(g_observer_mutex);
    g_latest_catalog_sequence =
        std::max(g_latest_catalog_sequence, draw.frame_sequence);
  }
  const VenueE33TitleCandidate candidate =
      ClassifyVenueE33TitleCandidate(guest_base, draw);

  const bool base_structure_matches =
      guest_base != nullptr && draw.player == 0 && draw.pass.valid &&
      draw.mesh.valid && draw.pass.pass_descriptor != 0 &&
      draw.pass.program_pair != 0 && draw.pass.vertex_shader != 0 &&
      draw.pass.pixel_shader != 0;
  const bool topology_matches =
      base_structure_matches &&
      draw.primitive_type == kTriangleStripPrimitive &&
      draw.mesh.aggregate_primitive_type == kTriangleStripPrimitive &&
      draw.submitted_index_count != 0;
  const bool stride_matches =
      topology_matches &&
      draw.mesh.vertex_stride == VenueE33VertexPayload::kStride;
  const bool endian_matches =
      stride_matches &&
      draw.mesh.vertex_endian == VenueE33VertexPayload::kEndian8In32;
  const bool index_layout_matches =
      endian_matches && draw.mesh.index_element_size == sizeof(uint16_t) &&
      !draw.mesh.index_is_32_bit;
  const uint64_t requested_index_bytes =
      static_cast<uint64_t>(draw.submitted_index_count) * sizeof(uint16_t);
  const bool buffer_bounds_matches =
      index_layout_matches && draw.mesh.vertex_buffer_bytes != 0 &&
      draw.mesh.vertex_buffer_bytes % VenueE33VertexPayload::kStride == 0 &&
      requested_index_bytes != 0 &&
      requested_index_bytes <= draw.mesh.index_buffer_bytes;
  bool log_gate_sample = false;
  bool candidate_admitted = false;
  bool capture_planned = false;
  std::shared_ptr<TitleToken> token;
  if (candidate.eligible) {
    token = std::make_shared<TitleToken>();
    token->candidate = candidate;
  }
  {
    std::lock_guard lock(g_observer_mutex);
    ++g_telemetry.title_draws_observed;
    g_telemetry.title_base_structure_matches += base_structure_matches;
    g_telemetry.title_topology_matches += topology_matches;
    g_telemetry.title_stride_matches += stride_matches;
    g_telemetry.title_endian_matches += endian_matches;
    g_telemetry.title_index_layout_matches += index_layout_matches;
    g_telemetry.title_buffer_bounds_matches += buffer_bounds_matches;
    g_telemetry.title_identity_matches += candidate.identity.valid();
    g_telemetry.title_declaration_matches +=
        candidate.vertex_declaration.valid();
    log_gate_sample = stride_matches && g_title_gate_sample_logs < 8;
    g_title_gate_sample_logs += log_gate_sample;
    if (candidate.eligible) {
      ++g_telemetry.title_candidates;
      const bool frame_sequence_matches =
          draw.frame_sequence != 0 &&
          (g_building_frame.sequence == 0 ||
           g_building_frame.sequence == draw.frame_sequence);
      if (!frame_sequence_matches ||
          g_building_frame.candidates.size() == kMaximumTitleCandidates) {
        ++g_building_frame.dropped_candidate_count;
      } else {
        if (g_building_frame.sequence == 0) {
          g_building_frame.sequence = draw.frame_sequence;
        }
        if (!g_building_frame.capture_identity_latched) {
          g_building_frame.capture_identity_latched = true;
          if (g_learned_identity != nullptr && g_learned_identity->valid()) {
            g_building_frame.capture_identity = g_learned_identity;
          }
        }
        const auto& capture_identity = g_building_frame.capture_identity;
        if (capture_identity != nullptr) {
          const bool generation_current =
              g_learned_identity != nullptr &&
              g_learned_identity->generation == capture_identity->generation;
          if (!generation_current) {
            g_building_frame.capture_generation_mismatches = 1;
          }
          const size_t position = g_building_frame.learned_match_cursor;
          if (MatchesLearnedPosition(candidate, *capture_identity, position)) {
            token->capture_generation = capture_identity->generation;
            ++g_building_frame.learned_match_cursor;
            capture_planned = generation_current;
            g_telemetry.title_capture_attempts += capture_planned;
          }
        }
        g_building_frame.candidates.push_back(token);
        candidate_admitted = true;
      }
    }
  }
  if (log_gate_sample) {
    const uint32_t declaration = draw.state.vertex_declaration != 0
                                     ? draw.state.vertex_declaration
                                     : draw.mesh.vertex_declaration;
    const VertexDeclarationProbe declaration_probe =
        ProbeVertexDeclaration(guest_base, declaration);
    const RawDeclarationSample raw_declaration =
        CaptureRawDeclarationSample(guest_base, declaration);
    REXLOG_INFO(
        "Table Tennis E33 title gate sample: ordinal={} count={} "
        "base={} topology={} stride={}/{} endian={}/{} index16={} "
        "vb_bytes={} ib_bytes={} identity={} declaration={} "
        "decl={:08X} eligible={} observer_only=true",
        draw.ordinal, draw.submitted_index_count, base_structure_matches,
        topology_matches, draw.mesh.vertex_stride,
        VenueE33VertexPayload::kStride, draw.mesh.vertex_endian,
        VenueE33VertexPayload::kEndian8In32, index_layout_matches,
        draw.mesh.vertex_buffer_bytes, draw.mesh.index_buffer_bytes,
        candidate.identity.valid(), candidate.vertex_declaration.valid(),
        draw.state.vertex_declaration, candidate.eligible);
    REXLOG_INFO(
        "  E33 declaration probe: address={:08X} raw_valid={} "
        "raw=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
        "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] "
        "valid={} failures={} count={} max_stream={} "
        "masks={:016X}/{:016X} cache={:08X}",
        declaration, raw_declaration.valid, raw_declaration.words[0],
        raw_declaration.words[1], raw_declaration.words[2],
        raw_declaration.words[3], raw_declaration.words[4],
        raw_declaration.words[5], raw_declaration.words[6],
        raw_declaration.words[7], raw_declaration.words[8],
        raw_declaration.words[9], raw_declaration.words[10],
        raw_declaration.words[11], raw_declaration.words[12],
        raw_declaration.words[13], raw_declaration.words[14],
        raw_declaration.words[15], declaration_probe.valid,
        declaration_probe.copy_failures, declaration_probe.element_count,
        declaration_probe.max_stream, declaration_probe.stream_mask_lo,
        declaration_probe.stream_mask_hi, declaration_probe.cache_id);
    if (declaration_probe.valid) {
      for (uint32_t element_index = 0;
           element_index < declaration_probe.element_count; ++element_index) {
        const VertexDeclarationElement& element =
            declaration_probe.elements[element_index];
        REXLOG_INFO(
            "    E33 declaration element[{}]: stream={} offset={} "
            "type={:08X} format={} signed={} normalized={} method={} "
            "usage={} usage_index={} padding={}",
            element_index, element.stream, element.byte_offset,
            element.packed_type, element.format(), element.is_signed(),
            element.normalized(), element.method, element.usage,
            element.usage_index, element.unused_padding);
      }
    }
  }
  if (!candidate.eligible) {
    return;
  }
  if (!candidate_admitted) {
    return;
  }
  if (!capture_planned) {
    return;
  }

  VenueE33TitleCapture capture =
      CaptureVenueE33TitleDraw(guest_base, draw, candidate);
  std::lock_guard lock(g_observer_mutex);
  if (g_learned_identity == nullptr ||
      g_learned_identity->generation != token->capture_generation ||
      g_building_frame.sequence != draw.frame_sequence) {
    token->capture_generation_mismatches = 1;
    if (g_building_frame.sequence == draw.frame_sequence) {
      g_building_frame.capture_generation_mismatches = 1;
    }
    return;
  }
  token->snapshot = std::move(capture.snapshot);
  token->guest_read_failures = capture.guest_read_failures;
  token->payload_copy_failures = capture.payload_copy_failures;
  token->texture_capture_failures = capture.texture_capture_failures;
  token->material_validation_failures = capture.material_validation_failures;
  token->renderer_full_mip_texture_count =
      capture.renderer_full_mip_texture_count;
  token->renderer_texture_shape_match_count =
      capture.renderer_texture_shape_match_count;
  g_telemetry.valid_title_snapshots += token->snapshot != nullptr;
  g_telemetry.title_guest_read_failures += token->guest_read_failures;
  g_telemetry.title_payload_copy_failures += token->payload_copy_failures;
  g_telemetry.title_texture_capture_failures += token->texture_capture_failures;
  g_telemetry.title_material_validation_failures +=
      token->material_validation_failures;
  g_telemetry.title_renderer_full_mip_textures +=
      token->renderer_full_mip_texture_count;
  g_telemetry.title_renderer_texture_shape_matches +=
      token->renderer_texture_shape_match_count;
}

void ObserveVenueE33BackendDraw(
    const rex::graphics::NativeGuestDrawContext& context) {
  if (!VenueE33ObserverEnabled()) {
    return;
  }
  const bool pixel_hash_matches = context.pixel_shader_hash == kPixelShaderHash;
  const bool shader_pair_matches =
      pixel_hash_matches && context.vertex_shader_hash == kVertexShaderHash;
  const bool rasterizer_contract_matches =
      shader_pair_matches && context.rasterizer_mode_control_valid;
  const bool exact_backend_draw = IsExactBackendDraw(context);
  std::lock_guard lock(g_observer_mutex);
  ++g_telemetry.backend_draws_observed;
  g_telemetry.latest_backend_frame_sequence =
      std::max(g_telemetry.latest_backend_frame_sequence,
               context.backend_frame_sequence);
  g_telemetry.backend_pixel_hash_matches += pixel_hash_matches;
  g_telemetry.backend_shader_pair_matches += shader_pair_matches;
  g_telemetry.backend_rasterizer_contract_matches +=
      rasterizer_contract_matches;
  if (rasterizer_contract_matches) {
    g_telemetry.latest_rasterizer_mode_control =
        context.rasterizer_mode_control;
  }
  g_telemetry.backend_contract_matches += exact_backend_draw;
  if (exact_backend_draw) {
    if (g_renderer_contract_logs < kMaximumRendererContractLogs) {
      ++g_renderer_contract_logs;
      rex::graphics::reg::PA_SU_SC_MODE_CNTL mode{};
      mode.value = context.rasterizer_mode_control;
      REXLOG_INFO(
          "Table Tennis E33 renderer raster diagnostic: mode={:08X} "
          "cull_front={} cull_back={} face_cw={} poly_mode={} "
          "poly_front={} poly_back={} offsets={}/{}/{} msaa={} "
          "window_offset={} multi_prim={} observer_only=true",
          mode.value, static_cast<uint32_t>(mode.cull_front),
          static_cast<uint32_t>(mode.cull_back),
          static_cast<uint32_t>(mode.face),
          static_cast<uint32_t>(mode.poly_mode),
          static_cast<uint32_t>(mode.polymode_front_ptype),
          static_cast<uint32_t>(mode.polymode_back_ptype),
          static_cast<uint32_t>(mode.poly_offset_front_enable),
          static_cast<uint32_t>(mode.poly_offset_back_enable),
          static_cast<uint32_t>(mode.poly_offset_para_enable),
          static_cast<uint32_t>(mode.msaa_enable),
          static_cast<uint32_t>(mode.vtx_window_offset_enable),
          static_cast<uint32_t>(mode.multi_prim_ib_ena));
    }
    BackendEvent event = {
        .backend_frame_sequence = context.backend_frame_sequence,
        .identity =
            {
                .primitive_type = context.primitive_type,
                .submitted_index_count = context.guest_vertex_or_index_count,
                .guest_index_base = context.guest_index_base,
                .guest_vertex_base =
                    context.primary_vertex_fetch.physical_address,
                .guest_vertex_bytes = context.primary_vertex_fetch.byte_count,
                .guest_vertex_endian = context.primary_vertex_fetch.endian,
            },
        .contract = CaptureBackendContract(context),
    };
    ++g_telemetry.backend_events;
    if (g_backend_events.size() == kMaximumQueuedBackendEvents) {
      g_backend_events.pop_front();
      ++g_telemetry.backend_events_dropped;
    }
    g_backend_events.push_back(std::move(event));
  }
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  UpdatePendingCountsLocked();
}

void VenueE33ObserverFrameEnd() {
  const bool enabled = VenueE33ObserverEnabled();
  std::lock_guard lock(g_observer_mutex);
  if (!enabled) {
    g_building_frame = {};
    g_frames.clear();
    g_backend_events.clear();
    g_learned_identity.reset();
    g_published_frame.reset();
    g_telemetry = {};
    g_latest_catalog_sequence = 0;
    g_learning_generation = 0;
    g_logged_published_generation = 0;
    g_announced_mismatch = false;
    g_announced_capture_rejection = false;
    g_title_gate_sample_logs = 0;
    g_renderer_contract_logs = 0;
    g_selected_material_contract_logs = 0;
    g_apply_pass_probe_claimed_generation = 0;
    g_apply_pass_probe = {};
    return;
  }

  ++g_telemetry.title_frames;
  if (!g_building_frame.candidates.empty()) {
    FrameLedger frame;
    frame.sequence = g_building_frame.sequence;
    frame.title = std::move(g_building_frame);
    g_frames.push_back(std::move(frame));
  }
  g_building_frame = {};
  ReconcileBackendEventsLocked();
  AnalyzeCompletedFramesLocked();
  ExpireFramesLocked();
  UpdatePendingCountsLocked();
}

std::shared_ptr<const VenueE33FrameSnapshot> LatestVenueE33FrameSnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_published_frame;
}

std::shared_ptr<const VenueE33LearnedIdentitySnapshot>
LatestVenueE33LearnedIdentitySnapshot() {
  std::lock_guard lock(g_observer_mutex);
  return g_learned_identity;
}

VenueE33ObserverTelemetry LatestVenueE33ObserverTelemetry() {
  std::lock_guard lock(g_observer_mutex);
  return g_telemetry;
}

}  // namespace tabletennis::native
