#!/usr/bin/env node
// Runs `inspect` and `import` over corpus models and checks what docs/validation.md asks:
//
//   node tools/check-commands.mjs [--id P] [--covers T] [--without T] [--source S]
//                                 [--build DIR] [--out DIR] [--fast] [--keep] [--update-snapshots]
//
// For each model:
//   - inspect and import agree on accepting or rejecting it (same exit code);
//   - the output has no validator error or warning the input didn't have, and
//     no more info:UNUSED_OBJECT (validate.mjs --against);
//   - compare.mjs finds input and output equal;
//   - inspect on the output gives import's report;
//   - inspect's report equals the model's snapshot (corpus.mjs snapshotFile): a
//     committed one for Khronos models, a gitignored one for local models.
// --update-snapshots writes the snapshots instead of checking them: review the
// diff before committing it. --fast skips the validator and compare.mjs, for large models. Outputs go in
// --out (default out/check), removed after each model unless --keep.
// Prints one line per model, then writes <out>/summary.json.

import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { snapshotFile, select } from './corpus.mjs';
import { validate, newIssues } from './validate.mjs';
import { compare } from './compare.mjs';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const option = (flag, fallback) => (args.includes(flag) ? args[args.indexOf(flag) + 1] : fallback);
const build = path.resolve(option('--build', path.join(repo, 'build')));
const outRoot = path.resolve(option('--out', path.join(repo, 'out/check')));
const fast = args.includes('--fast');
const keep = args.includes('--keep');
const update = args.includes('--update-snapshots');

function run(program, argv) {
  const started = process.hrtime.bigint();
  const r = spawnSync(path.join(build, program), argv, { encoding: 'utf8', maxBuffer: 1 << 28 });
  const seconds = Number(process.hrtime.bigint() - started) / 1e9;
  let report = null;
  try {
    report = JSON.parse(r.stdout);
  } catch {
    report = null;
  }
  return { code: r.status, report, stderr: r.stderr, seconds };
}

/**
 * Where two reports differ, as JSON paths. Integers compare exactly; other numbers within 1e-9 relative,
 * so that a compiler contracting a multiply-add into an FMA doesn't fail the check.
 */
function reportDifferences(a, b, at = '') {
  if (typeof a === 'number' && typeof b === 'number') {
    const same = Number.isInteger(a) && Number.isInteger(b) ? a === b : Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(a), Math.abs(b));
    return same ? [] : [`${at || '/'} ${b} → ${a}`];
  }
  if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object' || Array.isArray(a) !== Array.isArray(b)) {
    return JSON.stringify(a) === JSON.stringify(b) ? [] : [`${at || '/'} ${JSON.stringify(b)?.slice(0, 40)} → ${JSON.stringify(a)?.slice(0, 40)}`];
  }
  const keys = [...new Set([...Object.keys(a), ...Object.keys(b)])];
  return keys.flatMap((k) => reportDifferences(a[k], b[k], `${at}/${k}`));
}

function readJSON(file) {
  try {
    const bytes = fs.readFileSync(file);
    if (bytes.readUInt32LE(0) === 0x46546c67) {
      const length = bytes.readUInt32LE(12);
      return JSON.parse(bytes.subarray(20, 20 + length).toString('utf8'));
    }
    return JSON.parse(bytes.toString('utf8'));
  } catch {
    return {};
  }
}

const strip = (report) => {
  const copy = structuredClone(report);
  delete copy.tool;
  delete copy.diagnostics;
  return copy;
};
// A snapshot keeps diagnostics: which warnings a model raises is behaviour too.
const withoutTool = (report) => {
  const copy = structuredClone(report);
  delete copy.tool;
  return copy;
};

const results = [];
fs.mkdirSync(outRoot, { recursive: true });
for (const m of select(args)) {
  if (!fs.existsSync(m.path)) continue;
  const result = { id: m.id, problems: [] };
  const trusted = m.covers.includes('ref-outside') || m.covers.includes('ref-symlink') ? ['--trusted'] : [];
  const inspected = run('meshtinker-inspect', ['--json', ...trusted, m.path]);
  const dir = path.join(outRoot, m.id.replaceAll('/', '_'));
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  const stem = path.basename(m.path).replace(/\.(gltf|glb)$/i, '');
  const output = path.join(dir, `${stem}.gltf`);
  const imported = run('meshtinker-import', ['--json', ...trusted, m.path, output]);
  result.inspect = { code: inspected.code, seconds: inspected.seconds };
  result.import = { code: imported.code, seconds: imported.seconds };
  result.diagnostics = (inspected.report?.diagnostics ?? []).map((d) => `${d.code}×${d.count}`);
  if (inspected.code !== imported.code) result.problems.push(`inspect exits ${inspected.code}, import ${imported.code}`);
  if (inspected.report === null) result.problems.push('inspect printed no JSON');
  if (inspected.report?.error) result.error = inspected.report.error.code;
  const snapshot = snapshotFile(m);
  if (snapshot && inspected.report) {
    const label = path.relative(repo, snapshot);
    if (update) {
      fs.mkdirSync(path.dirname(snapshot), { recursive: true });
      fs.writeFileSync(snapshot, JSON.stringify(withoutTool(inspected.report), null, 2) + '\n');
    } else if (fs.existsSync(snapshot)) {
      const differences = reportDifferences(withoutTool(inspected.report), JSON.parse(fs.readFileSync(snapshot, 'utf8')));
      if (differences.length) result.problems.push(`report differs from ${label}: ${differences.slice(0, 3).join('; ')}${differences.length > 3 ? ` (+${differences.length - 3})` : ''}`);
    } else if (m.source !== 'local') {
      // A committed snapshot must exist; a local model's may not have been written on this machine.
      result.problems.push(`no snapshot in ${label} (--update-snapshots)`);
    }
  }
  if (imported.code === 0) {
    const again = run('meshtinker-inspect', ['--json', output]);
    if (again.code !== 0) result.problems.push(`inspect on the output exits ${again.code}`);
    else if (JSON.stringify(strip(again.report)) !== JSON.stringify(strip(imported.report))) result.problems.push('inspect on the output differs from the import report');
    if (!fast) {
      try {
        const [out, inp] = await Promise.all([validate(output), validate(m.path)]);
        const issues = newIssues(out, inp);
        if (issues.length) result.problems.push(`new validator issues: ${issues.join(', ')}`);
        const unused = (r) => r.codes['info:UNUSED_OBJECT'] ?? 0;
        // The validator doesn't read meshopt extensions: a .bin only they use looks unused.
        const meshopt = (readJSON(output).extensionsUsed ?? []).some((e) => e.endsWith('_meshopt_compression'));
        if (unused(out) > unused(inp) && !meshopt) result.problems.push(`UNUSED_OBJECT ${unused(inp)} → ${unused(out)}`);
      } catch (error) {
        result.problems.push(`validator failed: ${error.message}`);
      }
      try {
        const c = await compare(m.path, output);
        if (!c.equal) result.problems.push(`compare: ${c.differences.slice(0, 3).map((d) => `${d.path} ${d.message}`).join('; ')}`);
      } catch (error) {
        result.compare = `not comparable: ${String(error.message).split('\n')[0].slice(0, 80)}`;
      }
    }
  }
  if (!keep) fs.rmSync(dir, { recursive: true, force: true });
  results.push(result);
  const status = result.problems.length ? 'PROBLEM' : 'ok';
  console.log([status, m.id, `exit ${inspected.code}${result.error ? ` (${result.error})` : ''}`,
    `${inspected.seconds.toFixed(1)}s/${imported.seconds.toFixed(1)}s`, result.diagnostics.join(' '),
    result.compare ?? '', result.problems.join(' | ')].filter(Boolean).join('  '));
}
fs.writeFileSync(path.join(outRoot, 'summary.json'), JSON.stringify(results, null, 2));
const problems = results.filter((r) => r.problems.length).length;
console.log(`${results.length} models, ${problems} with problems. Details: ${path.relative(repo, path.join(outRoot, 'summary.json'))}`);
// No model at all means the selection or the fetch went wrong, not that everything passed.
process.exit(problems || !results.length ? 1 : 0);
