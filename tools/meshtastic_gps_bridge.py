#!/usr/bin/env python3
"""Puente local de laboratorio: SenseCAP T1000-E por USB -> portal MINA-LOCAL."""

from __future__ import annotations

import argparse
import json
import threading
import time
import urllib.error
import urllib.request

from pubsub import pub
from meshtastic.serial_interface import SerialInterface


NODE_ID = "!f72ad896"
NODE_NUM = 4146780310
DEFAULT_ENDPOINT = "http://192.168.4.1/api/gps/actualizar"
DEFAULT_TOKEN = "mina-local-rx-2026"


def first(mapping, *keys, default=None):
    if not isinstance(mapping, dict):
        return default
    for key in keys:
        value = mapping.get(key)
        if value is not None:
            return value
    return default


def normalize_position(position):
    if not isinstance(position, dict):
        return None
    latitude = first(position, "latitude", "latitudeDeg")
    longitude = first(position, "longitude", "longitudeDeg")
    if latitude is None:
        latitude_i = first(position, "latitudeI", "latitude_i")
        latitude = None if latitude_i is None else float(latitude_i) / 10_000_000
    if longitude is None:
        longitude_i = first(position, "longitudeI", "longitude_i")
        longitude = None if longitude_i is None else float(longitude_i) / 10_000_000
    timestamp = first(position, "time", "timestamp")
    if latitude is None or longitude is None or timestamp is None:
        return None
    timestamp = int(timestamp)
    if timestamp < 100_000_000_000:
        timestamp *= 1000
    return {
        "latitude": float(latitude),
        "longitude": float(longitude),
        "altitude": float(first(position, "altitude", default=0) or 0),
        "timestamp": timestamp,
        "precision_bits": int(first(position, "precisionBits", "precision_bits", default=32) or 32),
        "source": "meshtastic_usb",
    }


class TrackerBridge:
    def __init__(self, endpoint, token):
        self.endpoint = endpoint
        self.token = token
        self.position = None
        self.battery_level = None
        self.voltage = None
        self.last_sent_timestamp = None
        self.lock = threading.Lock()

    def update_node(self, node):
        if not isinstance(node, dict) or int(node.get("num", NODE_NUM)) != NODE_NUM:
            return
        with self.lock:
            position = normalize_position(node.get("position"))
            if position:
                self.position = position
            metrics = node.get("deviceMetrics") or node.get("device_metrics") or {}
            self.battery_level = first(metrics, "batteryLevel", "battery_level", default=self.battery_level)
            self.voltage = first(metrics, "voltage", default=self.voltage)

    def on_receive(self, packet, interface=None):
        if not isinstance(packet, dict) or int(packet.get("from", 0)) != NODE_NUM:
            return
        decoded = packet.get("decoded") or {}
        port = str(decoded.get("portnum", ""))
        with self.lock:
            if "POSITION" in port:
                position = normalize_position(decoded.get("position"))
                if position:
                    self.position = position
            if "TELEMETRY" in port:
                telemetry = decoded.get("telemetry") or {}
                metrics = telemetry.get("deviceMetrics") or telemetry.get("device_metrics") or {}
                self.battery_level = first(metrics, "batteryLevel", "battery_level", default=self.battery_level)
                self.voltage = first(metrics, "voltage", default=self.voltage)
        self.publish()

    def publish(self, force=False):
        with self.lock:
            if not self.position:
                return False
            timestamp = self.position["timestamp"]
            if not force and timestamp == self.last_sent_timestamp:
                return False
            position = dict(self.position)
            if self.battery_level is not None:
                position["battery_level"] = int(self.battery_level)
            if self.voltage is not None:
                position["voltage"] = float(self.voltage)
            payload = {
                "token": self.token,
                "node_id": NODE_ID,
                "node_num": NODE_NUM,
                "position": position,
            }
        request = urllib.request.Request(
            self.endpoint,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=4) as response:
                response.read()
            with self.lock:
                self.last_sent_timestamp = timestamp
            print(f"GPS enviado: {position['latitude']:.7f}, {position['longitude']:.7f}")
            return True
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            print(f"Portal no disponible ({error}). Conecta el notebook a MINA-LOCAL.")
            return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4", help="Puerto serie del T1000-E")
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT, help="API del Heltec coordinador")
    parser.add_argument("--token", default=DEFAULT_TOKEN, help="Token local del firmware")
    parser.add_argument("--once", action="store_true", help="Envía el punto actual y termina")
    args = parser.parse_args()

    bridge = TrackerBridge(args.endpoint, args.token)
    pub.subscribe(bridge.on_receive, "meshtastic.receive")
    interface = SerialInterface(args.port)
    try:
        time.sleep(2)
        bridge.update_node(interface.getMyNodeInfo())
        bridge.publish(force=True)
        if args.once:
            return
        print("Puente GPS activo. Ctrl+C para terminar.")
        while True:
            time.sleep(15)
            bridge.update_node(interface.getMyNodeInfo())
            bridge.publish()
    except KeyboardInterrupt:
        print("Puente GPS detenido.")
    finally:
        interface.close()


if __name__ == "__main__":
    main()
