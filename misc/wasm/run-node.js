// Minimal Node-based WASI harness for factor.wasm
// Usage: node misc/wasm/run-node.js [factor.wasm] [factor.image]
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { argv, env } from 'node:process';
import { WASI } from 'node:wasi';

const wasmPath = argv[2] || 'factor.wasm';
const imagePath = argv[3] || 'factor.image';

async function main() {
  const wasi = new WASI({
    version: 'preview1',
    args: [wasmPath, '-image=' + imagePath],
    env,
    preopens: {
      '.': process.cwd(),
    },
  });

  const wasmBytes = await fs.readFile(wasmPath);
  const module = await WebAssembly.compile(wasmBytes);
  const instance = await WebAssembly.instantiate(module, {
    wasi_snapshot_preview1: wasi.wasiImport,
  });

  wasi.start(instance);
}

main().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
