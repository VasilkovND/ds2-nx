import createDxcModule from '../../tools/dxc_web.mjs (the installer's WASM DXC, GPL-3)';
// Halo 3 shader library, step 2: HLSL (halo3_hlsl) -> SPIR-V via the WASM DXC. Prepends the deko3d define so the
// deko path is taken. Caveat: the installer DXC bakes -fvk-invert-y (Vulkan); deko3d wants no invert-y, fixed later
// in the renderer or a deko DXC. 212/508 compile to valid SPIR-V today; the rest hit a CF-structure bug (bool
// control flow emitted outside main) and the pixel-shader vfetch gap, both XenosRecomp refinements.
import * as fs from 'node:fs';
import * as path from 'node:path';

const HLSL = process.argv[2];      // folder of .hlsl
const OUT  = process.argv[3];      // folder for .spv
const COMMON = 'shaders/XenosRecomp/shader_common.h';
fs.mkdirSync(OUT, { recursive: true });

const dxc = await createDxcModule({ print: () => {}, printErr: () => {} });
dxc.FS.mkdirTree('/work');
dxc.FS.writeFile('/work/shader_common.h', fs.readFileSync(COMMON));

const files = fs.readdirSync(HLSL).filter(f => f.endsWith('.hlsl')).sort();
let ok = 0, fail = 0; const fails = {};
for (const f of files) {
  const stem = f.replace('.hlsl', '');
  const vertex = stem.startsWith('v_') ? 1 : 0;
  // Prepend the deko define so the HLSL takes the deko3d path (boolean block, combined samplers).
  const src = '#define NFSMW_DEKO3D 1\n' + fs.readFileSync(path.join(HLSL, f), 'utf8');
  dxc.FS.writeFile('/work/' + f, src);
  const rc = dxc.ccall('compile', 'number', ['string', 'string', 'number'],
                       ['/work/' + f, '/work/' + stem + '.spv', vertex]);
  if (rc === 0) {
    const spv = dxc.FS.readFile('/work/' + stem + '.spv');
    fs.writeFileSync(path.join(OUT, stem + '.spv'), spv);
    ok++;
  } else {
    fail++; fails[rc] = (fails[rc] || 0) + 1;
  }
  try { dxc.FS.unlink('/work/' + f); dxc.FS.unlink('/work/' + stem + '.spv'); } catch {}
}
console.log(`SPIR-V: ${ok} ok, ${fail} failed`, JSON.stringify(fails));
