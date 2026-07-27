# tabletennis - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tabletennis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TABLETENNIS_SOURCES
    src/main.cpp
    src/native/tabletennis_camera.cpp
    src/native/tabletennis_camera_observer.cpp
    src/native/tabletennis_crowd_diagnostic_suppression.cpp
    src/native/tabletennis_crowd_observer.cpp
    src/native/tabletennis_crowd_observer_renderer.cpp
    src/native/tabletennis_crowd_replacement_prewarm.cpp
    src/native/tabletennis_crowd_snapshot.cpp
    src/native/tabletennis_crowd_texture_decoder.cpp
    src/native/tabletennis_guest_memory.cpp
    src/native/tabletennis_draw_constants.cpp
    src/native/tabletennis_draw_replacer_dispatcher.cpp
    src/native/tabletennis_6ae_player_observer.cpp
    src/native/tabletennis_6ae_player_snapshot.cpp
    src/native/tabletennis_d47_player_observer.cpp
    src/native/tabletennis_d47_player_snapshot.cpp
    src/native/tabletennis_frame_scene.cpp
    src/native/tabletennis_fps_telemetry.cpp
    src/native/tabletennis_hud_swf_capture.cpp
    src/native/tabletennis_material_observer.cpp
    src/native/tabletennis_main_coverage_ledger.cpp
    src/native/tabletennis_model_material_tokens.cpp
    src/native/tabletennis_mesh_payload_probe.cpp
    src/native/tabletennis_mesh_snapshot.cpp
    src/native/tabletennis_net_bb903_observer.cpp
    src/native/tabletennis_native_scene_compositor.cpp
    src/native/tabletennis_native_scene_pass.cpp
    src/native/tabletennis_native_scene_targets.cpp
    src/native/tabletennis_native_scene_transaction.cpp
    src/native/tabletennis_texture_snapshot.cpp
    src/native/tabletennis_venue_diagnostic_suppression.cpp
    src/native/tabletennis_venue_14d_observer.cpp
    src/native/tabletennis_venue_14d_renderer.cpp
    src/native/tabletennis_venue_14d_snapshot.cpp
    src/native/tabletennis_venue_526a_observer.cpp
    src/native/tabletennis_venue_e33_observer.cpp
    src/native/tabletennis_venue_e33_snapshot.cpp
    src/native/tabletennis_venue_full_family.cpp
    src/native/tabletennis_venue_observer_renderer.cpp
    src/native/tabletennis_venue_snapshot.cpp
    src/native/tabletennis_native_capture.cpp
    src/native/tabletennis_native_hooks.cpp
    src/native/tabletennis_native_observer.cpp
    src/native/tabletennis_player_palette.cpp
    src/native/tabletennis_player_palette_write_observer.cpp
    src/native/tabletennis_player_2ac_payload.cpp
    src/native/tabletennis_player_2ac_observer.cpp
    src/native/tabletennis_player_2ac_renderer.cpp
    src/native/tabletennis_player_observer_renderer.cpp
    src/native/tabletennis_player_replacement_candidates.cpp
    src/native/tabletennis_player_skin_observer.cpp
    src/native/tabletennis_player_skin_snapshot.cpp
    src/native/tabletennis_observer_overlay.cpp
    src/native/tabletennis_native_renderer.cpp
    src/native/tabletennis_scene_draw_catalog.cpp
    src/native/tabletennis_scene_owner_observer.cpp
    src/native/tabletennis_vertex_declaration.cpp
    src/test/tabletennis_frontend_launch_test.cpp
)

if(WIN32)
    add_executable(tabletennis WIN32 ${TABLETENNIS_SOURCES})
else()
    add_executable(tabletennis ${TABLETENNIS_SOURCES})
endif()

rexglue_setup_target(tabletennis)

# GPU frame trace viewer. The SDK contains the shared trace reader/player UI,
# but doesn't expose an executable target for it.
if(REXGLUE_USE_VULKAN)
    add_executable(tabletennis_trace_viewer
        src/trace_viewer_main.cpp
    )
    target_link_libraries(tabletennis_trace_viewer PRIVATE rex::runtime)
    rexglue_configure_target(tabletennis_trace_viewer)
endif()
