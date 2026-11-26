import importlib.util
from pathlib import Path
import unittest
import contextlib
import io
import json
import sys
import tempfile
import types
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("overnight_log", Path(__file__).parents[1]/"tools"/"overnight_log.py")
logger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logger)


class LogEvents(unittest.TestCase):
    def test_events(self):
        cases = {
            "I (1) stonelink: BOOT version=0.4.0 reset_reason=1": "boot",
            "I (1) stonelink: SOAK CYCLE end=1 result=SUCCESS": "cycle_success",
            "I (1) stonelink: SOAK CYCLE end=2 result=FAILED": "cycle_failed",
            "E (5) USB_MSC: failed": "firmware_error",
            "I (6) local_sd: COPY VERIFIED path": "copy_verified",
            "I (6) local_sd: COPY SKIP verified path": "copy_skipped",
            "I (3) stonelink: USB device removed": "usb_remove",
            "I (3) stonelink: USB device connected address=1": "usb_connect",
            "*** Device descriptor ***": "line",
            "I (3) stonelink: READ /usb/x bytes=3 CRC32/ISO-HDLC=abc": "read_complete",
            "I (3) stonelink: POWER ACTION disconnect external +5 V": "power_action",
            "I (3) stonelink: POWER TIMEOUT waiting": "power_timeout",
            "I (3) stonelink: POWER ERROR acquisition": "power_error",
            "I (3) stonelink: POWER CANCELED": "power_canceled",
            "I (3) stonelink: POWER STATUS mode=MANUAL": "power_status",
            "I (3) stonelink: COLLECT end=1 result=SUCCESS": "collect_success",
            "I (3) stonelink: COLLECT end=2 result=FAILED": "collect_failed",
            "I (3) local_sd: DEVICE prefix=ESP-AABBCCDDEEFF": "device_identity",
            "I (3) local_sd: META source=/usb/file.mp4 camera_datetime=2026-11-26 14:30:05": "capture_metadata",
            "I (3) local_sd: CATALOG VERIFIED path=/sd/file.json": "catalog_verified",
            "I (3) local_sd: CAPTURE id=123 kind=PHOTO": "capture_group",
            "I (3) media_group: MEDIA FLAG from=a to=b": "media_reclassified",
            "I (3) media_group: MEDIA GROUP files=2": "media_group",
            "I (3) transfer: HD QUEUED request=123": "hd_queued",
            "I (3) transfer: HD ALREADY QUEUED request=123": "hd_duplicate",
            "I (3) transfer: HD READY request=123; NOT TRANSMITTED": "hd_ready",
            "W (3) transfer: HD BLOCKED request=123": "hd_blocked",
        }
        for line, expected in cases.items():
            with self.subTest(line=line):
                self.assertEqual(logger.classify(line), expected)

    def test_utc_timestamp(self):
        self.assertTrue(logger.timestamp().endswith("+00:00"))

    def test_capture_reconnect_and_final_summary(self):
        class SerialError(Exception):
            pass
        actions = iter([b"I (1) stonelink: SOAK HEALTH mode=1\npartial", SerialError("unplug"),
                        b"I (2) stonelink: SOAK CYCLE end=1 result=SUCCESS\n", KeyboardInterrupt()])
        sent = []
        class FakeSerial:
            def __init__(self, **kwargs):
                pass
            def open(self):
                self.assert_lines = None
            def close(self):
                pass
            def write(self, value):
                sent.append(value)
            def read(self, size):
                action = next(actions)
                if isinstance(action, BaseException):
                    raise action
                return action
        serial_module = types.SimpleNamespace(Serial=FakeSerial, SerialException=SerialError)
        with tempfile.TemporaryDirectory() as directory, patch.dict(sys.modules, {"serial": serial_module}), \
                patch.object(sys, "argv", ["logger", "--output", directory, "--command", "SOAK ON"]), \
                patch.object(logger.threading.Thread, "start"), contextlib.redirect_stdout(io.StringIO()):
            logger.main()
            summaries = list(Path(directory).glob("*.summary.json"))
            self.assertEqual(len(summaries), 1)
            summary = json.loads(summaries[0].read_text())
            self.assertEqual(summary["counts"]["serial_connected"], 2)
            self.assertEqual(summary["counts"]["serial_disconnected"], 1)
            self.assertEqual(summary["counts"]["cycle_success"], 1)
            self.assertEqual(summary["counts"]["partial_line"], 1)
            self.assertEqual(sent, [b"SOAK ON\n"])
            records = [json.loads(line) for line in Path(summary["events"]).read_text().splitlines()]
            self.assertTrue(all(record["utc"].endswith("+00:00") for record in records))


if __name__ == "__main__":
    unittest.main()
