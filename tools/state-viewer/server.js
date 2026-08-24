// RoNTR state viewer — dev stand-in for the TS plugin's HTTP bridge.
//
// Binds the bridge port (39442) and speaks the exact wire contract from
// docs/HTTP-BRIDGE.md, so a game mod posts to it unchanged. On top of that
// it serves a live dashboard (GET /) and a JSON API (GET /api/state) showing
// everything the game sends: listener, players, occlusion, radio channels,
// PTT, game id, update rate, plus any unknown keys the parser would ignore.
//
// Two ways to coexist with the real plugin:
//   - standalone: quit TeamSpeak or `/rtr set bridge.enable 0` (same port),
//   - relay: move the plugin (`/rtr set bridge.port 39500`), then run the
//     viewer with FORWARD=http://127.0.0.1:39500 — every game POST is
//     forwarded to the plugin and the plugin's talk response is returned to
//     the game, so TeamSpeak keeps working while the dashboard observes.
//
// Zero dependencies — plain node:http. CommonJS for maximum compat.
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = parseInt(process.env.PORT || '39442', 10);
// Default loopback: if the real plugin holds the port, the bind fails LOUDLY
// (a 0.0.0.0 bind would silently coexist on Windows and be shadowed by the
// plugin's exclusive 127.0.0.1 bind). Docker sets HOST=0.0.0.0 in the image
// and publishes the port loopback-only instead.
const HOST = process.env.HOST || '127.0.0.1';
const RTR_VERSION = 3;
const MAX_RADIOS = 4;
const MAX_BODY = 64 * 1024;

// Initial relay target, e.g. FORWARD=http://127.0.0.1:39500 (the plugin
// after `/rtr set bridge.port 39500`; from Docker:
// http://host.docker.internal:39500). Both the on/off switch and the target
// can be changed at runtime from the dashboard (POST /api/relay).
const FORWARD = normalizeTarget(process.env.FORWARD);
const FORWARD_TIMEOUT_MS = 800;
const forwardAgent = new http.Agent({ keepAlive: true });

// Inside a container 127.0.0.1 is the container itself, so bare ports must
// default to the host machine instead (Docker Desktop's host-side proxy
// makes even loopback-bound host services reachable that way).
const IN_DOCKER = fs.existsSync('/.dockerenv');
const RELAY_HOST = process.env.RELAY_DEFAULT_HOST
  || (IN_DOCKER ? 'host.docker.internal' : '127.0.0.1');

// "39500" -> http://<default host>:39500; bare host:port gets http://; only
// plain http (node:http does the forwarding). Returns null when unusable.
function normalizeTarget(t) {
  t = String(t || '').trim().replace(/\/+$/, '');
  if (!t) return null;
  if (/^\d+$/.test(t)) t = `http://${RELAY_HOST}:` + t;
  if (!/^[a-z]+:\/\//i.test(t)) t = 'http://' + t;
  try {
    const u = new URL(t);
    if (u.protocol !== 'http:' || !u.port) return null;
    return t;
  } catch {
    return null;
  }
}

// ---------------------------------------------------------------- state ----
const store = {
  raw: '',            // last accepted body, normalized
  state: null,        // last parsed state
  gameId: '',
  lastMs: 0,          // when the last good POST arrived
  postTimes: [],      // sliding window for Hz
  totalPosts: 0,
  badPosts: 0,
  lastError: null,    // { whenMs, why, preview }
};
let talkers = [];     // [{name, amp}] echoed into every POST /state response
const relay = {       // forward-to-plugin config + status (dashboard-editable)
  enabled: !!FORWARD,
  target: FORWARD,    // normalized http://host:port, or null
  lastOkMs: 0,
  lastErr: null,      // string
  forwarded: 0,
  failed: 0,
};

// Config + status plus a hint for the classic Docker footgun.
function relaySnapshot() {
  let hint = null;
  if (IN_DOCKER && relay.target && /\/\/(127\.0\.0\.1|localhost)[:/]/.test(relay.target))
    hint = 'target is loopback INSIDE the container — use host.docker.internal:<port> to reach the host';
  return Object.assign({ hint }, relay);
}

// ------------------------------------------------------------- wire parse --
// Mirrors ts3-plugin/src/httpbridge.hpp (normalizeStateBody + parseStateText).

function percentDecode(s) {
  let out = '';
  for (let i = 0; i < s.length; i++) {
    if (s[i] === '%' && i + 2 < s.length && /^[0-9a-f]{2}$/i.test(s.substr(i + 1, 2))) {
      out += String.fromCharCode(parseInt(s.substr(i + 1, 2), 16));
      i += 2;
    } else if (s[i] === '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

function normalizeBody(body) {
  if (body.startsWith('data=')) body = percentDecode(body.slice(5));
  else if (!body.includes('rtr=') && body.includes('%')) body = percentDecode(body);
  if (!body.includes('\n') && body.includes('\\n')) body = body.split('\\n').join('\n');
  return body;
}

function parseVec3(v) {
  const m = v.trim().split(/\s+/).map(Number);
  if (m.length !== 3 || m.some(Number.isNaN)) return null;
  return { x: m[0], y: m[1], z: m[2] };
}

const EAR_NAMES = { b: 'both', l: 'left', r: 'right' };

function parseState(rawBody) {
  const body = normalizeBody(rawBody);
  const st = {
    rtr: 0, game: '', ingame: 0, name: '',
    lpos: null, lfwd: null, lup: null,
    radioptt: 0, voiceptt: 0,
    freqs: new Array(MAX_RADIOS).fill(0),
    ears: new Array(MAX_RADIOS).fill('b'),
    activeradio: 0,
    players: [],
    extras: {},           // unknown keys, shown by the dashboard
  };
  let versionSeen = false;

  for (let line of body.split('\n')) {
    if (line.endsWith('\r')) line = line.slice(0, -1);
    const eq = line.indexOf('=');
    if (eq <= 0) continue;
    const key = line.slice(0, eq);
    const val = line.slice(eq + 1);

    switch (key) {
      case 'rtr':
        if (parseInt(val, 10) !== RTR_VERSION)
          return { error: 'version', got: val };
        versionSeen = true;
        st.rtr = RTR_VERSION;
        break;
      case 'game': st.game = val; break;
      case 'ingame': st.ingame = val === '1' ? 1 : 0; break;
      case 'name': st.name = val; break;
      case 'lpos': case 'lfwd': case 'lup': {
        const v = parseVec3(val);
        if (!v) return { error: 'malformed', why: `${key}=${val}` };
        st[key] = v;
        break;
      }
      case 'radioptt': st.radioptt = val === '1' ? 1 : 0; break;
      case 'voiceptt': st.voiceptt = val === '1' ? 1 : 0; break;
      case 'freq': st.freqs[0] = parseInt(val, 10) || 0; break;
      case 'freqs':
        val.split(',').slice(0, MAX_RADIOS).forEach((f, i) => {
          st.freqs[i] = parseInt(f, 10) || 0;
        });
        break;
      case 'activeradio': {
        const a = parseInt(val, 10) || 0;
        st.activeradio = Math.min(Math.max(a, 0), MAX_RADIOS - 1);
        break;
      }
      case 'ears':
        val.split(',').slice(0, MAX_RADIOS).forEach((e, i) => {
          const c = e.trim().toLowerCase();
          st.ears[i] = (c === 'l' || c === '1') ? 'l' : (c === 'r' || c === '2') ? 'r' : 'b';
        });
        break;
      case 'player': {
        const f = val.split('|');
        if (f.length < 6 || !f[0]) return { error: 'malformed', why: `player=${val}` };
        st.players.push({
          name: f[0],
          x: Number(f[1]) || 0, y: Number(f[2]) || 0, z: Number(f[3]) || 0,
          alive: f[4] === '0' ? 0 : 1,
          occl: Math.min(Math.max(parseInt(f[5], 10) || 0, 0), 255),
        });
        break;
      }
      default:
        st.extras[key] = val;
    }
  }
  if (!versionSeen) return { error: 'version', got: '(missing)' };
  return { state: st, body };
}

// ----------------------------------------------------------------- relay ---
// Forward one raw /state body to the real plugin; cb(err, {status, body}).
function forwardState(raw, cb) {
  const url = new URL(relay.target + '/state');
  const req = http.request({
    hostname: url.hostname,
    port: url.port || 80,
    path: url.pathname,
    method: 'POST',
    agent: forwardAgent,
    timeout: FORWARD_TIMEOUT_MS,
    headers: { 'Content-Type': 'text/plain', 'Content-Length': Buffer.byteLength(raw) },
  }, (res) => {
    let chunks = [];
    res.on('data', (c) => chunks.push(c));
    res.on('end', () => cb(null, {
      status: res.statusCode || 502,
      body: Buffer.concat(chunks).toString('utf8'),
    }));
  });
  req.on('timeout', () => req.destroy(new Error('timeout')));
  req.on('error', (e) => cb(e));
  req.end(raw);
}

// ------------------------------------------------------------- responses ---
function talkResponse() {
  let out = `rtr=${RTR_VERSION}\n`;
  for (const t of talkers.slice(0, 8))
    out += `talk=${t.name}|${Number(t.amp).toFixed(2)}\n`;
  return out;
}

function sendText(res, code, body) {
  res.writeHead(code, {
    'Content-Type': 'text/plain',
    'Content-Length': Buffer.byteLength(body),
    'Connection': 'close',
  });
  res.end(body);
}

function sendJson(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(body);
}

function readBody(req, cb) {
  let chunks = [], size = 0;
  req.on('data', (c) => {
    size += c.length;
    if (size > MAX_BODY) { req.destroy(); return; }
    chunks.push(c);
  });
  req.on('end', () => cb(Buffer.concat(chunks).toString('utf8')));
}

// ---------------------------------------------------------------- server ---
const server = http.createServer((req, res) => {
  const url = req.url.split('?')[0];

  if (req.method === 'POST' && url === '/state') {
    readBody(req, (raw) => {
      const now = Date.now();
      store.totalPosts++;
      const result = parseState(raw);
      if (result.error) {
        store.badPosts++;
        store.lastError = { whenMs: now, why: result.error, preview: raw.slice(0, 200) };
        if (result.error === 'version')
          return sendText(res, 409, `rtr=${RTR_VERSION}\nerror=version mismatch\n`);
        return sendText(res, 400, `rtr=${RTR_VERSION}\nerror=malformed state\n`);
      }
      store.state = result.state;
      store.raw = result.body;
      store.gameId = result.state.game || store.gameId;
      store.lastMs = now;
      store.postTimes.push(now);
      while (store.postTimes.length && store.postTimes[0] < now - 5000)
        store.postTimes.shift();

      if (!relay.enabled || !relay.target)
        return sendText(res, 200, talkResponse());

      // Relay mode: the plugin's response (the real talk list) goes back to
      // the game; injected talkers are appended on top. If the plugin is
      // unreachable the viewer answers alone so the game link stays up.
      forwardState(raw, (err, upstream) => {
        if (err) {
          relay.failed++;
          relay.lastErr = String(err.message || err);
          return sendText(res, 200, talkResponse());
        }
        relay.forwarded++;
        relay.lastOkMs = Date.now();
        relay.lastErr = null;
        let body = upstream.body;
        if (upstream.status === 200) {
          for (const t of talkers.slice(0, 8))
            body += `talk=${t.name}|${Number(t.amp).toFixed(2)}\n`;
        }
        sendText(res, upstream.status, body);
      });
    });
    return;
  }

  if (req.method === 'GET' && url === '/health')
    return sendText(res, 200,
      `rtr=${RTR_VERSION}\nplugin=state-viewer-dev\n` +
      (store.gameId ? `game=${store.gameId}\n` : ''));

  if (req.method === 'GET' && url === '/api/state') {
    const now = Date.now();
    return sendJson(res, 200, {
      now,
      lastMs: store.lastMs,
      ageMs: store.lastMs ? now - store.lastMs : null,
      hz: store.postTimes.length / 5,
      gameId: store.gameId,
      totalPosts: store.totalPosts,
      badPosts: store.badPosts,
      lastError: store.lastError,
      talkers,
      relay: relaySnapshot(),
      state: store.state,
      raw: store.raw,
      earNames: EAR_NAMES,
    });
  }

  if (url === '/api/relay') {
    if (req.method === 'GET') return sendJson(res, 200, relaySnapshot());
    if (req.method === 'POST') {
      readBody(req, (raw) => {
        try {
          const cfg = JSON.parse(raw || '{}');
          if ('target' in cfg) {
            const t = normalizeTarget(cfg.target);
            if (cfg.target && !t)
              return sendJson(res, 400, { error: 'bad target (want a port, host:port, or http:// url)' });
            relay.target = t;
          }
          if ('enabled' in cfg) relay.enabled = !!cfg.enabled;
          if (relay.enabled && !relay.target) relay.enabled = false;
          relay.lastErr = null; // fresh start after any config change
          console.log(`relay: ${relay.enabled ? 'ON -> ' + relay.target : 'off'}`);
          sendJson(res, 200, relaySnapshot());
        } catch (e) {
          sendJson(res, 400, { error: String(e.message || e) });
        }
      });
      return;
    }
  }

  if (url === '/api/talk') {
    if (req.method === 'GET') return sendJson(res, 200, talkers);
    if (req.method === 'POST') {
      readBody(req, (raw) => {
        try {
          const list = JSON.parse(raw || '[]');
          if (!Array.isArray(list)) throw new Error('not an array');
          talkers = list
            .filter((t) => t && typeof t.name === 'string' && t.name)
            .map((t) => ({
              name: t.name.replace(/[|\r\n]/g, ' ').slice(0, 63),
              amp: Math.min(Math.max(Number(t.amp) || 0, 0), 1),
            }))
            .slice(0, 8);
          sendJson(res, 200, talkers);
        } catch (e) {
          sendJson(res, 400, { error: String(e.message || e) });
        }
      });
      return;
    }
  }

  if (req.method === 'GET' && (url === '/' || url === '/index.html')) {
    const file = path.join(__dirname, 'public', 'index.html');
    return fs.readFile(file, (err, data) => {
      if (err) return sendText(res, 500, 'index.html missing');
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(data);
    });
  }

  sendText(res, 404, '');
});

server.listen(PORT, HOST, () => {
  console.log(`RoNTR state viewer on http://${HOST}:${PORT}/ (bridge contract rtr=${RTR_VERSION})`);
  console.log('Point the game at it as usual; open the same URL in a browser for the dashboard.');
  if (relay.enabled)
    console.log(`relay mode: forwarding state to ${relay.target}/state (plugin talk responses pass through)`);
  console.log('relay is switchable at runtime from the dashboard (or POST /api/relay).');
});
