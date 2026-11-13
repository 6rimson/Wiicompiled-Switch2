'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');

// The CPU clock (hblauncher/sd/clock.c): the console's clock service (clkrst), opened through the
// service manager session ns2jb holds for the file system, as tools in cpu-clock/ first tested.
const CPU_DEVICE = 0x40000001, CPU_MAGIC = 0x31555043, MAX_HZ = 1702400000;
const OPEN_DEVICE = 0, SET_RATE = 7, GET_RATE = 8, LIST_RATES = 10, RATE_SLOTS = 32;
const note = text => console.log('[wiicompiled] ' + text);
const mhz = hz => (hz / 1e6).toFixed(1) + ' MHz';
const check = (condition, message) => {if (!condition) throw Error(message);};

async function openCpuClock(run, session, domain) {
  const id = path.basename(path.resolve(run));
  const engine = path.join(run, 'switch2');
  const {connect} = require(path.join(engine, 'tools/fs-memory.cjs'));
  const ipc = require(path.join(run, 'shared/ipc.cjs'));
  const proof = JSON.parse(await fs.readFile(path.join(engine, 'ro', id + '-sm-proof.json'), 'utf8'));
  const memory = await connect();
  check(memory.info.session === id && proof.session === id && proof.report === memory.reportFile &&
    proof.service?.transport === 0 && proof.service.result === 0 && proof.service.handle === proof.export.handle,
  'the service-manager proof is not from this run');
  const lookup = await memory.request({command: 'root-native-sm-service', handle: proof.export.handle, name: 'clkrst'});
  check(lookup.transport === 0, 'clock service lookup failed');
  const manager = domain.oneHandle(lookup);
  const closeManager = () => memory.request({command: 'root-native-close-handle', handle: manager});
  try {
    const device = Buffer.alloc(8);
    device.writeUInt32LE(CPU_DEVICE);
    device.writeUInt32LE(3, 4);
    const cpu = domain.oneHandle(await session.ipc('Open CPU clock', manager, OPEN_DEVICE, device));
    const rate = async () => {
      const reply = await session.ipc('Read CPU rate', cpu, GET_RATE);
      check(!reply.result && reply.data.length >= 4, 'reading the CPU rate failed');
      return reply.data.readUInt32LE();
    };
    const setRate = async hz => {
      const bytes = Buffer.alloc(4);
      bytes.writeUInt32LE(hz);
      check(!(await session.ipc('Set CPU rate', cpu, SET_RATE, bytes)).result, 'setting the CPU rate failed');
    };
    // The stock range, as a two-entry range ending exactly at the maximum.
    const output = session.base + 0x2000, count = Buffer.alloc(4);
    count.writeInt32LE(RATE_SLOTS);
    const packet = ipc.packet(LIST_RATES, count, {receive: [{address: output, size: RATE_SLOTS * 4}]});
    packet.writeUInt32LE((3 << 10) | 10, 4); // the map-alias output form, with ten raw words
    const list = await domain.send(session, 'List CPU rates', cpu, packet);
    const rates = await session.readOwned(output, 8);
    check(!list.result && list.data.readInt32LE(0) === 2 && list.data.readInt32LE(4) === 2 &&
      rates.readUInt32LE(4) === MAX_HZ, 'the CPU does not offer ' + mhz(MAX_HZ));
    const launchHz = await rate();
    // A launch that ended without restoring leaves the maximum: its rate from before is lost.
    check(launchHz >= rates.readUInt32LE(0) && launchHz < MAX_HZ,
      'the CPU is already at ' + mhz(launchHz) + '; restart the console to get its normal rate back');
    return {cpu, launchHz, maxHz: MAX_HZ, rate, setRate, closeManager};
  } catch (error) {
    await closeManager();
    throw error;
  }
}

// The game puts the CPU back itself; this covers a game that ended without doing so (a crash).
async function restoreCpu(clock) {
  const now = await clock.rate();
  if (now === clock.launchHz) return;
  if (now !== clock.maxHz) return note('CPU at ' + mhz(now) + ' after the game, a rate the system chose; left as it is');
  await clock.setRate(clock.launchHz);
  const after = await clock.rate();
  note(after === clock.launchHz ? 'CPU back to ' + mhz(after) : 'CPU still at ' + mhz(after) +
    '; restart the console to get its normal rate back');
}

// The promoted service is owned by the runner. This launch owns a clone, the
// returned SD filesystem, the CPU clock, and one SDK-heap allocation shared with the app.
async function prepare(run) {
  const engine = path.join(run, 'switch2');
  const {Session, live} = require(path.join(engine, 'app/session.cjs'));
  const domain = require(path.join(engine, 'app/domain.cjs'));
  const proof = JSON.parse(await fs.readFile(path.join(engine, 'ro/root/current-root.json'), 'utf8'));
  const session = new Session('wiicompiled-sd', 0x80000);
  session.record = async () => {};
  let clock;
  const close = async () => {
    try {
      if (clock) await restoreCpu(clock);
    } catch (error) {
      note('CPU check after the game failed (' + error.message + '); restart the console if it runs hot');
    } finally {
      try {
        await session.cleanup();
      } finally {
        if (clock) await clock.closeManager();
      }
    }
  };
  try {
    await session.initialize();
    const root = domain.oneHandle(await domain.send(session, 'Clone FS for asset reads', proof.rootHandle, domain.wire.control(2)));
    const opened = await session.ipc('Open SD asset filesystem', root, 18);
    const sd = opened.result ? 0 : domain.oneHandle(opened);
    try {
      clock = await openCpuClock(run, session, domain);
      note('CPU clock ready: ' + mhz(clock.launchHz) + ' now, up to ' + mhz(clock.maxHz) + ' with CPU boost');
    } catch (error) {
      note('CPU boost uses the ordinary boost mode: ' + error.message);
    }
    if (!sd && !clock) {
      await close();
      return {descriptor: 0, result: opened.result, close: async () => {}};
    }
    const functions = require(path.join(run, 'shared/sdk-functions.json'));
    // The layout of struct sd_context in hblauncher/sd/storage.h.
    const descriptor = session.base + 0x10000;
    const header = Buffer.alloc(72);
    header.writeUInt32LE(sd ? 0x53444632 : 0);
    header.writeUInt32LE(sd, 4);
    const fields = [live.sdk + functions.send.offset, live.sdk + functions.close.offset,
      descriptor + 0x1000, descriptor + 0x2000, descriptor + 0x3000];
    fields.forEach((address, index) => header.writeBigUInt64LE(BigInt(address), 8 + index * 8));
    if (clock) {
      header.writeUInt32LE(CPU_MAGIC, 48);
      header.writeUInt32LE(clock.cpu, 52);
      header.writeUInt32LE(clock.launchHz, 56);
      header.writeUInt32LE(clock.maxHz, 60);
      // Its own page, after the 256 KiB bounce buffer: the game's clock and card requests can overlap.
      header.writeBigUInt64LE(BigInt(descriptor + 0x43000), 64);
    }
    await session.write(descriptor, header);
    return {descriptor, result: opened.result, close};
  } catch (error) {
    await close();
    throw error;
  }
}
module.exports = {prepare};
