'use strict';
/*
 * Runs a packed app through HBLauncher's own host-side checks without a console:
 *   1. loadPackage(): manifest, ABI, kind, file sizes and the code/data hashes;
 *   2. a launcher lifecycle on HBLauncher's mock backend with only this app installed: the
 *      launcher maps, the app's code and data are mapped at their bases (overlap checks against
 *      the resident launcher), the app is entered once and its images are released.
 * The mock does not execute app code and has no RPC size limits, so this is not a hardware test.
 *
 *   HBL_ROOT=/path/to/HBLauncher node hblauncher/tools/check-package.cjs hblauncher/dist/wiicompiled-rr
 */
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert');

const ROOT = process.env.HBL_ROOT;
const pkgDir = process.argv[2] && path.resolve(process.argv[2]);
if (!ROOT || !pkgDir) {
  console.error('usage: HBL_ROOT=/path/to/HBLauncher node check-package.cjs <package dir>');
  process.exit(2);
}
const {loadPackage} = require(path.join(ROOT, 'host/lib/packages.cjs'));
const {Runtime} = require(path.join(ROOT, 'host/lib/runtime.cjs'));
const {MockTarget} = require(path.join(ROOT, 'host/lib/target.cjs'));
const {createMockCallHandler} = require(path.join(ROOT, 'host/lib/mock-device.cjs'));
const {Log} = require(path.join(ROOT, 'host/lib/common.cjs'));

(async () => {
  const pkg = loadPackage(pkgDir);
  const report = pkg.report;
  console.log(`loadPackage: ok - ${pkg.manifest.id} abi ${pkg.manifest.abi}, code ${report.codeBytes} B ` +
              `at ${report.codeBase}, data region ${report.dataRegionBytes} B at ${report.dataBase}, ` +
              `entry ${report.entry}`);

  const apps = fs.mkdtempSync(path.join(os.tmpdir(), 'hbl-check-'));
  try {
    fs.cpSync(pkgDir, path.join(apps, pkg.manifest.id), {recursive: true});
    const log = new Log({quiet: true});
    const handler = createMockCallHandler({appFrames: 1});
    const target = new MockTarget({log, onCall: handler});
    const runtime = new Runtime({target, log, appsDir: apps});
    await runtime.open();
    assert.strictEqual(runtime.packages.length, 1, 'the package must be in the catalog');
    handler.setEntries(runtime.launcher.entry, Number(BigInt(runtime.packages[0].report.entry)));
    const result = await runtime.run({maxEntries: 4});
    assert.strictEqual(result.launches, 1, 'the app must be launched once');
    assert.strictEqual(handler.state.appCalls, 1, 'the app entry must be called once');
    const mapped = runtime.mappedImages.map(entry => entry.label);
    assert(!mapped.some(label => label.includes(pkg.manifest.id)), 'app images must be released');
    await runtime.stop();
    console.log('mock lifecycle: ok - mapped, entered once, released');
  } finally {
    fs.rmSync(apps, {recursive: true, force: true});
  }
})().catch(error => {
  console.error('FAILED: ' + (error && error.stack || error));
  process.exit(1);
});
