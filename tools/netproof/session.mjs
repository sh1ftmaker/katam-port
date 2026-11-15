// The online session's logic for each side lives in web/ln_session.js, the
// file the page ships; this is the rig's door to it, with node's zlib as the
// image codec and the rig's engine adapter over an instance.
// (tools/netproof/link.mjs is one transport, a simulated link; the page's
// WebRTC data channels or the relay are another.)
import zlib from 'zlib';
import { createRequire } from 'module';

const require = createRequire(import.meta.url);
const L = require('../../web/ln_session.js');

export const PERIOD = L.PERIOD;

const NODE_CODEC = {
    deflate: (b) => zlib.deflateRawSync(b, { level: 6 }),
    inflate: (b) => zlib.inflateRawSync(b),
};

// --- engine adapter ---------------------------------------------------------
export function engineOf(inst) {
    const M = inst.M;
    return {
        frame: () => M._PortFrameNumber(),
        localInput: (f, k, seat = 0) => M._PortLnLocalInputSeat(f, seat, k),
        remoteInput: (f, k, dev = 1, seat = 0) => M._PortLnRemoteInputSeat(f, dev, seat, k),
        sealedGet: (f) => inst.sealedGet(f),
        sealedEvent: (f, e) => M._PortLnSealedEvent(f, e[0], e[1], e[2], e[3], e[4]),
        sealed: (f, k0, k1, nev, k2 = 0, k3 = 0) => M._PortLnSealed4(f, k0, k1, k2, k3, nev),
        remoteHash: (f, lo, hi) => M._PortLnRemoteHash(f, lo, hi),
        event: (t, a, b, c, d) => M._PortLnEvent(t, a, b, c, d),
        tel: () => inst.tel.read(),
        imageRequest: () => M._PortLnImageRequest(),
        image: () => inst.image(),
        loadImage: (b) => inst.loadImage(b),
        seatRequest: (dev, seat, on) => M._PortLnSeatRequest(dev, seat, on),
        deviceGone: (dev) => M._PortLnDeviceGone(dev),
        freeSlots: () => M._PortLnFreeSlots(),
        setSelf: (dev) => M._PortLnSetSelf(dev),
        selfSeats: () => M._PortLnSelfSeats(),
        seatAskPoll: () => M._PortLnSeatAskPoll(),
        slotOwner: (s) => M._PortLnSlotOwner(s),
    };
}

export class HostSession extends L.HostSession {
    constructor(eng, tx, clock, opts = {}) { super(eng, tx, clock, Object.assign({ codec: NODE_CODEC }, opts)); }
}
export class HostHub extends L.HostHub {
    constructor(eng, clock, opts = {}) { super(eng, clock, Object.assign({ codec: NODE_CODEC }, opts)); }
}
export class GuestSession extends L.GuestSession {
    constructor(eng, tx, clock, opts = {}) { super(eng, tx, clock, Object.assign({ codec: NODE_CODEC }, opts)); }
}
