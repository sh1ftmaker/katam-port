// audio_compare.js -- what two captured audio streams sound like, in numbers.
//
//   node tools/netaudit/audio_compare.js A.f32 RATE_A B.f32 RATE_B [--json]
//
// The captures are raw float32 stereo (tools/netaudit/run_node.js, audio
// "capture"): what the device was handed.  Reports, for each and between the
// two: length in seconds, RMS, spectrum by third-octave band (B relative to A,
// dB), clicks (sample-to-sample steps far above the local level), and timing
// (the lag of B against A, from cross-correlation of the mono envelope... of
// the signal itself, at several points: a stream that is pitch- and
// speed-true keeps a constant lag).  Identical bytes are reported as such.
'use strict';
const fs = require('fs');

function load(path) {
    const b = fs.readFileSync(path);
    return new Float32Array(b.buffer, b.byteOffset, b.byteLength >> 2);
}

function mono(x) {
    const n = x.length >> 1, m = new Float32Array(n);
    for (let i = 0; i < n; i++) m[i] = 0.5 * (x[2 * i] + x[2 * i + 1]);
    return m;
}

// Linear resample (analysis only), so two rates can be compared.
function resample(m, from, to) {
    if (from === to) return m;
    const n = Math.floor(m.length * to / from), o = new Float32Array(n), r = from / to;
    for (let i = 0; i < n; i++) {
        const p = i * r, k = Math.floor(p), f = p - k;
        o[i] = (m[k] || 0) * (1 - f) + (m[k + 1] || 0) * f;
    }
    return o;
}

function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
    }
    for (let len = 2; len <= n; len <<= 1) {
        const a = -2 * Math.PI / len, wr = Math.cos(a), wi = Math.sin(a);
        for (let i = 0; i < n; i += len) {
            let cr = 1, ci = 0;
            for (let k = 0; k < len / 2; k++) {
                const ur = re[i + k], ui = im[i + k];
                const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

// Mean power spectrum (Welch, Hann, 4096) folded into third-octave bands.
function bands(m, rate) {
    const N = 4096, hop = 2048, P = new Float64Array(N / 2);
    let frames = 0;
    const re = new Float64Array(N), im = new Float64Array(N);
    for (let s = 0; s + N <= m.length; s += hop * 8) {      // every 8th hop: enough
        for (let i = 0; i < N; i++) { re[i] = m[s + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / N)); im[i] = 0; }
        fft(re, im);
        for (let k = 0; k < N / 2; k++) P[k] += re[k] * re[k] + im[k] * im[k];
        frames++;
    }
    const out = [];
    for (let fc = 50; fc <= 20000; fc *= Math.pow(2, 1 / 3)) {
        const lo = fc / Math.pow(2, 1 / 6), hi = fc * Math.pow(2, 1 / 6);
        let e = 0;
        for (let k = Math.ceil(lo * N / rate); k <= Math.floor(hi * N / rate) && k < N / 2; k++) e += P[k];
        out.push([Math.round(fc), e / Math.max(1, frames)]);
    }
    return out;
}

function clicks(x) {
    // a step of more than 8x the running mean absolute step, and > 0.05 FS
    let n = 0, avg = 0.001;
    for (let i = 2; i < x.length; i += 2) {
        const d = Math.abs(x[i] - x[i - 2]);
        if (d > 0.05 && d > 8 * avg) n++;
        avg += (d - avg) / 64;
    }
    return n;
}

function rms(m) { let s = 0; for (const v of m) s += v * v; return Math.sqrt(s / Math.max(1, m.length)); }

// Lag of b against a around position p (samples), +-maxLag, window w.
function lagAt(a, b, p, w, maxLag) {
    let best = 0, bestC = -2;
    let ea = 0;
    for (let i = 0; i < w; i++) ea += a[p + i] * a[p + i];
    for (let L = -maxLag; L <= maxLag; L++) {
        let c = 0, eb = 0;
        for (let i = 0; i < w; i++) { const y = b[p + i + L] || 0; c += a[p + i] * y; eb += y * y; }
        const r = c / Math.sqrt(ea * eb + 1e-20);
        if (r > bestC) { bestC = r; best = L; }
    }
    return [best, +bestC.toFixed(3)];
}

function main() {
    const [pa, ra, pb, rb] = process.argv.slice(2);
    const A = load(pa), B = load(pb), RA = +ra, RB = +rb;
    const identical = A.length === B.length && Buffer.compare(fs.readFileSync(pa), fs.readFileSync(pb)) === 0;
    const ma = mono(A), mb0 = mono(B), mb = resample(mb0, RB, RA);
    const ba = bands(ma, RA), bb = bands(mb, RA);
    const rel = ba.map(([f, e], i) => [f, e > 0 && bb[i][1] > 0 ? +(10 * Math.log10(bb[i][1] / e)).toFixed(1) : null]);
    const lags = [];
    for (const sec of [10, 30, 50, 70, 90, 110]) {
        const p = Math.floor(sec * RA);
        if (p + 8192 + 4000 < Math.min(ma.length, mb.length)) lags.push([sec, ...lagAt(ma, mb, p, 8192, 2000)]);
    }
    const res = {
        identical, secondsA: +(ma.length / RA).toFixed(3), secondsB: +(mb0.length / RB).toFixed(3),
        rmsA: +rms(ma).toFixed(4), rmsB: +rms(mb0).toFixed(4), clicksA: clicks(A), clicksB: clicks(B),
        bandsDbBvsA: rel, lagSamplesAtSeconds: lags,
    };
    if (process.argv.includes('--json')) { console.log(JSON.stringify(res)); return; }
    console.log(`identical bytes: ${identical}`);
    console.log(`length: A ${res.secondsA} s, B ${res.secondsB} s; RMS A ${res.rmsA}, B ${res.rmsB}; clicks A ${res.clicksA}, B ${res.clicksB}`);
    console.log('spectrum, B relative to A (dB) by third-octave: ' + rel.map(([f, d]) => `${f}:${d}`).join(' '));
    console.log('lag of B vs A [s, samples, corr]: ' + lags.map((x) => `[${x.join(', ')}]`).join(' '));
}

main();
