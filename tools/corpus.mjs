#!/usr/bin/env node
// Query test/corpus.json, merged with test/corpus.local.json when it exists: the gitignored list of
// the models on this machine (docs/validation.md, "The corpus").
//
//   node tools/corpus.mjs list [SELECTION]           "id<TAB>path" per model (globs expanded)
//   node tools/corpus.mjs paths [SELECTION]          absolute paths only, one per line
//   node tools/corpus.mjs tags                       every tag with its model count
//   node tools/corpus.mjs root SOURCE                the source's root folder, absolute (exit 1 if it has none)
//   node tools/corpus.mjs check                      which models are present; exit 1 if a fixture or Khronos model is missing
//
// SELECTION: --id ID (prefix match, repeatable), --covers TAG (all required, repeatable), --source S,
//            --without TAG (repeatable, e.g. --without large).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const corpus = JSON.parse(fs.readFileSync(path.join(repo, 'test/corpus.json'), 'utf8'));
const localFile = path.join(repo, 'test/corpus.local.json');
if (fs.existsSync(localFile)) {
  const local = JSON.parse(fs.readFileSync(localFile, 'utf8'));
  Object.assign(corpus.sources, local.sources);
  corpus.assets.push(...(local.assets ?? []));
}

function sourceRoot(source) {
  const root = corpus.sources[source].root;
  return path.isAbsolute(root) ? root : path.join(repo, root);
}

/** Every model, with globs expanded: {id, source, path (absolute), covers, notes}. */
export function models() {
  const out = [];
  for (const a of corpus.assets) {
    const root = sourceRoot(a.source);
    if (a.glob) {
      const matches = fs.existsSync(root) ? fs.globSync(a.glob, { cwd: root }).sort() : [];
      for (const m of matches) out.push({ ...a, id: `${a.id}/${m.split('/').at(-2)}`, path: path.join(root, m) });
    } else {
      out.push({ ...a, path: path.join(root, a.path) });
    }
  }
  return out;
}

/**
 * Where check-commands.mjs keeps a model's inspect snapshot: Khronos models in test/snapshots/<id>.json
 * (committed), local models in test/assets/snapshots (gitignored: figures of private models stay on the
 * machine). Fixtures have none: their expected.json is checked by ctest.
 */
export function snapshotFile(m) {
  if (m.source === 'fixture') return null;
  return path.join(repo, m.source === 'local' ? 'test/assets/snapshots' : 'test/snapshots', `${m.id}.json`);
}

export function select(args) {
  const many = (flag) => args.flatMap((a, i) => (args[i - 1] === flag ? [a] : []));
  const ids = many('--id');
  const covers = many('--covers');
  const without = many('--without');
  const sources = many('--source');
  return models().filter((m) =>
    (!ids.length || ids.some((id) => m.id.startsWith(id))) &&
    covers.every((t) => m.covers.includes(t)) &&
    !without.some((t) => m.covers.includes(t)) &&
    (!sources.length || sources.includes(m.source)));
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [command, ...args] = process.argv.slice(2);
  if (command === 'list') {
    for (const m of select(args)) console.log(`${m.id}\t${m.path}`);
  } else if (command === 'paths') {
    for (const m of select(args)) console.log(m.path);
  } else if (command === 'root') {
    if (!corpus.sources[args[0]]) {
      console.error(`no source '${args[0]}'${args[0] === 'local' ? ': test/corpus.local.json lists the local models' : ''}`);
      process.exit(1);
    }
    console.log(sourceRoot(args[0]));
  } else if (command === 'tags') {
    const tags = {};
    for (const m of models()) for (const t of m.covers) tags[t] = (tags[t] ?? 0) + 1;
    for (const t of Object.keys(tags).sort()) console.log(`${t}\t${tags[t]}`);
  } else if (command === 'check') {
    let missing = 0;
    const bySource = {};
    for (const m of models()) {
      const present = fs.existsSync(m.path);
      bySource[m.source] ??= { present: 0, missing: [] };
      if (present) bySource[m.source].present++;
      else {
        bySource[m.source].missing.push(m.id);
        if (m.source !== 'local') missing++;
      }
    }
    if (!corpus.sources.local || !fs.existsSync(sourceRoot('local'))) bySource.local = { present: 0, missing: ['(no local models on this machine)'] };
    console.log(JSON.stringify(bySource, null, 2));
    process.exitCode = missing ? 1 : 0;
  } else {
    console.error('Usage: node tools/corpus.mjs list|paths|tags|check [--id ID] [--covers TAG] [--without TAG] [--source S]');
    process.exitCode = 2;
  }
}
