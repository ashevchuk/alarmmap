#!/usr/bin/env python3
"""Builds data/cities.geojson from GeoNames.

Keeps populated places of at least 1000 inhabitants that lie inside the
regions of map.ukrainealarm.com, so the selection follows the map rather
than country codes. Names are the Ukrainian ones from the GeoNames
alternate names. The web-map zoom from which a place is shown follows from
its status and population.

GeoNames data is licensed under CC BY 4.0 (https://www.geonames.org).

Usage:
    curl -LO https://download.geonames.org/export/dump/cities1000.zip
    curl -LO https://download.geonames.org/export/dump/alternatenames/UA.zip
    unzip cities1000.zip && unzip UA.zip
    tools/extract_cities.py cities1000.txt UA.txt \
        ~/.cache/alarmmap/regions-standard-v1.geojson data/cities.geojson
"""

import json
import sys

OBLAST_RANK = 1
SOURCE = "GeoNames cities1000 and Ukrainian alternate names, CC BY 4.0"
COORDINATE_DECIMALS = 5
# Section of a populated place and historical place.
SKIPPED_FEATURE_CODES = {"PPLX", "PPLH"}
# Minimum web-map zoom by population, largest first.
ZOOM_BY_POPULATION = [
    (500_000, 5.0),
    (200_000, 6.0),
    (100_000, 6.5),
    (50_000, 7.0),
    (20_000, 8.0),
    (10_000, 8.5),
    (5_000, 9.0),
    (2_000, 9.5),
    (0, 10.0),
]
CAPITAL_ZOOM = 3.0
REGIONAL_CENTRE_ZOOM = 5.0
VILLAGE_POPULATION = 10_000

# cities1000.txt columns
GEONAME_ID, NAME, LATITUDE, LONGITUDE, FEATURE_CODE, POPULATION = 0, 1, 4, 5, 7, 14
# alternate names columns
ALT_GEONAME_ID, ALT_LANGUAGE, ALT_NAME, ALT_PREFERRED, ALT_SHORT, ALT_COLLOQUIAL, ALT_HISTORIC = (
    1, 2, 3, 4, 5, 6, 7)


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


def bounds_of(polygon):
    xs = [point[0] for point in polygon[0]]
    ys = [point[1] for point in polygon[0]]
    return min(xs), min(ys), max(xs), max(ys)


def is_cyrillic(text):
    return any("Ѐ" <= symbol <= "ӿ" for symbol in text)


def flag(columns, index):
    return len(columns) > index and columns[index] == "1"


def ukrainian_names(path):
    """Best Ukrainian name per GeoNames id: preferred, full, Cyrillic first."""
    best = {}
    with open(path, encoding="utf-8") as source:
        for line in source:
            columns = line.rstrip("\n").split("\t")
            if columns[ALT_LANGUAGE] != "uk" or flag(columns, ALT_COLLOQUIAL) or flag(columns, ALT_HISTORIC):
                continue
            name = columns[ALT_NAME]
            rank = (is_cyrillic(name), flag(columns, ALT_PREFERRED), not flag(columns, ALT_SHORT))
            current = best.get(columns[ALT_GEONAME_ID])
            if current is None or rank > current[0]:
                best[columns[ALT_GEONAME_ID]] = (rank, name)
    return {geoname_id: name for geoname_id, (rank, name) in best.items() if rank[0]}


def min_zoom(feature_code, population):
    if feature_code == "PPLC":
        return CAPITAL_ZOOM
    by_population = next(zoom for threshold, zoom in ZOOM_BY_POPULATION if population >= threshold)
    if feature_code == "PPLA":
        return min(REGIONAL_CENTRE_ZOOM, by_population)
    return by_population


def kind_of(feature_code, population):
    if feature_code == "PPLC":
        return "capital"
    if feature_code == "PPLA":
        return "regional"
    return "town" if population >= VILLAGE_POPULATION else "village"


def main(cities_path, names_path, regions_path, output_path):
    with open(regions_path, encoding="utf-8") as source:
        regions = json.load(source)["features"]
    oblasts = [(bounds_of(polygon), polygon) for feature in regions
               if feature["properties"]["rank"] == OBLAST_RANK
               for polygon in polygons_of(feature["geometry"])]
    if not oblasts:
        raise SystemExit("no oblast-level regions in the regions file")
    names = ukrainian_names(names_path)
    if not names:
        raise SystemExit("no Ukrainian names found in the alternate names file")

    features = []
    with open(cities_path, encoding="utf-8") as source:
        for line in source:
            columns = line.rstrip("\n").split("\t")
            if columns[FEATURE_CODE] in SKIPPED_FEATURE_CODES:
                continue
            x, y = float(columns[LONGITUDE]), float(columns[LATITUDE])
            if not any(left <= x <= right and bottom <= y <= top and polygon_contains(polygon, x, y)
                       for (left, bottom, right, top), polygon in oblasts):
                continue
            population = int(columns[POPULATION] or 0)
            features.append({
                "type": "Feature",
                "properties": {
                    "name": names.get(columns[GEONAME_ID], columns[NAME]),
                    "kind": kind_of(columns[FEATURE_CODE], population),
                    "population": population,
                    "minZoom": min_zoom(columns[FEATURE_CODE], population),
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
    untranslated = sum(1 for feature in features if not is_cyrillic(feature["properties"]["name"]))
    print(f"{len(features)} places written, {untranslated} without a Ukrainian name")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("usage: extract_cities.py <cities1000.txt> <UA alternate names.txt> "
                         "<regions.geojson> <output.geojson>")
    main(*sys.argv[1:])
