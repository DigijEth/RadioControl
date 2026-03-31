// RadioControl WebUI — Hardware Interface Controller

let config = {};
let currentTab = 'radio';
let toastTimer = null;
let atHistory = [];

// ---- API ----
async function api(path, opts = {}) {
  try {
    const res = await fetch(path, opts);
    return await res.json();
  } catch (e) {
    showToast('Connection error', 'error');
    return null;
  }
}

const post = (path, body) => api(path, {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(body)
});

// ---- Toast ----
function showToast(msg, type = 'success') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast ' + type + ' show';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.className = 'toast', 2500);
}

// ---- Tabs ----
function switchTab(tab) {
  currentTab = tab;
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab-item').forEach(t => t.classList.remove('active'));
  document.getElementById('page-' + tab).classList.add('active');
  document.querySelector(`[data-tab="${tab}"]`).classList.add('active');
  refreshTab(tab);
}

async function refreshTab(tab) {
  switch (tab) {
    case 'radio': await loadRadio(); await loadCPDebug(); break;
    case 'terminal': await loadTerminal(); break;
    case 'wifi': await loadWifi(); break;
    case 'carrier': await loadCarrier(); break;
    case 'debug': await loadDebug(); await loadThreadInfo(); break;
    case 'flags': await loadFlags(); break;
  }
}

// ---- Radio page ----
async function loadRadio() {
  config = await api('/api/status') || config;
  const radio = await api('/api/radio');

  setToggle('toggle-eng', config.engineering_mode);
  setToggle('toggle-factory', config.factory_test_mode);
  setToggle('toggle-hidden', config.hidden_menus);
  setToggle('toggle-diag', config.usb_diag_mode);
  setToggle('toggle-modem', config.modem_log);

  updateChips();

  if (radio) {
    setText('info-soc', radio.soc_family || 'unknown');
    setText('info-baseband', radio.baseband || 'N/A');
    setText('info-chipset', radio.chipset || 'N/A');
    setText('info-network', radio.network_type || 'N/A');
    setText('info-operator', radio.operator || 'N/A');
    setText('info-sim', radio.sim_state || 'N/A');
    setText('soc-badge', `${(radio.soc_family || 'unknown').toUpperCase()} — ${radio.chipset || '?'}`);
  }

  // Modem interfaces
  const ifaces = await api('/api/modem/interfaces');
  const mc = document.getElementById('modem-interfaces');
  if (ifaces && ifaces.length > 0) {
    mc.innerHTML = ifaces.map(i => `
      <div class="card-row">
        <div class="card-row-info">
          <span class="prop-name">${i.path}</span>
          <div class="card-row-desc">${i.perms}</div>
        </div>
      </div>`).join('');
  } else {
    mc.innerHTML = '<div class="card-row"><div class="card-row-desc">No modem interfaces detected</div></div>';
  }

  // Thermal
  const thermal = await api('/api/thermal');
  const tc = document.getElementById('thermal-info');
  if (thermal && thermal.length > 0) {
    tc.innerHTML = thermal.map(t => {
      const tempC = (t.temp / 1000).toFixed(1);
      const color = t.temp > 60000 ? 'var(--danger)' : t.temp > 45000 ? 'var(--warning)' : 'var(--success)';
      return `<div class="card-row">
        <div class="card-row-info">
          <div class="card-row-label">${t.type}</div>
          <div class="card-row-desc">${t.zone}</div>
        </div>
        <span style="color:${color};font-weight:600;font-family:monospace">${tempC}&deg;C</span>
      </div>`;
    }).join('');
  } else {
    tc.innerHTML = '<div class="card-row"><div class="card-row-desc">No RF thermal zones</div></div>';
  }
}

function setToggle(id, val) {
  const el = document.getElementById(id);
  if (el) el.checked = val === '1' || val === 1;
}
function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}

function updateChips() {
  setChip('chip-eng', config.engineering_mode === '1');
  setChip('chip-factory', config.factory_test_mode === '1');
  setChip('chip-wifi', config.wifi_mode !== 'managed' && config.wifi_mode);
}

function setChip(id, on) {
  const dot = document.querySelector(`#${id} .dot`);
  if (dot) dot.className = 'dot ' + (on ? '' : 'off');
}

async function handleToggle(key, el) {
  const val = el.checked ? '1' : '0';
  const res = await post('/api/config', { key, value: val });
  if (res && res.ok) {
    config[key.toLowerCase()] = val;
    updateChips();
    showToast('Updated — reboot to apply');
  } else {
    el.checked = !el.checked;
    showToast('Failed', 'error');
  }
}

async function reboot() {
  if (confirm('Reboot to apply all changes?')) {
    await post('/api/reboot', {});
    showToast('Rebooting...');
  }
}

// ---- AT Terminal ----

// AT command presets — verified against Shannon 5400 (S5400BUNUELO) AT+CLAC
// Pixel 10 Pro Fold / Tensor G5 / laguna
const AT_PRESETS = {
  common: [
    { cmd: 'AT', desc: 'Modem alive check' },
    { cmd: 'AT+CFUN?', desc: 'Radio function state (0=off, 1=on, 4=airplane)' },
    { cmd: 'AT+CFUN=1', desc: 'Turn radio ON' },
    { cmd: 'AT+CFUN=0', desc: 'Turn radio OFF' },
    { cmd: 'AT+COPS?', desc: 'Current operator' },
    { cmd: 'AT+COPS=?', desc: 'Scan all available networks (slow ~30s)' },
    { cmd: 'AT+CSQ', desc: 'Signal quality (RSSI, BER)' },
    { cmd: 'AT+CESQ', desc: 'Extended signal: RXLEV,BER,RSCP,Ec/No,RSRQ,RSRP,SINR' },
    { cmd: 'AT+CREG?', desc: '2G/3G registration status' },
    { cmd: 'AT+CEREG?', desc: 'LTE registration status' },
    { cmd: 'AT+CGREG?', desc: 'GPRS registration status' },
    { cmd: 'AT+CGDCONT?', desc: 'PDP context / APN list' },
    { cmd: 'AT+CGPADDR', desc: 'PDP addresses (IP assignments)' },
    { cmd: 'AT+CGATT?', desc: 'PS attach status' },
    { cmd: 'AT+CPIN?', desc: 'SIM status (READY/PIN/PUK/not inserted)' },
    { cmd: 'AT+CGSN', desc: 'IMEI(s)' },
    { cmd: 'AT+CIMI', desc: 'IMSI (SIM identity)' },
    { cmd: 'AT+CNUM', desc: 'Subscriber number (phone number)' },
    { cmd: 'AT+CGMI', desc: 'Manufacturer (Samsung Electronics)' },
    { cmd: 'AT+CGMM', desc: 'Model (S5400BUNUELO)' },
    { cmd: 'AT+CGMR', desc: 'Firmware revision' },
    { cmd: 'AT+CLAC', desc: 'List ALL supported AT commands' },
    { cmd: 'AT+WS46?', desc: 'Wireless data service type' },
    { cmd: 'AT+CEMODE?', desc: 'UE mode of operation (CS/PS)' },
  ],
  // Shannon 5400 specific commands (verified on device)
  tensor: [
    { cmd: 'AT+SCELL', desc: 'Serving cell info (band, EARFCN, PCI, RSRP)' },
    { cmd: 'AT+NCELL', desc: 'Neighbor cell list' },
    { cmd: 'AT+RSRP', desc: 'Reference Signal Received Power' },
    { cmd: 'AT+RSRQ', desc: 'Reference Signal Received Quality' },
    { cmd: 'AT+RSCP', desc: 'Received Signal Code Power (3G)' },
    { cmd: 'AT+ECNO', desc: 'Ec/No (3G signal quality)' },
    { cmd: 'AT$RSRP', desc: 'Extended RSRP readout' },
    { cmd: 'AT$RSRQ', desc: 'Extended RSRQ readout' },
    { cmd: 'AT$CSQ', desc: 'Extended signal quality' },
    { cmd: 'AT$CREG', desc: 'Extended registration info' },
    { cmd: 'AT$ROAM', desc: 'Roaming status/config' },
    { cmd: 'AT$ARME', desc: 'ARM Engine — modem subsystem control' },
    { cmd: 'AT$ARMEE', desc: 'ARM Engine extended' },
    { cmd: 'AT*CNTI', desc: 'Current Network Technology Indicator' },
    { cmd: 'AT+NBLTESCAN', desc: 'NB-IoT/LTE cell scan' },
    { cmd: 'AT+DEVFEAT', desc: 'Device feature flags' },
    { cmd: 'AT+HANDLEDRX', desc: 'DRX (Discontinuous Reception) control' },
    { cmd: 'AT+PING', desc: 'Modem-level ping test' },
    { cmd: 'AT+PACSP?', desc: 'Preferred ACSP setting' },
    { cmd: 'AT+SKYLOTEST', desc: 'Satellite IoT (Skylo/NTN) test mode' },
    { cmd: 'AT+CPSMS?', desc: 'Power Saving Mode settings' },
    { cmd: 'AT+CCIOTOPT?', desc: 'CIoT optimization config' },
    { cmd: 'AT+CGEQOS?', desc: 'EPS QoS parameters' },
    { cmd: 'AT+C5GQOS?', desc: '5G QoS parameters' },
    { cmd: 'AT+C5GNSSAI?', desc: '5G Network Slice (NSSAI) config' },
    { cmd: 'AT+C5GPNSSAI?', desc: '5G Preferred NSSAI' },
    { cmd: 'AT+C5GUSMS?', desc: '5G USMS config' },
    { cmd: 'AT+CSUPI?', desc: 'SUPI (5G subscriber permanent identity)' },
    { cmd: 'AT+CVMOD?', desc: 'Voice mode preference' },
    { cmd: 'AT+AIMSCH', desc: 'IMS channel status' },
    { cmd: 'AT+AIMSDB', desc: 'IMS debug info' },
    { cmd: 'AT+AIMSRX', desc: 'IMS receive status' },
    { cmd: 'AT+VZWRSRP', desc: 'Verizon RSRP (carrier-specific signal)' },
    { cmd: 'AT+VZWRSRQ', desc: 'Verizon RSRQ' },
    { cmd: 'AT+VZWAPNE?', desc: 'Verizon APN entries' },
    { cmd: 'AT+SKT', desc: 'SK Telecom specific query' },
    { cmd: 'AT+CEER', desc: 'Extended error report (last call/data failure reason)' },
  ],
  // Qualcomm still relevant for other devices
  qualcomm: [
    { cmd: 'AT+QENG="servingcell"', desc: 'Serving cell engineering info' },
    { cmd: 'AT+QENG="neighbourcell"', desc: 'Neighbor cell list' },
    { cmd: 'AT+QCAINFO', desc: 'Active carrier aggregation status' },
    { cmd: 'AT+QNWPREFCFG="mode_pref"', desc: 'Network mode preference' },
    { cmd: 'AT+QNWPREFCFG="lte_band"', desc: 'LTE band preference' },
    { cmd: 'AT+QNWPREFCFG="nr5g_band"', desc: 'NR band preference' },
    { cmd: 'AT+QCFG="band"', desc: 'Band lock config (hex bitmask)' },
    { cmd: 'AT+QRSRP', desc: 'Per-antenna RSRP' },
    { cmd: 'AT+QMBNCFG="list"', desc: 'MBN carrier config profiles' },
    { cmd: 'AT+QNWLOCK="common/4g"', desc: 'Cell lock status' },
    { cmd: 'AT+QSCAN=1', desc: 'Quick cell scan' },
  ],
  exynos: [
    { cmd: 'AT+SCELL', desc: 'Serving cell info' },
    { cmd: 'AT+NCELL', desc: 'Neighbor cells' },
    { cmd: 'AT+RSRP', desc: 'RSRP' },
    { cmd: 'AT+RSRQ', desc: 'RSRQ' },
    { cmd: 'AT$RSRP', desc: 'Extended RSRP' },
    { cmd: 'AT$RSRQ', desc: 'Extended RSRQ' },
    { cmd: 'AT$ARME', desc: 'ARM Engine control' },
    { cmd: 'AT*CNTI', desc: 'Network technology indicator' },
    { cmd: 'AT+NBLTESCAN', desc: 'Cell scan' },
    { cmd: 'AT+DEVFEAT', desc: 'Device features' },
  ],
};

// Full Shannon 5400 AT command reference — verified via AT+CLAC on Pixel 10 Pro Fold
// Modem: S5400BUNUELO, Firmware: g5400i-251201-260127-B-14784805
// RIL: Samsung S.LSI Vendor RIL 5400 V2.3
const DISCOVERED_COMMANDS = {
  'Signal & Cell Info': [
    { cmd: 'AT+SCELL', desc: 'Serving cell info — band, EARFCN, PCI, RSRP, timing advance' },
    { cmd: 'AT+NCELL', desc: 'Neighbor cell list with signal levels' },
    { cmd: 'AT+RSRP', desc: 'Reference Signal Received Power (LTE/NR)' },
    { cmd: 'AT+RSRQ', desc: 'Reference Signal Received Quality (LTE/NR)' },
    { cmd: 'AT+RSCP', desc: 'Received Signal Code Power (3G WCDMA)' },
    { cmd: 'AT+ECNO', desc: 'Ec/No ratio (3G signal quality metric)' },
    { cmd: 'AT$RSRP', desc: 'Extended RSRP — detailed per-antenna/per-carrier readout' },
    { cmd: 'AT$RSRQ', desc: 'Extended RSRQ — detailed per-antenna/per-carrier readout' },
    { cmd: 'AT$CSQ', desc: 'Extended signal quality beyond standard +CSQ' },
    { cmd: 'AT+CESQ', desc: 'Extended signal: RXLEV, BER, RSCP, Ec/No, RSRQ, RSRP, SINR (all in one)' },
    { cmd: 'AT+CSQ', desc: 'Basic signal quality — RSSI (0-31) and BER (0-7)' },
    { cmd: 'AT+NBLTESCAN', desc: 'NB-IoT / LTE cell scan — finds all visible cells' },
    { cmd: 'AT*CNTI', desc: 'Current Network Technology Indicator (GSM/WCDMA/LTE/NR)' },
  ],
  'Registration & Network': [
    { cmd: 'AT+CREG?', desc: 'CS (circuit-switched) registration — 2G/3G voice' },
    { cmd: 'AT+CGREG?', desc: 'PS (packet-switched) registration — GPRS/EDGE data' },
    { cmd: 'AT+CEREG?', desc: 'EPS registration — LTE status, TAC, cell ID' },
    { cmd: 'AT$CREG', desc: 'Extended registration with extra detail' },
    { cmd: 'AT+COPS?', desc: 'Current operator (MCC/MNC, access tech)' },
    { cmd: 'AT+COPS=?', desc: 'Scan all networks (~30 seconds, returns PLMN list)' },
    { cmd: 'AT$ROAM', desc: 'Roaming status and configuration' },
    { cmd: 'AT+COPN', desc: 'Read operator name lookup table' },
  ],
  'Radio Control': [
    { cmd: 'AT+CFUN=1', desc: 'Turn radio ON (full functionality)' },
    { cmd: 'AT+CFUN=0', desc: 'Turn radio OFF (minimum functionality)' },
    { cmd: 'AT+CFUN=4', desc: 'Airplane mode (TX disabled, some RX may work)' },
    { cmd: 'AT+CFUN?', desc: 'Query current radio state' },
    { cmd: 'AT+WS46?', desc: 'Wireless data service type selection' },
    { cmd: 'AT+CEMODE?', desc: 'UE mode of operation — CS only, PS only, CS+PS' },
    { cmd: 'AT+CVMOD?', desc: 'Voice mode preference (CS voice vs VoLTE)' },
    { cmd: 'AT+CGATT=1', desc: 'PS attach (connect to data)' },
    { cmd: 'AT+CGATT=0', desc: 'PS detach (disconnect data)' },
    { cmd: 'AT+HANDLEDRX', desc: 'DRX (Discontinuous Reception) control — affects power/latency' },
    { cmd: 'AT+CPSMS?', desc: 'Power Saving Mode config (for IoT/battery optimization)' },
    { cmd: 'AT+CCIOTOPT?', desc: 'CIoT EPS optimization — control plane vs user plane' },
  ],
  '5G / NR / Network Slicing': [
    { cmd: 'AT+C5GNSSAI?', desc: '5G network slice selection (S-NSSAI list)' },
    { cmd: 'AT+C5GNSSAIRDP', desc: 'Read dynamic 5G NSSAI parameters' },
    { cmd: 'AT+C5GPNSSAI?', desc: '5G preferred NSSAI configuration' },
    { cmd: 'AT+C5GQOS?', desc: '5G QoS flow parameters (5QI, MBR, GBR)' },
    { cmd: 'AT+C5GUSMS?', desc: '5G USMS (User Services Management) config' },
    { cmd: 'AT+CSUPI?', desc: 'SUPI — 5G Subscription Permanent Identifier' },
  ],
  'Data / APN / Bearer': [
    { cmd: 'AT+CGDCONT?', desc: 'PDP context list — all configured APNs' },
    { cmd: 'AT+CGPADDR', desc: 'PDP addresses — current IP assignments per CID' },
    { cmd: 'AT+CGACT?', desc: 'PDP context activation status' },
    { cmd: 'AT+CGCONTRDP', desc: 'Read dynamic PDP context parameters' },
    { cmd: 'AT+CGEQOS?', desc: 'EPS bearer QoS parameters' },
    { cmd: 'AT+CGEQOSRDP', desc: 'Read dynamic EPS QoS' },
    { cmd: 'AT+CGTFT?', desc: 'Traffic Flow Template — packet filters' },
    { cmd: 'AT+CGTFTRDP', desc: 'Read dynamic TFT parameters' },
    { cmd: 'AT+CGAUTH?', desc: 'PDP context authentication (PAP/CHAP)' },
    { cmd: 'AT+CGDSCONT?', desc: 'Secondary PDP context definitions' },
    { cmd: 'AT+CGSCONTRDP', desc: 'Read dynamic secondary PDP params' },
    { cmd: 'AT+CGEREP?', desc: 'Packet domain event reporting' },
    { cmd: 'AT+CRTDCP?', desc: 'Reporting of terminating data via control plane' },
    { cmd: 'AT+CSODCP?', desc: 'Sending originating data via control plane' },
    { cmd: 'AT+PING', desc: 'Modem-level ping test (bypasses Android IP stack)' },
  ],
  'IMS (IP Multimedia Subsystem)': [
    { cmd: 'AT+AIMSCH', desc: 'IMS channel status' },
    { cmd: 'AT+AIMSCH8', desc: 'IMS channel status (extended/8-bit)' },
    { cmd: 'AT+AIMSCY', desc: 'IMS cycle/session info' },
    { cmd: 'AT+AIMSCY8', desc: 'IMS cycle info (extended)' },
    { cmd: 'AT+AIMSDB', desc: 'IMS debug information dump' },
    { cmd: 'AT+AIMSDEREG', desc: 'IMS deregistration — force disconnect from IMS' },
    { cmd: 'AT+AIMSRX', desc: 'IMS receive status/config' },
    { cmd: 'AT+AIMSTR', desc: 'IMS transfer/transaction info' },
  ],
  'SIM & Identity': [
    { cmd: 'AT+CPIN?', desc: 'SIM status — READY, SIM PIN, SIM PUK, not inserted' },
    { cmd: 'AT+CGSN', desc: 'IMEI(s) — both slots on dual-SIM' },
    { cmd: 'AT+CIMI', desc: 'IMSI — subscriber identity from SIM' },
    { cmd: 'AT+CNUM', desc: 'Subscriber phone number from SIM' },
    { cmd: 'AT+CPLS?', desc: 'Preferred PLMN list selector' },
    { cmd: 'AT+CPOL?', desc: 'Preferred operator list from SIM' },
    { cmd: 'AT+CUAD', desc: 'UICC application discovery' },
    { cmd: 'AT+CCHO', desc: 'Open logical channel to UICC app' },
    { cmd: 'AT+CCHC', desc: 'Close logical channel' },
    { cmd: 'AT+CGLA', desc: 'Generic UICC logical channel access (raw APDU)' },
    { cmd: 'AT+CRSM', desc: 'Restricted SIM access (read/write SIM files)' },
    { cmd: 'AT+CSIM', desc: 'Generic SIM access (raw APDU to SIM)' },
    { cmd: 'AT+CLCK', desc: 'Facility lock (PIN enable/disable, network lock query)' },
    { cmd: 'AT+CPWD', desc: 'Change password (PIN/PIN2/PUK)' },
  ],
  'Modem Internals & Debug': [
    { cmd: 'AT+CGMI', desc: 'Manufacturer: Samsung Electronics' },
    { cmd: 'AT+CGMM', desc: 'Model: S5400BUNUELO (Shannon 5400)' },
    { cmd: 'AT+CGMR', desc: 'Firmware: g5400i-251201-260127-B-14784805' },
    { cmd: 'AT+GCAP', desc: 'Modem capabilities (+CGSM)' },
    { cmd: 'AT$ARME', desc: 'ARM Engine — low-level modem subsystem control' },
    { cmd: 'AT$ARMEE', desc: 'ARM Engine extended — deeper subsystem access' },
    { cmd: 'AT+DEVFEAT', desc: 'Device feature flags — query supported hardware features' },
    { cmd: 'AT+CEER', desc: 'Extended error report — last call/data failure cause code' },
    { cmd: 'AT+CMEE=2', desc: 'Enable verbose error messages (CME ERROR text)' },
    { cmd: 'AT+CLAC', desc: 'List ALL supported AT commands' },
    { cmd: 'AT+PACSP?', desc: 'Preferred ACSP setting' },
  ],
  'Satellite / NTN': [
    { cmd: 'AT+SKYLOTEST', desc: 'Satellite IoT (Skylo/NTN) test mode — non-terrestrial network testing' },
  ],
  'Carrier-Specific (Hidden)': [
    { cmd: 'AT+VZWRSRP', desc: 'Verizon RSRP — carrier-specific signal measurement' },
    { cmd: 'AT+VZWRSRQ', desc: 'Verizon RSRQ — carrier-specific signal quality' },
    { cmd: 'AT+VZWAPNE?', desc: 'Verizon APN entries — hidden APN config' },
    { cmd: 'AT+VZWMRUC', desc: 'Verizon most recently used channel' },
    { cmd: 'AT+VZWMRUE', desc: 'Verizon most recently used EARFCN' },
    { cmd: 'AT+VZWRPLMNC', desc: 'Verizon roaming PLMN config' },
    { cmd: 'AT+SKT', desc: 'SK Telecom specific query/config' },
  ],
  'SMS & Call': [
    { cmd: 'AT+CMGF?', desc: 'SMS message format (0=PDU, 1=text)' },
    { cmd: 'AT+CMGL="ALL"', desc: 'List SMS messages' },
    { cmd: 'AT+CNMI?', desc: 'New message indication settings' },
    { cmd: 'AT+CSCA?', desc: 'SMS service center address' },
    { cmd: 'AT+CGSMS?', desc: 'Select service for MO SMS (PS vs CS)' },
    { cmd: 'AT+CNMPSD', desc: 'Non-MO PS data indication' },
    { cmd: 'AT+CLCC', desc: 'List current calls' },
    { cmd: 'AT+CHLD=?', desc: 'Call hold/multiparty capabilities' },
    { cmd: 'AT+CLIP?', desc: 'Caller ID presentation status' },
    { cmd: 'AT+CLIR?', desc: 'Caller ID restriction status' },
    { cmd: 'AT+CCWA?', desc: 'Call waiting status' },
    { cmd: 'AT+CCFC=?', desc: 'Call forwarding capabilities' },
    { cmd: 'AT+CUSD=?', desc: 'USSD capabilities' },
  ],
};

async function loadTerminal() {
  config = await api('/api/status') || config;
  const soc = config.detected_soc || 'unknown';

  // Render quick presets
  const presets = [...AT_PRESETS.common, ...(AT_PRESETS[soc] || [])];
  const container = document.getElementById('at-presets');
  document.getElementById('at-presets-title').textContent =
    `Quick Commands — ${soc.toUpperCase()}`;

  container.innerHTML = presets.map(p => `
    <div class="card-row" style="cursor:pointer" onclick="runPreset('${p.cmd.replace(/'/g, "\\'")}')">
      <div class="card-row-info">
        <span class="prop-name" style="color:var(--accent)">${escHtml(p.cmd)}</span>
        <div class="card-row-desc">${p.desc}</div>
      </div>
      <svg width="14" height="14" viewBox="0 0 24 24" fill="var(--text-muted)"><path d="M8 5v14l11-7z"/></svg>
    </div>
  `).join('');

  // Render discovered commands reference
  renderDiscoveredCommands();

  renderATHistory();
}

function runPreset(cmd) {
  document.getElementById('at-input').value = cmd;
  sendATCmd();
}

async function sendATCmd() {
  const input = document.getElementById('at-input');
  const cmd = input.value.trim();
  if (!cmd) return;

  atHistory.push({ type: 'cmd', text: cmd, time: new Date().toLocaleTimeString() });
  renderATHistory();
  input.value = '';

  const res = await post('/api/at', { cmd, timeout: '5' });
  if (res && res.response) {
    atHistory.push({ type: 'resp', text: res.response, time: new Date().toLocaleTimeString() });
  } else {
    atHistory.push({ type: 'err', text: 'No response / timeout', time: new Date().toLocaleTimeString() });
  }
  renderATHistory();
}

function renderATHistory() {
  const el = document.getElementById('at-history');
  if (atHistory.length === 0) {
    el.innerHTML = '<div style="color:var(--text-muted);font-size:12px;padding:8px">No commands sent yet. Type an AT command or tap a preset below.</div>';
    return;
  }
  el.innerHTML = atHistory.map(h => {
    if (h.type === 'cmd') return `<div class="term-cmd"><span class="term-prompt">&gt;</span> ${escHtml(h.text)} <span class="term-time">${h.time}</span></div>`;
    if (h.type === 'err') return `<div class="term-err">${escHtml(h.text)}</div>`;
    return `<div class="term-resp">${escHtml(h.text)}</div>`;
  }).join('');
  el.scrollTop = el.scrollHeight;
}

function renderDiscoveredCommands() {
  const container = document.getElementById('discovered-commands');
  if (!container) return;

  container.innerHTML = Object.entries(DISCOVERED_COMMANDS).map(([category, cmds]) => `
    <div class="card" style="margin-bottom:8px">
      <div class="card-row" style="cursor:pointer" onclick="this.nextElementSibling.style.display=this.nextElementSibling.style.display==='none'?'block':'none'">
        <div class="card-row-info">
          <div class="card-row-label">${category}</div>
          <div class="card-row-desc">${cmds.length} commands</div>
        </div>
        <span style="color:var(--text-muted);font-size:12px">tap to expand</span>
      </div>
      <div style="display:none">
        ${cmds.map(c => `
          <div class="card-row" style="cursor:pointer" onclick="runPreset('${c.cmd.replace(/'/g, "\\'")}')">
            <div class="card-row-info">
              <span class="prop-name" style="color:var(--accent)">${escHtml(c.cmd)}</span>
              <div class="card-row-desc">${c.desc}</div>
            </div>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="var(--text-muted)"><path d="M8 5v14l11-7z"/></svg>
          </div>
        `).join('')}
      </div>
    </div>
  `).join('');
}

// ---- WiFi page ----
async function loadWifi() {
  config = await api('/api/status') || config;

  // Kmod status
  const kmod = await api('/api/kmod');
  if (kmod) {
    updateKmodBtn('btn-kmod-wifi', kmod.wifi_mon);
    updateKmodBtn('btn-kmod-shannon', kmod.shannon_cmd);
    updateKmodBtn('btn-kmod-diag', kmod.diag_bridge);
    setChip('chip-kmod', kmod.wifi_mon || kmod.shannon_cmd || kmod.diag_bridge);
  }

  // WiFi mode buttons
  document.querySelectorAll('.mode-btn').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.mode === (config.wifi_mode || 'managed'));
  });

  // BCM4390 driver parameters
  const params = await api('/api/wifi/params');
  const pc = document.getElementById('wifi-params');
  if (params && params.length > 0) {
    pc.innerHTML = params.map(p => `
      <div class="card-row">
        <div class="card-row-info">
          <span class="prop-name">${p.name}</span>
          <div class="card-row-desc" style="word-break:break-all">${escHtml(p.value) || '(empty)'}</div>
        </div>
        ${p.writable ? `<button class="btn" style="padding:4px 8px;font-size:11px;flex-shrink:0" onclick="editWifiParam('${p.name}','${escHtml(p.value)}')">Edit</button>` : ''}
      </div>`).join('');
  }

  // WiFi firmware files
  const fw = await api('/api/wifi/firmware');
  const fwc = document.getElementById('wifi-firmware');
  if (fw) {
    let html = '';
    if (fw.info) {
      html += `<div class="card-row"><div class="card-row-info">
        <div class="card-row-label">Active Firmware</div>
        <div class="card-row-desc" style="white-space:pre-wrap;font-family:monospace;font-size:11px">${escHtml(fw.info)}</div>
      </div></div>`;
    }
    if (fw.files && fw.files.length > 0) {
      html += fw.files.map(f => `
        <div class="card-row">
          <div class="card-row-info">
            <span class="prop-name">${f.name}</span>
            <div class="card-row-desc">${(f.size / 1024).toFixed(0)} KB — ${f.path}</div>
          </div>
        </div>`).join('');
    }
    fwc.innerHTML = html || '<div class="card-row"><div class="card-row-desc">No firmware files found</div></div>';
  }

  // WiFi hardware info
  const wifiText = await fetch('/api/wifi/info').then(r => r.text()).catch(() => '');
  const wc = document.getElementById('wifi-hw-info');
  if (wifiText.trim()) {
    const lines = wifiText.trim().split('\n').filter(l => l.includes('='));
    wc.innerHTML = lines.map(l => {
      const [k, ...v] = l.split('=');
      return `<div class="card-row">
        <div class="card-row-info">
          <span class="prop-name">${k}</span>
          <div class="card-row-desc">${v.join('=')}</div>
        </div>
      </div>`;
    }).join('');
  }
}

async function editWifiParam(name, currentVal) {
  const newVal = prompt(`Set ${name}:`, currentVal);
  if (newVal === null) return;
  const res = await post('/api/wifi/param', { name, value: newVal });
  if (res && res.ok) {
    showToast(`${name} = ${res.value}`);
    loadWifi();
  } else {
    showToast(res?.error || 'Failed to set param', 'error');
  }
}

function updateKmodBtn(id, loaded) {
  const btn = document.getElementById(id);
  if (!btn) return;
  if (loaded) {
    btn.textContent = 'Unload';
    btn.className = 'btn btn-danger';
    btn.style.cssText = 'padding:6px 12px;font-size:12px';
  } else {
    btn.textContent = 'Load';
    btn.className = 'btn btn-primary';
    btn.style.cssText = 'padding:6px 12px;font-size:12px';
  }
}

async function toggleKmod(mod) {
  const kmod = await api('/api/kmod');
  const key = mod.replace('rc_', '').replace('_cmd', '_cmd');
  const mapKey = mod === 'rc_wifi_mon' ? 'wifi_mon' : mod === 'rc_shannon_cmd' ? 'shannon_cmd' : 'diag_bridge';
  const loaded = kmod && kmod[mapKey];
  const action = loaded ? 'unload' : 'load';

  const res = await post(`/api/kmod/${action}`, { module: mod });
  if (res) {
    showToast(res.ok ? `${mod}: ${res.status}` : (res.error || 'Failed'), res.ok ? 'success' : 'error');
  }
  loadWifi();
}

async function handleWifiMode(mode) {
  document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
  document.querySelector(`[data-mode="${mode}"]`).classList.add('active');

  const res = await post('/api/wifi/mode', { mode });
  if (res && res.ok) {
    config.wifi_mode = mode;
    updateChips();
    showToast(`WiFi: ${mode} on ${res.iface}`);
  } else {
    showToast(res?.error || 'Failed — load rc_wifi_mon first?', 'error');
    loadWifi();
  }
}

// ---- debugfs browser ----
async function loadDebug() {
  const paths = await api('/api/debugfs');
  const sc = document.getElementById('debugfs-shortcuts');
  if (paths && paths.length > 0) {
    sc.innerHTML = paths.map(p => `
      <div class="card-row" style="cursor:pointer" onclick="browseToPath('${p}')">
        <div class="card-row-info">
          <span class="prop-name">${p.replace('/sys/kernel/debug/', '/d/')}</span>
        </div>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="var(--text-muted)"><path d="M10 6L8.59 7.41 13.17 12l-4.58 4.59L10 18l6-6z"/></svg>
      </div>
    `).join('');
  }
}

function browseToPath(path) {
  document.getElementById('fs-path').value = path;
  browsePath();
}

async function browsePath() {
  const path = document.getElementById('fs-path').value.trim();
  if (!path) return;

  const res = await post('/api/fs/read', { path });
  const fc = document.getElementById('fs-content');

  if (!res) {
    fc.innerHTML = '<div class="card-row"><div class="card-row-desc">Error reading path</div></div>';
    return;
  }

  // Breadcrumb
  const parts = path.split('/').filter(Boolean);
  document.getElementById('fs-breadcrumb').innerHTML =
    parts.map((p, i) => {
      const fullPath = '/' + parts.slice(0, i + 1).join('/');
      return `<span class="crumb" onclick="browseToPath('${fullPath}')">${p}</span>`;
    }).join(' / ');

  if (res.type === 'dir' && res.entries) {
    fc.innerHTML = res.entries.map(e => {
      const icon = e.type === 'dir' ? '&#128193;' : (e.writable ? '&#128221;' : '&#128196;');
      return `<div class="card-row" style="cursor:pointer" onclick="${e.type === 'dir' ? `browseToPath('${path}/${e.name}')` : `readFile('${path}/${e.name}')`}">
        <div class="card-row-info">
          <div class="card-row-label">${icon} ${e.name}</div>
          <div class="card-row-desc">${e.type}${e.writable ? ' (rw)' : ' (ro)'}</div>
        </div>
      </div>`;
    }).join('') || '<div class="card-row"><div class="card-row-desc">Empty directory</div></div>';
  } else if (res.type === 'file') {
    const content = res.content || '(empty)';
    fc.innerHTML = `<div style="padding:12px 16px">
      <div class="section-title" style="margin-bottom:8px">${path.split('/').pop()}</div>
      <pre class="file-content">${escHtml(content)}</pre>
    </div>`;
  }
}

function readFile(path) {
  document.getElementById('fs-path').value = path;
  browsePath();
}

function fsUp() {
  const input = document.getElementById('fs-path');
  const parts = input.value.split('/').filter(Boolean);
  if (parts.length > 1) {
    parts.pop();
    input.value = '/' + parts.join('/');
    browsePath();
  }
}

// ---- Carrier page ----
async function loadCarrier() {
  const cc = await api('/api/carrier/config');
  if (cc) {
    setToggle('toggle-volte', cc.volte);
    setToggle('toggle-wfc', cc.wfc);
    setToggle('toggle-nettype', cc.hide_network_type === '0' ? '1' : '0');
  }

  // Carrier settings files
  const files = await api('/api/carrier/files');
  const fc = document.getElementById('carrier-files');
  if (files && files.length > 0) {
    fc.innerHTML = files.map(f => `
      <div class="card-row">
        <div class="card-row-info">
          <span class="prop-name">${f.name}</span>
          <div class="card-row-desc">${(f.size / 1024).toFixed(1)} KB</div>
        </div>
      </div>`).join('');
  } else {
    fc.innerHTML = '<div class="card-row"><div class="card-row-desc">No CarrierSettings directory found</div></div>';
  }
}

async function handleCarrierFlag(flag, el) {
  const val = el.checked ? '1' : '0';
  const res = await post('/api/carrier/set', { flag, value: val });
  if (res && res.ok) {
    showToast(`${flag} = ${val}`);
  } else {
    el.checked = !el.checked;
    showToast(res?.error || 'Failed', 'error');
  }
}

async function dumpCarrierConfig() {
  const pre = document.getElementById('carrier-dump');
  pre.style.display = 'block';
  pre.textContent = 'Loading...';
  const res = await api('/api/carrier/dump');
  if (res && res.dump) {
    pre.textContent = res.dump;
  } else {
    pre.textContent = 'Failed to dump carrier_config';
  }
}

// ---- CP Debug (in Radio page) ----
async function loadCPDebug() {
  const cp = await api('/api/cp');
  if (cp) {
    setText('cp-state', cp.state || 'unknown');
    document.getElementById('cp-pcie').textContent = cp.pcie_stats ? cp.pcie_stats.substring(0, 200) : 'N/A';
    document.getElementById('cp-sbb').textContent = cp.sbb_debug ? cp.sbb_debug.substring(0, 200) : 'N/A';
  }
}

async function triggerCPCrash() {
  if (!confirm('This will crash the modem and generate a ramdump.\nThe modem will restart automatically.\n\nContinue?')) return;
  const res = await post('/api/cp/crash', {});
  if (res && res.ok) {
    showToast(res.msg || 'CP crash triggered');
  } else {
    showToast(res?.error || 'Failed', 'error');
  }
}

// ---- Thread / Wonder radio (in Debug page) ----
async function loadThreadInfo() {
  const info = await api('/api/thread');
  const tc = document.getElementById('thread-info');
  if (info && info.present) {
    tc.innerHTML = `
      <div class="card-row"><div class="card-row-info">
        <div class="card-row-label">Wonder 802.15.4 Radio</div>
        <div class="card-row-desc">Thread / Matter mesh networking</div>
      </div></div>
      <div class="card-row"><div class="card-row-info">
        <span class="prop-name">MAC</span>
        <div class="card-row-desc">${info.mac || 'N/A'}</div>
      </div></div>
      <div class="card-row"><div class="card-row-info">
        <span class="prop-name">PHY</span>
        <div class="card-row-desc">${info.name || 'N/A'} (index ${info.index || '?'})</div>
      </div></div>
      <div class="card-row"><div class="card-row-info">
        <span class="prop-name">WPAN Interface</span>
        <div class="card-row-desc">${info.wpan_iface ? 'thread-wpan (active)' : 'not present'}</div>
      </div></div>
      ${info.debugfs_entries ? `<div class="card-row"><div class="card-row-info">
        <span class="prop-name">debugfs nodes</span>
        <div class="card-row-desc">${info.debugfs_entries}</div>
      </div></div>` : ''}
      ${info.force_stop_tx !== undefined ? `<div class="card-row"><div class="card-row-info">
        <span class="prop-name">force_stop_tx</span>
        <div class="card-row-desc">${info.force_stop_tx}</div>
      </div></div>` : ''}
    `;
  } else {
    tc.innerHTML = '<div class="card-row"><div class="card-row-desc">No Thread/Wonder radio detected</div></div>';
  }
}

// ---- Flags page ----
async function loadFlags() {
  const flags = await api('/api/flags');
  const tbody = document.getElementById('flags-body');
  if (flags && flags.length > 0) {
    tbody.innerHTML = flags.map(f => `<tr>
      <td class="prop-name">${f.prop}</td>
      <td class="prop-value">${f.value || '<span style="color:var(--text-muted)">unset</span>'}</td>
    </tr>`).join('');
  }
}

// ---- Helpers ----
function escHtml(s) {
  return (s || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// ---- Init ----
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('.tab-item').forEach(t =>
    t.addEventListener('click', () => switchTab(t.dataset.tab)));

  // Radio page toggles
  document.getElementById('toggle-eng')?.addEventListener('change', function() { handleToggle('ENGINEERING_MODE', this); });
  document.getElementById('toggle-factory')?.addEventListener('change', function() { handleToggle('FACTORY_TEST_MODE', this); });
  document.getElementById('toggle-hidden')?.addEventListener('change', function() { handleToggle('HIDDEN_MENUS', this); });
  document.getElementById('toggle-diag')?.addEventListener('change', function() { handleToggle('USB_DIAG_MODE', this); });
  document.getElementById('toggle-modem')?.addEventListener('change', function() { handleToggle('MODEM_LOG', this); });

  // WiFi mode buttons
  document.querySelectorAll('.mode-btn').forEach(btn =>
    btn.addEventListener('click', () => handleWifiMode(btn.dataset.mode)));

  // Carrier config toggles
  document.getElementById('toggle-volte')?.addEventListener('change', function() { handleCarrierFlag('volte', this); });
  document.getElementById('toggle-vonr')?.addEventListener('change', function() { handleCarrierFlag('vonr', this); });
  document.getElementById('toggle-wfc')?.addEventListener('change', function() { handleCarrierFlag('wfc', this); });
  document.getElementById('toggle-vt')?.addEventListener('change', function() { handleCarrierFlag('vt', this); });
  document.getElementById('toggle-apn')?.addEventListener('change', function() { handleCarrierFlag('apn', this); });
  document.getElementById('toggle-nradv')?.addEventListener('change', function() { handleCarrierFlag('nradv', this); });
  document.getElementById('toggle-nettype')?.addEventListener('change', function() { handleCarrierFlag('nettype', this); });

  switchTab('radio');
});
