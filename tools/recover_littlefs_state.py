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


def history_markers(text: str):
    start = 0
    while True:
        start = text.find('{"version":1,"eventos":', start)
        if start < 0:
            return
        yield start
        start += 1


def salvage_history(text: str):
    """Recupera el mayor documento aunque una copia tenga bytes reciclados.

    Si raw_decode no alcanza el cierre del documento, extrae objetos de la
    matriz de eventos y conserva los contadores si siguen disponibles.
    """
    decoder = json.JSONDecoder()
    candidates = []
    for start in history_markers(text):
        try:
            value, _ = decoder.raw_decode(text[start:])
            if isinstance(value, dict) and isinstance(value.get("eventos"), list):
                candidates.append(value)
                continue
        except json.JSONDecodeError:
            pass
        events_start = text.find("[", start)
        if events_start < 0:
            continue
        cursor = events_start + 1
        events = []
        while cursor < len(text):
            while cursor < len(text) and text[cursor] in " \r\n\t,":
                cursor += 1
            if cursor >= len(text) or text[cursor] != "{":
                break
            try:
                item, consumed = decoder.raw_decode(text[cursor:])
            except json.JSONDecodeError:
                break
            if not isinstance(item, dict) or "beacon_id" not in item:
                break
            events.append(item)
            cursor += consumed
        if events:
            candidates.append({"version": 1, "eventos": events, "contadores": []})
    if not candidates:
        return None
    return max(
        candidates,
        key=lambda value: (
            max((int(item.get("fecha") or 0) for item in value["eventos"]), default=0),
            len(value["eventos"]),
        ),
    )


def latest_history(text: str):
    candidates = []
    for offset, value in json_candidates(text, '{"version":1,"eventos":'):
        if not isinstance(value, dict) or not isinstance(value.get("eventos"), list):
            continue
        latest = max((int(item.get("fecha") or 0) for item in value["eventos"]), default=0)
        candidates.append(((latest, offset), value))
    valid = max(candidates, default=(None, None), key=lambda item: item[0] or (0, 0))[1]
    return valid or salvage_history(text)


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


def latest_mesh_messages(text: str):
    candidates = []
    for offset, value in json_candidates(text, '{"version":1,"mensajes":'):
        if not isinstance(value, dict) or not isinstance(value.get("mensajes"), list):
            continue
        messages = value["mensajes"]
        latest = max(
            [int(item.get("actualizado") or item.get("fecha") or 0) for item in messages]
            or [0]
        )
        candidates.append(((latest, len(messages), offset), value))
    return max(
        candidates,
        default=(None, None),
        key=lambda item: item[0] or (0, 0, 0),
    )[1]


def latest_supervisor_messages(text: str):
    """Recupera la matriz raíz de mensajes operacionales del portal."""
    candidates = []
    for offset, value in json_candidates(text, '[{"id":"'):
        if not isinstance(value, list):
            continue
        if value and not all(
            isinstance(item, dict)
            and {"id", "destino", "nivel", "titulo", "mensaje", "vigente"}.issubset(item)
            for item in value
        ):
            continue
        latest = max(
            [int(item.get("confirmado_fecha") or item.get("fecha") or 0) for item in value]
            or [0]
        )
        candidates.append(((latest, len(value), offset), value))
    return max(
        candidates,
        default=(None, None),
        key=lambda item: item[0] or (0, 0, 0),
    )[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    text = args.image.read_bytes().decode("utf-8", errors="ignore")
    recovered = {
        "mensajes.json": latest_supervisor_messages(text),
        "historial.json": latest_history(text),
        "gps_tracker.json": latest_gps(text),
        "mensajes_mesh.json": latest_mesh_messages(text),
    }
    for name, value in recovered.items():
        if value is None:
            print(f"{name}: no recuperado")
            continue
        if name == "mensajes.json":
            items = value
            latest = max(
                [int(item.get("confirmado_fecha") or item.get("fecha") or 0) for item in items]
                or [0]
            )
        elif name == "historial.json":
            items = value.get("eventos", [])
            latest = max((int(item.get("fecha") or 0) for item in items), default=0)
        elif name == "gps_tracker.json":
            items = value.get("history", [])
            latest = int((value.get("position") or {}).get("timestamp") or 0)
        else:
            items = value.get("mensajes", [])
            latest = max(
                [int(item.get("actualizado") or item.get("fecha") or 0) for item in items]
                or [0]
            )
        print(f"{name}: {len(items)} registros, ultima marca {latest}")
        if args.output:
            args.output.mkdir(parents=True, exist_ok=True)
            (args.output / name).write_text(
                json.dumps(value, ensure_ascii=False, separators=(",", ":")),
                encoding="utf-8",
            )


if __name__ == "__main__":
    main()
