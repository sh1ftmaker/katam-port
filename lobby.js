/* The online lobby (netplay protocol v2): create a game, show its 4-letter
 * code, join by code, seats, ready, host controls, and the session's status
 * once it runs.  Self-contained -- its own styles, its own DOM -- so the
 * shell only includes it and calls nothing: web/shell.html adds the script
 * tag and one guard line in its v1 hookup.
 *
 *   ?room=BCDF                    join (or rejoin) that game
 *   ?room=BCDF&relay=http://...   ... on a non-default server (dev relay,
 *                                 wrangler dev)
 *   ?room=BCDF&watch=1            join as a spectator
 *
 * Create/Join from the menu reload the page into ?room=CODE: a session
 * starts from power-on with blank save memory, so the game has to be held
 * at frame 0 -- which only a fresh page can promise.
 *
 * Needs web/net_session.js (window.KatamNet) and web/net_game.js
 * (window.createKatamNetGame), loaded before this.
 */
(function () {
    'use strict';
    if (typeof window === 'undefined' || !window.KatamNet) return;
    var Net = window.KatamNet;
    var q = new URLSearchParams(location.search);
    var CODE = q.get('room') ? Net.normalizeCode(q.get('room')) : null;
    var BASE = (q.get('relay') || Net.DEFAULT_BASE).replace(/\/+$/, '').replace(/\/parties\/.*$/, '');
    var KIRBY = ['Pink', 'Yellow', 'Red', 'Green'];

    /* The v1 hookup in shell.html asks this before it claims ?room=. */
    window.KatamLobby = { claims: !!CODE || q.has('online'), code: CODE, base: BASE };

    /* --- styles ------------------------------------------------------------- */
    var css = [
        '#netlobby{position:fixed;inset:0;z-index:60;display:none;align-items:center;justify-content:center;',
        ' background:rgba(8,10,13,.72);padding:max(12px,env(safe-area-inset-top)) 12px max(12px,env(safe-area-inset-bottom));}',
        '#netlobby.open{display:flex}',
        '#netlobby .nl-card{width:min(440px,100%);max-height:100%;overflow:auto;background:var(--panel,#1c1f25);',
        ' border:1px solid var(--edge,#2b3038);border-radius:12px;padding:16px 16px 14px;box-shadow:0 12px 40px rgba(0,0,0,.5);',
        ' color:var(--text,#d5d9e0);font:14px/1.45 var(--mono,monospace);user-select:text;-webkit-user-select:text}',
        '#netlobby h2{margin:0 0 10px;font-size:15px;letter-spacing:.04em;text-transform:uppercase;color:var(--dim,#838b98)}',
        '#netlobby .nl-row{display:flex;gap:8px;align-items:center;margin:8px 0;flex-wrap:wrap}',
        '#netlobby button{font:inherit;color:var(--text,#d5d9e0);background:var(--panel-2,#232730);border:1px solid var(--edge,#2b3038);',
        ' border-radius:8px;padding:9px 14px;min-height:42px;cursor:pointer}',
        '#netlobby button.nl-main{background:var(--accent,#6fb2ff);color:#08121f;border-color:transparent;font-weight:700}',
        '#netlobby button:disabled{opacity:.45;cursor:default}',
        '#netlobby button.nl-small{min-height:30px;padding:3px 9px;font-size:12px}',
        '#netlobby input{font:inherit;color:var(--text,#d5d9e0);background:var(--void,#0c0e11);border:1px solid var(--edge,#2b3038);',
        ' border-radius:8px;padding:9px 10px;min-height:42px;flex:1 1 140px;min-width:0}',
        '#netlobby input.nl-code{font-size:26px;letter-spacing:.35em;text-transform:uppercase;text-align:center;flex:1 1 160px;font-weight:700}',
        '#netlobby .nl-big{font-size:min(19vw,64px);letter-spacing:.18em;font-weight:800;text-align:center;margin:4px 0 2px;',
        ' color:var(--accent,#6fb2ff);user-select:all;-webkit-user-select:all}',
        '#netlobby .nl-hint{color:var(--dim,#838b98);font-size:12px}',
        '#netlobby .nl-err{color:var(--err,#ff7b72);background:rgba(255,123,114,.08);border:1px solid rgba(255,123,114,.3);',
        ' border-radius:8px;padding:8px 10px;margin:8px 0;white-space:pre-wrap}',
        '#netlobby .nl-ok{color:#7ee787}',
        '#netlobby .nl-seats{list-style:none;margin:8px 0;padding:0;max-height:46vh;overflow:auto}',
        '#netlobby .nl-seat{display:flex;align-items:center;gap:8px;padding:7px 8px;border-bottom:1px solid var(--hair,rgba(255,255,255,.09))}',
        '#netlobby .nl-seat .nl-n{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
        '#netlobby .nl-dot{width:9px;height:9px;border-radius:50%;flex:0 0 auto;background:#555}',
        '#netlobby .nl-dot.on{background:#3fb950}#netlobby .nl-dot.lag{background:var(--warn,#e8b23a)}',
        '#netlobby .nl-tag{font-size:11px;padding:1px 6px;border-radius:6px;background:var(--panel-2,#232730);color:var(--dim,#838b98)}',
        '#netlobby .nl-tag.k0{color:#ff9bd2}#netlobby .nl-tag.k1{color:#ffe46b}#netlobby .nl-tag.k2{color:#ff7b72}#netlobby .nl-tag.k3{color:#7ee787}',
        '#netlobby .nl-tag.rdy{color:#7ee787}',
        '#netchip{position:fixed;z-index:55;left:calc(10px + env(safe-area-inset-left));top:calc(52px + env(safe-area-inset-top));',
        ' display:none;gap:6px;align-items:center;background:rgba(20,22,26,.86);border:1px solid var(--edge,#2b3038);border-radius:999px;',
        ' padding:4px 10px;font:12px var(--mono,monospace);color:var(--text,#d5d9e0);cursor:pointer}',
        '#netchip.show{display:flex}',
        '#netlobby .nl-close{float:right;min-height:30px;padding:2px 10px}',
    ].join('\n');
    var style = document.createElement('style');
    style.textContent = css;
    document.head.appendChild(style);

    /* --- DOM ------------------------------------------------------------------ */
    var root = document.createElement('div');
    root.id = 'netlobby';
    root.setAttribute('role', 'dialog');
    root.setAttribute('aria-label', 'Online play');
    root.innerHTML = '<div class="nl-card"></div>';
    var card = root.firstChild;
    var chip = document.createElement('button');
    chip.id = 'netchip';
    chip.type = 'button';
    function mount() {
        document.body.appendChild(root);
        document.body.appendChild(chip);
        var bar = document.querySelector('#chrome .bar');
        if (bar) {
            var b = document.createElement('button');
            b.type = 'button';
            b.id = 'netonline';
            b.textContent = 'Play online…';
            b.onclick = function () { open(); };
            bar.insertBefore(b, bar.children[1] || null);
        }
        var top = document.getElementById('topbar');
        if (top) {
            var t = document.createElement('button');
            t.type = 'button';
            t.id = 'netonline2';
            t.textContent = 'Online';
            t.onclick = function () { open(); };
            var h1 = top.querySelector('h1');
            top.insertBefore(t, h1 ? h1.nextSibling : null);
        }
        render();
        if (CODE || q.has('online')) open();
    }
    chip.onclick = function () { open(); };
    root.addEventListener('click', function (e) { if (e.target === root && S.dismissable()) close(); });

    function open() { root.classList.add('open'); render(); }
    function close() { root.classList.remove('open'); render(); }

    function esc(s) {
        return String(s).replace(/[&<>"']/g, function (c) {
            return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
        });
    }

    function storedName() {
        try { return localStorage.katamNetName || ''; } catch (e) { return ''; }
    }
    function saveName(n) { try { localStorage.katamNetName = n; } catch (e) { /* private mode */ } }

    function identity() {
        /* Per tab, like the v1 driver: two tabs are two players. */
        var id = null;
        try { id = sessionStorage.katamNetIdentity; } catch (e) { /* none */ }
        if (!id) {
            id = Net.randomHex(16);
            try { sessionStorage.katamNetIdentity = id; } catch (e) { /* none */ }
        }
        return id;
    }

    function shareLink(code) {
        var u = new URL(location.href);
        u.search = '';
        u.searchParams.set('room', code);
        if (q.get('relay')) u.searchParams.set('relay', q.get('relay'));
        return u.toString();
    }

    function go(code, extra) {
        var u = new URL(location.href);
        u.search = '';
        u.searchParams.set('room', code);
        if (q.get('relay')) u.searchParams.set('relay', q.get('relay'));
        if (extra) for (var k in extra) u.searchParams.set(k, extra[k]);
        location.href = u.toString();
    }

    /* --- state ------------------------------------------------------------------ */
    var S = {
        view: CODE ? 'room' : 'home',
        busy: false, error: null, info: null,
        net: null, game: null, romWait: !!CODE,
        dismissable: function () { return !CODE || (S.net && S.net.phase === 'playing'); },
    };

    /* --- home: create / join -------------------------------------------------- */
    function renderHome() {
        var name = storedName();
        card.innerHTML =
            '<button class="nl-close nl-small" type="button" data-a="close">Close</button>' +
            '<h2>Play online</h2>' +
            '<div class="nl-row"><input id="nl-name" maxlength="20" placeholder="Your name" autocomplete="nickname" value="' + esc(name) + '"></div>' +
            '<div class="nl-row"><button class="nl-main" type="button" data-a="create" ' + (S.busy ? 'disabled' : '') + '>Create game</button>' +
            '<span class="nl-hint">You get a 4-letter code to share.</span></div>' +
            '<div class="nl-row" style="margin-top:14px"><input id="nl-code" class="nl-code" maxlength="64" placeholder="CODE" ' +
            'autocapitalize="characters" autocomplete="off" autocorrect="off" spellcheck="false" inputmode="text" enterkeyhint="go" aria-label="Room code">' +
            '<button type="button" data-a="join" ' + (S.busy ? 'disabled' : '') + '>Join</button></div>' +
            '<div class="nl-hint" id="nl-codehint">Codes are 4 letters, no vowels. Pasting a link works too.</div>' +
            (S.error ? '<div class="nl-err">' + esc(S.error) + '</div>' : '') +
            '<p class="nl-hint">Up to 4 players share one world; join or leave any time. ' +
            'Everyone needs their own copy of the same ROM. Your save file is not used.</p>';
        var codeIn = card.querySelector('#nl-code');
        codeIn.addEventListener('input', function () {
            var hint = Net.explainCode(codeIn.value);
            card.querySelector('#nl-codehint').textContent = hint ||
                ('Ready: ' + Net.normalizeCode(codeIn.value));
        });
        codeIn.addEventListener('keydown', function (e) { if (e.key === 'Enter') act('join'); });
    }

    function act(a, arg) {
        var nameIn = card.querySelector('#nl-name');
        if (nameIn) saveName(nameIn.value.trim().slice(0, 20));
        if (a === 'close') return close();
        if (a === 'create') {
            S.busy = true; S.error = null; render();
            Net.createRoom(BASE).then(function (r) {
                S.busy = false;
                if (!r.code) { S.error = Net.explainError(r) || 'Could not create a game.'; return render(); }
                try { if (r.resume) localStorage.setItem('katamNetResume.' + r.code, r.resume); } catch (e) { /* none */ }
                go(r.code, { host: '1' });
            });
            return;
        }
        if (a === 'join') {
            var raw = card.querySelector('#nl-code').value;
            var code = Net.normalizeCode(raw);
            if (!code) { S.error = Net.explainCode(raw); return render(); }
            S.busy = true; S.error = null; render();
            Net.lookupRoom(BASE, code).then(function (r) {
                S.busy = false;
                if (r.status !== 200) { S.error = Net.explainError(r); return render(); }
                go(code);
            });
            return;
        }
        var net = S.net;
        if (!net) return;
        switch (a) {
        case 'copy':
            var link = shareLink(CODE);
            if (navigator.share && /Mobi|Android|iPhone/i.test(navigator.userAgent))
                navigator.share({ title: 'Kirby & The Amazing Mirror', text: 'Join my game: ' + CODE, url: link }).catch(function () {});
            else if (navigator.clipboard)
                navigator.clipboard.writeText(link).then(function () { S.info = 'Link copied.'; render(); },
                                                         function () { S.info = link; render(); });
            else { S.info = link; render(); }
            return;
        case 'ready': net.ready(!me().ready); return;
        case 'sit': net.sit(); return;
        case 'stand': net.stand(); return;
        case 'start': net.start(); return;
        case 'lock': net.lock(!net.roster.locked); return;
        case 'approval': net.approval(!net.roster.approval); return;
        case 'kick': net.kick(arg); return;
        case 'approve': net.approve(arg, true); return;
        case 'deny': net.approve(arg, false); return;
        case 'rename': if (nameIn) net.rename(nameIn.value.trim()); return;
        case 'leave':
            net.leave();
            var u = new URL(location.href); u.search = ''; location.href = u.toString();
            return;
        case 'reload': location.reload(); return;
        case 'hide': close(); return;
        }
    }

    card.addEventListener('click', function (e) {
        var b = e.target.closest('[data-a]');
        if (b) act(b.getAttribute('data-a'), b.getAttribute('data-mid') | 0);
    });

    /* --- the room ------------------------------------------------------------------ */
    function me() {
        var n = S.net;
        if (!n || !n.roster) return {};
        for (var i = 0; i < n.roster.members.length; i++)
            if (n.roster.members[i].mid === n.mid) return n.roster.members[i];
        return {};
    }

    function connLine() {
        var n = S.net;
        if (S.romWait) return 'Load your ROM to join ' + CODE + ' (Menu → Load ROM…). It stays on this device.';
        if (!n) return 'Preparing…';
        if (n.state === 'connecting') return n.reconnects ? 'Reconnecting…' : 'Connecting…';
        if (n.state === 'lost') return 'Connection lost — reconnecting…';
        if (n.state === 'closed') return 'Disconnected.';
        var g = S.game && S.game.state;
        var parts = [];
        if (n.phase === 'lobby') parts.push(n.role === 'pending' ? 'Waiting for the host to let you in…' : 'In the lobby');
        else if (g && g.phase === 'live') parts.push('Playing — frame ' + (window.Module && Module._PortFrameNumber ? Module._PortFrameNumber() : '?'));
        else parts.push('Catching up with the game…');
        if (n.rtt !== null) parts.push(n.rtt + ' ms');
        if (g && g.syncs) parts.push(g.syncs + ' sync' + (g.syncs > 1 ? 's' : ''));
        return parts.join(' · ');
    }

    function renderRoom() {
        var n = S.net, r = n && n.roster, mine = me();
        var host = r && n.mid === r.host;
        var h = '';
        if (S.dismissable()) h += '<button class="nl-close nl-small" type="button" data-a="hide">Hide</button>';
        h += '<h2>Online game</h2>';
        h += '<div class="nl-big" aria-label="Room code">' + esc(CODE) + '</div>';
        h += '<div class="nl-row" style="justify-content:center"><button type="button" data-a="copy">Copy invite link</button>' +
             '<span class="nl-hint">or tell them the code</span></div>';
        if (S.info) h += '<div class="nl-hint" style="text-align:center;word-break:break-all">' + esc(S.info) + '</div>';
        h += '<div class="nl-hint" id="nl-conn">' + esc(connLine()) + '</div>';
        if (n && n.lastError) {
            h += '<div class="nl-err">' + esc(Net.explainError(n.lastError)) + '</div>';
            h += '<div class="nl-row"><button type="button" data-a="reload">Reload page</button><button type="button" data-a="leave">Leave</button></div>';
            card.innerHTML = h;
            return;
        }
        if (S.error) h += '<div class="nl-err">' + esc(S.error) + '</div>';
        if (r) {
            var seats = r.seats, bySeat = {}, others = [];
            r.members.forEach(function (m) { if (m.seat >= 0) bySeat[m.seat] = m; else others.push(m); });
            h += '<ul class="nl-seats">';
            for (var s = 0; s < seats; s++) {
                var m = bySeat[s];
                h += '<li class="nl-seat">';
                if (!m) {
                    h += '<span class="nl-dot"></span><span class="nl-n nl-hint">Seat ' + (s + 1) + ' — open' +
                         (s >= r.kirbys ? ' (no Kirby left)' : '') + '</span>';
                    if (mine.seat < 0 && mine.role !== 'pending' && n.state === 'open')
                        h += '<button class="nl-small" type="button" data-a="sit">Sit</button>';
                } else {
                    var lag = m.connected && m.behind > 30;
                    h += '<span class="nl-dot ' + (m.connected ? (lag ? 'lag' : 'on') : '') + '" title="' +
                         (m.connected ? (lag ? 'lagging' : 'connected') : 'away') + '"></span>';
                    h += '<span class="nl-n">' + esc(m.name) + (m.mid === n.mid ? ' (you)' : '') + '</span>';
                    if (m.kirby >= 0) h += '<span class="nl-tag k' + m.kirby + '">' + KIRBY[m.kirby] + '</span>';
                    if (m.host) h += '<span class="nl-tag">host</span>';
                    if (r.phase === 'lobby' && m.ready) h += '<span class="nl-tag rdy">ready</span>';
                    if (!m.connected) h += '<span class="nl-tag">away</span>';
                    else if (lag) h += '<span class="nl-tag">lagging</span>';
                    if (m.rtt !== null && m.connected) h += '<span class="nl-hint">' + m.rtt + 'ms</span>';
                    if (host && m.mid !== n.mid) h += '<button class="nl-small" type="button" data-a="kick" data-mid="' + m.mid + '">Kick</button>';
                }
                h += '</li>';
            }
            h += '</ul>';
            var watchers = others.filter(function (m) { return m.role !== 'pending'; });
            var pending = others.filter(function (m) { return m.role === 'pending'; });
            if (watchers.length)
                h += '<div class="nl-hint">Watching: ' + watchers.map(function (m) { return esc(m.name) + (m.mid === n.mid ? ' (you)' : ''); }).join(', ') + '</div>';
            if (host && pending.length) {
                h += '<div class="nl-hint" style="margin-top:6px">Asking to join:</div>';
                pending.forEach(function (m) {
                    h += '<div class="nl-row"><span class="nl-n">' + esc(m.name) + '</span>' +
                         '<button class="nl-small" type="button" data-a="approve" data-mid="' + m.mid + '">Let in</button>' +
                         '<button class="nl-small" type="button" data-a="deny" data-mid="' + m.mid + '">Decline</button></div>';
                });
            }
            h += '<div class="nl-row"><input id="nl-name" maxlength="20" placeholder="Your name" value="' + esc(mine.name || storedName()) + '">' +
                 '<button class="nl-small" type="button" data-a="rename">Rename</button></div>';
            h += '<div class="nl-row">';
            if (r.phase === 'lobby' && mine.seat >= 0)
                h += '<button type="button" data-a="ready">' + (mine.ready ? 'Not ready' : 'Ready') + '</button>';
            if (mine.seat >= 0) h += '<button type="button" data-a="stand">Watch instead</button>';
            if (host && r.phase === 'lobby') {
                var nReady = r.members.filter(function (m) { return m.seat >= 0 && (m.ready || m.host); }).length;
                var nSeat = r.members.filter(function (m) { return m.seat >= 0; }).length;
                h += '<button class="nl-main" type="button" data-a="start"' + (nSeat ? '' : ' disabled') + '>Start' +
                     (nSeat > nReady ? ' (' + (nSeat - nReady) + ' not ready)' : '') + '</button>';
            }
            h += '</div>';
            if (host) {
                h += '<div class="nl-row"><button class="nl-small" type="button" data-a="lock">' +
                     (r.locked ? 'Unlock lobby' : 'Lock lobby') + '</button>' +
                     '<button class="nl-small" type="button" data-a="approval">' +
                     (r.approval ? 'Anyone can join' : 'Approve joins') + '</button>' +
                     '<span class="nl-hint">' + (r.locked ? 'Locked: no new players.' : r.approval ? 'You approve each new player.' : '') + '</span></div>';
            } else if (r.phase === 'lobby') {
                h += '<div class="nl-hint">The host starts the game. Others can drop in later.</div>';
            }
            h += '<div class="nl-row" style="justify-content:flex-end"><button class="nl-small" type="button" data-a="leave">Leave game</button></div>';
        }
        card.innerHTML = h;
    }

    function renderChip() {
        var n = S.net;
        if (!n || root.classList.contains('open')) { chip.classList.remove('show'); return; }
        var r = n.roster;
        var players = r ? r.members.filter(function (m) { return m.seat >= 0 && m.connected; }).length : 0;
        var dot = n.state === 'open' ? '●' : '○';
        chip.textContent = dot + ' ' + CODE + ' · ' + players + 'P' + (n.state !== 'open' ? ' · reconnecting' : '');
        chip.classList.add('show');
    }

    function render() {
        if (S.view === 'home') renderHome(); else renderRoom();
        renderChip();
    }

    /* --- session wiring ----------------------------------------------------------- */
    function sha1Hex(bytes) {
        if (!crypto.subtle) return Promise.resolve('nosubtle');
        return crypto.subtle.digest('SHA-1', bytes).then(function (d) {
            var a = new Uint8Array(d), s = '';
            for (var i = 0; i < a.length; i++) s += (a[i] < 16 ? '0' : '') + a[i].toString(16);
            return s;
        });
    }

    function simVersion() {
        if (window.KATAM_SIM) return Promise.resolve(window.KATAM_SIM);
        /* An unstamped (dev) page: the wasm itself is the version. */
        return fetch('katam.wasm').then(function (r) { return r.arrayBuffer(); })
            .then(function (b) { return sha1Hex(b); })
            .then(function (h) { return 'dev.w' + h.slice(0, 12); }, function () { return 'dev'; });
    }

    function startSession() {
        var M = window.Module;
        if (!M) return;
        /* Blank, unpersisted save memory: a session starts from power-on
         * for everyone, and must never write over the player's own save. */
        M.portSramRestore = function () { return 0; };
        M.portSramDirty = function () {};
        M.portRomReady = M.portRomReady.then(function () {
            S.romWait = false;
            render();
            var romSize = (window.shell && shell.rom && shell.rom.size) || 16 * 1024 * 1024;
            var rom = M.HEAPU8.subarray(0x08000000, 0x08000000 + romSize);
            return Promise.all([sha1Hex(rom.slice()), simVersion()]).then(function (v) {
                var dpr = 1;
                var net = Net.connect({
                    base: BASE, code: CODE, identity: identity(),
                    log: function (t) { if (M.portDiag) M.portDiag(t, 0); else console.log(t); },
                    hello: {
                        sim: v[1], rom: v[0], content: 'vanilla',
                        name: storedName() || 'Player', platform: 'web',
                        caps: { seats: 4, sync: !!M._PortRbSnapshotAt },
                        want: q.get('watch') ? 'watch' : 'play',
                        view: { w: 240 * dpr, h: 160 * dpr },
                    },
                });
                S.net = net;
                ['state', 'roster', 'welcome', 'error', 'start', 'stats', 'desync', 'notice'].forEach(function (ev) {
                    net.on(ev, function (m) {
                        if (ev === 'notice') S.error = m.msg;
                        if (ev === 'start' || (ev === 'welcome' && m.phase === 'playing')) close();
                        render();
                    });
                });
                net.on('join_req', function () { open(); });
                var game = window.createKatamNetGame({
                    Module: M, net: net, want: q.get('watch') ? 'watch' : 'play',
                    log: function (t) { if (M.portDiag) M.portDiag(t, 0); },
                    onCorrection: function () { render(); },
                });
                S.game = game;
                window.katamNetplay = window.katamNetplay || {};
                window.katamNetplay.v2 = { net: net, game: game };
                M.portNetIdle = function () { game.idle(); };
                var present = M.portPresent;
                M.portPresent = function (ptr, w, h) {
                    if (present) present(ptr, w, h);
                    game.tick();
                };
                setInterval(function () { if (root.classList.contains('open')) {
                    var c = document.getElementById('nl-conn'); if (c) c.textContent = connLine(); } renderChip(); }, 1000);
                render();
                return game.ready;
            });
        });
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', mount);
    else mount();
    if (CODE) startSession();
    else if (q.get('room')) S.error = Net.explainCode(q.get('room'));
}());
