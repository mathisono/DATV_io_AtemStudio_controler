/* Local dashboard. All live state comes from /api/status. No external assets. */
(() => {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const form = $('settings-form');
  const state = { status: null, reachable: false, busy: false, polling: false,
    dirty: false, formReady: false, configSignature: '', eventSignature: '',
    rebootRequired: false, reauthRequired: false };
  const cards = [];
  const mappings = [];
  const policyText = {
    round_robin: 'Rotate between qualified, enabled inputs. The interval starts when more than one input is locked.',
    priority: 'Select the qualified, enabled input with the lowest priority number. The interval is not used.',
    conflict: 'If two or more inputs remain locked for the full delay, take the selected source. A single remaining lock resumes normal selection.'
  };
  const automaticReasonText = {
    manual: 'Automatic routing is paused. Resume auto when ready.',
    single_lock: 'Following the one receiver with a qualified lock.',
    round_robin: 'Multiple receivers are locked. Rotating through their mapped inputs.',
    conflict_wait: 'Multiple receivers are locked. Waiting for the delay before taking the selected source.',
    conflict_output: 'Multiple locks persisted through the delay. Holding the selected source.',
    no_lock_fallback: 'No enabled receiver has a qualified lock. Using the fallback source.',
    priority: 'Multiple receivers are locked. Following the highest-priority input.'
  };

  function sourceOptions(select, includeBlack) {
    for (let source = includeBlack ? 0 : 1; source <= 6; source++) {
      const option = document.createElement('option');
      option.value = String(source);
      option.textContent = source === 0 ? 'Black' : `Input ${source}`;
      select.appendChild(option);
    }
  }

  for (let index = 0; index < 6; index++) {
    const card = $('input-template').content.firstElementChild.cloneNode(true);
    card.querySelector('.input-number').textContent = `RECEIVER ${index + 1}`;
    card.querySelector('.input-label').textContent = `Receiver ${index + 1}`;
    card.querySelector('.input-route').textContent = 'Waiting for mapping';
    card.querySelectorAll('[data-action]').forEach((button) => { button.dataset.receiver = String(index); });
    $('inputs').appendChild(card);
    cards.push(card);
    const mapping = $('mapping-template').content.firstElementChild.cloneNode(true);
    mapping.querySelector('h3').textContent = `Receiver ${index + 1}`;
    for (const key of ['label', 'source', 'priority', 'enabled', 'active-high']) {
      const field = mapping.querySelector(`.map-${key}`);
      field.name = `receiver-${index}-${key}`;
      field.setAttribute('aria-label', `Receiver ${index + 1} ${key === 'active-high' ? 'GPIO HIGH means locked' : key.replace('-', ' ')}`);
    }
    sourceOptions(mapping.querySelector('.map-source'), false);
    $('input-settings').appendChild(mapping);
    mappings.push(mapping);
  }
  document.querySelectorAll('.source-select').forEach((select) => sourceOptions(select, true));

  function field(name) { return form.elements.namedItem(name); }
  function number(value, fallback = 0) { return Number.isFinite(Number(value)) ? Number(value) : fallback; }
  function duration(ms) {
    const seconds = Math.max(0, Math.ceil(number(ms) / 1000));
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  }
  function uptime(ms) {
    let seconds = Math.floor(Math.max(0, number(ms)) / 1000);
    const days = Math.floor(seconds / 86400);
    seconds %= 86400;
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    return `${days ? `${days}d ` : ''}${hours}h ${minutes}m`;
  }
  function eventTime(ms) {
    const total = Math.floor(Math.max(0, number(ms)) / 1000);
    return `${String(Math.floor(total / 3600)).padStart(2, '0')}:${String(Math.floor(total / 60) % 60).padStart(2, '0')}:${String(total % 60).padStart(2, '0')}`;
  }
  function sourceLabel(source, inputs) {
    if (source == null) return 'Waiting for the switcher';
    if (source === 0) return 'Internal black source';
    const mapped = (inputs || []).filter((input) => input.source === source);
    return mapped.length ? mapped.map((input) => input.label || `Receiver ${input.index + 1}`).join(' / ') : `ATEM source ${source}`;
  }
  function notify(message, type = 'success') {
    $('notification-text').textContent = message;
    $('notification').className = `notice ${type}`;
    $('notification').hidden = false;
  }
  function setLight(id, condition) {
    $(id).className = `status-light ${condition ? 'good' : 'bad'}`;
  }
  function chooseTab(tab) {
    if (tab !== 'control' && tab !== 'setup') return;
    document.querySelectorAll('[data-tab]').forEach((button) => {
      const active = button.dataset.tab === tab;
      button.classList.toggle('active', active);
      if (active) button.setAttribute('aria-current', 'page');
      else button.removeAttribute('aria-current');
    });
    $('view-control').hidden = tab !== 'control';
    $('view-setup').hidden = tab !== 'setup';
  }

  function updateAvailability() {
    const status = state.status;
    const canWrite = state.reachable && Boolean(status && status.csrfToken) && !state.busy && !state.reauthRequired;
    const canControl = canWrite && Boolean(status.atem && status.atem.connected);
    document.querySelectorAll('[data-action]').forEach((button) => {
      const needsSource = button.dataset.action === 'program' || button.dataset.action === 'preview';
      const source = Number(button.dataset.source);
      button.disabled = !canControl || (needsSource && (!Number.isInteger(source) || source < 0 || source > 6));
    });
    $('resume-auto').disabled = !canControl || status.mode === 'auto';
    $('pause-auto').disabled = !canWrite || status.mode !== 'auto';
    $('settings-fields').disabled = !canWrite || !state.formReady;
    $('save-settings').disabled = !canWrite || !state.dirty;
    $('discard-settings').disabled = !canWrite || !state.dirty;
    $('restart-controller').disabled = !canWrite;
    $('save-state').textContent = state.busy ? 'Applying change…' : !state.formReady ? 'Waiting for settings…' : state.dirty ? 'Unsaved changes' : state.rebootRequired ? 'Saved · restart required for network changes' : 'Settings are up to date';
    $('save-state').classList.toggle('dirty', state.dirty || state.rebootRequired);
  }

  function updateConnection() {
    const status = state.status;
    setLight('controller-light', state.reachable);
    if (!state.reachable) {
      $('controller-status').textContent = state.status ? 'Controller offline' : 'Connecting…';
      $('connection-alert-text').textContent = state.status ? 'Connection to the controller is lost. Displayed switcher feedback may be stale; controls are unavailable until it reconnects.' : 'Waiting for the controller. If this continues, check your connection to its local network.';
      $('connection-alert').hidden = false;
      $('atem-state').textContent = 'Feedback unavailable';
      setLight('atem-light', false);
    } else {
      $('controller-status').textContent = 'Controller online';
      $('controller-ip').textContent = status.network?.ip || status.network?.apIp || '';
      $('connection-alert').hidden = true;
      const atem = status.atem || {};
      setLight('atem-light', atem.connected);
      $('atem-state').textContent = atem.connected ? 'Connected' : atem.configured ? 'Reconnecting…' : 'Setup required';
      $('atem-product').textContent = (!atem.connected && atem.error) || atem.product || (atem.configured ? 'Waiting for ATEM feedback' : 'Enter the ATEM address in Setup');
      $('atem-address').textContent = status.config?.atemIp ? `${status.config.atemIp}${atem.version ? ` · ${atem.version}` : ''}` : 'Switcher address not configured';
    }
    const fresh = state.reachable && Boolean(status?.atem?.connected);
    for (const type of ['program', 'preview']) {
      $(`${type}-source`).closest('.monitor').classList.toggle('stale', !fresh);
      $(`${type}-feedback`).textContent = fresh ? 'LIVE FEEDBACK' : state.status ? 'FEEDBACK UNAVAILABLE' : 'AWAITING FEEDBACK';
    }
    if (!fresh) cards.forEach((card) => {
      card.classList.remove('is-program', 'is-preview');
      card.querySelectorAll('.tally').forEach((tally) => tally.classList.remove('active'));
    });
    updateAvailability();
  }

  function updatePolicyHelp() {
    const policy = field('multiPolicy').value;
    $('policy-help').textContent = policyText[policy] || '';
    $('conflict-source-label').hidden = policy !== 'conflict';
    $('interval-help').textContent = policy === 'priority' ? 'Stored for the other policies; ignored by priority selection.' : policy === 'conflict' ? 'Continuous multiple-lock delay. 300 seconds = 5 minutes.' : 'Dwell time between locked inputs. 300 seconds = 5 minutes.';
    field('wifiPassword').disabled = field('clearWifiPassword').checked;
    if (field('clearWifiPassword').checked) field('wifiPassword').value = '';
  }

  function fillSettings(config) {
    if (!config) return;
    for (const key of ['atemIp', 'hostname', 'wifiSsid', 'acquireMs', 'releaseMs', 'switchIntervalSec', 'multiPolicy', 'conflictSource', 'fallbackSource']) {
      field(key).value = config[key] == null ? '' : String(config[key]);
    }
    field('respectExternalControl').checked = Boolean(config.respectExternalControl);
    field('wifiPassword').value = '';
    field('clearWifiPassword').checked = false;
    field('adminPassword').value = '';
    $('wifi-password-help').textContent = config.wifiPasswordSet ? 'A password is stored. Leave blank to keep it.' : 'No password is stored. Leave blank for an open network.';
    mappings.forEach((mapping, index) => {
      const input = config.inputs?.[index] || {};
      mapping.querySelector('.map-label').value = input.label || `Receiver ${index + 1}`;
      mapping.querySelector('.map-source').value = String(input.source ?? index + 1);
      mapping.querySelector('.map-priority').value = String(input.priority ?? index + 1);
      mapping.querySelector('.map-enabled').checked = Boolean(input.enabled);
      mapping.querySelector('.map-active-high').checked = input.activeHigh !== false;
    });
    state.formReady = true;
    state.dirty = false;
    state.configSignature = JSON.stringify(config);
    updatePolicyHelp();
  }

  function renderInputs(status) {
    const live = state.reachable && status.atem?.connected;
    cards.forEach((card, index) => {
      const input = status.inputs?.find((entry) => entry.index === index);
      if (!input) return;
      const label = input.label || `Receiver ${index + 1}`;
      card.querySelector('.input-label').textContent = label;
      card.querySelector('.input-label').title = label;
      card.querySelector('.input-route').textContent = `ATEM input ${input.source} · GPIO ${input.gpio}`;
      const badge = card.querySelector('.lock-badge');
      badge.textContent = !input.enabled ? 'DISABLED' : input.qualifying ? (input.locked ? 'RELEASING' : 'ACQUIRING') : input.locked ? 'LOCKED' : 'NO LOCK';
      badge.className = `badge lock-badge ${!input.enabled ? 'muted' : input.qualifying ? 'warning' : input.locked ? 'good' : 'muted'}`;
      card.classList.toggle('is-disabled', !input.enabled);
      const isProgram = Boolean(live && status.atem.program === input.source);
      const isPreview = Boolean(live && status.atem.preview === input.source);
      card.classList.toggle('is-program', isProgram);
      card.classList.toggle('is-preview', isPreview);
      card.querySelector('.program-tally').classList.toggle('active', isProgram);
      card.querySelector('.preview-tally').classList.toggle('active', isPreview);
      card.querySelector('.raw-level').textContent = `GPIO ${input.rawHigh ? 'HIGH' : 'LOW'} · ${input.activeHigh ? 'active H' : 'active L'}`;
      card.querySelector('.lock-timing').textContent = input.qualifying ? `${Math.ceil(number(input.remainingMs))} ms` : '';
      card.querySelectorAll('[data-action]').forEach((button) => {
        button.dataset.source = String(input.source);
        button.setAttribute('aria-label', `${button.dataset.action === 'program' ? 'Take' : 'Preview'} ${label}, ATEM input ${input.source}`);
      });
    });
  }

  function renderEvents(events) {
    const selected = (events || []).slice(0, 12);
    const signature = JSON.stringify(selected);
    if (signature === state.eventSignature) return;
    state.eventSignature = signature;
    const list = $('events');
    list.replaceChildren();
    if (!selected.length) {
      const li = document.createElement('li');
      li.className = 'empty-state';
      li.textContent = 'No events yet.';
      list.appendChild(li);
      return;
    }
    selected.forEach((event) => {
      const li = document.createElement('li');
      const time = document.createElement('time');
      time.textContent = eventTime(event.uptimeMs);
      time.setAttribute('aria-label', `${time.textContent} since controller start`);
      const message = document.createElement('span');
      message.textContent = event.message || '';
      li.append(time, message);
      list.appendChild(li);
    });
  }

  function renderStatus(status) {
    const atem = status.atem || {};
    for (const type of ['program', 'preview']) {
      const source = atem[type];
      $(`${type}-source`).textContent = source == null ? '—' : source === 0 ? 'Black' : `${source >= 1 && source <= 6 ? 'Input' : 'Source'} ${source}`;
      $(`${type}-label`).textContent = sourceLabel(source, status.inputs);
    }
    const automatic = status.automatic || {};
    const isAuto = status.mode === 'auto';
    $('mode-badge').textContent = isAuto ? 'ARMED' : 'PAUSED';
    $('mode-badge').className = `badge ${isAuto ? 'good' : 'warning'}`;
    $('automation-reason').textContent = Object.hasOwn(automaticReasonText, automatic.reason) ? automaticReasonText[automatic.reason] : automatic.reason || (isAuto ? 'Monitoring receiver locks.' : 'Manual control. Resume auto when ready.');
    const eligible = number(automatic.eligibleCount);
    $('eligible-count').textContent = `${eligible} eligible ${eligible === 1 ? 'input' : 'inputs'}`;
    const remaining = number(automatic.remainingMs);
    const policy = status.config?.multiPolicy;
    $('policy-timer').hidden = !isAuto || eligible < 2 || remaining <= 0 || policy === 'priority';
    $('policy-timer').textContent = `${policy === 'conflict' ? 'Multiple-lock delay' : 'Rotation in'} ${duration(remaining)}`;
    $('ftb-button').setAttribute('aria-pressed', String(Boolean(atem.fadeToBlack)));
    $('ftb-button').textContent = atem.fadeToBlack ? 'Return from black' : 'Fade to black';
    $('transition-state').textContent = atem.fadeToBlack ? 'Fade to black is active on the switcher.' : atem.inTransition ? 'A transition is in progress on the switcher.' : 'Program and Preview above reflect ATEM feedback, not queued commands.';
    $('transition-state').classList.toggle('active', Boolean(atem.fadeToBlack || atem.inTransition));
    renderInputs(status);
    renderEvents(status.events);
    if (!state.formReady || (!state.dirty && !state.busy && JSON.stringify(status.config) !== state.configSignature)) fillSettings(status.config);
    $('board-name').textContent = status.board || 'ESP32 controller';
    const network = status.network || {};
    $('network-detail').textContent = [network.interface, network.ip ? `IP ${network.ip}` : '', network.apSsid ? `Setup Wi-Fi: ${network.apSsid}${network.apIp ? ` (${network.apIp})` : ''}` : ''].filter(Boolean).join(' · ') || 'No network details reported';
    $('footer-status').textContent = `${uptime(status.uptimeMs)} uptime${network.interface ? ` · ${network.interface}` : ''}`;
    updateConnection();
  }

  async function api(path, body) {
    if (body !== undefined && !state.status?.csrfToken) throw new Error('Wait for a live controller connection before making changes.');
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 6000);
    const options = { signal: controller.signal, cache: 'no-store', credentials: 'same-origin', headers: { Accept: 'application/json' } };
    if (body !== undefined) {
      options.method = 'POST';
      options.headers['Content-Type'] = 'application/json';
      options.headers['X-CSRF-Token'] = state.status.csrfToken;
      options.body = JSON.stringify(body);
    }
    try {
      const response = await fetch(path, options);
      const result = await response.json().catch(() => ({}));
      if (!response.ok || result.ok === false) throw new Error(response.status === 401 ? 'Sign-in is required. Reload this page to authenticate.' : result.error || `Controller request failed (${response.status}).`);
      return result;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('The controller did not respond in time. Check its network connection.');
      throw error;
    } finally { window.clearTimeout(timeout); }
  }

  async function poll() {
    if (state.polling || state.reauthRequired) return;
    state.polling = true;
    try {
      const status = await api('/api/status');
      if (state.reauthRequired) return;
      if (!status || !status.atem || !status.config || !status.csrfToken) throw new Error('The controller returned an incomplete status.');
      state.status = status;
      state.reachable = true;
      renderStatus(status);
    } catch (error) {
      if (state.reauthRequired) return;
      state.reachable = false;
      updateConnection();
      if (String(error.message).includes('Sign-in')) {
        state.reauthRequired = true;
        $('connection-alert-text').textContent = error.message;
        $('reload-dashboard').hidden = false;
        $('controller-status').textContent = 'Sign-in required';
        updateAvailability();
      }
    } finally { state.polling = false; }
  }

  async function mutate(path, body, message, success) {
    if (state.busy || !state.reachable || state.reauthRequired) return;
    state.busy = true;
    updateAvailability();
    try {
      const result = await api(path, body);
      if (success) success(result);
      notify(result.message || message);
    } catch (error) { notify(error.message || 'The change could not be applied.', 'error'); }
    finally {
      state.busy = false;
      updateAvailability();
      if (!state.reauthRequired) await poll();
    }
  }

  function readConfig() {
    const atemIp = field('atemIp').value.trim();
    if (atemIp && !/^(\d{1,3}\.){3}\d{1,3}$/.test(atemIp)) throw new Error('Enter a valid ATEM IPv4 address, for example 192.168.1.240.');
    if (atemIp && atemIp.split('.').some((part) => Number(part) > 255)) throw new Error('Each part of the ATEM IPv4 address must be between 0 and 255.');
    if (atemIp && ([0, 127].includes(Number(atemIp.split('.')[0])) || Number(atemIp.split('.')[0]) >= 224)) throw new Error('The ATEM needs a unicast IPv4 address on a reachable network.');
    const wifiPassword = field('wifiPassword').value;
    if (wifiPassword && wifiPassword.length < 8) throw new Error('A new Wi-Fi password must contain at least 8 characters.');
    const adminPassword = field('adminPassword').value;
    if (adminPassword && adminPassword.length < 12) throw new Error('A new administrator password must contain at least 12 characters.');
    if (mappings.some((mapping) => !mapping.querySelector('.map-label').value.trim())) throw new Error('Give each receiver a label before saving.');
    const enabledSources = mappings.filter((mapping) => mapping.querySelector('.map-enabled').checked).map((mapping) => mapping.querySelector('.map-source').value);
    if (new Set(enabledSources).size !== enabledSources.length) throw new Error('Enabled receivers must map to different ATEM inputs.');
    return {
      atemIp, hostname: field('hostname').value.trim(), wifiSsid: field('wifiSsid').value,
      wifiPassword, adminPassword, clearWifiPassword: field('clearWifiPassword').checked,
      acquireMs: Number(field('acquireMs').value), releaseMs: Number(field('releaseMs').value),
      switchIntervalSec: Number(field('switchIntervalSec').value), multiPolicy: field('multiPolicy').value,
      conflictSource: Number(field('conflictSource').value), fallbackSource: Number(field('fallbackSource').value),
      respectExternalControl: field('respectExternalControl').checked,
      inputs: mappings.map((mapping) => ({
        label: mapping.querySelector('.map-label').value.trim(), source: Number(mapping.querySelector('.map-source').value),
        priority: Number(mapping.querySelector('.map-priority').value), enabled: mapping.querySelector('.map-enabled').checked,
        activeHigh: mapping.querySelector('.map-active-high').checked
      }))
    };
  }

  document.querySelectorAll('[data-tab]').forEach((button) => button.addEventListener('click', () => chooseTab(button.dataset.tab)));
  $('dismiss-notification').addEventListener('click', () => { $('notification').hidden = true; });
  $('reload-dashboard').addEventListener('click', () => window.location.reload());
  $('resume-auto').addEventListener('click', () => mutate('/api/mode', { mode: 'auto' }, 'Automatic routing resumed. Check Program feedback above.'));
  $('pause-auto').addEventListener('click', () => mutate('/api/mode', { mode: 'manual' }, 'Automatic routing paused.'));
  document.querySelectorAll('[data-action]').forEach((button) => button.addEventListener('click', () => {
    const action = button.dataset.action;
    const body = { action };
    if (action === 'program' || action === 'preview') body.source = Number(button.dataset.source);
    mutate('/api/action', body, 'Command accepted. Waiting for ATEM feedback.');
  }));
  form.addEventListener('input', () => { state.dirty = true; updateAvailability(); });
  form.addEventListener('change', () => { state.dirty = true; updatePolicyHelp(); updateAvailability(); });
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    if (!form.reportValidity()) return;
    let config;
    try { config = readConfig(); } catch (error) { notify(error.message, 'error'); return; }
    mutate('/api/config', config, 'Settings saved. Automatic routing is paused.', (result) => {
      state.dirty = false;
      state.configSignature = '';
      field('wifiPassword').value = '';
      field('adminPassword').value = '';
      state.rebootRequired = state.rebootRequired || Boolean(result.rebootRequired);
      $('restart-controller').closest('.restart-panel').classList.toggle('reboot-needed', state.rebootRequired);
      $('restart-help').textContent = state.rebootRequired ? 'Settings saved. Restart to apply network changes, then resume automatic routing when ready.' : 'Network changes take effect after restarting. Automatic routing starts paused.';
      if (result.reauthRequired) {
        state.reauthRequired = true;
        $('connection-alert-text').textContent = 'Administrator password changed. Reload this page and sign in with the new password to continue.';
        $('reload-dashboard').hidden = false;
        $('connection-alert').hidden = false;
      }
    });
  });
  $('discard-settings').addEventListener('click', () => { fillSettings(state.status?.config); updateAvailability(); });
  $('restart-controller').addEventListener('click', () => {
    if (!window.confirm('Restart the controller? Automatic routing will start paused.')) return;
    mutate('/api/reboot', {}, 'Restart requested. Wait for the controller to reconnect.', () => {
      state.rebootRequired = false;
      $('restart-controller').closest('.restart-panel').classList.remove('reboot-needed');
      $('restart-help').textContent = 'Network changes take effect after restarting. Automatic routing starts paused.';
    });
  });
  window.addEventListener('beforeunload', (event) => {
    if (!state.dirty) return;
    event.preventDefault();
    event.returnValue = '';
  });
  document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });
  updateAvailability();
  poll();
  window.setInterval(() => { if (!document.hidden) poll(); }, 1000);
})();
