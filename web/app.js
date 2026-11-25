'use strict';
const $ = selector => document.querySelector(selector);
const escapeHtml = value => String(value ?? '').replace(/[&<>"']/g, character => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[character]);
let state, connected = false, polling = false, matrixSignature = '', signalSignature = '', activitySignature = '', toastTimer;
const pending = new Set();
let capturePath, captureBlob, captureGeneration = 0;
const db = rms => rms > 0 ? `${(20 * Math.log10(rms)).toFixed(1)} dBFS` : '−∞ dBFS';
function notify(message) { $('#toast').textContent = message; $('#toast').hidden = false; clearTimeout(toastTimer); toastTimer = setTimeout(() => { $('#toast').hidden = true; }, 3500); }
function endpoints() {
  const sources = [], inputs = [];
  for (const device of state.devices) {
    for (const stream of device.streams) sources.push({ device, channel: stream });
    for (const input of device.inputs) inputs.push({ device, channel: input });
  }
  return { sources, inputs };
}
function findRoute(receiver, input) { return state.routes.find(route => route.receiver === receiver && route.input === input); }
function render() {
  if (!state) return;
  syncHardwareControls();
  const { sources, inputs } = endpoints(), online = state.devices.filter(device => device.online);
  $('#stat-devices').textContent = online.length;
  $('#device-detail').textContent = `${online.filter(d => d.streams.length).length} sending · ${online.filter(d => d.inputs.length).length} receiving`;
  $('#nav-count').textContent = state.devices.length;
  $('#stat-streams').textContent = sources.filter(source => source.device.online).length;
  $('#stat-routes').textContent = state.routes.filter(route => route.active).length;
  $('#route-detail').textContent = `${state.routes.length} saved · Routes reconnect automatically`;
  const structure = JSON.stringify(state.devices.map(d => [d.id, d.name, d.online, d.host, ...d.streams.map(c => [c.id, c.name]), ...d.inputs.map(c => [c.id, c.name])]));
  if (matrixSignature !== structure) {
    matrixSignature = structure;
    if (!sources.length || !inputs.length) $('#matrix').innerHTML = '<div class="empty">Add a sender and a receiver to start routing.<br>For a complete local demo, run <code>openaoip demo</code></div>';
    else $('#matrix').innerHTML = `<table><caption class="sr-only">Audio routing: sources in rows, receiver inputs in columns</caption><thead><tr><th class="source-col" scope="col">SOURCES ↓ &nbsp; RECEIVERS →</th>${inputs.map(({ device, channel }) => `<th class="destination" scope="col">${escapeHtml(device.name)}${device.online ? '' : ' <small class="offline-label">offline</small>'}<span>${escapeHtml(channel.name)}</span></th>`).join('')}</tr></thead><tbody>${sources.map(({ device: source, channel: stream }) => `<tr><th class="source" scope="row">${escapeHtml(source.name)}${source.online ? '' : ' <small class="offline-label">offline</small>'}<span>${escapeHtml(stream.name)} · stereo</span></th>${inputs.map(({ device: receiver, channel: input }) => `<td><button class="route-button" data-sender="${source.id}" data-stream="${stream.id}" data-receiver="${receiver.id}" data-input="${input.id}" aria-label="Route ${escapeHtml(source.name)} ${escapeHtml(stream.name)} to ${escapeHtml(receiver.name)} ${escapeHtml(input.name)}" aria-pressed="false">+</button></td>`).join('')}</tr>`).join('')}</tbody></table>`;
  }
  for (const button of document.querySelectorAll('.route-button')) {
    const { sender, stream, receiver, input } = button.dataset, route = findRoute(receiver, input);
    const selected = route?.sender === sender && route?.stream === stream, busy = pending.has(`${receiver}:${input}`);
    button.classList.toggle('on', selected && route.active); button.classList.toggle('pending', selected && !route.active);
    button.textContent = busy ? '…' : selected ? '✓' : '+'; button.setAttribute('aria-pressed', String(!!selected));
    button.title = selected ? route.active ? 'Connected. Click to disconnect.' : 'Saved route. Waiting for device.' : 'Connect this source';
    button.disabled = !connected || busy || !state.devices.find(d => d.id === sender)?.online || !state.devices.find(d => d.id === receiver)?.online;
  }
  if (signalSignature !== structure) {
    signalSignature = structure;
    $('#signals').innerHTML = inputs.length ? inputs.map(({ device, channel }) => `<article class="signal-card" data-device="${device.id}" data-channel="${channel.id}"><div class="signal-heading"><div><h3>${escapeHtml(device.name)}</h3><small>${escapeHtml(channel.name)}</small></div><span class="status"></span></div><p class="signal-source"></p><div class="meter-row"><div class="meter" role="meter" aria-label="Received audio level" aria-valuemin="-60" aria-valuemax="0"><span></span></div><span class="db"></span></div><div class="signal-bottom"><small class="signal-info"></small><button class="audition" data-device="${device.id}" data-channel="${channel.id}">▷ Audition</button></div></article>`).join('') : '<div class="panel empty">Receiver inputs will appear here.</div>';
  }
  for (const card of document.querySelectorAll('.signal-card')) {
    const device = state.devices.find(d => d.id === card.dataset.device), channel = device.inputs.find(c => c.id === card.dataset.channel), stats = channel.stats;
    const status = !connected ? 'disconnected' : !device.online ? 'offline' : stats.state || 'unrouted';
    const badge = card.querySelector('.status'); badge.className = `status ${status}`; badge.textContent = status;
    card.querySelector('.signal-source').textContent = stats.source || 'No source connected';
    const rms = status === 'receiving' ? Number(stats.rms || 0) : 0, decibels = rms > 0 ? Math.max(-60, Math.min(0, 20 * Math.log10(rms))) : -60;
    card.querySelector('.meter > span').style.width = `${(decibels + 60) / 60 * 100}%`; card.querySelector('.meter').setAttribute('aria-valuenow', decibels.toFixed(1));
    card.querySelector('.db').textContent = db(rms);
    const losses = Number(stats.lost || 0) + Number(stats.late || 0), info = card.querySelector('.signal-info');
    info.textContent = `${stats.latencyMs || 60} ms buffer · ${(Number(stats.jitterMs) || 0).toFixed(2)} ms jitter · ${losses} lost/late`;
    info.title = `${stats.received || 0} received, ${stats.lost || 0} concealed, ${stats.late || 0} late, ${stats.duplicates || 0} duplicates, ${stats.resets || 0} buffer resets, ${stats.schedulerSkipped || 0} scheduler skips`;
    card.querySelector('.audition').disabled = !connected || !device.online || !(stats.capturedSeconds > 0);
  }
  $('#devices').innerHTML = state.devices.length ? state.devices.map(device => `<article class="device-row"><div><h3>${escapeHtml(device.name)} <span class="status ${device.online ? 'receiving' : ''}">${device.online ? 'online' : 'offline'}</span></h3><p>${escapeHtml(device.id)} · ${escapeHtml(device.host)} · ${device.role === 'device' ? 'network device' : device.role}</p>${hardwareDetails(device.audio)}</div><div class="device-meta">${device.streams.length} TX · ${device.inputs.length} RX stereo pairs<br>${device.inputs.length ? `UDP ${device.inputs.map(input => input.port).join(', ')} · monitor TCP ${Number(device.monitorPort)}` : 'RTP / L16 / 48 kHz'}</div></article>`).join('') : '<div class="empty">No registered devices yet.</div>';
  const activity = JSON.stringify(state.events);
  if (activitySignature !== activity) {
    activitySignature = activity;
    $('#activity').innerHTML = state.events.length ? state.events.map(event => `<li><time datetime="${new Date(event.at).toISOString()}">${new Date(event.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}</time><span>${escapeHtml(event.message)}</span></li>`).join('') : '<li>No activity yet.</li>';
  }
}
async function poll() {
  if (polling) return; polling = true;
  try {
    const response = await fetch('/api/state', { cache: 'no-store', signal: AbortSignal.timeout(2500) }); if (!response.ok) throw new Error('Controller is unavailable');
    state = await response.json(); connected = true; $('#connection').className = 'connection online'; $('#connection').innerHTML = '<i></i> Controller online';
    $('#last-update').textContent = `Updated ${new Date().toLocaleTimeString()}`; document.body.classList.remove('disconnected');
  } catch {
    connected = false; $('#connection').className = 'connection offline'; $('#connection').innerHTML = '<i></i> Controller offline';
    $('#last-update').textContent = 'Reconnecting…'; document.body.classList.add('disconnected');
  } finally { polling = false; render(); }
}
$('#matrix').addEventListener('click', async event => {
  const button = event.target.closest('.route-button'); if (!button || button.disabled) return;
  const { sender, stream, receiver, input } = button.dataset, key = `${receiver}:${input}`;
  const old = findRoute(receiver, input), removing = old?.sender === sender && old?.stream === stream;
  pending.add(key); render();
  try {
    const response = await fetch('/api/routes', { method: removing ? 'DELETE' : 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sender, stream, receiver, input }), signal: AbortSignal.timeout(3000) });
    const result = await response.json(); if (!response.ok) throw new Error(result.error || 'Route change failed');
    notify(removing ? 'Route disconnected and saved' : 'Route connected and saved');
    state.routes = state.routes.filter(route => route.receiver !== receiver || route.input !== input);
    if (!removing) state.routes.push({ sender, stream, receiver, input, active: true });
  } catch (error) { notify(error.message); } finally { pending.delete(key); render(); await poll(); }
});
const viewTitles = { routing: ['Network routing', 'Routing matrix', 'Connect your sources. Keep your signal moving.'], devices: ['Your devices', 'Devices', 'Every endpoint, every connection, in one place.'], activity: ['Network activity', 'Activity', 'Follow your connections as they happen.'] };
document.querySelector('nav').addEventListener('click', event => {
  const button = event.target.closest('[data-view]'); if (!button) return;
  for (const [view, titles] of Object.entries(viewTitles)) {
    $(`#${view}-view`).hidden = view !== button.dataset.view; $(`[data-view="${view}"]`).classList.toggle('selected', view === button.dataset.view);
    if (view === button.dataset.view) { $('#title').textContent = titles[0]; $('#breadcrumb').textContent = titles[1]; $('#subtitle').textContent = titles[2]; }
  }
});
async function refreshCapture() {
  if (!capturePath) return;
  const generation = ++captureGeneration; $('#audio-error').textContent = ''; $('#refresh-audio').disabled = true;
  try {
    const response = await fetch(capturePath, { signal: AbortSignal.timeout(4000), cache: 'no-store' });
    if (!response.ok) { const result = await response.json(); throw new Error(result.error || 'Capture unavailable'); }
    const blob = await response.blob(); if (generation !== captureGeneration || !$('#audition-dialog').open) return;
    $('#player').pause(); if (captureBlob) URL.revokeObjectURL(captureBlob); captureBlob = URL.createObjectURL(blob);
    $('#player').src = captureBlob; $('#download-audio').href = captureBlob; $('#download-audio').download = capturePath.split('/').slice(-2).join('-'); $('#download-audio').hidden = false;
    $('#player').play().catch(() => {});
  } catch (error) { if (generation === captureGeneration) $('#audio-error').textContent = error.message; }
  finally { if (generation === captureGeneration) $('#refresh-audio').disabled = false; }
}
$('#signals').addEventListener('click', event => {
  const button = event.target.closest('.audition'); if (!button || button.disabled) return;
  const device = state.devices.find(d => d.id === button.dataset.device);
  $('#audition-title').textContent = `${device.name} / ${device.inputs.find(c => c.id === button.dataset.channel).name}`;
  capturePath = `/api/audio/${button.dataset.device}/${button.dataset.channel}.wav`;
  $('#download-audio').hidden = true; $('#audition-dialog').showModal(); refreshCapture();
});
$('#refresh-audio').addEventListener('click', refreshCapture);
$('#close-audition').addEventListener('click', () => $('#audition-dialog').close());
$('#audition-dialog').addEventListener('close', () => { ++captureGeneration; capturePath = null; $('#player').pause(); $('#player').removeAttribute('src'); if (captureBlob) URL.revokeObjectURL(captureBlob); captureBlob = null; });
let hardwareBusy = false, hardwareLoaded = false;
function hardwareDetails(audio) {
  if (audio?.mode === 'device') return hardwareDetails(audio.capture) + hardwareDetails(audio.playback);
  if (!audio || !['capture', 'playback'].includes(audio.mode)) return '';
  return `<p class="hardware-details"><strong>${escapeHtml(audio.device)}</strong><br>${escapeHtml(audio.backend)} · ${escapeHtml(audio.mode)} ${escapeHtml(audio.state)} · ${Number(audio.channels)} channels · map ${escapeHtml((audio.channelMap || []).join(','))}<br>${Number(audio.droppedFrames || 0)} dropped frames · ${Number(audio.underrunFrames || 0)} underrun frames · ${Number(audio.driftCorrections || 0)} playback adjustments</p>`;
}
function syncHardwareControls() {
  const active = !!state?.localAudioActive;
  $('#open-hardware').textContent = active ? 'Audio hardware ●' : 'Audio hardware';
  $('#hardware-fields').disabled = hardwareBusy || !hardwareLoaded || active;
  $('#start-hardware').disabled = hardwareBusy || !hardwareLoaded || active || !connected;
  $('#stop-hardware').disabled = hardwareBusy || !active || !connected;
  $('#hardware-status').textContent = hardwareBusy ? 'Updating audio devices…' : active ? 'Local audio is running. Use the routing matrix to connect it. Stop local audio to change devices.' : '';
}
$('#open-hardware').addEventListener('click', async () => {
  $('#hardware-dialog').showModal(); $('#hardware-error').textContent = ''; hardwareLoaded = false; syncHardwareControls();
  try {
    const response = await fetch('/api/audio-devices', { cache: 'no-store', signal: AbortSignal.timeout(8000) });
    const devices = await response.json(); if (!response.ok) throw new Error(devices.error || 'Audio device scan failed');
    $('#hardware-backend').textContent = `Native audio backend: ${devices.backend}`;
    for (const direction of ['capture', 'playback']) {
      const items = devices[direction];
      $(`#${direction}-device`).innerHTML = '<option value="">Off</option>' + (items.length ? '<option value="default">System default</option>' : '') + items.map(device => `<option value="${Number(device.index)}">${escapeHtml(device.name)}${device.default ? ' (default)' : ''}</option>`).join('');
      $(`#${direction}-device`).value = items.length ? 'default' : '';
    }
    if (state?.localAudioActive && state.localAudioConfig) {
      const config = state.localAudioConfig;
      $('#capture-device').value = config.capture; $('#playback-device').value = config.playback;
      $('#capture-streams').value = config.streams; $('#playback-inputs').value = config.inputs;
      $('#capture-map').value = config.captureMap.join(','); $('#playback-map').value = config.playbackMap.join(',');
      $('#playback-volume').value = config.volume; $('#hardware-latency').value = config.latency;
    }
    hardwareLoaded = true;
    updateChannelMapHints();
  } catch (error) { $('#hardware-backend').textContent = 'Audio devices unavailable'; $('#hardware-error').textContent = error.message; }
  finally { syncHardwareControls(); }
});
$('#close-hardware').addEventListener('click', () => $('#hardware-dialog').close());
function readChannelMap(selector, streams, direction) {
  const text = $(selector).value.trim(); if (!text) return [];
  const parts = text.split(',').map(value => value.trim());
  if (parts.some(value => !/^\d+$/.test(value) || Number(value) < 1 || Number(value) > 32)) throw new Error('Use physical channel numbers 1–32, separated by commas.');
  if (parts.length !== streams * 2) throw new Error(`${direction}: ${streams} stereo ${streams === 1 ? 'stream needs' : 'streams need'} ${streams * 2} channel numbers; you entered ${parts.length}. ${streams === 1 ? direction === 'Capture' ? 'Use 1,1 for a mono microphone.' : 'Use 1,2 for left/right output.' : 'Enter two numbers for each stereo pair.'}`);
  if (direction === 'Playback' && new Set(parts.map(Number)).size !== parts.length) throw new Error('Playback: each output channel can be assigned only once. Use 1,2 for stereo output.');
  return parts.map(Number);
}
function updateChannelMapHints() {
  for (const [kind, countSelector, direction] of [['capture', '#capture-streams', 'Capture'], ['playback', '#playback-inputs', 'Playback']]) {
    const input = $(`#${kind}-map`), help = $(`#${kind}-map-help`);
    const count = Number($(countSelector).value), enabled = !!$(`#${kind}-device`).value;
    let error = '';
    try { if (enabled) readChannelMap(`#${kind}-map`, count, direction); } catch (failure) { error = failure.message; }
    input.setCustomValidity(error); input.setAttribute('aria-invalid', String(!!error));
    help.textContent = error || (!enabled ? `${direction} is off.` : `Auto or ${count * 2} channel numbers (${count} stereo ${count === 1 ? 'pair' : 'pairs'}). ${kind === 'capture' ? 'Mono microphone: 1,1.' : 'Left/right output: 1,2.'}`);
    help.classList.toggle('error', !!error);
  }
}
for (const selector of ['#capture-streams', '#playback-inputs', '#capture-map', '#playback-map', '#capture-device', '#playback-device']) $(selector).addEventListener('input', updateChannelMapHints);
async function hardwareAction(method, body) {
  hardwareBusy = true; $('#hardware-error').textContent = ''; syncHardwareControls();
  try {
    const response = await fetch('/api/hardware', { method, headers: { 'Content-Type': 'application/json' }, ...(body ? { body: JSON.stringify(body) } : {}), signal: AbortSignal.timeout(15000) });
    const result = await response.json(); if (!response.ok) throw new Error(result.error || 'Hardware configuration failed');
    if (state) state.localAudioActive = method === 'POST';
    notify(method === 'POST' ? 'Audio hardware started. Connect it in the routing matrix.' : 'Local audio stopped.'); await poll();
  } catch (error) { $('#hardware-error').textContent = error.message; }
  finally { hardwareBusy = false; syncHardwareControls(); }
}
$('#hardware-form').addEventListener('submit', event => {
  event.preventDefault();
  try {
    const capture = $('#capture-device').value, playback = $('#playback-device').value;
    if (!capture && !playback) throw new Error('Select at least one microphone or playback device.');
    hardwareAction('POST', { capture, playback, streams: Number($('#capture-streams').value), inputs: Number($('#playback-inputs').value),
      captureMap: capture ? readChannelMap('#capture-map', Number($('#capture-streams').value), 'Capture') : [], playbackMap: playback ? readChannelMap('#playback-map', Number($('#playback-inputs').value), 'Playback') : [], volume: Number($('#playback-volume').value), latency: Number($('#hardware-latency').value) });
  } catch (error) { $('#hardware-error').textContent = error.message; }
});
$('#stop-hardware').addEventListener('click', () => hardwareAction('DELETE'));
let networkInfo;
function updateNetworkCommand() {
  const id = $('#network-device-id').value.trim(), url = $('#network-address').value;
  const valid = /^[a-zA-Z0-9_-]{1,48}$/.test(id) && !!url;
  $('#copy-network-command').disabled = !valid;
  $('#network-copy-status').textContent = '';
  $('#network-command').textContent = valid ? `openaoip.exe device --id ${id} --controller ${url} ${$('#network-test').checked ? '--tone 440 --playback off --port 0 --monitor-port 0' : '--capture default --playback default'}` : 'Choose a controller address and a unique device ID.';
}
$('#open-network').addEventListener('click', async () => {
  $('#network-dialog').showModal(); $('#network-warning').textContent = ''; $('#network-command').textContent = 'Loading network addresses…'; $('#copy-network-command').disabled = true;
  try {
    const response = await fetch('/api/network', { cache: 'no-store', signal: AbortSignal.timeout(5000) });
    networkInfo = await response.json(); if (!response.ok) throw new Error(networkInfo.error || 'Network information unavailable');
    const addresses = networkInfo.addresses;
    $('#network-address').innerHTML = addresses.length ? addresses.map(item => `<option value="${escapeHtml(item.controllerUrl)}">${escapeHtml(item.controllerUrl)}${item.reachable ? '' : ' (controller bound locally)'}</option>`).join('') : `<option value="${escapeHtml(networkInfo.loopbackUrl)}">Local computer only</option>`;
    const current = addresses.find(item => item.address === location.hostname && item.reachable) || addresses.find(item => item.reachable);
    if (current) $('#network-address').value = current.controllerUrl;
    if (!addresses.some(item => item.reachable)) $('#network-warning').textContent = `This controller is available locally only. To accept devices from other computers, restart it with: openaoip.exe controller --host 0.0.0.0 --port ${networkInfo.port}`;
    updateNetworkCommand();
  } catch (error) { $('#network-warning').textContent = error.message; }
});
$('#close-network').addEventListener('click', () => $('#network-dialog').close());
for (const id of ['network-address', 'network-device-id', 'network-test']) $(`#${id}`).addEventListener('input', updateNetworkCommand);
$('#copy-network-command').addEventListener('click', async () => {
  try { await navigator.clipboard.writeText($('#network-command').textContent); $('#network-copy-status').textContent = 'Command copied'; }
  catch { $('#network-copy-status').textContent = 'Select the command above and copy it manually.'; }
});
poll(); setInterval(poll, 750);
