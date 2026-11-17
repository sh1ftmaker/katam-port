// The online session's sealed record (web/ln_session.js encSealed /
// decSealed), no ROM, no module: records whose slots 4.. are idle encode to
// exactly the bytes the 4-slot encoder made (a copy of it is below), and
// records of 8 and 16 slots round-trip (keys, hash, events), with recBytes
// equal to what is written.  node tools/ln_wire_test.cjs
const L = require(require('path').join(__dirname, '../web/ln_session.js'));
// 4-slot records: byte-identical to the old encoder
function oldEnc(P, first, recs, gc, mm, echo, hold) {
  function recKeys(r) { return [r.k0 | 0, r.k1 | 0, r.k2 | 0, r.k3 | 0]; }
  var n = 23, r, i, k, s;
  for (i = 0; i < recs.length; i++) { var kk = recKeys(recs[i]); n += 1 + (recs[i].ev.length ? 1 : 0) + (recs[i].hashed ? 8 : 0) + recs[i].ev.length * 17; for (s = 0; s < 4; s++) if (kk[s]) n += 2; }
  var b = new Uint8Array(n), d = new DataView(b.buffer);
  b[0] = 0x32; d.setUint32(1, P >>> 0, true); d.setUint32(5, first >>> 0, true); d.setUint16(9, recs.length, true);
  d.setUint32(11, gc >>> 0, true); d.setInt16(15, Math.max(-32768, Math.min(32767, mm)), true); d.setUint32(17, echo >>> 0, true);
  d.setUint16(21, Math.min(65535, Math.round(hold * 10)), true);
  var o = 23;
  for (i = 0; i < recs.length; i++) { r = recs[i]; var keys = recKeys(r), m = 0; for (s = 0; s < 4; s++) if (keys[s]) m |= 1 << s;
    b[o++] = m | (r.hashed ? 16 : 0) | (r.ev.length ? 32 : 0); if (r.ev.length) b[o++] = r.ev.length;
    for (s = 0; s < 4; s++) if (m & (1 << s)) { d.setUint16(o, keys[s], true); o += 2; }
    if (r.hashed) { d.setUint32(o, r.hlo >>> 0, true); d.setUint32(o + 4, r.hhi >>> 0, true); o += 8; }
    for (k = 0; k < r.ev.length; k++) { var e = r.ev[k]; b[o] = e[0]; for (var j = 1; j < 5; j++) d.setInt32(o + 1 + 4 * (j - 1), e[j] | 0, true); o += 17; } }
  return b;
}
let seed = 7; const rnd = (n) => { seed = (seed * 1103515245 + 12345) >>> 0; return (seed >>> 8) % n; };
const mk = (ns) => { const k = []; for (let s = 0; s < ns; s++) k.push(rnd(3) ? 0 : rnd(1024)); const ev = []; const ne = rnd(4) ? 0 : rnd(3);
  for (let i = 0; i < ne; i++) ev.push([rnd(11), rnd(1e6) - 5e5, rnd(100), rnd(5), -rnd(9)]);
  const r = { k0: k[0], k1: k[1], k2: k[2], k3: k[3], hashed: rnd(16) ? 0 : 1, hlo: rnd(1 << 30), hhi: rnd(1 << 30), ev }; if (ns > 4) r.k = k; return r; };
let same = 0, rt = 0;
for (let t = 0; t < 2000; t++) {
  const recs4 = Array.from({ length: rnd(64) }, () => mk(4));
  const a = L.encSealed(1000 + t, 900, recs4, 5, -3, 777, 1.5), b = oldEnc(1000 + t, 900, recs4, 5, -3, 777, 1.5);
  if (Buffer.compare(Buffer.from(a), Buffer.from(b))) throw new Error('4-slot bytes differ at ' + t);
  same++;
  for (const ns of [8, 16]) {
    const recs = Array.from({ length: rnd(64) }, () => mk(ns));
    const enc = L.encSealed(5, 3, recs, 1, 2, 3, 4), dec = L.decSealed(enc);
    let bytes = 23; recs.forEach((r) => { bytes += L.recBytes(r); });
    if (bytes !== enc.length) throw new Error('recBytes');
    recs.forEach((r, i) => { const d = dec.recs[i], want = r.k, got = L.recKeys(d);
      for (let s = 0; s < ns; s++) if ((want[s] | 0) !== (got[s] | 0)) throw new Error(`slot ${s} rec ${i}: ${want[s]} vs ${got[s]}`);
      if (d.hashed !== r.hashed || (r.hashed && (d.hlo !== r.hlo || d.hhi !== r.hhi))) throw new Error('hash');
      if (JSON.stringify(d.ev) !== JSON.stringify(r.ev)) throw new Error('ev'); });
    rt++;
  }
}
// eight slots, all held: the bytes per record
const full = { k0: 1, k1: 1, k2: 1, k3: 1, k: [1, 1, 1, 1, 1, 1, 1, 1], hashed: 0, ev: [] };
console.log('4-slot byte-identical', same, 'round trips', rt, 'record with 8 pads held', L.recBytes(full), 'B; 64 of them', 23 + 64 * L.recBytes(full), 'B');
// The connection readout (web/ln_session.js "the connection readout"): the
// host's table reaches every guest and the host's own engine as sent, rows
// clamped, the late share over the window; no simulation call on the way.
{
  const sim = [];
  const eng = { frame: () => 100, seatRequest: () => 1, setSeatSlots() {}, slots: () => 16, maxDevices: () => 16,
                imageRequest: () => sim.push('imageRequest'), setNetQuality: (q, stale) => sim.push(['host', q, stale]) };
  const hub = new L.HostHub(eng, () => 0, { seats: 8 });
  const sent = [];
  for (const d of [1, 2]) hub.add(d, { datagram() {}, reliable: (m, n) => sent.push([d, m, n]) });
  const rows = [L.netqRow(1, 41.6, L.NETQ_PATH.DIRECT, 2.4), L.netqRow(2, null, L.NETQ_PATH.RELAY, 250), L.netqRow(3, 12000, 0, -5)];
  if (JSON.stringify(rows) !== JSON.stringify([[1, 42, 1, 2], [2, -1, 2, 100], [3, 9999, 0, -1]])) throw new Error('netqRow ' + JSON.stringify(rows));
  hub.sendNetQuality(rows);
  const msgs = sent.filter((x) => x[1].t === L.NETQ_MSG);
  if (msgs.length !== 2 || msgs.some((x) => x[1].q !== rows || x[2] > 64)) throw new Error('netq to every guest: ' + JSON.stringify(msgs));
  if (!sim.some((x) => x[0] === 'host' && x[1] === rows && x[2] === false)) throw new Error('netq on the host');
  const got = [];
  const gs = new L.GuestSession({ setNetQuality: (q, stale) => got.push([q, stale]) }, { datagram() {}, reliable() {} }, () => 5);
  gs.onReliable(msgs[0][1]);
  if (got.length !== 1 || got[0][0] !== rows || got[0][1] !== false || gs.netq !== rows || gs.netqAt !== 5) throw new Error('netq on a guest');
  const late = new L.NetqLate(5);
  if (late.sample({ inputFrames: 100, inputsUseless: 0 }) !== null) throw new Error('late: first sample');
  if (late.sample({ inputFrames: 160, inputsUseless: 6 }) !== 10) throw new Error('late: 6 of 60');
  for (let i = 0; i < 6; i++) late.sample({ inputFrames: 220 + 60 * i, inputsUseless: 6 });
  if (late.sample({ inputFrames: 600, inputsUseless: 6 }) !== 0) throw new Error('late: the window forgets');
  console.log('netq: table to', msgs.length, 'guests and the host,', msgs[0][2], 'B; late share windowed');
}
