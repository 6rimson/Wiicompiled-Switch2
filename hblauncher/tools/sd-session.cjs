'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');

// The promoted service is owned by the runner. This launch owns a clone, the
// returned SD filesystem, and one SDK-heap allocation shared with the app.
async function prepare(run) {
  const engine = path.join(run, 'switch2');
  const {Session, live} = require(path.join(engine, 'app/session.cjs'));
  const domain = require(path.join(engine, 'app/domain.cjs'));
  const proof = JSON.parse(await fs.readFile(path.join(engine, 'ro/root/current-root.json'), 'utf8'));
  const session = new Session('wiicompiled-sd', 0x80000);
  session.record = async () => {};
  const close = () => session.cleanup();
  try {
    await session.initialize();
    const root = domain.oneHandle(await domain.send(session, 'Clone FS for asset reads', proof.rootHandle, domain.wire.control(2)));
    const opened = await session.ipc('Open SD asset filesystem', root, 18);
    if (opened.result) {
      await close();
      return {descriptor: 0, result: opened.result, close: async () => {}};
    }
    const sd = domain.oneHandle(opened);
    const functions = require(path.join(run, 'shared/sdk-functions.json'));
    const descriptor = session.base + 0x10000;
    const header = Buffer.alloc(48);
    header.writeUInt32LE(0x53444632);
    header.writeUInt32LE(sd, 4);
    const fields = [live.sdk + functions.send.offset, live.sdk + functions.close.offset,
      descriptor + 0x1000, descriptor + 0x2000, descriptor + 0x3000];
    fields.forEach((address, index) => header.writeBigUInt64LE(BigInt(address), 8 + index * 8));
    await session.write(descriptor, header);
    return {descriptor, close};
  } catch (error) {
    await close();
    throw error;
  }
}
module.exports = {prepare};
