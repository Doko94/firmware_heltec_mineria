"""Recupera los JSON mutables desde un volcado LittleFS del Heltec.

LittleFS conserva versiones antiguas por desgaste uniforme. Este script busca
todos los documentos JSON validos presentes en el volcado y selecciona el mas
reciente segun las marcas de tiempo de cada documento.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def json_candidates(text: str, marker: str):
    decoder = json.JSONDecoder()
    offset = 0
    while True:
        offset = text.find(marker, offset)
        if offset < 0:
            return
        try:
            value, _ = decoder.raw_decode(text[offset:])
            yield offset, value
        except (json.JSONDecodeError, UnicodeError):
            pass
        offset += 1


def latest_history(text: str):
    candidates = []
    for offset, value in json_candidates(text, '{"version":1,"eventos":'):
        if not isinstance(value, dict) or not isinstance(value.get("eventos"), list):
            continue
        latest = max((int(item.get("fecha") or 0) for item in value["eventos"]), default=0)
        candidates.append(((latest, offset), value))
    return max(candidates, default=(None, None), key=lambda item: item[0] or (0, 0))[1]


def latest_gps(text: str):
    candidates = []
    for offset, value in json_candidates(text, '{"version":1,"node_id":'):
        if not isinstance(value, dict) or not isinstance(value.get("history"), list):
            continue
        latest = max(
            [int((value.get("position") or {}).get("timestamp") or 0)]
            + [int(item.get("timestamp") or 0) for item in value["history"]]
        )
        candidates.append(((latest, offset), value))
    return max(candidates, default=(None, None), key=lambda item: item[0] or (0, 0))[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    text = args.image.read_bytes().decode("utf-8", errors="ignore")
    recovered = {
        "historial.json": latest_history(text),
        "gps_tracker.json": latest_gps(text),
    }
    for name, value in recovered.items():
        if value is None:
            print(f"{name}: no recuperado")
            continue
        if name == "historial.json":
            items = value.get("eventos", [])
            latest = max((int(item.get("fecha") or 0) for item in items), default=0)
        else:
            items = value.get("history", [])
            latest = int((value.get("position") or {}).get("timestamp") or 0)
        print(f"{name}: {len(items)} registros, ultima marca {latest}")
        if args.output:
            args.output.mkdir(parents=True, exist_ok=True)
            (args.output / name).write_text(
                json.dumps(value, ensure_ascii=False, separators=(",", ":")),
                encoding="utf-8",
            )


if __name__ == "__main__":
    main()
