# Downloads the site's region GeoJSON, to refresh the copy in data/:
#   cmake -DOUTPUT=data/regions-standard-v1.geojson -P tools/geometry/download.cmake
# The site turns away requests that do not look like a browser.
# Also kRegionsUrl in src/site.hpp.
set(URL "https://map.ukrainealarm.com/data/real-map/regions-standard-v1.geojson")
file(DOWNLOAD "${URL}" "${OUTPUT}.partial" STATUS status TLS_VERIFY ON
    HTTPHEADER "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36")
list(GET status 0 code)
if(NOT code EQUAL 0)
    file(REMOVE "${OUTPUT}.partial")
    list(GET status 1 message)
    message(FATAL_ERROR "cannot download ${URL}: ${message}")
endif()
file(RENAME "${OUTPUT}.partial" "${OUTPUT}")
