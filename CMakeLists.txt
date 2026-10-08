# tabletennis - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tabletennis VERSION 0.1.1 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
elseif(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep the checkout's absolute path (and with it the builder's user name) out
# of __FILE__ strings and debug info. Set before the SDK is added so it covers
# the runtime library too.
add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>"
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=build>")

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
    src/native/tabletennis_6ae_player_renderer.cpp
    src/native/tabletennis_6ae_player_snapshot.cpp
    src/native/tabletennis_6ae_texture_decoder.cpp
    src/native/tabletennis_d47_player_observer.cpp
    src/native/tabletennis_d47_player_snapshot.cpp
    src/native/tabletennis_player_a406_observer.cpp
    src/native/tabletennis_player_a406_snapshot.cpp
    src/native/tabletennis_player_bbb5_observer.cpp
    src/native/tabletennis_player_bbb5_snapshot.cpp
    src/native/tabletennis_frame_scene.cpp
    src/native/tabletennis_fps_telemetry.cpp
    src/native/tabletennis_hud_swf_backend_observer.cpp
    src/native/tabletennis_hud_swf_capture.cpp
    src/native/tabletennis_late_comp_first_slice.cpp
    src/native/tabletennis_late_phase_ledger.cpp
    src/native/tabletennis_logical_draw_replay_packet.cpp
    src/native/tabletennis_ps328_logical_replay_join.cpp
    src/native/tabletennis_phase0_rectangle_replay.cpp
    src/native/tabletennis_phase0_rectangle_readback.cpp
    src/native/tabletennis_translated_shader_artifact_store.cpp
    src/native/tabletennis_transition_state_observer.cpp
    src/native/tabletennis_material_observer.cpp
    src/native/tabletennis_main_coverage_ledger.cpp
    src/native/tabletennis_ps328_tile_invariance.cpp
    src/native/tabletennis_generic_main_private_batch_replay.cpp
    src/native/tabletennis_guarded_venue_private_batch_replay.cpp
    src/native/tabletennis_guarded_venue_capture_retirement.cpp
    src/native/tabletennis_model_material_tokens.cpp
    src/native/tabletennis_mesh_payload_probe.cpp
    src/native/tabletennis_mesh_snapshot.cpp
    src/native/tabletennis_net_bb903_observer.cpp
    src/native/tabletennis_native_scene_compositor.cpp
    src/native/tabletennis_offscreen_target_owner.cpp
    src/native/tabletennis_exact_main_targets.cpp
    src/native/tabletennis_native_scene_pass.cpp
    src/native/tabletennis_native_scene_targets.cpp
    src/native/tabletennis_native_scene_transaction.cpp
    src/native/tabletennis_texture_snapshot.cpp
    src/native/tabletennis_venue_diagnostic_suppression.cpp
    src/native/tabletennis_venue_14d_observer.cpp
    src/native/tabletennis_venue_14d_renderer.cpp
    src/native/tabletennis_venue_14d_snapshot.cpp
    src/native/tabletennis_venue_526a_observer.cpp
    src/native/tabletennis_venue_9e_observer.cpp
    src/native/tabletennis_venue_9e_snapshot.cpp
    src/native/tabletennis_venue_e33_observer.cpp
    src/native/tabletennis_venue_e33_renderer.cpp
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
    src/native/tabletennis_voice_session_guard.cpp
    src/test/tabletennis_frontend_launch_test.cpp
    src/tabletennis_defaults.cpp
    src/tabletennis_iso_installer.cpp
)

if(APPLE)
    list(APPEND TABLETENNIS_SOURCES src/tabletennis_macos.mm)

    # The dock icon is set at runtime from the PNG bytes compiled into the exe.
    set(TABLETENNIS_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/tabletennis_icon.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TABLETENNIS_ICON_PNG}")
    file(READ "${TABLETENNIS_ICON_PNG}" icon_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," icon_bytes "${icon_hex}")
    set(TABLETENNIS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_include")
    file(WRITE "${TABLETENNIS_GENERATED_DIR}/tabletennis_icon_png.h"
        "#pragma once\ninline constexpr unsigned char kTabletennisIconPng[] = {${icon_bytes}};\n")
endif()

if(WIN32)
    set(TABLETENNIS_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/tabletennis_icon.ico")
    set(TABLETENNIS_RC_VERSION
        "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    configure_file(src/tabletennis.rc.in "${CMAKE_CURRENT_BINARY_DIR}/tabletennis.rc" @ONLY)
    add_executable(tabletennis WIN32 ${TABLETENNIS_SOURCES}
        "${CMAKE_CURRENT_BINARY_DIR}/tabletennis.rc")
else()
    add_executable(tabletennis ${TABLETENNIS_SOURCES})
endif()

rexglue_setup_target(tabletennis)

if(APPLE)
    target_include_directories(tabletennis PRIVATE "${TABLETENNIS_GENERATED_DIR}")
    target_link_libraries(tabletennis PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers")
endif()
if(WIN32)
    target_link_libraries(tabletennis PRIVATE comdlg32)
endif()

# GPU frame trace viewer. The SDK contains the shared trace reader/player UI,
# but doesn't expose an executable target for it.
if(REXGLUE_USE_VULKAN)
    add_executable(tabletennis_trace_viewer
        src/trace_viewer_main.cpp
    )
    target_link_libraries(tabletennis_trace_viewer PRIVATE rex::runtime)
    rexglue_configure_target(tabletennis_trace_viewer)
endif()
