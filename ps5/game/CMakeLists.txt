# SPDX-License-Identifier: GPL-3.0-or-later
# Included by the isolated PS5 SDK build. The desktop project stays independent.
file(GLOB guest_sources CONFIGURE_DEPENDS "${REXGLUE_PS5_GAME_SOURCE}/generated/default/*.cpp")
file(GLOB native_sources CONFIGURE_DEPENDS "${REXGLUE_PS5_GAME_SOURCE}/src/native/*.cpp")
# OFF builds a perf A/B variant: no guest-function overrides from the native
# renderer work and no per-draw observers. Bug-fix overrides stay linked.
option(TABLETENNIS_PS5_NATIVE_HOOKS "Link native gameplay hooks and install native observers" ON)
if(NOT TABLETENNIS_PS5_NATIVE_HOOKS)
    list(FILTER native_sources EXCLUDE REGEX "/tabletennis_native_hooks\\.cpp$")
endif()
add_library(tabletennis-ps5 STATIC
    ${guest_sources}
    ${native_sources}
    "${REXGLUE_PS5_GAME_SOURCE}/src/tabletennis_defaults.cpp"
    "${REXGLUE_PS5_GAME_SOURCE}/src/test/tabletennis_frontend_launch_test.cpp"
    main.cpp
)
target_include_directories(tabletennis-ps5 PRIVATE
    "${REXGLUE_PS5_GAME_SOURCE}"
    "${REXGLUE_PS5_GAME_SOURCE}/src"
    "${REXGLUE_PS5_GAME_SOURCE}/generated/default"
)
target_link_libraries(tabletennis-ps5 PRIVATE rex::runtime)
if(NOT TABLETENNIS_PS5_NATIVE_HOOKS)
    target_compile_definitions(tabletennis-ps5 PRIVATE TABLETENNIS_PS5_NO_NATIVE_HOOKS=1)
endif()
target_compile_options(tabletennis-ps5 PRIVATE
    -mcmodel=large -fno-strict-aliasing -ffp-contract=off -g0
)
target_precompile_headers(tabletennis-ps5 PRIVATE
    "${REXGLUE_PS5_GAME_SOURCE}/generated/default/tabletennis_init.h")
