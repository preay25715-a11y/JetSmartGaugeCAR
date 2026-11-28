// Jet Smart Gauge v1.0.17 - app-side helpers for the MFB-LINK sync (USB serial 115200 baud, or Bluetooth LE: Nordic UART service 6e400001-b5a3-f393-e0a9-e50e24dcca9e,
// write frames to ...0002 (write / write-without-response), answers arrive as notifications on ...0003, the gauge is advertised as "Jet Smart Gauge")
// frame: 55 AA | type | lenL lenH | payload | xor(type, lenL, lenH, payload...)
// reply: type|0x80, payload[0] = status (0 ok, 1 no ECU answer, 2 refused / bad request), then the data
// Every decoder takes the reply payload WITHOUT the status byte (that is what parseFrames() gives you in .data).
'use strict';
const CMD = { SENSORS: 0x20, LIVE: 0x21, LOG_GET: 0x22, PIDS: 0x23, TRIP: 0x24, INFO: 0x25, CODES: 0x26, GRAPH: 0x27 };
const u16 = (p, i) => p[i] | (p[i + 1] << 8), s16 = (p, i) => (u16(p, i) << 16) >> 16;
const u32 = (p, i) => (p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (p[i + 3] << 24)) >>> 0, s32 = (p, i) => (u32(p, i) | 0);
const NODATA32 = 0x80000000 | 0;                          // s32 -2147483648 = "no data" (a PID that is supported but silent right now)

// ---- framing ----
function buildFrame(type, payload = []) {
  const n = payload.length, f = [0x55, 0xAA, type, n & 255, n >> 8, ...payload]; let x = type ^ (n & 255) ^ (n >> 8);
  for (const b of payload) x ^= b; f.push(x & 255); return Uint8Array.from(f);
}
class FrameParser {                                       // feed() it every chunk the serial port gives you; it returns the complete, checksum-good frames
  constructor() { this.buf = []; }
  feed(bytes) {
    this.buf.push(...bytes); const out = [];
    for (;;) {
      const b = this.buf, i = b.findIndex((v, k) => v === 0x55 && b[k + 1] === 0xAA);
      if (i < 0) { if (b.length > 1) this.buf = b.slice(-1); break; }
      if (i > 0) b.splice(0, i);
      if (b.length < 5) break;
      const len = b[3] | (b[4] << 8); if (b.length < 6 + len) break;
      let x = 0; for (let k = 2; k < 5 + len; k++) x ^= b[k];
      if (x === b[5 + len]) { out.push({ type: b[2], reply: !!(b[2] & 0x80), cmd: b[2] & 0x7F, status: len ? b[5] : null, data: b.slice(6, 5 + len) }); b.splice(0, 6 + len); }
      else b.splice(0, 2);                                // bad checksum: drop the two sync bytes and look for the next frame
    }
    return out;
  }
}

// ---- LIVE 0x21 ----
function decodeLive(p) {
  if (!p || p.length < 48 || p[0] !== 1) return null;
  const f = u16(p, 4), has = (b) => !!(f & (1 << b)), v = (b, x) => (has(b) ? x : null);
  return { mode: p[1] ? 'kline' : 'car', linked: !!(p[2] & 1), demo: !!(p[2] & 2), diesel: !!(p[2] & 4), hybrid: !!(p[2] & 8), bike: !!(p[2] & 16), mil: !!(p[2] & 32),
    rpm: v(0, u16(p, 6)), speed: v(1, u16(p, 8) / 10), tps: v(2, u16(p, 10) / 10), map: v(3, u16(p, 12) / 10), ect: v(4, s16(p, 14) / 10), iat: v(5, s16(p, 16) / 10),
    batt: v(6, u16(p, 18) / 100), timing: v(7, s16(p, 20) / 10), inj: v(8, u16(p, 22) / 100), afr: v(9, u16(p, 24) / 100), lambda: v(10, u16(p, 26) / 1000),
    crpMPa: v(11, u16(p, 28) / 10), crpV: v(11, u16(p, 30) / 1000), boost: v(12, s16(p, 32) / 10), fuelLh: v(13, u16(p, 34) / 100), oil: v(14, s16(p, 36) / 10), load: v(15, u16(p, 38) / 10),
    tripKm: u32(p, 40) / 1000, tripSec: u32(p, 44) };
}
// ---- LOG_GET 0x22: kind 0 = AFR per rpm band, 1 = CRP volts per rpm band, 2 = AFR map (throttle rows x rpm columns) ----
function decodeLog(p) {
  if (!p || p.length < 4 || p[0] > 2) return null;
  const kind = p[0], rows = p[1], cols = p[2], rpmStep = p[3] * 100; let o = 4;
  if (kind === 2) {
    const tps = Array.from(p.slice(o, o + rows)); o += rows; const cells = [];
    for (let r = 0; r < rows; r++) { const row = []; for (let c = 0; c < cols; c++, o += 4) { const n = u16(p, o); row.push({ n, avg: n ? u16(p, o + 2) / 100 : null }); } cells.push(row); }
    return { kind, tps, rpmStep, cells };                // cells[row][col]; row = throttle tps[row] %, col = rpm band col * rpmStep
  }
  const bands = [];
  for (let c = 0; c < cols; c++, o += 8) { const n = u16(p, o); bands.push({ n, avg: n ? u16(p, o + 2) / 100 : null, min: n ? u16(p, o + 4) / 100 : null, max: n ? u16(p, o + 6) / 100 : null }); }
  return { kind, rpmStep, bands };                       // band c = rpm c*rpmStep .. (c+1)*rpmStep (the last one is open-ended)
}

// ---- PIDS 0x23: the ALL PIDS page. Request payload [page]; reply [page, pages, count, total, count x (id u8, value s32 x100)]. Ask page 0, then 1 .. pages-1. ----
function decodePids(p) {
  if (!p || p.length < 4) return null; const count = p[2], list = [];
  for (let k = 0, o = 4; k < count && o + 5 <= p.length; k++, o += 5) { const raw = s32(p, o + 1); list.push({ id: p[o], value: raw === NODATA32 ? null : raw / 100 }); }
  return { page: p[0], pages: p[1], total: p[3], list };  // id = the OBD-II PID (0x04 load, 0x0C rpm ...); 0xF1 = injector pulse (ms) of the Honda K-line
}
// ---- TRIP 0x24: the TRIP and PEAKS pages ----
function decodePeaks(p, o) {
  const t = (i) => u32(p, o + 18 + 4 * i);
  return { rpm: u16(p, o), speed: u16(p, o + 2) / 10, coolant: s16(p, o + 4) / 10, oil: s16(p, o + 6) / 10, load: u16(p, o + 8) / 10, intake: s16(p, o + 10) / 10,
    battMin: u16(p, o + 12) / 100, battMax: u16(p, o + 14) / 100, zeroTo100: u16(p, o + 16) ? u16(p, o + 16) / 100 : null,
    at: { rpm: t(0), speed: t(1), coolant: t(2), oil: t(3), load: t(4), intake: t(5), battMin: t(6), battMax: t(7) } };   // at = seconds since the gauge started
}
function decodeTrip(p) {
  if (!p || p.length < 24 || p[0] !== 1) return null; const nm = p[3]; let o = 24;
  const minutes = []; for (let i = 0; i < nm; i++, o += 4) minutes.push({ fuelMl: u16(p, o), distM: u16(p, o + 2) });   // per minute, oldest first
  const drive = decodePeaks(p, o), allTime = decodePeaks(p, o + 50);
  return { estimated: !!(p[1] & 1), unitL100: !!(p[1] & 4), tankL: p[2], distKm: u32(p, 4) / 1000, seconds: u32(p, 8), fuelL: u32(p, 12) / 1000,
    nowL100: u16(p, 16) / 100, nowKmL: u16(p, 18) / 100, avgL100: u16(p, 20) / 100, rangeKm: (p[1] & 2) ? u16(p, 22) : null, minutes, peaksDrive: drive, peaksAll: allTime };
}
// ---- INFO 0x25: link, settings, dash tiles, vehicle, ECU, monitors ----
const METRICS = ['coolant', 'oil', 'intake', 'battery', 'afr', 'load', 'throttle', 'map', 'boost', 'maf', 'timing', 'stft', 'ltft', 'fuel level', 'rpm', 'speed',
  'egt', 'ambient', 'rail pressure', 'pedal', 'torque', 'fuel rate', 'hv battery', 'catalyst temp', 'baro', 'lambda', 'runtime', 'crp signal'];   // index = MI_* in the firmware (dash tile ids)
function decodeInfo(p) {
  if (!p || p.length < 37 || p[0] !== 1) return null;
  let o = 37; const str = () => { const L = p[o++]; const s = Buffer.from(p.slice(o, o + L)).toString('latin1'); o += L; return s; };
  const f = p[2], sb = p[14], klN = Math.min(p[26], 10);
  const out = { mode: p[1] ? 'kline' : 'car', linked: !!(f & 1), demo: !!(f & 2), diesel: !!(f & 4), hybrid: !!(f & 8), bike: !!(f & 16), mil: !!(f & 32), monitorsValid: !!(f & 128), bleConnected: !!(f & 64),
    protocol: p[3], codeCount: p[4], codeState: ['unread', 'reading', 'done', 'cleared', 'not readable'][p[5]] || p[5], model: u16(p, 6) === 65535 ? null : u16(p, 6),
    shiftRpm: u16(p, 8), tankL: p[10], fuelCal: p[11], brightness: p[12], theme: p[13],
    settings: { thai: !!(sb & 1), alerts: !!(sb & 2), parkingScreen: !!(sb & 4), colourful: !!(sb & 8), invert: !!(sb & 16), flip: !!(sb & 32), unitL100: !!(sb & 64), bleLink: !!(sb & 128) },
    dashCenter: p[15], dashTiles: Array.from(p.slice(16, 22)).map((i) => ({ id: i, name: METRICS[i] || '?' })), monitorsRaw: u32(p, 22),
    klEcuId: Array.from(p.slice(27, 27 + klN)).map((b) => b.toString(16).padStart(2, '0')).join(' ') };
  out.firmware = str(); out.car = str(); out.vin = str(); out.ecu = str(); out.calId = str(); return out;
}
// ---- CODES 0x26: CODES + FREEZE pages ----
const FREEZE_FIELDS = ['rpm', 'speed', 'coolant', 'load', 'stft', 'ltft', 'map', 'intake', 'throttle', 'maf', 'timing', 'battery'];
function decodeCodes(p) {
  if (!p || p.length < 4 || p[0] !== 1) return null; const cn = p[2]; let o = 4; const codes = [];
  for (let i = 0; i < cn; i++, o += 6) { let s = ''; for (let k = 0; k < 5 && p[o + k]; k++) s += String.fromCharCode(p[o + k]); codes.push({ code: s, pending: !!p[o + 5] }); }
  const st = p[o++]; let code = ''; for (let k = 0; k < 5 && p[o + k]; k++) code += String.fromCharCode(p[o + k]); o += 5;
  const vm = u16(p, o); o += 2; const values = {};
  FREEZE_FIELDS.forEach((n, i) => { const raw = s32(p, o + 4 * i); values[n] = (vm & (1 << i)) && raw !== NODATA32 ? raw / 100 : null; });
  return { state: ['unread', 'reading', 'done', 'cleared', 'not readable'][p[1]] || p[1], mil: !!p[3], codes, freeze: { state: ['none', 'reading', 'done', 'no data'][st] || st, code, values } };
}
// ---- GRAPH 0x27: request payload [mode, channel]; mode 0 = last 60 s, 1 = last 15 min; channel 0..7 ----
const GRAPH_CHANNELS = ['rpm', 'speed', 'coolant', 'oil', 'intake', 'afr', 'stft', 'battery'];
function decodeGraph(p) {
  if (!p || p.length < 8) return null; const cnt = p[2], scale = p[3] || 1, v = [];
  for (let i = 0; i < cnt && 8 + 2 * i + 2 <= p.length; i++) { const r = s16(p, 8 + 2 * i); v.push(r === -32768 ? null : r / scale); }
  return { mode: p[0] ? '15min' : '60s', channel: GRAPH_CHANNELS[p[1]] || p[1], pushCounter: u32(p, 4), values: v };   // oldest first; pushCounter moves with every new sample (use it to see if anything changed)
}

// ---- everything in one go. request(cmd, payloadArray) -> Promise<{status, data}> over your serial port ----
async function syncAll(request, { graphs = true } = {}) {
  const ask = async (cmd, pl, dec) => { const r = await request(cmd, pl); return r && r.status === 0 ? dec(Array.from(r.data)) : null; };
  const snap = { info: await ask(CMD.INFO, [], decodeInfo), live: await ask(CMD.LIVE, [], decodeLive), trip: await ask(CMD.TRIP, [], decodeTrip), codes: await ask(CMD.CODES, [], decodeCodes),
    pids: [], logs: {}, graphs: {} };
  for (let pg = 0, pages = 1; pg < pages; pg++) { const d = await ask(CMD.PIDS, [pg], decodePids); if (!d) break; pages = d.pages; snap.pids.push(...d.list); }
  for (const k of [0, 1, 2]) { const d = await ask(CMD.LOG_GET, [k], decodeLog); if (d) snap.logs[k === 0 ? 'afr' : k === 1 ? 'crp' : 'afrMap'] = d; }
  if (graphs) for (const md of [0, 1]) for (let ch = 0; ch < GRAPH_CHANNELS.length; ch++) { const d = await ask(CMD.GRAPH, [md, ch], decodeGraph); if (d) snap.graphs[d.mode + '/' + d.channel] = d; }
  return snap;
}
module.exports = { CMD, METRICS, GRAPH_CHANNELS, FREEZE_FIELDS, buildFrame, FrameParser, decodeLive, decodeLog, decodePids, decodeTrip, decodeInfo, decodeCodes, decodeGraph, syncAll };
