// A TURN server on this machine, for testing the TURN tier of online play
// (web/online.js ?netturn=1) without Cloudflare: node-turn (a small pure-JS
// STUN/TURN server, MIT), installed OUTSIDE the tree -- it is not a
// dependency of the port:
//
//   mkdir -p /tmp/nt && (cd /tmp/nt && npm init -y && npm install node-turn@0.0.6)
//   sbg start turn -- node tools/local_turn.mjs --node-turn /tmp/nt --port 19478
//   TURN_ICE_SERVERS='[{"urls":["turn:127.0.0.1:19478?transport=udp"],"username":"katam","credential":"katam-test"}]' \
//       sbg start relay -- node netplay/dev-relay.mjs 18797
//   node tools/online_browser_test.mjs ... --relay http://127.0.0.1:18797 --cases turn
//
// UDP only, long-term credentials (--user / --pass), relaying on 127.0.0.1.
import { createRequire } from 'module';
import path from 'path';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i >= 0 ? argv[i + 1] : d; };
const dir = flag('node-turn', '');
if (!dir) { console.error('--node-turn DIR (where node-turn is installed) is needed'); process.exit(2); }
const require = createRequire(path.resolve(dir, 'package.json'));
const Turn = require('node-turn');
const port = +flag('port', 19478), ip = flag('ip', '127.0.0.1');
const user = flag('user', 'katam'), pass = flag('pass', 'katam-test');
const server = new Turn({
    listeningPort: port, listeningIps: [ip], relayIps: [ip],
    minPort: +flag('min-port', 49300), maxPort: +flag('max-port', 49400),
    authMech: 'long-term', credentials: { [user]: pass }, realm: 'katam-test',
    debugLevel: flag('debug', 'WARN'),
});
server.start();
console.log(`[turn] node-turn on udp ${ip}:${port}, user ${user}`);
for (const s of ['SIGINT', 'SIGTERM']) process.on(s, () => { try { server.stop(); } catch (e) { /* gone */ } process.exit(0); });
