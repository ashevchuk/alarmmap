cmake_minimum_required(VERSION 3.24)
project(alarmmap VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# Header-only; SOURCE_SUBDIR points nowhere so its tests and demos are not built.
FetchContent_Declare(earcut
    GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
    GIT_TAG v2.2.4
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(nlohmann_json earcut)

if(ANDROID)
    # Network access is not ported yet; the app shows the data its APK carries.
    set(NETWORK_SOURCES src/offline_network.cpp)
else()
    # WebSocket support in libcurl is stable since 8.11.
    find_package(CURL 8.11 REQUIRED)
    set(NETWORK_SOURCES src/http.cpp src/live_feed.cpp)
endif()
find_package(raylib 5.5 REQUIRED)
find_package(Threads REQUIRED)

set(WARNING_FLAGS -Wall -Wextra -Wpedantic)

add_library(alarmmap_core STATIC
    src/base64url.cpp
    src/basemap.cpp
    src/cities.cpp
    src/heat_cells.cpp
    src/heat_timing.cpp
    ${NETWORK_SOURCES}
    src/map_update.cpp
    src/map_view.cpp
    src/marker_layout.cpp
    src/projection.cpp
    src/region_geometry.cpp
    src/target_kind.cpp
    src/target_motion.cpp
    src/target_snapshot.cpp
    src/tile_store.cpp
    src/vector_tile.cpp)
target_include_directories(alarmmap_core PUBLIC src)
target_include_directories(alarmmap_core SYSTEM PRIVATE ${earcut_SOURCE_DIR}/include)
target_link_libraries(alarmmap_core PUBLIC nlohmann_json::nlohmann_json)
if(NOT ANDROID)
    target_link_libraries(alarmmap_core PUBLIC CURL::libcurl)
endif()
target_compile_options(alarmmap_core PRIVATE ${WARNING_FLAGS})

set(APP_SOURCES
    app/main.cpp
    app/app_icon.cpp
    app/gaussian_blur.cpp
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

add_executable(ws_probe tools/ws_probe.cpp)
target_link_libraries(ws_probe PRIVATE alarmmap_core)
target_compile_options(ws_probe PRIVATE ${WARNING_FLAGS})

enable_testing()
foreach(test_name target_snapshot geometry heat basemap)
    add_executable(${test_name}_test tests/${test_name}_test.cpp)
    target_link_libraries(${test_name}_test PRIVATE alarmmap_core)
    target_compile_options(${test_name}_test PRIVATE ${WARNING_FLAGS})
    add_test(NAME ${test_name}
        COMMAND ${test_name}_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
endforeach()

include(GNUInstallDirs)
install(TARGETS alarmmap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/cities.geojson data/crimea.geojson DESTINATION ${CMAKE_INSTALL_DATADIR}/alarmmap)
install(FILES packaging/alarmmap.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/alarmmap.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
