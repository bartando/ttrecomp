#include "native/tabletennis_6ae_player_observer.h"
#include "native/tabletennis_camera_observer.h"
#include "native/tabletennis_crowd_observer.h"
#include "native/tabletennis_d47_player_observer.h"
#include "native/tabletennis_draw_constants.h"
#include "native/tabletennis_frame_scene.h"
#include "native/tabletennis_hud_swf_backend_observer.h"
#include "native/tabletennis_hud_swf_capture.h"
#include "native/tabletennis_late_comp_first_slice.h"
#include "native/tabletennis_late_phase_ledger.h"
#include "native/tabletennis_main_coverage_ledger.h"
#include "native/tabletennis_material_observer.h"
#include "native/tabletennis_model_material_tokens.h"
#include "native/tabletennis_native_capture.h"
#include "native/tabletennis_native_observer.h"
#include "native/tabletennis_net_bb903_observer.h"
#include "native/tabletennis_player_2ac_observer.h"
#include "native/tabletennis_player_2ac_renderer.h"
#include "native/tabletennis_player_a406_observer.h"
#include "native/tabletennis_player_bbb5_observer.h"
#include "native/tabletennis_player_palette.h"
#include "native/tabletennis_player_palette_write_observer.h"
#include "native/tabletennis_player_skin_observer.h"
#include "native/tabletennis_player_skin_snapshot.h"
#include "native/tabletennis_ps328_tile_invariance.h"
#include "native/tabletennis_scene_draw_catalog.h"
#include "native/tabletennis_scene_owner_observer.h"
#include "native/tabletennis_venue_14d_observer.h"
#include "native/tabletennis_venue_526a_observer.h"
#include "native/tabletennis_venue_9e_observer.h"
#include "native/tabletennis_venue_e33_observer.h"
#include "native/tabletennis_venue_full_family.h"
#include "native/tabletennis_venue_snapshot.h"

#include "generated/default/tabletennis_init.h"

#include <atomic>
#include <bit>

#include <rex/logging.h>

namespace {

std::atomic<uint64_t> g_post_ps328_retirement_swap_count = 0;

bool ShouldLogPostPs328RetirementSwap(uint64_t swap) {
  return swap <= 3 || swap % 120 == 0;
}

} // namespace

// pongPlayer's draw-bucket render callback. RTTI and Ghidra establish that
// r3 is the embedded renderable at player+8; all character model submissions
// occur synchronously below this call.
extern "C" REX_FUNC(sub_8218E6E8) {
  const uint32_t renderable = ctx.r3.u32;
  const uint32_t player = renderable >= 8 ? renderable - 8 : 0;
  const uint32_t creature = player != 0 ? REX_LOAD_U32(player + 0x1C4) : 0;
  tabletennis::native::RegisterPlayerPaletteCreatureOwner(player, creature);
  tabletennis::native::BeginPlayerPalettePlayerScope(player);
  tabletennis::native::BeginPlayerPaletteWriteOwnerScope(player);
  tabletennis::native::BeginSceneDrawCatalogPlayerScope(player);
  __imp__sub_8218E6E8(ctx, base);
  tabletennis::native::EndSceneDrawCatalogPlayerScope();
  tabletennis::native::EndPlayerPaletteWriteOwnerScope();
  tabletennis::native::EndPlayerPalettePlayerScope();
}

// Paired pongPlayer render callback. It receives the same embedded renderable
// at player+8, but dispatches the second vtable slot.
extern "C" REX_FUNC(sub_8218E860) {
  const uint32_t renderable = ctx.r3.u32;
  const uint32_t player = renderable >= 8 ? renderable - 8 : 0;
  const uint32_t creature = player != 0 ? REX_LOAD_U32(player + 0x1C4) : 0;
  tabletennis::native::RegisterPlayerPaletteCreatureOwner(player, creature);
  tabletennis::native::BeginPlayerPalettePlayerScope(player);
  tabletennis::native::BeginPlayerPaletteWriteOwnerScope(player);
  tabletennis::native::BeginSceneDrawCatalogPlayerScope(player);
  __imp__sub_8218E860(ctx, base);
  tabletennis::native::EndSceneDrawCatalogPlayerScope();
  tabletennis::native::EndPlayerPaletteWriteOwnerScope();
  tabletennis::native::EndPlayerPalettePlayerScope();
}

// pongCreature's render-interface implementation. Keep the live creature
// identity scoped through the nested pongDrawable submission.
extern "C" REX_FUNC(sub_820C6378) {
  const uint32_t creature_interface = ctx.r3.u32;
  const uint32_t player =
      tabletennis::native::ResolvePlayerPaletteCreatureOwner(
          creature_interface);
  const uint32_t drawable_container =
      creature_interface != 0 ? REX_LOAD_U32(creature_interface + 0x90) : 0;
  const uint32_t drawable =
      drawable_container != 0 ? REX_LOAD_U32(drawable_container + 0x14) : 0;
  tabletennis::native::RegisterPlayerPaletteDrawableOwner(player, drawable);
  tabletennis::native::BeginPlayerPalettePlayerScope(player);
  tabletennis::native::BeginPlayerPaletteWriteOwnerScope(player);
  tabletennis::native::BeginSceneDrawCatalogPlayerScope(player);
  tabletennis::native::BeginPlayerPaletteCreatureScope(creature_interface);
  __imp__sub_820C6378(ctx, base);
  tabletennis::native::EndPlayerPaletteCreatureScope();
  tabletennis::native::EndSceneDrawCatalogPlayerScope();
  tabletennis::native::EndPlayerPaletteWriteOwnerScope();
  tabletennis::native::EndPlayerPalettePlayerScope();
}

// pongDrawable::SubmitModels runs after the creature render path has resolved
// the current skin palette and immediately before its model-list walk.
extern "C" REX_FUNC(sub_8225C7E0) {
  const uint32_t drawable = ctx.r3.u32;
  const uint32_t player =
      tabletennis::native::ResolvePlayerPaletteOwner(drawable);
  tabletennis::native::BeginPlayerPalettePlayerScope(player);
  tabletennis::native::BeginPlayerPaletteWriteOwnerScope(player);
  tabletennis::native::BeginSceneDrawCatalogPlayerScope(player);
  tabletennis::native::ObservePlayerDrawablePalette(base, drawable);
  __imp__sub_8225C7E0(ctx, base);
  tabletennis::native::EndSceneDrawCatalogPlayerScope();
  tabletennis::native::EndPlayerPaletteWriteOwnerScope();
  tabletennis::native::EndPlayerPalettePlayerScope();
}

// Title matrix-to-GPU-palette packer. Ghidra proves r3 is the destination,
// r4 is the 64-byte matrix source, and r5 is the record count. Observe only
// after the original has completed all 28-byte records and cache-block stores.
extern "C" REX_FUNC(sub_8225C510) {
  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t record_count = ctx.r5.u32;
  __imp__sub_8225C510(ctx, base);
  tabletennis::native::ObserveCompletedPlayerPaletteWrite(base, destination,
                                                          source, record_count);
}

// pongDrawable's primary-palette binder always binds the current stream-3
// buffer, but conditionally skips repacking when its cached generation/source
// is current. Preserve that postcondition under the exact player scope.
extern "C" REX_FUNC(sub_8225C668) {
  const uint32_t drawable = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t requested_record_count = ctx.r5.u32;
  __imp__sub_8225C668(ctx, base);
  tabletennis::native::ObserveCompletedPrimaryPlayerPaletteBinding(
      base, drawable, source, requested_record_count);
}

// pongDrawable's alternate-palette binder writes the second matrix-count
// half of the same stream-3 ring buffer through sub_8225C510. Its gameplay
// caller runs after the creature submission scope has unwound, so recover
// the exact drawable owner learned by ObservePlayerDrawablePalette.
extern "C" REX_FUNC(sub_8225C720) {
  const uint32_t drawable = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t player =
      tabletennis::native::ResolvePlayerPaletteOwner(drawable);
  tabletennis::native::BeginPlayerPaletteWriteOwnerScope(player);
  __imp__sub_8225C720(ctx, base);
  tabletennis::native::EndPlayerPaletteWriteOwnerScope();
  tabletennis::native::ObserveCompletedAlternatePlayerPaletteBinding(
      base, drawable, source);
  tabletennis::native::FinalizeD47PendingPaletteSnapshots(base);
}

// pongPlayer::Draw(bool alternate_pass)
extern "C" REX_FUNC(sub_8218E1E0) {
  const uint32_t player = ctx.r3.u32;
  const bool alternate_pass = (ctx.r4.u32 & 0xFF) != 0;
  __imp__sub_8218E1E0(ctx, base);
  tabletennis::native::CapturePlayerDraw(player, alternate_pass);
}

// pongDrawBucket::AddEntry(pongRenderable*, uint32_t bucket_mask)
extern "C" REX_FUNC(sub_822278D8) {
  const uint32_t bucket_manager = ctx.r3.u32;
  const uint32_t renderable = ctx.r4.u32;
  const uint32_t bucket_mask = ctx.r5.u32;
  const uint32_t vtable = renderable == 0 ? 0 : REX_LOAD_U32(renderable);
  __imp__sub_822278D8(ctx, base);
  tabletennis::native::CaptureDrawBucketEntry(bucket_manager, renderable,
                                              vtable, bucket_mask);
  tabletennis::native::ObserveSceneOwnerDrawBucketEntry(renderable, vtable);
  tabletennis::native::ObserveDrawBucketEntry(base, renderable, vtable);
}

// lvlTable's draw-bucket callback. RTTI fixes r3 at owner+0x60 and the
// generated body synchronously dispatches the table/net render chain.
extern "C" REX_FUNC(sub_822432A0) {
  tabletennis::native::BeginSceneOwnerTableRenderScope(base, ctx.r3.u32);
  __imp__sub_822432A0(ctx, base);
  tabletennis::native::EndSceneOwnerRenderScope();
}

// gmBallNode's embedded draw-bucket callback (owner+0x60). It delegates to
// the node's live drawable, so this is the exact core-ball render scope.
extern "C" REX_FUNC(sub_82281CF8) {
  tabletennis::native::BeginSceneOwnerBallNodeRenderScope(base, ctx.r3.u32);
  __imp__sub_82281CF8(ctx, base);
  tabletennis::native::EndSceneOwnerRenderScope();
}

// gmBallNode submission. Generated code queues owner fields +0xB4 and
// +0xBC+4; keep the scope active so AddEntry confirms both identities.
extern "C" REX_FUNC(sub_82281C10) {
  tabletennis::native::BeginSceneOwnerBallSubmission(base, ctx.r3.u32);
  __imp__sub_82281C10(ctx, base);
  tabletennis::native::EndSceneOwnerSubmission();
}

// fxBallSplash2D's draw-bucket callback. The exact gmBallNode owner is
// resolved from the same-frame +0xBC+4 AddEntry captured above.
extern "C" REX_FUNC(sub_8238A488) {
  tabletennis::native::BeginSceneOwnerMappedBallRenderScope(base, ctx.r3.u32);
  __imp__sub_8238A488(ctx, base);
  tabletennis::native::EndSceneOwnerRenderScope();
}

// pongPaddle's draw-bucket callback and queue helper. RTTI proves the paddle
// is itself the renderable at vtable 0x82071678.
extern "C" REX_FUNC(sub_823D4208) {
  tabletennis::native::BeginSceneOwnerPaddleRenderScope(base, ctx.r3.u32);
  __imp__sub_823D4208(ctx, base);
  tabletennis::native::EndSceneOwnerRenderScope();
}

extern "C" REX_FUNC(sub_823D44E0) {
  tabletennis::native::BeginSceneOwnerPaddleSubmission(base, ctx.r3.u32);
  __imp__sub_823D44E0(ctx, base);
  tabletennis::native::EndSceneOwnerSubmission();
}

// fxCrowdGfx::Render. RTTI proves r3 is the exact crowd owner. This outer
// scope validates the owner and brackets its two owned submit pairs.
extern "C" REX_FUNC(sub_82385AB0) {
  tabletennis::native::BeginCrowdRenderScope(base, ctx.r3.u32);
  __imp__sub_82385AB0(ctx, base);
  tabletennis::native::EndCrowdRenderScope();
}

// fxCrowdGfx's drawable/model-record submit helper. Generated title code
// proves r4/r5 come directly from owner fields {+0x28,+0x30} or
// {+0x2C,+0x34}; this routine synchronously reaches sub_820EE910 and the
// indexed draw. Use that exact pair as the semantic crowd token.
extern "C" REX_FUNC(sub_82385C88) {
  tabletennis::native::BeginCrowdDrawableSubmitScope(base, ctx.r3.u32,
                                                     ctx.r4.u32, ctx.r5.u32);
  __imp__sub_82385C88(ctx, base);
  tabletennis::native::EndCrowdDrawableSubmitScope();
}

// rage::grmModelGeom submission. Observe before the original routine so the
// streaming-owned model and buffer pointers are sampled at their draw-time
// lifetime. The original draw always runs unchanged.
extern "C" REX_FUNC(sub_820F19C8) {
  const uint32_t model = ctx.r3.u32;
  const uint32_t shader_group = ctx.r4.u32;
  const uint32_t render_category = ctx.r5.u32;
  const uint32_t lod = ctx.r6.u32;
  tabletennis::native::ObserveModelMaterialValueTokens(
      base, model, shader_group, render_category, lod);
  const bool observed_table_model =
      tabletennis::native::ObserveModelGeometrySubmission(
          base, model, shader_group, render_category, lod);
  tabletennis::native::BeginModelDrawScope(observed_table_model, model,
                                           shader_group);
  __imp__sub_820F19C8(ctx, base);
  tabletennis::native::EndModelDrawScope();
}

// rage::grmShaderFx::DrawModelGeometry. Ghidra and live RTTI identify this as
// the exact material-to-geometry dispatch: BeginTechnique, one or more passes,
// then the model's real indexed submission. Observe at entry while every
// title-owned pointer is live; the original routine always runs unchanged.
extern "C" REX_FUNC(sub_820EFB30) {
  const uint32_t shader = ctx.r3.u32;
  const uint32_t model = ctx.r4.u32;
  const uint32_t geometry_index = ctx.r5.u32;
  const uint32_t lod = ctx.r6.u32;
  const bool alternate_pass = (ctx.r7.u32 & 0xFF) != 0;
  tabletennis::native::BeginSceneDrawCatalogScope(shader, model, geometry_index,
                                                  lod, alternate_pass);
  tabletennis::native::BeginTableMaterialDraw(
      base, shader, model, geometry_index, lod, alternate_pass);
  __imp__sub_820EFB30(ctx, base);
  tabletennis::native::EndTableMaterialDraw();
  tabletennis::native::EndSceneDrawCatalogScope();
}

// rage::grmShaderFx ApplyPass. The pass descriptor selects the real VS/PS
// program pair and owns the command lists that bind material constants,
// textures, and sampler state.
extern "C" REX_FUNC(sub_82158C48) {
  const uint32_t runtime_state = ctx.r3.u32;
  const uint32_t pass_descriptor = ctx.r4.u32;
  tabletennis::native::ObserveSceneDrawCatalogPass(base, runtime_state,
                                                   pass_descriptor);
  tabletennis::native::ObserveTableMaterialPass(base, runtime_state,
                                                pass_descriptor);
  tabletennis::native::BeginVenueE33ApplyPassProbe(base, runtime_state,
                                                   pass_descriptor);
  __imp__sub_82158C48(ctx, base);
  tabletennis::native::EndVenueE33ApplyPassProbe(base);
}

// Low-level gfx shader binders are the authoritative title-device
// postconditions. COMP may rebind outside the generic ApplyPass scope.
extern "C" REX_FUNC(sub_82357240) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t shader = ctx.r4.u32;
  __imp__sub_82357240(ctx, base);
  tabletennis::native::ObserveSceneDrawCatalogBoundVertexShader(base, device,
                                                                shader);
}

extern "C" REX_FUNC(sub_82356F50) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t shader = ctx.r4.u32;
  __imp__sub_82356F50(ctx, base);
  tabletennis::native::ObserveSceneDrawCatalogBoundPixelShader(base, device,
                                                               shader);
}

// grmModelGeom's direct draw helper. At entry r3 is the live vertex
// aggregate, r4 selects matching vertex/index slots, r5 is the optional
// secondary stream, and r6 chooses aggregate+0x30 as the primary stream.
// Keep this scope active through the helper's nested indexed submission.
extern "C" REX_FUNC(sub_820EE6E8) {
  const uint32_t vertex_aggregate = ctx.r3.u32;
  const uint32_t stream_selector = ctx.r4.u32;
  const uint32_t secondary_vertex_stream = ctx.r5.u32;
  const bool alternate_primary_stream = (ctx.r6.u32 & 0xFF) != 0;
  tabletennis::native::BeginSceneDrawCatalogMeshSelection(
      base, vertex_aggregate, stream_selector, secondary_vertex_stream,
      alternate_primary_stream);
  __imp__sub_820EE6E8(ctx, base);
  tabletennis::native::EndSceneDrawCatalogMeshSelection();
}

// grmModelGeom's global-selector draw helper. It reads the active selector
// from 0x8260634C and always binds aggregate+0x30 as its primary stream.
extern "C" REX_FUNC(sub_820EE7D8) {
  const uint32_t vertex_aggregate = ctx.r3.u32;
  tabletennis::native::BeginSceneDrawCatalogGlobalMeshSelection(
      base, vertex_aggregate);
  __imp__sub_820EE7D8(ctx, base);
  tabletennis::native::EndSceneDrawCatalogMeshSelection();
}

// Fx type-6 resource parameter setter. This is the live slot+texture-object
// interception point used by grmShaderFx before the effect runtime unwraps
// the resource into its underlying GPU binding.
extern "C" REX_FUNC(sub_8215A830) {
  const uint32_t fx_runtime = ctx.r3.u32;
  const uint32_t encoded_handle = ctx.r4.u32;
  const uint32_t resource = ctx.r5.u32;
  tabletennis::native::ObserveTableTextureParameterBind(
      base, fx_runtime, encoded_handle, resource);
  __imp__sub_8215A830(ctx, base);
}

// grcTextureReference::GetGpuBinding (+0x50).
extern "C" REX_FUNC(sub_8215D510) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_8215D510(ctx, base);
  tabletennis::native::ObserveHudSwfTextureResourceUnwrap(base, resource,
                                                          ctx.r3.u32);
  tabletennis::native::ObserveTextureResourceUnwrap(base, resource, ctx.r3.u32);
}

// grcTextureXenon::GetGpuBinding (+0x50).
extern "C" REX_FUNC(sub_8215FC98) {
  const uint32_t resource = ctx.r3.u32;
  __imp__sub_8215FC98(ctx, base);
  tabletennis::native::ObserveHudSwfTextureResourceUnwrap(base, resource,
                                                          ctx.r3.u32);
  tabletennis::native::ObserveTextureResourceUnwrap(base, resource, ctx.r3.u32);
}

// Gameplay HUD Scaleform draw. Only the exact live SWF context owned by the
// hudHUD singleton is accepted by the observer; other shell/frontend SWFs
// continue through this hook without producing capture data.
extern "C" REX_FUNC(sub_823F9238) {
  const uint32_t swf_context = ctx.r3.u32;
  tabletennis::native::BeginHudSwfDrawScope(base, swf_context);
  __imp__sub_823F9238(ctx, base);
  tabletennis::native::EndHudSwfDrawScope(base);
}

// Scaleform texture bind. r3 is the guest texture handle, with zero meaning
// the title's default texture global.
extern "C" REX_FUNC(sub_822EC298) {
  const uint32_t handle_or_zero = ctx.r3.u32;
  tabletennis::native::ObserveHudSwfTextureBind(base, handle_or_zero);
  __imp__sub_822EC298(ctx, base);
}

// Immediate-mode 36-byte vertex batch allocation. At entry the title globals
// still describe the previous completed batch; at exit they contain the new
// allocation base that the SWF renderer will fill.
extern "C" REX_FUNC(sub_82152A78) {
  const uint32_t batch_kind = ctx.r3.u32;
  const uint32_t requested_vertex_count = ctx.r4.u32;
  tabletennis::native::BeginHudSwfDynamicBatch(base, batch_kind,
                                               requested_vertex_count);
  __imp__sub_82152A78(ctx, base);
  tabletennis::native::CompleteHudSwfDynamicBatchStart(base);
}

// rage::grcDevice::DrawIndexedPrimitive. The title has finished staging the
// draw's vertex constants by this point; sub_82363C00 at the top of the
// original function flushes device+1920 as the 0x4000 vertex-constant bank.
extern "C" REX_FUNC(sub_82357B30) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t submitted_index_count = ctx.r7.u32;
  tabletennis::native::ObserveIndexedDrawConstants(base, device, primitive_type,
                                                   submitted_index_count);
  __imp__sub_82357B30(ctx, base);
  // The original call commits the draw's vertex fetch constants. Sampling
  // them at entry observes the preceding mesh; sampling at exit keeps the
  // catalog identity paired with the fetches actually submitted by this draw.
  tabletennis::native::ObserveSceneDrawCatalogIndexedDraw(
      base, device, primitive_type, submitted_index_count);
}

// Completed rage render-context bind. By this point the title has materialized
// WorldView, WorldViewProjection, ViewInverse, View and Projection in its
// 0x390-byte context and uploaded c0-c19 to both shader stages.
extern "C" REX_FUNC(sub_82152E80) {
  const uint32_t render_context = ctx.r3.u32;
  __imp__sub_82152E80(ctx, base);
  tabletennis::native::ObserveRenderContextCamera(base, render_context);
}

// pongBallInstance::GetPositionTransform() exposes the live instance whenever
// gameplay/render code asks for it. The authoritative center is the float3 at
// +64, also consumed by pongBallInstance::ActivateBall.
extern "C" REX_FUNC(sub_82280028) {
  const uint32_t ball = ctx.r3.u32;
  const float x = std::bit_cast<float>(REX_LOAD_U32(ball + 64));
  const float y = std::bit_cast<float>(REX_LOAD_U32(ball + 68));
  const float z = std::bit_cast<float>(REX_LOAD_U32(ball + 72));
  tabletennis::native::ObserveSceneOwnerBallTransform(ball);
  __imp__sub_82280028(ctx, base);
  tabletennis::native::CaptureBall(ball, x, y, z);
}

// Title-side D3D Swap. Capture before the original call because VdSwap inside
// it is what schedules the guest-output refresh and native-render callback.
extern "C" REX_FUNC(sub_8235B750) {
  const bool post_ps328_retirement =
      tabletennis::native::Ps328TitleCaptureRetired();
  const uint64_t post_ps328_retirement_swap =
      post_ps328_retirement
          ? g_post_ps328_retirement_swap_count.fetch_add(
                1, std::memory_order_relaxed) +
                1
          : 0;
  if (post_ps328_retirement &&
      ShouldLogPostPs328RetirementSwap(post_ps328_retirement_swap)) {
    REXLOG_INFO("Table Tennis post-PS328-retirement guest swap entered "
                "swap={} observer_only=true",
                post_ps328_retirement_swap);
  }
  // Publish guest-owned venue buffers before CaptureFrameEnd decides whether
  // this swap needs the native observer post-process.
  tabletennis::native::VenueFullFamilyFrameEnd();
  tabletennis::native::VenueFamilyFrameEnd();
  tabletennis::native::Venue14DObserverFrameEnd();
  tabletennis::native::Venue526AObserverFrameEnd();
  tabletennis::native::Venue9EObserverFrameEnd();
  tabletennis::native::VenueE33ObserverFrameEnd();
  tabletennis::native::NetBB903ObserverFrameEnd();
  tabletennis::native::Player2ACObserverFrameEnd();
  tabletennis::native::Player2ACCompositeObserverFrameEnd();
  tabletennis::native::Player6AEObserverFrameEnd();
  tabletennis::native::D47PlayerObserverFrameEnd();
  tabletennis::native::PlayerA406ObserverFrameEnd();
  tabletennis::native::PlayerBBB5ObserverFrameEnd();
  tabletennis::native::PlayerPaletteWriteObserverFrameEnd();
  tabletennis::native::PlayerPaletteFrameEnd();
  tabletennis::native::PlayerSkinSnapshotFrameEnd();
  tabletennis::native::PlayerSkinObserverFrameEnd();
  tabletennis::native::CrowdObserverFrameEnd();
  tabletennis::native::CaptureFrameEnd();
  tabletennis::native::MaterialObserverFrameEnd();
  tabletennis::native::ModelMaterialValueTokensFrameEnd();
  tabletennis::native::ObserverFrameEnd();
  tabletennis::native::SceneDrawCatalogFrameEnd();
  tabletennis::native::MainCoverageLedgerFrameEnd();
  tabletennis::native::SceneOwnerObserverFrameEnd();
  tabletennis::native::HudSwfCaptureFrameEnd();
  tabletennis::native::HudSwfBackendObserverFrameEnd();
  tabletennis::native::LatePhaseLedgerFrameEnd();
  tabletennis::native::LateCompFirstSliceObserverFrameEnd();
  tabletennis::native::NativeFrameSceneFrameEnd();
  __imp__sub_8235B750(ctx, base);
  if (post_ps328_retirement &&
      ShouldLogPostPs328RetirementSwap(post_ps328_retirement_swap)) {
    REXLOG_INFO("Table Tennis post-PS328-retirement guest swap returned "
                "swap={} observer_only=true",
                post_ps328_retirement_swap);
  }
}
