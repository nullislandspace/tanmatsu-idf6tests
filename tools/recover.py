#!/usr/bin/env python3
"""Get the badge back to the launcher after a crash or hang.

Resets the chip over the debug console the way `make monitor` does (DTR/RTS),
which restarts the test app. Once the app announces itself (READY/PONG), sends
EXIT so it returns to the launcher. If no app answers within the timeout the
badge is assumed to have come up in the launcher already.

Exit codes: 0 app answered and was sent EXIT, 4 reset done but no app
answered (probably in the launcher), 1 could not open the port.
"""

import argparse
import os
import sys
import time

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdtest import connect, parse_record, read_line  # noqa: E402


def hard_reset(url):
    from esp_pylib.serial_reset import hard_reset as pylib_hard_reset
    from esp_idf_monitor.base.chip_specific_config import get_chip_config

    port = serial.serial_for_url(url, baudrate=115200, timeout=1, do_not_open=True)
    # Same order as esp_idf_monitor's SerialReader.open_serial(reset=True).
    port.rts = False
    port.dtr = False
    port.open()
    port.rts = True
    port.dtr = True
    pylib_hard_reset(port, hold_delay=get_chip_config("esp32p4")["reset"])
    time.sleep(0.2)
    port.close()


def recover(url, timeout=60, log=None):
    log = log or open(os.devnull, "w")
    print(f"Recovery: resetting the badge via {url}...")
    try:
        hard_reset(url)
    except Exception as exc:  # noqa: BLE001
        print(f"Recovery: reset failed: {exc}", file=sys.stderr)
        return 1
    port, ready = connect(url, timeout, log)
    if port is None:
        print("Recovery: no app answered after the reset (probably in the launcher)")
        return 4
    print(f"Recovery: app answered ({ready.get('variant')}), sending EXIT")
    buffer = bytearray()
    try:
        port.write(b"EXIT\n")
        port.flush()
        deadline = time.time() + 5
        while time.time() < deadline:
            line = read_line(port, buffer)
            if line is not None and parse_record(line)[0] == "BYE":
                break
    except Exception:  # noqa: BLE001 - the link drops as the badge restarts
        pass
    try:
        port.close()
    except Exception:  # noqa: BLE001
        pass
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("PORT", "/dev/ttyACM0"))
    ap.add_argument("--timeout", type=float, default=60)
    args = ap.parse_args()
    return recover(args.port, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
