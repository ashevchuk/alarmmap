#!/usr/bin/env python3
"""Builds data/cities.geojson from Natural Earth populated places.

Keeps places that lie inside the regions of map.ukrainealarm.com, so the
selection follows the map rather than Natural Earth's country codes (which
put Crimea under another country). Names are the Ukrainian ones where
Natural Earth has them.

Usage:
    curl -L -o ne_places.geojson https://raw.githubusercontent.com/nvkelso/\
natural-earth-vector/v5.1.2/geojson/ne_10m_populated_places.geojson
    tools/extract_cities.py ne_places.geojson \
        ~/.cache/alarmmap/regions-standard-v1.geojson data/cities.geojson
"""

import json
import sys

OBLAST_RANK = 1
SOURCE = "Natural Earth 10m populated places v5.1.2"
KINDS = {
    "Admin-0 capital": "capital",
    "Admin-1 capital": "regional",
}
COORDINATE_DECIMALS = 5


def polygons_of(geometry):
    if geometry["type"] == "Polygon":
        return [geometry["coordinates"]]
    if geometry["type"] == "MultiPolygon":
        return geometry["coordinates"]
    raise ValueError(f"unexpected geometry type {geometry['type']}")


def ring_contains(ring, x, y):
    inside = False
    j = len(ring) - 1
    for i in range(len(ring)):
        (ax, ay), (bx, by) = ring[j][:2], ring[i][:2]
        if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
            inside = not inside
        j = i
    return inside


def polygon_contains(polygon, x, y):
    return ring_contains(polygon[0], x, y) and not any(ring_contains(hole, x, y) for hole in polygon[1:])


def main(places_path, regions_path, output_path):
    with open(regions_path, encoding="utf-8") as source:
        regions = json.load(source)["features"]
    oblasts = [polygon for feature in regions if feature["properties"]["rank"] == OBLAST_RANK
               for polygon in polygons_of(feature["geometry"])]
    if not oblasts:
        raise SystemExit("no oblast-level regions in the regions file")

    with open(places_path, encoding="utf-8") as source:
        places = json.load(source)["features"]

    features = []
    for place in places:
        properties = place["properties"]
        x, y = place["geometry"]["coordinates"][:2]
        if not any(polygon_contains(polygon, x, y) for polygon in oblasts):
            continue
        features.append({
            "type": "Feature",
            "properties": {
                "name": properties.get("NAME_UK") or properties["NAME"],
                "kind": KINDS.get(properties["FEATURECLA"], "town"),
                "population": properties["POP_MAX"],
                "minZoom": properties["MIN_ZOOM"],
            },
            "geometry": {"type": "Point",
                         "coordinates": [round(x, COORDINATE_DECIMALS), round(y, COORDINATE_DECIMALS)]},
        })
    if not features:
        raise SystemExit("no places found inside the regions")
    features.sort(key=lambda feature: -feature["properties"]["population"])

    with open(output_path, "w", encoding="utf-8") as output:
        json.dump({"type": "FeatureCollection", "source": SOURCE, "features": features}, output,
                  ensure_ascii=False, separators=(",", ":"))
        output.write("\n")
    print(f"{len(features)} places written")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: extract_cities.py <ne_places.geojson> <regions.geojson> <output.geojson>")
    main(sys.argv[1], sys.argv[2], sys.argv[3])
