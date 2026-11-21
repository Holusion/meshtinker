#!/usr/bin/env python3
"""Runs meshtinker-uv on models and prints one CSV row per material group.

usage: measure.py <meshtinker-uv> <outdir> <label> <model.gltf>... [-- <uv options>...]

Each run writes into <outdir>/<n>/, on a real disk (outputs of large models are large), removed after the run. Columns: label, model, materials,
status, charts, texels before, atlas texels (what the atlas would be, also for no-gain groups),
gain, seconds of the group, seconds and peak RSS (KiB) of the whole run.
"""
import json, os, shutil, subprocess, sys, time

args = sys.argv[1:]
options = []
if '--' in args:
    options = args[args.index('--') + 1:]
    args = args[:args.index('--')]
uv, outdir, label, models = args[0], args[1], args[2], args[3:]
print('label,model,materials,status,charts,texels_before,texels_atlas,gain,group_seconds,run_seconds,peak_rss_kib')
for n, model in enumerate(models):
    out = os.path.join(outdir, str(n))
    os.makedirs(out, exist_ok=True)
    start = time.monotonic()
    process = subprocess.Popen([uv, '--json', *options, model, os.path.join(out, 'model.gltf')],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    stdout = process.stdout.read()
    _, status, usage = os.wait4(process.pid, 0)
    seconds = time.monotonic() - start
    rss = usage.ru_maxrss
    returncode = os.waitstatus_to_exitcode(status)
    # Only the report is kept: outputs of large models add up.
    shutil.rmtree(out, ignore_errors=True)
    name = os.path.basename(os.path.dirname(model))
    try:
        report = json.loads(stdout)
    except json.JSONDecodeError:
        print(f'{label},{name},,failed {returncode},,,,,,{seconds:.1f},{rss}')
        continue
    for g in report.get('groups', []):
        before_t, after_t = g['texels']['before'], g['texels']['after']
        gain = 1 - after_t / before_t if before_t and after_t else ''
        status = g['status'] + (':' + g['reason'] if 'reason' in g else '')
        print(f'{label},{name},{len(g["materials"])},{status},{g["charts"]},{before_t},{after_t},'
              f'{gain if gain == "" else round(gain, 3)},{g["seconds"]:.2f},{seconds:.1f},{rss}')
    sys.stdout.flush()
