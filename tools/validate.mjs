#!/usr/bin/env node
// Khronos glTF validator, summarized.
//
//   node tools/validate.mjs FILE...                  one JSON summary per file; exit 1 if any has errors
//   node tools/validate.mjs OUTPUT --against INPUT   exit 1 if OUTPUT has an error or warning code INPUT lacks
//   --full                                           include every issue message, not only counts per code
//
// Unsupported extensions (meshopt, KTX2...) are reported by the validator as infos, not errors.
import fs from 'node:fs';
import path from 'node:path';
import validator from 'gltf-validator';

const SEVERITY = ['error', 'warning', 'info', 'hint'];

export async function validate(file, { full = false } = {}) {
  const report = await validator.validateBytes(new Uint8Array(fs.readFileSync(file)), {
    uri: path.basename(file),
    maxIssues: 0,
    externalResourceFunction: async (uri) =>
      new Uint8Array(fs.readFileSync(path.resolve(path.dirname(file), decodeURIComponent(uri)))),
  });
  const codes = {};
  for (const m of report.issues.messages) {
    const key = `${SEVERITY[m.severity]}:${m.code}`;
    codes[key] = (codes[key] ?? 0) + 1;
  }
  const summary = {
    file,
    validator: validator.version(),
    errors: report.issues.numErrors,
    warnings: report.issues.numWarnings,
    infos: report.issues.numInfos,
    hints: report.issues.numHints,
    codes,
  };
  if (full) summary.messages = report.issues.messages;
  return summary;
}

/** Error and warning codes present in `output` but not in `input`. */
export function newIssues(output, input) {
  return Object.keys(output.codes).filter(
    (k) => (k.startsWith('error:') || k.startsWith('warning:')) && !(k in input.codes),
  );
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const args = process.argv.slice(2);
  const full = args.includes('--full');
  const at = args.indexOf('--against');
  const against = at >= 0 ? args.splice(at, 2)[1] : null;
  const files = args.filter((a) => a !== '--full');
  if (!files.length) {
    console.error('Usage: node tools/validate.mjs FILE... [--full] | OUTPUT --against INPUT');
    process.exit(2);
  }
  try {
    if (against) {
      const [out, inp] = [await validate(files[0], { full }), await validate(against)];
      const added = newIssues(out, inp);
      console.log(JSON.stringify({ output: out, input: inp, newIssues: added }, null, 2));
      process.exitCode = added.length ? 1 : 0;
    } else {
      for (const f of files) {
        const s = await validate(f, { full });
        console.log(JSON.stringify(s, null, 2));
        if (s.errors) process.exitCode = 1;
      }
    }
  } catch (e) {
    console.error(e?.message ?? e);
    process.exitCode = 2;
  }
}
