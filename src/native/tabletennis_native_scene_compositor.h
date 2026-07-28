#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace tabletennis::native {

struct TableTennisFrameScene;

// The first shared native-scene slice is intentionally limited to families
// whose immutable real geometry/material renderers already exist. This is an
// observer composition plan, not a declaration that the whole title frame is
// captured or safe to serve.
enum class NativeSceneDrawFamily : uint8_t {
  kVenuePs328,
  kVenue14D,
  kCrowdC6,
  kPlayerCa9,
};

struct NativeSceneDrawRef {
  NativeSceneDrawFamily family = NativeSceneDrawFamily::kVenuePs328;
  uint32_t ordinal = 0;
  uint32_t family_draw_index = 0;
};

enum class NativeSceneCompositionRejectReason : uint8_t {
  kNone,
  kMissingScene,
  kGameplayInactive,
  kMissingCatalog,
  kCatalogFrameMismatch,
  kCatalogIncomplete,
  kMissingFamilyFrame,
  kFamilyFrameMismatch,
  kInvalidFamilyFrame,
  kOrdinalOutsideCatalog,
  kCatalogIdentityMismatch,
  kDuplicateOrdinal,
};

struct NativeSceneCompositionReadiness {
  uint64_t title_sequence = 0;
  uint32_t catalog_draw_count = 0;
  uint32_t venue_ps328_draw_count = 0;
  uint32_t venue_14d_draw_count = 0;
  uint32_t crowd_c6_draw_count = 0;
  uint32_t player_ca9_draw_count = 0;
  NativeSceneDrawFamily failed_family =
      NativeSceneDrawFamily::kVenuePs328;
  uint32_t failed_family_draw_index = 0;
  uint32_t failed_ordinal = 0;
  NativeSceneCompositionRejectReason reject_reason =
      NativeSceneCompositionRejectReason::kMissingScene;
  bool gameplay_active = false;
  bool catalog_exact = false;
  bool family_frames_exact = false;
  bool family_payloads_valid = false;
  bool ordinal_identity_exact = false;
  bool observer_composition_ready = false;
};

// Owns the immutable frame scene so every draw reference remains valid after
// publication. Entries are strictly increasing by the title's original
// DrawIndexedPrimitive ordinal and contain no cross-family duplicates.
struct NativeSceneCompositionPlan {
  std::shared_ptr<const TableTennisFrameScene> scene;
  NativeSceneCompositionReadiness readiness{};
  std::vector<NativeSceneDrawRef> draws;

  bool valid() const {
    return scene != nullptr && readiness.observer_composition_ready &&
           readiness.reject_reason ==
               NativeSceneCompositionRejectReason::kNone &&
           !draws.empty();
  }
};

// Pure observer-side planning. This allocates no GPU resources, records no
// commands and cannot enable native output suppression.
NativeSceneCompositionPlan BuildNativeSceneCompositionPlan(
    std::shared_ptr<const TableTennisFrameScene> scene);

const char *NativeSceneCompositionRejectReasonName(
    NativeSceneCompositionRejectReason reason);

} // namespace tabletennis::native
