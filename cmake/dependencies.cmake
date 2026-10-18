# Libraries fetched at configure time, shared by the app and the geometry tool.
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
