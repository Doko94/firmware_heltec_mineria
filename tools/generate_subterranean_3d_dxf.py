"""Genera el DXF 3D conceptual usado por el visor subterraneo local."""

from __future__ import annotations

import json
import math
import unicodedata
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAYOUT_PATH = ROOT / "data" / "layout_mina.json"
OUTPUT_PATH = ROOT / "data" / "static" / "mina_niveles.dxf"

LAYER_COLORS = {
    "SUPERFICIE": 7,
    "PLANOS_NIVEL": 8,
    "MACROBLOQUES": 94,
    "GALERIA_PRODUCCION": 3,
    "GALERIA_RETORNO": 4,
    "GALERIA_SERVICIO": 3,
    "GALERIA_PERIMETRAL": 8,
    "CAMARA_MANIOBRAS": 2,
    "CRUCERO": 9,
    "CALLE_HUNDIMIENTO": 33,
    "RAMPA_ACCESO": 5,
    "RAMPA_CARACOL": 5,
    "ENLACE_RAMPA": 5,
    "GALERIA_SECUNDARIA": 8,
    "GALERIA_VENTILACION": 4,
    "VIA_EVACUACION": 3,
    "DRENAJE": 5,
    "PIQUE_PRINCIPAL": 7,
    "VENTILACION_INYECCION": 4,
    "VENTILACION_EXTRACCION": 30,
    "CHIMENEA_ESCAPE": 7,
    "REFUGIO": 3,
    "TALLER": 5,
    "SUBESTACION": 2,
    "CHANCADO": 2,
    "BOMBEO": 4,
    "TRASPASO_MINERAL": 6,
    "NODOS_RX": 2,
    "BEACONS_REF": 1,
    "TEXTOS_COTAS": 7,
}


def ascii_text(value: object) -> str:
    normalized = unicodedata.normalize("NFKD", str(value))
    return normalized.encode("ascii", "ignore").decode("ascii")


class Dxf:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def pair(self, code: int, value: object) -> None:
        self.lines.extend((str(code), str(value)))

    def section(self, name: str) -> None:
        self.pair(0, "SECTION")
        self.pair(2, name)

    def end_section(self) -> None:
        self.pair(0, "ENDSEC")

    def line(self, layer: str, a: list[float], b: list[float]) -> None:
        self.pair(0, "LINE")
        self.pair(8, layer)
        for code, value in zip((10, 20, 30), a):
            self.pair(code, value)
        for code, value in zip((11, 21, 31), b):
            self.pair(code, value)

    def face(self, layer: str, points: list[list[float]]) -> None:
        vertices = (points + [points[-1]] * 4)[:4]
        self.pair(0, "3DFACE")
        self.pair(8, layer)
        for index, point in enumerate(vertices):
            base = 10 + index
            self.pair(base, point[0])
            self.pair(base + 10, point[1])
            self.pair(base + 20, point[2])

    def text(self, layer: str, point: list[float], value: object, height: float = 10) -> None:
        self.pair(0, "TEXT")
        self.pair(8, layer)
        self.pair(10, point[0])
        self.pair(20, point[1])
        self.pair(30, point[2])
        self.pair(40, height)
        self.pair(1, ascii_text(value))


def cuboid_faces(block: dict) -> list[list[list[float]]]:
    x, y = float(block["x"]), float(block["y"])
    hx, hy = float(block["ancho"]) / 2, float(block["fondo"]) / 2
    top, bottom = float(block["z_superior"]), float(block["z_inferior"])
    t = [[x-hx,y-hy,top], [x+hx,y-hy,top], [x+hx,y+hy,top], [x-hx,y+hy,top]]
    b = [[x-hx,y-hy,bottom], [x+hx,y-hy,bottom], [x+hx,y+hy,bottom], [x-hx,y+hy,bottom]]
    return [t, b, [t[0],t[1],b[1],b[0]], [t[1],t[2],b[2],b[1]],
            [t[2],t[3],b[3],b[2]], [t[3],t[0],b[0],b[3]]]


def add_cross(dxf: Dxf, layer: str, point: list[float], size: float) -> None:
    x, y, z = point
    dxf.line(layer, [x-size, y, z], [x+size, y, z])
    dxf.line(layer, [x, y-size, z], [x, y+size, z])
    dxf.line(layer, [x, y, z-size], [x, y, z+size])


def helical_segments(layout: dict):
    """Convierte rampas helicoidales compactas en tramos LINE 3D compatibles con R12."""
    for ramp in layout.get("rampas_helicoidales", []):
        center_x, center_y = (float(value) for value in ramp.get("centro", [0, 0]))
        radius = max(1.0, float(ramp.get("radio_m", 1)))
        start_angle = math.radians(float(ramp.get("angulo_inicial_deg", 0)))
        turns = float(ramp.get("vueltas", 1))
        start_z = float(ramp.get("z_inicio", 0))
        end_z = float(ramp.get("z_fin", 0))
        steps = max(16, min(120, int(ramp.get("resolucion", 64))))

        def point_at(index: int) -> list[float]:
            progress = index / steps
            angle = start_angle + turns * math.tau * progress
            return [
                center_x + math.cos(angle) * radius,
                center_y + math.sin(angle) * radius,
                start_z + (end_z - start_z) * progress,
            ]

        for index in range(steps):
            yield ramp.get("capa", "RAMPA_CARACOL"), point_at(index), point_at(index + 1)


def generate() -> None:
    layout = json.loads(LAYOUT_PATH.read_text(encoding="utf-8"))
    dxf = Dxf()

    dxf.section("HEADER")
    dxf.pair(9, "$ACADVER")
    dxf.pair(1, "AC1009")
    dxf.end_section()

    dxf.section("TABLES")
    dxf.pair(0, "TABLE")
    dxf.pair(2, "LAYER")
    dxf.pair(70, len(LAYER_COLORS))
    for name, color in LAYER_COLORS.items():
        dxf.pair(0, "LAYER")
        dxf.pair(2, name)
        dxf.pair(70, 0)
        dxf.pair(62, color)
        dxf.pair(6, "CONTINUOUS")
    dxf.pair(0, "ENDTAB")
    dxf.end_section()

    dxf.section("ENTITIES")
    for plane in layout.get("planos_nivel", []):
        dxf.face("PLANOS_NIVEL", plane["puntos"])
        dxf.text("TEXTOS_COTAS", plane["puntos"][0], plane["nombre"], 12)

    for block in layout.get("macrobloques", []):
        for face in cuboid_faces(block):
            dxf.face("MACROBLOQUES", face)
        dxf.text(
            "TEXTOS_COTAS",
            [block["x"], block["y"], block["z_superior"] + 6],
            f'{block["id"]} {block["nivel"]}',
            8,
        )

    for segment in layout.get("segmentos", []):
        dxf.line(segment["capa"], segment["a"], segment["b"])

    for layer, start, end in helical_segments(layout):
        dxf.line(layer, start, end)

    for label in layout.get("etiquetas", []):
        dxf.text("TEXTOS_COTAS", [label["x"], label["y"], label["z"]], label["id"], 9)

    for reader_id, zone in layout.get("zonas_operativas", {}).items():
        points = zone.get("puntos", [])
        if not points:
            continue
        center = [sum(float(p[i]) for p in points) / len(points) for i in range(3)]
        add_cross(dxf, "NODOS_RX", center, 16)
        dxf.text("NODOS_RX", [center[0] + 20, center[1], center[2] + 8],
                 f'{reader_id} COTA -{zone["profundidad_m"]} m', 11)
        for index, point in enumerate(points, start=1):
            add_cross(dxf, "BEACONS_REF", point, 5)
            dxf.text("BEACONS_REF", [point[0] + 7, point[1], point[2] + 3],
                     f"TAG REF {reader_id}-{index:02d}", 6)

    notes = [
        "MODELO 3D CONCEPTUAL DE MINA SUBTERRANEA - NO USAR PARA INGENIERIA",
        "PROFUNDIDAD DEL BEACON INFERIDA POR COTA DEL READER BLE QUE CONFIRMA LA SENAL",
        "RX02 REPRESENTADO COMO NIVEL LOGICO; EL EQUIPO ACTUAL OPERA COMO GATEWAY MESHTASTIC",
    ]
    for index, note in enumerate(notes):
        dxf.text("TEXTOS_COTAS", [20, -260, 25 - index * 18], note, 11)

    dxf.end_section()
    dxf.pair(0, "EOF")
    OUTPUT_PATH.write_text("\n".join(dxf.lines) + "\n", encoding="ascii")
    print(f"DXF 3D generado: {OUTPUT_PATH} ({len(dxf.lines) // 2} pares)")


if __name__ == "__main__":
    generate()
