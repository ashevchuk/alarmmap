cmake_minimum_required(VERSION 3.24)
project(alarmmap VERSION 0.2.10 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fused multiply-add rounds differently from a separate multiply and add,
# which makes earcut's geometric tests inconsistent: on ARM, where compilers
# fuse by default, it looped forever on some region outlines.
add_compile_options(-ffp-contract=off)

include(cmake/dependencies.cmake)

# WebSocket support in libcurl is stable since 8.11.
find_package(CURL 8.11 REQUIRED)
find_package(raylib 5.5 REQUIRED)
find_package(Threads REQUIRED)

set(WARNING_FLAGS -Wall -Wextra -Wpedantic)

add_library(alarmmap_core STATIC
    src/base64url.cpp
    src/basemap.cpp
    src/cities.cpp
    src/heat_cells.cpp
    src/heat_timing.cpp
    src/http.cpp
    src/live_feed.cpp
    src/map_update.cpp
    src/map_view.cpp
    src/marker_layout.cpp
    src/mesh_builder.cpp
    src/packed_packet.cpp
    src/projection.cpp
    src/region_geometry.cpp
    src/region_store.cpp
    src/target_kind.cpp
    src/target_motion.cpp
    src/target_snapshot.cpp
    src/tile_store.cpp
    src/update_check.cpp
    src/vector_tile.cpp)
target_include_directories(alarmmap_core PUBLIC src)
target_include_directories(alarmmap_core SYSTEM PRIVATE ${earcut_SOURCE_DIR}/include)
target_link_libraries(alarmmap_core PUBLIC nlohmann_json::nlohmann_json CURL::libcurl)
target_compile_options(alarmmap_core PRIVATE ${WARNING_FLAGS})

set(APP_SOURCES
    app/main.cpp
    app/app_icon.cpp
    app/gaussian_blur.cpp
    app/gpu_geometry.cpp
    app/heat_renderer.cpp
    app/map_layer.cpp
    app/render_util.cpp
    app/snout_icon.cpp
    app/target_effects.cpp
    app/ui_font.cpp)
if(ANDROID)
    # NativeActivity loads the program as libalarmmap.so and enters it
    # through raylib's android_main.
    add_library(alarmmap SHARED ${APP_SOURCES})
    target_include_directories(alarmmap PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(alarmmap PRIVATE android log GLESv3)
    target_link_options(alarmmap PRIVATE -u ANativeActivity_onCreate)
else()
    add_executable(alarmmap ${APP_SOURCES})
endif()
target_link_libraries(alarmmap PRIVATE alarmmap_core raylib Threads::Threads)
target_compile_definitions(alarmmap PRIVATE
    ALARMMAP_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ALARMMAP_VERSION="${PROJECT_VERSION}")
target_compile_options(alarmmap PRIVATE ${WARNING_FLAGS})

if(ANDROID)
    return()
endif()

# tools/geometry prepares the site's region geometry, kept in data/, into the
# file the app loads; android/build_apk.sh does the same for the APK.
set(SITE_REGIONS ${CMAKE_CURRENT_SOURCE_DIR}/data/regions-standard-v1.geojson)
set(PREPARED_REGIONS ${CMAKE_BINARY_DIR}/regions.bin)
add_subdirectory(tools/geometry)
add_custom_command(OUTPUT ${PREPARED_REGIONS}
    COMMAND alarmmap_geometry ${SITE_REGIONS} ${CMAKE_CURRENT_SOURCE_DIR}/data/crimea.geojson ${PREPARED_REGIONS}
    DEPENDS alarmmap_geometry ${SITE_REGIONS} ${CMAKE_CURRENT_SOURCE_DIR}/data/crimea.geojson
    COMMENT "Preparing region geometry")
add_custom_target(prepared_regions ALL DEPENDS ${PREPARED_REGIONS})
add_dependencies(alarmmap prepared_regions)
target_compile_definitions(alarmmap PRIVATE ALARMMAP_PREPARED_REGIONS="${PREPARED_REGIONS}")

add_executable(ws_probe tools/ws_probe.cpp)
target_link_libraries(ws_probe PRIVATE alarmmap_core)
target_compile_options(ws_probe PRIVATE ${WARNING_FLAGS})

add_executable(api_check tools/api_check.cpp)
target_link_libraries(api_check PRIVATE alarmmap_core)
target_compile_definitions(api_check PRIVATE ALARMMAP_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(api_check PRIVATE ${WARNING_FLAGS})

enable_testing()
foreach(test_name target_snapshot geometry heat basemap mesh update_check)
    add_executable(${test_name}_test tests/${test_name}_test.cpp)
    target_link_libraries(${test_name}_test PRIVATE alarmmap_core)
    target_compile_options(${test_name}_test PRIVATE ${WARNING_FLAGS})
    add_test(NAME ${test_name}
        COMMAND ${test_name}_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
endforeach()

include(GNUInstallDirs)
install(TARGETS alarmmap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/cities.geojson ${PREPARED_REGIONS} DESTINATION ${CMAKE_INSTALL_DATADIR}/alarmmap)
install(FILES packaging/alarmmap.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/alarmmap.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
