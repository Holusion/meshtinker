// gltf-transform I/O configured as eCorpus configures it (source/server/utils/gltf/io.ts):
// every extension gltf-transform knows, with Draco and meshopt decoders.
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import draco3d from 'draco3dgltf';
import { MeshoptDecoder, MeshoptEncoder } from 'meshoptimizer';

let io;

/** Shared NodeIO instance. Log output goes to stderr so stdout stays JSON. */
export async function getIO() {
  if (io) return io;
  await MeshoptDecoder.ready;
  await MeshoptEncoder.ready;
  io = new NodeIO()
    .registerExtensions(ALL_EXTENSIONS)
    .registerDependencies({
      'draco3d.decoder': await draco3d.createDecoderModule(),
      'draco3d.encoder': await draco3d.createEncoderModule(),
      'meshopt.decoder': MeshoptDecoder,
      'meshopt.encoder': MeshoptEncoder,
    });
  return io;
}

/** Logger for Document.setLogger that keeps stdout clean. */
export const stderrLogger = {
  debug: () => {},
  info: (...a) => console.error(...a),
  warn: (...a) => console.error(...a),
  error: (...a) => console.error(...a),
};

/** Extension names gltf-transform reads; anything else is dropped by io.read(). */
export const KNOWN_EXTENSIONS = new Set(ALL_EXTENSIONS.map((e) => e.EXTENSION_NAME));

/** Raw glTF JSON of a .gltf or .glb file, without loading resources. */
export async function readRawJSON(path) {
  const fs = await import('node:fs');
  const bytes = fs.readFileSync(path);
  if (bytes.readUInt32LE(0) === 0x46546c67) {
    const length = bytes.readUInt32LE(12);
    return JSON.parse(bytes.subarray(20, 20 + length).toString('utf8'));
  }
  return JSON.parse(bytes.toString('utf8'));
}
