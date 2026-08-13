#!/usr/bin/env python3
"""Genera una cartografia vectorial compacta de Iquique desde OpenStreetMap."""

from __future__ import annotations

import json
import math
import pathlib
import urllib.parse
import urllib.request


BOUNDS = (-20.3100, -70.1600, -20.2050, -70.1050)  # sur, oeste, norte, este
OVERPASS_URL = "https://overpass-api.de/api/interpreter"
OUTPUT = pathlib.Path("data/static/iquique_map.json")


def distance_to_line(point, start, end):
    px, py = point
    ax, ay = start
    bx, by = end
    dx, dy = bx - ax, by - ay
    if dx == 0 and dy == 0:
        return math.hypot(px - ax, py - ay)
    factor = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + factor * dx), py - (ay + factor * dy))


def simplify(points, tolerance):
    if len(points) <= 2:
        return points
    first, last = points[0], points[-1]
    max_distance, split = 0.0, 0
    for index, point in enumerate(points[1:-1], 1):
        distance = distance_to_line(point, first, last)
        if distance > max_distance:
            max_distance, split = distance, index
    if max_distance <= tolerance:
        return [first, last]
    return simplify(points[: split + 1], tolerance)[:-1] + simplify(points[split:], tolerance)


def local_points(geometry):
    center_lat = (BOUNDS[0] + BOUNDS[2]) / 2
    longitude_factor = 111_320 * math.cos(math.radians(center_lat))
    return [
        ((float(item["lon"]) - BOUNDS[1]) * longitude_factor,
         (float(item["lat"]) - BOUNDS[0]) * 111_320)
        for item in geometry
        if "lat" in item and "lon" in item
    ]


def geographic_points(points):
    center_lat = (BOUNDS[0] + BOUNDS[2]) / 2
    longitude_factor = 111_320 * math.cos(math.radians(center_lat))
    return [
        [round(BOUNDS[1] + x / longitude_factor, 6), round(BOUNDS[0] + y / 111_320, 6)]
        for x, y in points
    ]


def main():
    south, west, north, east = BOUNDS
    bbox = f"{south},{west},{north},{east}"
    query = f"""
[out:json][timeout:90][bbox:{bbox}];
(
  way[\"highway\"~\"motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street\"];
  way[\"natural\"=\"coastline\"];
  way[\"leisure\"~\"park|sports_centre|pitch\"];
  way[\"landuse\"~\"industrial|commercial|recreation_ground\"];
  node[\"place\"~\"city|suburb|quarter|neighbourhood\"];
  node[\"amenity\"~\"hospital|university|police|fire_station|bus_station\"];
);
out tags geom;
""".strip()
    request = urllib.request.Request(
        OVERPASS_URL,
        data=urllib.parse.urlencode({"data": query}).encode("utf-8"),
        headers={"User-Agent": "MINA-LOCAL-offline-map/1.0 (local Heltec prototype)"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        document = json.load(response)

    roads, areas, coast, labels = [], [], [], []
    major = {"motorway", "trunk", "primary", "secondary", "tertiary"}
    for element in document.get("elements", []):
        tags = element.get("tags") or {}
        if element.get("type") == "node":
            name = tags.get("name")
            if not name:
                continue
            labels.append([
                tags.get("place") or tags.get("amenity") or "punto",
                name,
                round(float(element["lon"]), 6),
                round(float(element["lat"]), 6),
            ])
            continue
        geometry = element.get("geometry") or []
        points = local_points(geometry)
        if len(points) < 2:
            continue
        highway = tags.get("highway")
        if highway:
            tolerance = 2.5 if highway in major else 6.0
            compact = geographic_points(simplify(points, tolerance))
            roads.append([highway, tags.get("name", ""), compact])
        elif tags.get("natural") == "coastline":
            coast.append(geographic_points(simplify(points, 3.0)))
        else:
            compact = geographic_points(simplify(points, 7.0))
            if len(compact) >= 3:
                areas.append([
                    tags.get("leisure") or tags.get("landuse") or "area",
                    tags.get("name", ""),
                    compact,
                ])

    result = {
        "version": 1,
        "name": "Iquique urbano",
        "bounds": [south, west, north, east],
        "attribution": "© OpenStreetMap contributors · ODbL",
        "license_url": "https://www.openstreetmap.org/copyright",
        "roads": roads,
        "areas": areas,
        "coast": coast,
        "labels": labels,
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print(f"{OUTPUT}: {OUTPUT.stat().st_size} bytes, {len(roads)} calles, {len(labels)} etiquetas")


if __name__ == "__main__":
    main()
