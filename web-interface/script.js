let espIp = '';
let isConnected = false;
let isScanning = false;

let logEntries = 0;
let toastTimeout = null;

function getBaseUrl() {
  return `http://${espIp}`;
}

function setConnectionState(state, text) {
  const dot = document.getElementById('statusDot');
  const deviceState = document.getElementById('deviceState');

  if (dot) dot.className = `status-indicator ${state}`;
  if (deviceState) deviceState.textContent = text;
}

async function connectESP() {
  espIp = document.getElementById('espIp').value.trim();

  if (!espIp) {
    showToast('warning', 'IP manquante', 'Entre l’adresse IP de l’ESP32-CAM');
    return;
  }

  isConnected = true;
  localStorage.setItem('facedoor_esp_ip', espIp);

  setConnectionState('busy', 'connexion...');

  try {
    const res = await fetch(`${getBaseUrl()}/status?t=${Date.now()}`, {
      cache: 'no-store'
    });

    if (!res.ok) throw new Error('status failed');

    const data = await res.json();

    updateDashboard(data);
    setConnectionState('connected', 'connecté');

    updateCameraOnce();

    showToast('connected', 'Connecté', `ESP32-CAM @ ${espIp}`);
  } catch (e) {
    console.error(e);
    isConnected = false;
    setConnectionState('error', 'hors ligne');
    showToast('warning', 'Connexion impossible', 'Vérifie l’IP ou le Wi-Fi');
  }
}

function updateCameraOnce() {
  if (!isConnected || isScanning) return;

  const img = document.getElementById('streamImg');
  const noFeed = document.getElementById('noFeedMsg');
  const cameraState = document.getElementById('cameraState');

  if (!img) return;

  img.onload = () => {
    img.style.display = 'block';

    if (noFeed) noFeed.style.display = 'none';
    if (cameraState) cameraState.textContent = 'image capturée';
  };

  img.onerror = () => {
    img.style.display = 'none';

    if (noFeed) noFeed.style.display = 'flex';
    if (cameraState) cameraState.textContent = 'image indisponible';
  };

  img.src = `${getBaseUrl()}/capture?t=${Date.now()}`;
}

async function scanNow() {
  if (!isConnected) {
    await connectESP();
    return;
  }

  if (isScanning) return;

  isScanning = true;
  setConnectionState('busy', 'analyse...');

  const scanBtn = document.getElementById('scanBtn');

  if (scanBtn) {
    scanBtn.disabled = true;
    scanBtn.textContent = 'Analyse...';
  }

  try {
    const res = await fetch(`${getBaseUrl()}/scan?t=${Date.now()}`, {
      cache: 'no-store'
    });

    if (!res.ok) throw new Error('scan failed');

    const data = await res.json();

    console.log('SCAN DATA:', data);

    updateDashboard(data);
    addLogFromScan(data);

    setConnectionState('connected', 'connecté');

    setTimeout(() => {
      updateCameraOnce();
    }, 500);
  } catch (e) {
    console.error(e);
    setConnectionState('error', 'erreur analyse');
    showToast('warning', 'Analyse impossible', 'Vérifie alimentation, IP ou caméra');
  } finally {
    isScanning = false;

    if (scanBtn) {
      scanBtn.disabled = false;
      scanBtn.textContent = 'Analyser';
    }
  }
}

function updateDashboard(data) {
  const auth = Number(data.authorized || 0);
  const notAuth = Number(data.not_authorized || 0);
  const decision = String(data.decision || 'WAITING');
  const door = String(data.door || 'CLOSED');

  setText('authorizedScore', auth.toFixed(4));
  setText('notAuthorizedScore', notAuth.toFixed(4));

  setWidth('authorizedBar', `${Math.round(auth * 100)}%`);
  setWidth('notAuthorizedBar', `${Math.round(notAuth * 100)}%`);

  setText('statAuthorized', data.total_authorized ?? 0);
  setText('statRefused', data.total_refused ?? 0);

  setText('modelMode', data.model || 'float32');

  setText(
    'thresholdValue',
    Number(data.threshold || 0.70).toFixed(2)
  );

  setText(
    'frameInfo',
    data.frame_width ? `${data.frame_width}x${data.frame_height}` : '—'
  );

  setText(
    'heapInfo',
    data.free_heap ? `${Math.round(data.free_heap / 1024)} KB` : '—'
  );

  updateDoor(door, decision, auth, notAuth, data);
}

function updateDoor(door, decision, auth, notAuth, data = {}) {
  const doorVisual = document.getElementById('doorVisual');
  const doorText = document.getElementById('doorStatusText');
  const doorSub = document.getElementById('doorSub');

  if (!doorVisual || !doorText || !doorSub) return;

  const hits = Number(data.authorized_hits || 0);
  const required = Number(data.required_hits || 3);

  if (door === 'OPEN') {
    doorVisual.className = 'door-visual open';
    doorText.className = 'door-status-text open';
    doorText.textContent = 'OUVERTE';
    doorSub.textContent = `Accès validé — ${hits}/${required} autorisations`;
    return;
  }

  doorVisual.className = 'door-visual closed';
  doorText.className = 'door-status-text closed';
  doorText.textContent = 'FERMÉE';

  if (decision === 'AUTHORIZED') {
    doorSub.textContent = `Autorisé ${hits}/${required} — score ${auth.toFixed(2)}`;
  } else if (decision === 'NOT_AUTHORIZED') {
    doorSub.textContent = `Refusé — score ${notAuth.toFixed(2)}`;
  } else {
    doorSub.textContent = 'En attente de détection';
  }
}

function addLogFromScan(data) {
  const decision = String(data.decision || 'WAITING');

  if (decision === 'WAITING') return;

  const isAuthorized = decision === 'AUTHORIZED';

  const score = isAuthorized
    ? Number(data.authorized || 0)
    : Number(data.not_authorized || 0);

  const confidence = Math.round(score * 1000) / 10;
  const timeStr = new Date().toLocaleTimeString('fr-FR');

  setText('statAuthorized', data.total_authorized ?? 0);
  setText('statRefused', data.total_refused ?? 0);
  setText('statLastTime', timeStr.substring(0, 5));
  setText('statLastName', isAuthorized ? 'authorized' : 'not_authorized');

  addLogItem(isAuthorized, confidence, timeStr, data);

  showToast(
    isAuthorized ? 'authorized' : 'refused',
    isAuthorized ? 'Accès autorisé' : 'Accès refusé',
    `Confiance: ${confidence}%`
  );
}

function addLogItem(isAuthorized, confidence, timeStr, data = {}) {
  const list = document.getElementById('logList');
  const logCount = document.getElementById('logCount');

  if (!list) return;

  const empty = list.querySelector('.log-empty');
  if (empty) empty.remove();

  logEntries++;

  if (logCount) {
    logCount.textContent = logEntries + (logEntries > 1 ? ' entrées' : ' entrée');
  }

  const cls = isAuthorized ? 'auth' : 'ref';
  const label = isAuthorized ? 'AUTHORIZED' : 'NOT_AUTHORIZED';
  const badge = isAuthorized ? '✓ OK' : '✗ REFUSÉ';

  const hits = Number(data.authorized_hits || 0);
  const required = Number(data.required_hits || 3);

  const item = document.createElement('div');
  item.className = 'log-item';

  item.innerHTML = `
    <div class="log-info">
      <div class="log-name ${cls}">${label}</div>
      <div class="log-meta">${timeStr} — score ${confidence}%</div>
    </div>

    <div class="log-right">
      <div class="log-badge ${cls}">${badge}</div>
      <div class="log-conf">${confidence}%</div>
      <div class="log-bar-bg">
        <div class="log-bar ${cls}" style="width:${confidence}%"></div>
      </div>
    </div>
  `;

  list.prepend(item);

  while (list.children.length > 50) {
    list.removeChild(list.lastChild);
  }
}

async function clearLog() {
  const list = document.getElementById('logList');

  if (list) {
    list.innerHTML = '<div class="log-empty">Aucun accès enregistré</div>';
  }

  logEntries = 0;

  setText('statAuthorized', '0');
  setText('statRefused', '0');
  setText('statLastTime', '—');
  setText('statLastName', 'aucune');
  setText('logCount', '0 entrées');

  setText('authorizedScore', '0.0000');
  setText('notAuthorizedScore', '0.0000');

  setWidth('authorizedBar', '0%');
  setWidth('notAuthorizedBar', '0%');

  updateDoor('CLOSED', 'WAITING', 0, 0, {
    authorized_hits: 0,
    required_hits: 3
  });

  if (!isConnected) return;

  try {
    const res = await fetch(`${getBaseUrl()}/reset?t=${Date.now()}`, {
      cache: 'no-store'
    });

    if (res.ok) {
      const data = await res.json();
      updateDashboard(data);
    }

    showToast('connected', 'Reset effectué', 'Journal et compteurs remis à zéro');
  } catch (e) {
    console.error(e);
    showToast('warning', 'Reset local seulement', 'ESP32 non joignable');
  }
}

function showToast(type, title, msg) {
  const toast = document.getElementById('toast');

  if (!toast) return;

  setText('toastTitle', title);
  setText('toastMsg', msg);

  toast.className = `toast ${type} show`;

  clearTimeout(toastTimeout);

  toastTimeout = setTimeout(() => {
    toast.className = 'toast';
  }, 3000);
}

function openThingSpeak() {
  window.open('https://thingspeak.com/channels/3346571', '_blank');
}

function setText(id, value) {
  const el = document.getElementById(id);

  if (el) {
    el.textContent = value;
  }
}

function setWidth(id, value) {
  const el = document.getElementById(id);

  if (el) {
    el.style.width = value;
  }
}

window.addEventListener('load', () => {
  const savedIp = localStorage.getItem('facedoor_esp_ip');

  if (savedIp) {
    const ipInput = document.getElementById('espIp');
    if (ipInput) ipInput.value = savedIp;
  }

  const ipInput = document.getElementById('espIp');

  if (ipInput) {
    ipInput.addEventListener('change', (e) => {
      localStorage.setItem('facedoor_esp_ip', e.target.value.trim());
    });
  }
});