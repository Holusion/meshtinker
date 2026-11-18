#!/usr/bin/env python3
"""Run a command and record its memory use on Linux. Adapted from mesh2gltf's tools/monitor_import.py.

    python3 tools/monitor.py OUT_PREFIX -- COMMAND [ARGS...]

Samples /proc/<pid>/status every 0.2 s and writes, next to OUT_PREFIX:
  OUT_PREFIX.memory.csv   the samples
  OUT_PREFIX.stdout       the command's stdout (meshtinker's data output, such as --json)
  OUT_PREFIX.stderr       the command's stderr
  OUT_PREFIX.json         the summary, also printed: exit code, time, peaks

RSS counts memory-mapped file pages, which are evictable page cache. RssAnon is the process's own
heap and stacks: that is the figure the "map, don't load" constraint bounds. Peaks are sampled, so a
spike shorter than 0.2 s can be missed, except for peak RSS, which the kernel tracks (VmHWM).
Exits with the command's exit code. Refuses to overwrite existing reports.
"""
import csv
import json
import pathlib
import resource
import subprocess
import sys
import time

if "--" not in sys.argv[2:3] or len(sys.argv) < 4:
    sys.exit(__doc__)
prefix = pathlib.Path(sys.argv[1])
command = sys.argv[3:]
paths = {k: pathlib.Path(f"{prefix}.{k}") for k in ("memory.csv", "stdout", "stderr", "json")}
for p in paths.values():
    if p.exists():
        sys.exit(f"Refusing to overwrite {p}")
prefix.parent.mkdir(parents=True, exist_ok=True)

FIELDS = ("VmRSS", "VmHWM", "VmSize", "RssAnon", "RssFile")
peaks = {k: 0 for k in FIELDS}
started = time.monotonic()
with paths["stdout"].open("x") as out, paths["stderr"].open("x") as err, paths["memory.csv"].open("x", newline="") as samples:
    writer = csv.writer(samples)
    writer.writerow(["elapsed_seconds", *(f"{k}_kib" for k in FIELDS)])
    process = subprocess.Popen(command, stdout=out, stderr=err)
    while process.poll() is None:
        try:
            values = {}
            for line in pathlib.Path(f"/proc/{process.pid}/status").read_text().splitlines():
                key, _, rest = line.partition(":")
                if key in FIELDS:
                    values[key] = int(rest.split()[0])
            for k in FIELDS:
                peaks[k] = max(peaks[k], values.get(k, 0))
            writer.writerow([round(time.monotonic() - started, 2), *(values.get(k, 0) for k in FIELDS)])
        except (FileNotFoundError, ProcessLookupError):
            pass
        time.sleep(0.2)
    code = process.wait()

report = {
    "command": command,
    "exit_code": code,
    "elapsed_seconds": round(time.monotonic() - started, 2),
    "peak_rss_kib": max(peaks["VmHWM"], resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss),
    "peak_anon_kib": peaks["RssAnon"],
    "peak_file_kib": peaks["RssFile"],
    "peak_virtual_kib": peaks["VmSize"],
    "files": {k: str(p) for k, p in paths.items()},
}
paths["json"].write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
sys.exit(code)
