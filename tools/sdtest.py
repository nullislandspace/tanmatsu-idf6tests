#!/usr/bin/env python3
"""Drive one IDF6 SD test run on the badge and capture the results.

The app (see main/console.h) must already be running, normally started by
`make run`. This script connects to the debug console, waits for the app to
announce itself, sends `RUN <suite> <args>`, collects the CRC-framed records
until END, sends EXIT so the badge returns to the launcher, and writes

    results/<UTC>-<variant>-<suite>.log    everything the console printed
    results/<UTC>-<variant>-<suite>.json   parsed records and the verdict

Exit codes: 0 all reads correct, 3 corrupted reads found, 2 crash/abort/error
on the badge, 1 could not connect, stalled or timed out, 5 usage.

Structure and the two load-bearing read_line() details follow
tanmatsu-fonttest's tools/testrun.py.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time
import zlib
from collections import defaultdict

try:
    import serial
except ImportError:
    print("pyserial missing (run inside the ESP-IDF environment)", file=sys.stderr)
    sys.exit(5)

RECORD = re.compile(r"^@@SDT-([A-Z]+)@@ (.*?) @@([0-9a-f]{8})@@\s*$")
CRASH = re.compile(r"Guru Meditation|abort\(\) was called|Backtrace:|rst:0x|ESP-ROM:|assert failed|Stack smashing")

EXIT_OK, EXIT_LINK, EXIT_CRASH, EXIT_BAD, EXIT_USAGE = 0, 1, 2, 3, 5


def read_line(port, buffer):
    """One complete line or None. Drain the buffer before reading again, and
    read what is waiting rather than a fixed count (see fonttest testrun.py)."""
    if b"\n" not in buffer:
        chunk = port.read(max(1, port.in_waiting))
        if not chunk:
            return None
        buffer.extend(chunk)
        if b"\n" not in buffer:
            return None
    line, _, rest = bytes(buffer).partition(b"\n")
    buffer.clear()
    buffer.extend(rest)
    return line.decode("utf-8", errors="replace").rstrip("\r")


def parse_record(line):
    match = RECORD.match(line)
    if not match:
        return None, None
    payload = match.group(2)
    if zlib.crc32(payload.encode()) != int(match.group(3), 16):
        return "CORRUPT", None
    try:
        return match.group(1), json.loads(payload)
    except json.JSONDecodeError:
        return "CORRUPT", None


def wait_ready(port, timeout, log):
    deadline = time.time() + timeout
    buffer = bytearray()
    last_ping = 0.0
    while time.time() < deadline:
        if time.time() - last_ping >= 1.0:
            port.write(b"PING\n")
            port.flush()
            last_ping = time.time()
        line = read_line(port, buffer)
        if line is None:
            continue
        log.write(line + "\n")
        kind, rec = parse_record(line)
        if kind in ("READY", "PONG"):
            return rec
    return None


def connect(url, total_timeout, log):
    """Open the console and wait for READY/PONG, reopening as needed: the badge
    resets after `make run` and the proxy may drop the first connections."""
    deadline = time.time() + total_timeout
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        port = None
        try:
            port = serial.serial_for_url(url, baudrate=115200, timeout=1, do_not_open=True)
            port.open()
            ready = wait_ready(port, min(10.0, max(1.0, deadline - time.time())), log)
            if ready is not None:
                return port, ready
            last = "no READY/PONG"
        except Exception as exc:  # noqa: BLE001 - pyserial raises several types
            last = exc
        if port is not None:
            try:
                port.close()
            except Exception:  # noqa: BLE001
                pass
        print(f"  connect attempt {attempt}: {last}", file=sys.stderr)
        time.sleep(2.0)
    return None, None


def git_info():
    def run(*args):
        try:
            out = subprocess.run(["git", *args], capture_output=True, text=True, timeout=10)
            return out.stdout.strip() if out.returncode == 0 else None
        except (OSError, subprocess.SubprocessError):
            return None

    return {
        "commit": run("rev-parse", "--short=10", "HEAD") or "unknown",
        "dirty": bool(run("status", "--porcelain", "--untracked-files=no")),
    }


def case_key(c):
    method = c.get("method", "?")
    if method in ("staged", "chunked"):
        method += f"{c.get('chunk', 0) // 1024}K"
    io = c.get("io", "?")
    if io == "fast":
        io += f"{c.get('vbuf', 0) // 1024}K"
    return f"{c.get('dest', '?')}/{io}/{method}/db{c.get('dmabuf', '?')}"


def print_summary(result):
    cases = [c for c in result["cases"] if not c.get("skipped")]
    bad = [c for c in cases if not c.get("ok")]
    print()
    print(f"variant={result['meta']['variant']} suite={result['meta']['suite']} args='{result['meta']['args']}'")
    print(f"verdict={result['verdict']} cases={len(cases)} bad={len(bad)} "
          f"skipped={sum(1 for c in result['cases'] if c.get('skipped'))} corrupt_lines={result['corrupt_lines']}")

    # Correctness and speed per method and size.
    table = defaultdict(lambda: [0, 0, 0, 0])  # good, total, bytes, us
    sizes = set()
    for c in cases:
        row = table[(case_key(c), c["size"])]
        row[0] += 1 if c.get("ok") else 0
        row[1] += 1
        row[2] += c.get("n", 0)
        row[3] += c.get("us", 0)
        sizes.add(c["size"])
    sizes = sorted(sizes)
    keys = sorted({k for k, _ in table})
    if keys:
        width = max(len(k) for k in keys)
        print()
        print(" " * width + "".join(f" {s:>14}" for s in sizes))
        for k in keys:
            cells = []
            for s in sizes:
                row = table.get((k, s))
                if not row:
                    cells.append(f" {'-':>14}")
                    continue
                kbps = row[2] * 1_000_000 // row[3] // 1024 if row[3] else 0
                cells.append(f" {row[0]:>3}/{row[1]:<3}{kbps:>6}K/s")
            print(k.ljust(width) + "".join(cells))

    if bad:
        print()
        print("Bad cases (first 20):")
        for c in bad[:20]:
            print(f"  {case_key(c)} size={c['size']} off={c['off']} rep={c['rep']} addr={c.get('addr')} "
                  f"n={c.get('n')} bad={c.get('bad')} fill={c.get('bad_fill')} zero={c.get('bad_zero')} "
                  f"runs={c.get('runs')} gran={c.get('gran')}")
            for d in c.get("detail", [])[:4]:
                print(f"      +{d['s']} len {d['l']}: {d['d']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("PORT", "/dev/ttyACM0"))
    ap.add_argument("--suite", default="smoke")
    ap.add_argument("--args", default="")
    ap.add_argument("--variant", default=None, help="expected variant; refuse to run if the app reports another")
    ap.add_argument("--out-dir", default="results")
    ap.add_argument("--connect-timeout", type=float, default=90)
    ap.add_argument("--run-timeout", type=float, default=3600)
    ap.add_argument("--stall-timeout", type=float, default=180)
    ap.add_argument("--no-exit", action="store_true", help="leave the app running afterwards")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = os.path.join(args.out_dir, f"{stamp}-{args.variant or 'any'}-{args.suite}")
    log = open(base + ".log", "w", encoding="utf-8")

    result = {
        "meta": {
            "host_time": stamp, "port": args.port, "suite": args.suite, "args": args.args,
            "variant": args.variant, **{f"git_{k}": v for k, v in git_info().items()},
        },
        "ready": None, "begin": None, "pattern": None, "cases": [], "summary": None, "end": None,
        "corrupt_lines": 0, "crash_lines": [], "verdict": None,
    }

    def finish(verdict, code):
        result["verdict"] = verdict
        log.close()
        with open(base + ".json", "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1)
        print_summary(result)
        print(f"\nwrote {base}.json and .log")
        return code

    print(f"Connecting to {args.port}...")
    port, ready = connect(args.port, args.connect_timeout, log)
    if port is None:
        return finish("no-connection", EXIT_LINK)
    result["ready"] = ready
    print(f"App ready: {ready}")
    if args.variant and ready.get("variant") != args.variant:
        print(f"App reports variant {ready.get('variant')!r}, expected {args.variant!r}", file=sys.stderr)
        return finish("wrong-variant", EXIT_USAGE)
    if args.variant is None:
        result["meta"]["variant"] = ready.get("variant")

    # Let any queued banner land, then start.
    time.sleep(0.3)
    port.reset_input_buffer()
    port.write(f"RUN {args.suite} {args.args}".rstrip().encode() + b"\n")
    port.flush()

    started = last_line = time.time()
    buffer = bytearray()
    crash_at = None
    verdict, code = None, None
    while verdict is None:
        now = time.time()
        if crash_at is not None and now - crash_at > 5:
            verdict, code = "crash", EXIT_CRASH
            break
        if now - started > args.run_timeout:
            verdict, code = "timeout", EXIT_LINK
            break
        if now - last_line > args.stall_timeout:
            verdict, code = "stalled", EXIT_LINK
            break
        try:
            line = read_line(port, buffer)
        except Exception as exc:  # noqa: BLE001 - the link drops when the badge resets
            log.write(f"### link error: {exc}\n")
            verdict, code = ("crash" if crash_at else "link-lost"), (EXIT_CRASH if crash_at else EXIT_LINK)
            break
        if line is None:
            continue
        last_line = now
        log.write(line + "\n")
        kind, rec = parse_record(line)
        if kind is None:
            if not line.startswith("@@"):
                print(line)
            if CRASH.search(line):
                result["crash_lines"].append(line)
                crash_at = crash_at or now
            continue
        if kind == "CORRUPT":
            result["corrupt_lines"] += 1
        elif kind == "BEGIN":
            result["begin"] = rec
        elif kind == "PATTERN":
            result["pattern"] = rec
        elif kind == "CASE":
            result["cases"].append(rec)
        elif kind == "SUMMARY":
            result["summary"] = rec
        elif kind == "END":
            result["end"] = rec
            status = rec.get("status")
            verdict, code = {"ok": ("ok", EXIT_OK), "bad": ("bad", EXIT_BAD)}.get(status, ("error", EXIT_CRASH))

    if verdict not in ("crash", "link-lost") and not args.no_exit:
        try:
            port.write(b"EXIT\n")
            port.flush()
            deadline = time.time() + 5
            while time.time() < deadline:
                line = read_line(port, buffer)
                if line is None:
                    continue
                log.write(line + "\n")
                if parse_record(line)[0] == "BYE":
                    break
        except Exception:  # noqa: BLE001
            pass
    try:
        port.close()
    except Exception:  # noqa: BLE001
        pass
    return finish(verdict, code)


if __name__ == "__main__":
    sys.exit(main())
