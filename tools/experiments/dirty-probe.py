#!/usr/bin/env python3
"""Runs a command and prints, every 0.25 s, the system's dirty and writeback memory and the MiB written to a disk.

    python3 tools/experiments/dirty-probe.py DEVICE -- COMMAND [ARGS...]     (DEVICE: sdd for the SSD, sda for the HDD)
"""
import subprocess, sys, time

device = sys.argv[1]
command = sys.argv[sys.argv.index('--') + 1:]
def vm():
    d = dict(line.split() for line in open('/proc/vmstat'))
    return int(d['nr_dirty']) * 4 // 1024, int(d['nr_writeback']) * 4 // 1024
def written():
    return int(open(f'/sys/block/{device}/stat').read().split()[6]) * 512 // 2**20
subprocess.run(['sync'])
w0 = written()
process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
t0 = time.monotonic()
print('seconds dirty_mib writeback_mib written_mib')
while process.poll() is None:
    dirty, writeback = vm()
    print(round(time.monotonic() - t0, 2), dirty, writeback, written() - w0, flush=True)
    time.sleep(0.25)
print('end', round(time.monotonic() - t0, 2), 'written_mib', written() - w0, 'exit', process.returncode)
