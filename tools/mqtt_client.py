"""BRIQON MQTT test client. Targets a fresh device boot/session automatically."""
import argparse
import json
import os
import threading
import time
import uuid

import paho.mqtt.client as mqtt


def build_command(snapshot, action, params, command_id=None):
    return {
        "schema_version": 1,
        "device_id": snapshot["device_id"],
        "target_boot_id": snapshot["boot_id"],
        "target_session_id": snapshot["session_id"],
        "command_id": command_id or f"cmd-{uuid.uuid4().hex}",
        "expires_at_uptime_ms": int(snapshot["uptime_ms"]) + 15000,
        "action": action,
        "params": params,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--broker", default="broker.emqx.io")
    p.add_argument("--port", type=int, default=None)
    p.add_argument("--tls", action="store_true")
    p.add_argument("--environment", default="demo")
    p.add_argument("--device", help="Copy device= from ESP serial log; use a unique ID per simulator")
    p.add_argument("--seconds", type=float, default=30, help="Watch duration / command timeout")
    p.add_argument("action", choices=["discover", "watch", "on", "off", "status", "config"])
    p.add_argument("--values", default="{}", help='Config patch JSON, e.g. {"telemetry_interval_ms":3000}')
    args = p.parse_args()
    if args.action != "discover" and not args.device:
        p.error("--device diperlukan; lihat device= pada Serial Monitor")
    if args.device and any(c in args.device for c in "/+#"):
        p.error("device ID tidak boleh berisi / + #")
    namespace = f"briqon/v1/{args.environment}"
    base = f"{namespace}/{args.device}" if args.device else namespace
    fresh = threading.Event()
    done = threading.Event()
    connected = threading.Event()
    shared = {"snapshot": None, "command_id": None, "terminal": None, "connection_error": None}
    lock = threading.Lock()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"briqon-test-{uuid.uuid4().hex[:16]}", clean_session=True)
    if os.getenv("BRIQON_MQTT_USERNAME"):
        client.username_pw_set(os.environ["BRIQON_MQTT_USERNAME"], os.getenv("BRIQON_MQTT_PASSWORD"))
    if args.tls:
        client.tls_set()  # OS trust store + hostname validation; no insecure fallback

    def on_connect(c, _userdata, _flags, reason_code, _properties):
        if reason_code.is_failure:
            shared["connection_error"] = str(reason_code)
            done.set()
            return
        c.subscribe(f"{namespace}/+/state" if args.action == "discover" else f"{base}/#", qos=1)
        connected.set()

    def on_message(_c, _userdata, message):
        try:
            data = json.loads(message.payload)
        except (ValueError, UnicodeDecodeError):
            print(message.topic, "[invalid JSON]", flush=True)
            return
        if args.action in ("watch", "discover"):
            print(json.dumps({"topic": message.topic, "retained": message.retain,
                              "payload": data}, ensure_ascii=False), flush=True)
        if (message.topic in (f"{base}/heartbeat", f"{base}/state") and
                not message.retain and data.get("ready", data.get("online", False)) and
                all(k in data for k in ("boot_id", "session_id", "uptime_ms"))):
            with lock:
                shared["snapshot"] = data
            fresh.set()
        if (message.topic == f"{base}/command-results" and
                data.get("command_id") == shared["command_id"]):
            print(json.dumps(data, ensure_ascii=False), flush=True)
            if data.get("status") in ("applied", "rejected", "failed"):
                shared["terminal"] = data["status"]
                done.set()

    client.on_connect = on_connect
    client.on_message = on_message
    try:
        client.connect(args.broker, args.port or (8883 if args.tls else 1883), keepalive=20)
        client.loop_start()
        if not connected.wait(min(args.seconds, 15)):
            raise RuntimeError(shared["connection_error"] or "Broker connection timeout")
        if args.action in ("watch", "discover"):
            done.wait(args.seconds)
            return 0
        if not fresh.wait(min(args.seconds, 15)):
            raise RuntimeError("Tidak ada state/heartbeat live. Pastikan simulator ONLINE; retained online tidak cukup.")
        with lock:
            snap = dict(shared["snapshot"])
        if args.action in ("on", "off"):
            action, params = "set_output", {"channel": "relay-1", "state": args.action.upper()}
        elif args.action == "status":
            action, params = "get_status", {}
        else:
            action, params = "set_config", json.loads(args.values)
            if not isinstance(params, dict):
                raise ValueError("--values harus object JSON")
            params = dict(params)
            params.setdefault("expected_config_version", snap["config_version"])
        command = build_command(snap, action, params)
        shared["command_id"] = command["command_id"]
        print("SEND", json.dumps(command), flush=True)
        client.publish(f"{base}/commands", json.dumps(command), qos=1, retain=False).wait_for_publish(timeout=5)
        if not done.wait(args.seconds):
            raise RuntimeError("Hasil belum diterima: outcome unknown. Periksa state sebelum mengirim perintah baru.")
        return 0 if shared["terminal"] == "applied" else 2
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", flush=True)
        return 1
    finally:
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    raise SystemExit(main())
