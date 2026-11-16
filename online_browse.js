/* The server browser, the page's half: public games and passwords.
 *
 * The screens are the game's (platform/online_browse.c, on the ONLINE screens
 * of platform/online_ui.c); web/online.js runs the rooms and calls in here
 * at four points (marked "server browser" there).  This file:
 *
 *   list      while ONLINE > BROWSE GAMES is up: GET /v2/public at once and
 *             every 5 s, into the screen (PortObrListSet).  A server without
 *             the list (404: the worker before netplay/listing.mjs) shows
 *             "SERVER NEEDS AN UPDATE"; hosting and joining by code do not
 *             need it and work as before.
 *   publish   a host that chose PUBLIC: POST /v2/public with the room's code
 *             and resume token (the ownership proof), the card's name, the
 *             players, the room's seats (`max`: 4, or 8 in an 8-player game)
 *             and module (`mod`: '16' for the 8-player module), the mode and
 *             whether there is a password -- at once,
 *             on every change, and as a heartbeat (every 25 s, the server's
 *             `every`).  PRIVATE again, the game ending or the page closing:
 *             POST /v2/public/remove.  The server also drops it when the room
 *             empties or the heartbeat stops (75 s).
 *   password  checked on the HOST's device, inside web/online.js's `hi`
 *             handshake, by challenge and response:
 *               host -> guest  {k:'pw', sid, nonce}         (16 random bytes, hex)
 *               guest -> host  {k:'pwr', sid, h}            h = hex SHA-256 of
 *                                                          "katam-pw1:" + nonce + ":" + PASSWORD
 *               host -> guest  hiok (as before) | {k:'pwbad', sid, nonce, left}
 *                              | {k:'pwlock', sid} after MAX_TRIES wrong
 *             then the host stops answering that device (its tab token and
 *             its room member id).  The password never goes to the server:
 *             the room relays these like any `signal`, and a listing only
 *             says locked yes / no.  A door latch, not security: see
 *             docs or the report -- anyone who has the password can pass it
 *             on, the relay sees the nonce and the hash, and four letters can
 *             be guessed offline from one captured exchange.
 *
 * The list shows "players/max" from the listing's `max` when it has one, else
 * "/4".  A worker from before `max` and `mod` (netplay/listing.mjs with
 * MAX_PLAYERS 4) clamps n to 4, says max 4 and full from 4 players on, and
 * drops `mod`: an 8-player game then lists as n/4 and FULL from its fourth
 * player (joining it by code or invite still works; the room's version gate
 * moves a joiner on the wrong module over).
 *
 * Joiners type the password on the game's PASSWORD screen (it opens by
 * itself when the host asks) -- or, for an invite link, which joins from
 * wherever the game is, in a small box over the picture. */
(function () {
    'use strict';
    var Net = window.KatamNet;
    if (!Net) return;
    var M = window.Module;
    var q = new URLSearchParams(location.search);
    var BASE = (q.get('netserver') || Net.DEFAULT_BASE).replace(/\/+$/, '').replace(/\/parties\/.*$/, '');

    /* platform/port/online_ui.h, platform/port/online_browse.h */
    var UI = { J_CONNECTING: 6, J_PASSWORD: 15, J_PWBAD: 16, J_PWLOCK: 17 };
    var LIST = { LOADING: 0, OK: 1, NOCONN: 2, OLD: 3, BUSY: 4 };
    var HOSTL = { PRIVATE: 0, PUBLISHING: 1, LISTED: 2, OLD: 3, NOCONN: 4, LIMIT: 5 };
    var EV_PW_ANSWER = 1;
    var POLL_MS = 5000, MAX_TRIES = 5;

    function log(t) {
        t = '[browse] ' + t;
        console.log(t);
        if (M.portDiag) { try { M.portDiag(t, 0); } catch (e) { /* console only */ } }
    }
    function up() { return !!(M.portRuntimeUp && M._PortObrBrowsing); }
    function now() { return Date.now(); }
    function str(fn) { try { return M.ccall(fn, 'string', [], []) || ''; } catch (e) { return ''; } }

    function call(method, path, body, keepalive) {
        return fetch(BASE + path, {
            method: method, keepalive: !!keepalive,
            headers: body ? { 'Content-Type': 'application/json' } : undefined,
            body: body ? JSON.stringify(body) : undefined,
        }).then(function (r) {
            return r.json().catch(function () { return {}; }).then(function (j) {
                j = j || {};
                j.status = r.status;
                j.retry = +(r.headers.get('Retry-After') || j.retryAfter || 0);
                return j;
            });
        }, function (e) { return { status: 0, error: 'network', msg: e.message }; });
    }
    /* a server from before the list: no such endpoint */
    function missing(r) { return r.status === 404 || r.status === 405 || r.status === 501; }

    function hex(buf) {
        var a = new Uint8Array(buf), s = '';
        for (var i = 0; i < a.length; i++) s += (a[i] < 16 ? '0' : '') + a[i].toString(16);
        return s;
    }
    function nonce() {
        var b = new Uint8Array(16);
        crypto.getRandomValues(b);
        return hex(b);
    }
    function pwHash(n, pw) {
        if (!crypto.subtle) return Promise.reject(new Error('no WebCrypto (not a secure page)'));
        return crypto.subtle.digest('SHA-256', new TextEncoder().encode('katam-pw1:' + n + ':' + String(pw).toUpperCase()))
            .then(hex);
    }

    var st = {
        browsing: false, nextList: 0, listInFlight: false, listStatus: -1, games: [], lists: 0,
        host: null,               /* {code, resume, players, listed, sig, next, retryAt, dead, state, inflight} */
        fails: {}, pending: {},   /* host: wrong answers by device; challenges out */
        guest: null,              /* guest: the challenge being answered {nonce, answer, code} */
        cache: {},                /* guest: the password that worked, by code (a rejoin) */
        events: [],
    };

    /* --- the list -------------------------------------------------------------- */
    function setStatus(s) {
        st.listStatus = s;
        if (up()) M._PortObrListStatus(s);
    }
    function fetchList() {
        st.listInFlight = true;
        call('GET', '/v2/public').then(function (r) {
            st.listInFlight = false;
            st.lists++;
            if (!st.browsing) return;
            if (r.status === 200 && r.games) {
                st.games = r.games;
                var packed = r.games.map(function (g) {
                    var max = g.max >= 2 && g.max <= 16 ? g.max | 0 : 4;
                    var full = g.max ? g.n >= max : g.full;
                    var fl = (g.locked ? 1 : 0) | (g.mode === 'roam' ? 2 : 0) | (full ? 4 : 0);
                    return [g.code, String(g.name || '').replace(/[\t\n]/g, ' '), g.n, fl, max].join('\t');
                }).join('\n');
                try { M.ccall('PortObrListSet', null, ['string'], [packed]); } catch (e) { log('list: ' + e); }
                setStatus(LIST.OK);
                st.nextList = now() + POLL_MS;
                return;
            }
            if (missing(r)) {
                if (st.listStatus !== LIST.OLD) log('this game server has no public list yet (it needs an update)');
                setStatus(LIST.OLD);
                st.nextList = now() + 30000;
            } else if (r.status === 429) {
                setStatus(LIST.BUSY);
                st.nextList = now() + Math.max(POLL_MS, r.retry * 1000);
            } else {
                setStatus(LIST.NOCONN);
                st.nextList = now() + POLL_MS;
            }
        });
    }

    /* --- the host's listing ------------------------------------------------------- */
    function hostSet(s) {
        if (!st.host) return;
        st.host.state = s;
        if (up()) M._PortObrSetHostList(s);
    }
    function fields() {
        var h = st.host, name = str('PortCardNameJs');
        var f = { code: h.code, resume: h.resume, name: name || 'PLAYER', n: h.players(),
                  mode: M._PortOuiRoomsMode && M._PortOuiRoomsMode() ? 'roam' : 'together',
                  locked: !!str('PortObrHostPassJs') };
        /* an 8-player game: its seats and module (a worker before them ignores both) */
        if (h.max && h.max !== 4) f.max = h.max;
        if (h.mod) f.mod = h.mod;
        return f;
    }
    function publish(f) {
        var h = st.host, sig = JSON.stringify(f);
        h.inflight = true;
        h.lastSent = now();
        if (!h.listed) hostSet(HOSTL.PUBLISHING);
        call('POST', '/v2/public', f).then(function (r) {
            h.inflight = false;
            if (st.host !== h) return;
            if (r.status === 200) {
                if (!h.listed) log('game ' + h.code + ' is on the public list');
                h.listed = true; h.sig = sig;
                h.next = now() + (r.every || 25) * 1000;
                hostSet(HOSTL.LISTED);
            } else if (missing(r)) {
                log('this game server cannot list games yet (it needs an update); the game stays private');
                h.dead = true; hostSet(HOSTL.OLD);
            } else if (r.status === 403) {
                h.dead = true; h.listed = false; hostSet(HOSTL.NOCONN);
            } else if (r.status === 409) {
                h.listed = false; h.retryAt = now() + 60000; hostSet(HOSTL.LIMIT);
            } else if (r.status === 429) {
                h.retryAt = now() + Math.max(2, r.retry) * 1000;
            } else {
                h.retryAt = now() + 10000; hostSet(HOSTL.NOCONN);
            }
        });
    }
    function unpublish(h, keepalive) {
        if (!h || !h.listed) return;
        h.listed = false;
        log('game ' + h.code + ' off the public list');
        call('POST', '/v2/public/remove', { code: h.code, resume: h.resume }, keepalive);
    }
    function hostTick() {
        var h = st.host;
        if (!h || !up()) return;
        var role = window.katamOnline && katamOnline.role ? katamOnline.role() : null;
        if (role !== 'host') { unpublish(h); st.host = null; return; }
        var want = !!M._PortObrHostPublic();
        if (want !== h.want) {
            h.want = want;
            h.dead = false; h.retryAt = 0;
            if (!want) { unpublish(h); hostSet(HOSTL.PRIVATE); }
        }
        if (!want || h.dead || h.inflight) return;
        var t = now(), f = fields(), sig = JSON.stringify(f);
        var due = !h.listed || sig !== h.sig || t >= h.next;
        if (due && t >= h.retryAt && t - (h.lastSent || 0) >= 2000) publish(f);
    }

    /* --- passwords: the host ------------------------------------------------------ */
    function devKey(m, d) { return d.tok ? 't' + d.tok : 'm' + m.from; }
    function tooMany(m, d) {
        return (st.fails[devKey(m, d)] || 0) >= MAX_TRIES || (st.fails['m' + m.from] || 0) >= MAX_TRIES;
    }
    function challenge(net, m, d, kind, extra) {
        var c = { nonce: nonce(), sid: d.sid, m: m, d: d, net: net };
        st.pending[m.from] = c;
        var data = { k: kind, sid: d.sid, nonce: c.nonce };
        if (extra) for (var k in extra) data[k] = extra[k];
        net.send({ t: 'signal', to: m.from, data: data });
    }
    /* A `hi` on a locked game: true = taken over here (challenged, or a
     * device that tried too often: no answer at all). */
    function gate(m, d, net) {
        if (!str('PortObrHostPassJs')) return false;
        if (tooMany(m, d)) { log('ignoring a device: too many wrong passwords'); return true; }
        challenge(net, m, d, 'pw');
        return true;
    }
    function verify(m, d, net, proceed) {
        var c = st.pending[m.from];
        if (!c || c.sid !== d.sid || c.net !== net || typeof d.h !== 'string') return;
        var pw = str('PortObrHostPassJs');
        delete st.pending[m.from];
        if (!pw) { proceed(c.m); return; }          /* the password was taken off meanwhile */
        pwHash(c.nonce, pw).then(function (h) {
            if (h === d.h.toLowerCase()) { log('password right'); proceed(c.m); return; }
            var k = devKey(c.m, c.d);
            st.fails[k] = (st.fails[k] || 0) + 1;
            if (k !== 'm' + m.from) st.fails['m' + m.from] = (st.fails['m' + m.from] || 0) + 1;
            var left = MAX_TRIES - Math.max(st.fails[k], st.fails['m' + m.from] || 0);
            log('wrong password (' + Math.max(0, left) + ' tries left for that device)');
            if (left <= 0) net.send({ t: 'signal', to: m.from, data: { k: 'pwlock', sid: c.sid } });
            else challenge(net, c.m, c.d, 'pwbad', { left: left });
        }, function (e) { log('cannot check passwords here: ' + e.message); });
    }

    /* --- passwords: the guest -------------------------------------------------- */
    function ouiState(s) { if (up()) M._PortOuiSet(s, 0, 0, 0, 0); }
    function answerWith(pw) {
        var g = st.guest;
        if (!g || !pw) return;
        st.cache[g.code] = String(pw).toUpperCase();
        pwHash(g.nonce, pw).then(function (h) {
            if (st.guest !== g) return;
            st.guest = null;
            g.answer(h);
        }, function (e) { log('cannot answer the password here: ' + e.message); });
    }
    /* the host asked (`pw`) or said wrong (`pwbad`, with a new nonce) */
    function onChallenge(d, code, invite, answer) {
        st.guest = { nonce: d.nonce, answer: answer, code: code };
        if (d.k === 'pwbad') delete st.cache[code];
        else if (st.cache[code]) { answerWith(st.cache[code]); return; }      /* back after a drop */
        log(d.k === 'pwbad' ? 'wrong password, ' + d.left + ' tries left' : 'the game ' + code + ' asks for a password');
        if (invite) box(code, d.k === 'pwbad');
        else ouiState(d.k === 'pwbad' ? UI.J_PWBAD : UI.J_PASSWORD);
    }

    /* the invite link's box (the game may be anywhere, not on a menu) */
    function box(code, wrong) {
        var el = document.getElementById('obrpw');
        if (!el) {
            el = document.createElement('form');
            el.id = 'obrpw';
            el.style.cssText = 'position:fixed;left:50%;top:38%;transform:translate(-50%,-50%);z-index:80;' +
                'width:min(88vw,360px);padding:14px 16px;border-radius:12px;background:rgba(24,16,64,.94);' +
                'border:3px solid #f8d850;color:#fff;font:bold 15px/1.35 ui-monospace,Menlo,Consolas,monospace;' +
                'letter-spacing:.05em;text-transform:uppercase;box-shadow:0 4px 0 rgba(0,0,0,.35);text-align:center';
            el.innerHTML = '<div id="obrpwmsg"></div>' +
                '<input id="obrpwin" autocomplete="off" autocapitalize="characters" spellcheck="false" maxlength="8" ' +
                'style="margin:10px 0;width:80%;font:bold 20px ui-monospace,monospace;text-align:center;letter-spacing:.2em;' +
                'text-transform:uppercase;padding:6px;border-radius:8px;border:2px solid #f8d850">' +
                '<div><button id="obrpwok" type="submit" style="margin:0 6px;padding:6px 14px;border-radius:10px;border:2px solid #f8d850;' +
                'background:#f8d850;color:#181040;font:inherit">Join</button>' +
                '<button id="obrpwno" type="button" style="margin:0 6px;padding:6px 14px;border-radius:10px;border:2px solid #f8d850;' +
                'background:transparent;color:#fff;font:inherit">Cancel</button></div>';
            document.body.appendChild(el);
            el.addEventListener('submit', function (e) {
                e.preventDefault();
                var v = document.getElementById('obrpwin').value.toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 8);
                if (!v) return;
                el.style.display = 'none';
                answerWith(v);
            });
            document.getElementById('obrpwno').addEventListener('click', function () {
                el.style.display = 'none';
                st.guest = null;
                if (window.katamOnline && katamOnline.cancelJoin) katamOnline.cancelJoin();
            });
        }
        document.getElementById('obrpwmsg').textContent = wrong ? 'Wrong password - try again' : 'Game ' + code + ' needs a password';
        document.getElementById('obrpwin').value = '';
        el.style.display = 'block';
        setTimeout(function () { try { document.getElementById('obrpwin').focus(); } catch (e) { /* no focus */ } }, 0);
    }

    /* --- typing a password with a keyboard (the card's NAME row does the same) --- */
    window.addEventListener('keydown', function (e) {
        if (!up() || e.ctrlKey || e.metaKey || e.altKey) return;
        if (document.activeElement && /^(INPUT|TEXTAREA)$/.test(document.activeElement.tagName)) return;
        var typing = false;
        try { typing = !!M._PortObrTyping(); } catch (err) { return; }
        if (!typing) return;
        var printable = e.key && e.key.length === 1 && /[A-Za-z0-9]/.test(e.key);
        if (!printable && e.key !== 'Enter' && e.key !== 'Escape' && e.key !== 'Backspace') return;
        e.preventDefault();
        e.stopImmediatePropagation();         /* a letter of the password, not a key of the game */
        if (e.repeat) return;
        if (e.key === 'Enter' || e.key === 'Escape') M._PortObrTypingEnd();
        else if (e.key === 'Backspace') M._PortObrTypeChar(8);
        else M._PortObrTypeChar(e.key.toUpperCase().charCodeAt(0));
    }, true);

    /* ... and on a phone, a button over the picture for the device's keyboard */
    var padBtn = null;
    function phoneButton() {
        var on = false;
        try { on = up() && !!M._PortObrWantsText(); } catch (e) { on = false; }
        if (on && !padBtn) {
            padBtn = document.createElement('button');
            padBtn.id = 'obrpwbtn';
            padBtn.type = 'button';
            padBtn.textContent = 'Type password';
            padBtn.style.cssText = 'position:fixed;right:12px;top:12px;z-index:72;padding:8px 14px;border-radius:12px;' +
                'border:3px solid #f8d850;background:#f8d850;color:#181040;font:bold 14px/1.2 ui-monospace,Menlo,Consolas,monospace;' +
                'letter-spacing:.05em;text-transform:uppercase;box-shadow:0 4px 0 rgba(0,0,0,.35);cursor:pointer;display:none';
            padBtn.addEventListener('click', function () {
                var v = window.prompt('Password (up to 8 letters or digits)', '');
                if (v === null) return;
                try { M.ccall('PortObrSetTextJs', null, ['string'], [v]); } catch (e) { /* gone */ }
            });
            document.body.appendChild(padBtn);
        }
        if (padBtn) padBtn.style.display = on && matchMedia('(pointer: coarse)').matches ? 'block' : 'none';
    }

    /* --- the loop ---------------------------------------------------------------- */
    setInterval(function () {
        if (!up()) return;
        var ev;
        while ((ev = M._PortObrPoll())) {
            if (ev === EV_PW_ANSWER) answerWith(str('PortObrPassJs'));
        }
        var b = !!M._PortObrBrowsing();
        if (b && !st.browsing) { st.browsing = true; st.nextList = 0; setStatus(LIST.LOADING); }
        else if (!b) st.browsing = false;
        if (st.browsing && !st.listInFlight && now() >= st.nextList) fetchList();
        hostTick();
        phoneButton();
    }, 100);

    window.addEventListener('pagehide', function () { if (st.host) unpublish(st.host, true); });

    window.KatamBrowse = {
        UI: UI,
        /* web/online.js: a room of ours, its resume token, a head count */
        hosting: function (code, resume, players, opts) {
            if (st.host) unpublish(st.host);
            st.host = { code: code, resume: resume, players: players, listed: false, sig: null, next: 0,
                        retryAt: 0, dead: false, inflight: false, want: false, state: HOSTL.PRIVATE,
                        max: opts && opts.max || 4, mod: opts && opts.mod || null };
            st.fails = {}; st.pending = {};
            hostSet(HOSTL.PRIVATE);
        },
        ended: function () {
            if (st.host) { unpublish(st.host); st.host = null; }
            st.pending = {};
            st.guest = null;
            var el = document.getElementById('obrpw');
            if (el) el.style.display = 'none';
        },
        gate: gate, verify: verify, onChallenge: onChallenge,
        /* web/online.js: the module a listed game runs on ('16', '4'), or
         * null (not listed).  The list names only the 16-Kirby module's games
         * (netplay/listing.mjs keeps `mod` 16 or nothing): any other is a
         * 4-Kirby module's. */
        modOf: function (code) {
            for (var i = 0; i < st.games.length; i++)
                if (st.games[i].code === code) return st.games[i].mod === '16' ? '16' : '4';
            return null;
        },
        hash: pwHash,
        stats: function () {
            return { browsing: st.browsing, listStatus: st.listStatus, games: st.games, lists: st.lists,
                     host: st.host ? { code: st.host.code, listed: st.host.listed, state: st.host.state, want: st.host.want } : null,
                     fails: st.fails, guestAsked: !!st.guest };
        },
        base: BASE,
    };
}());
