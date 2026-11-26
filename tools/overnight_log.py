"""Timestamp every UART line, reconnect without resetting ESP32, save JSON summary.

Run with PlatformIO's Python (pyserial is already installed). No physical disks
are formatted and no source files deleted. Power commands are sent only when
the user supplies them; the logger does not automatically reset RTS/DTR or VBUS.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import queue
import threading
import time


def classify(line):
    if "stonelink: BOOT " in line:
        return "boot"
    if "SOAK CYCLE end=" in line:
        return "cycle_success" if "result=SUCCESS" in line else "cycle_failed"
    if "COLLECT end=" in line:
        return "collect_success" if "result=SUCCESS" in line else "collect_failed"
    for text, kind in [
        ("SOAK HEALTH", "health"), ("COPY VERIFIED", "copy_verified"),
        ("COPY SKIP verified", "copy_skipped"), ("FAT filesystem mounted", "usb_mount"),
        ("USB device connected", "usb_connect"), ("USB device removed", "usb_remove"),
        ("WATCH media_readable=0", "probe_failed"),
        ("CRC32/ISO-HDLC=", "read_complete"), ("PLAN complete", "plan_complete"),
        ("POWER ACTION", "power_action"), ("POWER TIMEOUT", "power_timeout"),
        ("POWER ERROR", "power_error"), ("POWER CANCELED", "power_canceled"),
        ("POWER STATUS", "power_status"),
    ]:
        if text in line:
            return kind
    if line.startswith("E (") or " E (" in line:
        return "firmware_error"
    return "line"


def timestamp():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, default=Path("logs"))
    parser.add_argument("--command", action="append", default=[],
                        help="Send once at first connection; can repeat. SOAK persists itself across ESP resets.")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = args.output / datetime.now().strftime("overnight_%Y%m%d_%H%M%S_%f")
    counts = Counter()
    latest = {}
    started = timestamp()
    commands = queue.Queue()
    for command in args.command:
        commands.put(command)

    def console():
        while True:
            try:
                command = input()
            except EOFError:
                return
            if command.strip():
                commands.put(command.strip())

    threading.Thread(target=console, daemon=True).start()
    text_path, json_path = Path(str(prefix)+".log"), Path(str(prefix)+".jsonl")
    summary_path = Path(str(prefix)+".summary.json")
    connection = None
    partial = bytearray()
    with text_path.open("x", encoding="utf-8", buffering=1) as raw, json_path.open("x", encoding="utf-8", buffering=1) as events:
        def record(kind, line):
            counts[kind] += 1
            if kind in ("boot", "health", "cycle_success", "cycle_failed", "copy_verified",
                        "collect_success", "collect_failed", "power_action", "power_status"):
                latest[kind] = line
            stamp = timestamp()
            events.write(json.dumps({"utc": stamp, "kind": kind, "text": line})+"\n")
            raw.write(f"{stamp} [{kind}] {line}\n")
            print(f"{stamp} {line}", flush=True)

        def summary():
            raw.flush()
            events.flush()
            os.fsync(raw.fileno())
            os.fsync(events.fileno())
            temporary = Path(str(summary_path)+".tmp")
            temporary.write_text(json.dumps({"started_utc": started, "updated_utc": timestamp(),
                                            "port": args.port, "counts": dict(counts), "latest": latest,
                                            "raw_log": str(text_path), "events": str(json_path)}, indent=2), encoding="utf-8")
            temporary.replace(summary_path)

        record("logger_start", f"Logs: {text_path}; type firmware commands here; Ctrl+C stops logger only")
        saved = time.monotonic()
        try:
            while True:
                if connection is None:
                    try:
                        connection = serial.Serial(port=None, baudrate=args.baud, timeout=0.2, write_timeout=2)
                        connection.dtr = False
                        connection.rts = False
                        connection.port = args.port
                        connection.open()
                        record("serial_connected", args.port)
                    except (serial.SerialException, OSError) as error:
                        if connection:
                            connection.close()
                        connection = None
                        record("serial_unavailable", str(error))
                        summary()
                        time.sleep(2)
                        continue
                try:
                    while not commands.empty():
                        command = commands.get_nowait()
                        record("command", command)
                        connection.write((command+"\n").encode("utf-8"))
                    chunk = connection.read(4096)
                    if chunk:
                        partial.extend(chunk)
                        while b"\n" in partial:
                            line, _, remaining = partial.partition(b"\n")
                            partial[:] = remaining
                            decoded = line.rstrip(b"\r").decode("utf-8", errors="replace")
                            record(classify(decoded), decoded)
                        if len(partial) > 65536:
                            record("partial_line", partial.decode("utf-8", errors="replace"))
                            partial.clear()
                except (serial.SerialException, OSError) as error:
                    record("serial_disconnected", str(error))
                    if partial:
                        record("partial_line", partial.decode("utf-8", errors="replace"))
                        partial.clear()
                    connection.close()
                    connection = None
                if time.monotonic()-saved >= 30:
                    summary()
                    saved = time.monotonic()
        except KeyboardInterrupt:
            record("logger_stop", "Stopped by user; persistent SOAK remains on until SOAK OFF")
        finally:
            if partial:
                record("partial_line", partial.decode("utf-8", errors="replace"))
            if connection:
                connection.close()
            summary()


if __name__ == "__main__":
    main()
