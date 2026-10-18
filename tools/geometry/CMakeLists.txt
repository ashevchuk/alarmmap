# Turns the site's region GeoJSON into the prepared regions file the app
# loads. It runs on the build machine, so it is its own project: a cross
# build of the app configures it separately for the host.
cmake_minimum_required(VERSION 3.24)
project(alarmmap_geometry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Same rounding as the app, see the top-level CMakeLists.txt.
add_compile_options(-ffp-contract=off)

set(ALARMMAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${ALARMMAP_ROOT}/cmake/dependencies.cmake)

add_executable(alarmmap_geometry
    main.cpp
    ${ALARMMAP_ROOT}/src/projection.cpp
    ${ALARMMAP_ROOT}/src/region_geometry.cpp
    ${ALARMMAP_ROOT}/src/region_store.cpp)
target_include_directories(alarmmap_geometry PRIVATE ${ALARMMAP_ROOT}/src)
target_include_directories(alarmmap_geometry SYSTEM PRIVATE ${earcut_SOURCE_DIR}/include)
target_link_libraries(alarmmap_geometry PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(alarmmap_geometry PRIVATE -Wall -Wextra -Wpedantic)
