/*
 * DOM integration checks for the real dashboard assets. No hardware is contacted.
 * Install jsdom 26.x as a development dependency, then run:
 *   node test/web/dashboard.test.cjs
 * This checks interactions and request/feedback behavior, not visual rendering.
 */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { JSDOM, VirtualConsole } = require('jsdom');

const root = path.resolve(__dirname, '../..');
const html = fs.readFileSync(path.join(root, 'web/index.html'), 'utf8');
const script = fs.readFileSync(path.join(root, 'web/app.js'), 'utf8');
const config = {
  atemIp: '192.168.1.240', hostname: 'datv-controller', wifiSsid: 'DATV', wifiPasswordSet: true,
  acquireMs: 1000, releaseMs: 1500, switchIntervalSec: 300, multiPolicy: 'conflict',
  conflictSource: 6, fallbackSource: 0, respectExternalControl: true,
  inputs: Array.from({ length: 6 }, (_, index) => ({ label: `Receiver ${index + 1}`,
    source: [1, 2, 6, 4, 5, 3][index], enabled: index !== 5, activeHigh: true, priority: index + 1 }))
};
config.inputs[1].label = '<img src=x onerror=alert(1)>';
let status = {
  csrfToken: 'fixture-token-a', uptimeMs: 150000, mode: 'manual', board: 'ESP32 test fixture',
  network: { ip: '192.168.1.230', interface: 'Ethernet', link: true },
  atem: { configured: true, connected: true, program: 1, preview: 2, inTransition: false,
    fadeToBlack: false, product: 'ATEM Television Studio', version: '2.16', lastRxMs: 2, commandsSent: 0, errors: 0 },
  automatic: { desiredSource: 1, reason: 'manual', eligibleCount: 2, remainingMs: 120000 },
  inputs: config.inputs.map((input, index) => ({ ...input, index, gpio: 32 + index, rawHigh: index < 2, locked: index < 2, qualifying: false, remainingMs: 0 })),
  events: [{ uptimeMs: 12000, message: 'Connected to ATEM' }], config
};
const requests = [];
let unavailable = false;
let authenticated = true;
let configResponse = { ok: true, rebootRequired: false };
const errors = [];
const virtualConsole = new VirtualConsole();
virtualConsole.on('jsdomError', (error) => errors.push(error));
const dom = new JSDOM(html, { url: 'http://192.168.1.230/', runScripts: 'outside-only', pretendToBeVisual: true, virtualConsole });
const { window } = dom;
const document = window.document;
const get = (id) => document.getElementById(id);
const field = (name) => get('settings-form').elements.namedItem(name);
window.fetch = async (url, options = {}) => {
  if (url === '/api/status') {
    if (unavailable) throw new Error('Network unavailable');
    if (!authenticated) return { ok: false, status: 401, json: async () => ({ ok: false }) };
    return { ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(status)) };
  }
  assert.equal(options.method, 'POST', 'Mutations use POST');
  requests.push({ url, ...options, body: JSON.parse(options.body) });
  let response = { ok: true };
  if (url === '/api/mode') {
    status.mode = JSON.parse(options.body).mode;
    status.automatic.reason = status.mode === 'auto' ? 'conflict_wait' : 'manual';
  }
  if (url === '/api/config') {
    const { wifiPassword, adminPassword, clearWifiPassword, ...publicConfig } = JSON.parse(options.body);
    status.config = { ...status.config, ...publicConfig };
    if (clearWifiPassword) status.config.wifiPasswordSet = false;
    else if (wifiPassword) status.config.wifiPasswordSet = true;
    response = configResponse;
  }
  return { ok: true, status: 200, json: async () => response };
};
window.confirm = () => true;
const settle = () => new Promise((resolve) => setTimeout(resolve, 30));
async function refresh() { document.dispatchEvent(new window.Event('visibilitychange')); await settle(); }
function edit(input, value) { input.value = value; input.dispatchEvent(new window.Event('input', { bubbles: true })); }
function submit() { get('settings-form').dispatchEvent(new window.Event('submit', { bubbles: true, cancelable: true })); }

(async () => {
  try {
    assert.ok([...document.querySelectorAll('[data-action]')].every((button) => button.disabled), 'No live actions are enabled before initial status');
    window.eval(script);
    await settle();
    assert.equal(document.querySelectorAll('.input-card').length, 6);
    assert.equal(get('program-source').textContent, 'Input 1');
    assert.equal(get('preview-source').textContent, 'Input 2');
    assert.equal(document.querySelectorAll('.input-card img').length, 0, 'Receiver labels cannot inject HTML');
    assert.equal(document.querySelectorAll('.input-card')[1].querySelector('.input-label').textContent, config.inputs[1].label);
    assert.equal(get('resume-auto').disabled, false);
    assert.equal(get('pause-auto').disabled, true);
    assert.match(get('automation-reason').textContent, /Automatic routing is paused/);
    for (const reason of ['single_lock', 'round_robin', 'conflict_wait', 'conflict_output', 'no_lock_fallback', 'priority', 'manual']) {
      status.automatic.reason = reason;
      await refresh();
      assert.notEqual(get('automation-reason').textContent, reason, `Operator text replaces ${reason}`);
      assert.match(get('automation-reason').textContent, / /);
    }
    assert.match(document.querySelector('.map-active-high').closest('label').textContent, /GPIO HIGH means locked/);

    document.querySelectorAll('.input-card')[2].querySelector('[data-action="program"]').click();
    await settle();
    assert.equal(requests.at(-1).url, '/api/action');
    assert.deepEqual(requests.at(-1).body, { action: 'program', source: 6 }, 'Manual Take uses configured ATEM mapping');
    assert.equal(requests.at(-1).headers['X-CSRF-Token'], 'fixture-token-a');
    assert.equal(requests.at(-1).headers['Content-Type'], 'application/json');
    assert.equal(get('program-source').textContent, 'Input 1', 'Accepted commands do not invent on-air feedback');
    status.atem.program = 6;
    await refresh();
    assert.equal(get('program-source').textContent, 'Input 6', 'Actual feedback updates Program');

    get('resume-auto').click();
    await settle();
    assert.equal(status.mode, 'auto');
    assert.equal(get('mode-badge').textContent, 'ARMED');
    assert.equal(get('policy-timer').textContent, 'Multiple-lock delay 2:00');

    edit(field('hostname'), 'unsaved-name');
    status.config.hostname = 'changed-elsewhere';
    await refresh();
    assert.equal(field('hostname').value, 'unsaved-name', 'Polling preserves unsaved form edits');
    get('discard-settings').click();
    assert.equal(field('hostname').value, 'changed-elsewhere');
    assert.equal(get('save-settings').disabled, true);

    edit(field('atemIp'), '192.168.400.1');
    const beforeInvalid = requests.length;
    submit();
    await settle();
    assert.equal(requests.length, beforeInvalid, 'Invalid IPv4 is not sent');
    assert.match(get('notification-text').textContent, /between 0 and 255/);
    get('discard-settings').click();
    configResponse = { ok: true, rebootRequired: true };
    edit(field('wifiSsid'), 'DATV New');
    submit();
    await settle();
    assert.equal(requests.at(-1).url, '/api/config');
    assert.equal(requests.at(-1).body.wifiPassword, '', 'Blank passwords preserve stored credentials');
    assert.equal(requests.at(-1).body.adminPassword, '');
    assert.equal(requests.at(-1).body.clearWifiPassword, false);
    assert.equal(requests.at(-1).body.inputs.length, 6);
    assert.equal(requests.at(-1).body.switchIntervalSec, 300);
    assert.ok(get('restart-controller').closest('.restart-panel').classList.contains('reboot-needed'));

    field('clearWifiPassword').click();
    assert.equal(field('wifiPassword').disabled, true, 'Clearing credentials disables the new password input');
    submit();
    await settle();
    assert.equal(requests.at(-1).body.clearWifiPassword, true, 'Open-network migration sends an explicit clear flag');
    assert.equal(requests.at(-1).body.wifiPassword, '');
    assert.equal(status.config.wifiPasswordSet, false);

    status.atem.connected = false;
    await refresh();
    assert.ok([...document.querySelectorAll('[data-action]')].every((button) => button.disabled), 'ATEM disconnection disables all manual switcher commands');
    assert.equal(document.querySelectorAll('.tally.active').length, 0, 'Stale tally indicators are removed');
    assert.equal(get('settings-fields').disabled, false, 'Local setup remains usable when the ATEM is disconnected');
    status.atem.connected = true;
    await refresh();
    unavailable = true;
    await refresh();
    assert.equal(get('connection-alert').hidden, false);
    assert.equal(get('settings-fields').disabled, true, 'Controller loss disables configuration writes');
    assert.equal(document.querySelectorAll('.tally.active').length, 0);
    assert.match(get('program-feedback').textContent, /UNAVAILABLE/);

    unavailable = false;
    status.csrfToken = 'fixture-token-b';
    await refresh();
    get('pause-auto').click();
    await settle();
    assert.equal(requests.at(-1).headers['X-CSRF-Token'], 'fixture-token-b', 'Reconnection uses the current CSRF token');
    configResponse = { ok: true, rebootRequired: false, reauthRequired: true };
    edit(field('adminPassword'), 'test-password-only');
    submit();
    await settle();
    assert.equal(field('adminPassword').value, '', 'A saved password is cleared from the input');
    assert.equal(get('reload-dashboard').hidden, false);
    assert.equal(get('settings-fields').disabled, true, 'Password changes stop writes until sign-in is refreshed');
    assert.match(get('connection-alert-text').textContent, /Administrator password changed/);
    assert.equal(errors.length, 0, errors.map((error) => error.message).join('\n'));
    console.log('PASS: dashboard mapping, real feedback, CSRF, unsaved edits, validation, reconnect, and reauthentication');
  } finally { window.close(); }
})().catch((error) => { console.error(error); process.exitCode = 1; });
