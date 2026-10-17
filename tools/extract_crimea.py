#!/usr/bin/env python3
"""Builds data/crimea.geojson from Natural Earth admin-1 boundaries.

The regions GeoJSON of map.ukrainealarm.com draws Crimea along its 12-mile
territorial sea limit, so the peninsula looks bloated. Natural Earth has the
land outline. Crimea (UA-43) and Sevastopol (UA-40) are combined into one
feature under the site's region ID for Crimea.

Usage:
    curl -L -o ne_admin1.geojson https://raw.githubusercontent.com/nvkelso/\
natural-earth-vector/v5.1.2/geojson/ne_10m_admin_1_states_provinces.geojson
    tools/extract_crimea.py ne_admin1.geojson data/crimea.geojson
"""

import json
import sys

SITE_REGION_ID = "9999"
ISO_CODES = ("UA-43", "UA-40")
SOURCE = "Natural Earth 10m admin-1 v5.1.2, UA-43 and UA-40"
COORDINATE_DECIMALS = 5


def polygons_of(geometry):
    if geometry["type"] == "Polygon":
        return [geometry["coordinates"]]
    if geometry["type"] == "MultiPolygon":
        return geometry["coordinates"]
    raise ValueError(f"unexpected geometry type {geometry['type']}")


def rounded(polygon):
    return [[[round(x, COORDINATE_DECIMALS), round(y, COORDINATE_DECIMALS)] for x, y in ring]
            for ring in polygon]


def main(input_path, output_path):
    with open(input_path, encoding="utf-8") as source:
        features = json.load(source)["features"]

    polygons = []
    for code in ISO_CODES:
        matches = [f for f in features if f["properties"].get("iso_3166_2") == code]
        if len(matches) != 1:
            raise SystemExit(f"expected one feature with iso_3166_2={code}, found {len(matches)}")
        polygons.extend(rounded(polygon) for polygon in polygons_of(matches[0]["geometry"]))

    collection = {
        "type": "FeatureCollection",
        "features": [{
            "type": "Feature",
            "properties": {"regionId": SITE_REGION_ID, "source": SOURCE},
            "geometry": {"type": "MultiPolygon", "coordinates": polygons},
        }],
    }
    with open(output_path, "w", encoding="utf-8") as output:
        json.dump(collection, output, separators=(",", ":"))
        output.write("\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: extract_crimea.py <ne_admin1.geojson> <output.geojson>")
    main(sys.argv[1], sys.argv[2])
