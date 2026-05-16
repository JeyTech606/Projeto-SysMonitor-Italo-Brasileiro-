/* =============================================
   SysMonitor - dashboard.js
   Le dados em tempo real da API C.
   Se a API nao estiver rodando, usa sysdata.json.
   ============================================= */

const UPDATE_INTERVAL_MS = 2000;
const API_PATH = 'http://localhost:8888/metrics';
const FALLBACK_JSON_PATH = './sysdata.json';
let previousNetworkSample = null;

function formatBytes(bytes) {
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + ' GB';
  if (bytes >= 1048576)    return (bytes / 1048576).toFixed(1)    + ' MB';
  if (bytes >= 1024)       return (bytes / 1024).toFixed(1)       + ' KB';
  return bytes + ' B';
}

function barClass(pct) {
  if (pct >= 85) return 'crit';
  if (pct >= 60) return 'warn';
  return '';
}

function statusBadge(cpu) {
  if (cpu >= 20) return '<span class="badge badge-crit">Alto</span>';
  if (cpu >= 5)  return '<span class="badge badge-warn">Medio</span>';
  return               '<span class="badge badge-ok">Normal</span>';
}

async function fetchJson(url) {
  const response = await fetch(`${url}?t=${Date.now()}`);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

async function fetchMetrics() {
  try {
    return await fetchJson(API_PATH);
  } catch (apiErr) {
    try {
      return await fetchJson(FALLBACK_JSON_PATH);
    } catch (fileErr) {
      throw new Error(`API: ${apiErr.message} | JSON: ${fileErr.message}`);
    }
  }
}

function updateMetric(valueId, barId, subId, value, barPct, subText) {
  const el = document.getElementById(valueId);
  const bar = document.getElementById(barId);
  const sub = document.getElementById(subId);

  if (el) el.textContent = value;
  if (sub) sub.textContent = subText;

  if (bar) {
    bar.style.width = Math.min(barPct, 100) + '%';
    bar.className = 'metric-fill';
    const cls = barClass(barPct);
    if (cls) bar.classList.add(cls);
  }
}

function updateProcessTable(processes) {
  const tbody = document.getElementById('process-tbody');
  const countEl = document.getElementById('process-count');
  if (!tbody) return;

  if (!processes || processes.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" class="loading-msg">Nenhum processo encontrado.</td></tr>';
    if (countEl) countEl.textContent = '0 processos';
    return;
  }

  const sorted = [...processes].sort((a, b) => (b.cpu_pct ?? 0) - (a.cpu_pct ?? 0));

  if (countEl) countEl.textContent = `${sorted.length} processos`;

  tbody.innerHTML = sorted.map(proc => {
    const cpu = proc.cpu_pct ?? 0;
    const ram = proc.ram_bytes ?? 0;

    return `
      <tr>
        <td>${proc.pid}</td>
        <td>${proc.name}</td>
        <td style="color:${cpu >= 20 ? '#f85149' : cpu >= 5 ? '#d29922' : '#3fb950'}">
          ${cpu.toFixed(1)}%
        </td>
        <td>${formatBytes(ram)}</td>
        <td>${statusBadge(cpu)}</td>
      </tr>
    `;
  }).join('');
}

function getNetworkSpeeds(network) {
  let send = network?.send_bytes_sec ?? 0;
  let recv = network?.recv_bytes_sec ?? 0;
  const sentTotal = network?.sent_bytes_total;
  const recvTotal = network?.recv_bytes_total;
  const now = Date.now();

  if (sentTotal != null && recvTotal != null) {
    if (previousNetworkSample) {
      const elapsed = Math.max((now - previousNetworkSample.time) / 1000, 1);
      const sentDelta = Math.max(sentTotal - previousNetworkSample.sentTotal, 0);
      const recvDelta = Math.max(recvTotal - previousNetworkSample.recvTotal, 0);

      if (send === 0) send = Math.round(sentDelta / elapsed);
      if (recv === 0) recv = Math.round(recvDelta / elapsed);
    }

    previousNetworkSample = { sentTotal, recvTotal, time: now };
  }

  return { send, recv };
}

async function fetchAndUpdate() {
  try {
    const data = await fetchMetrics();

    const cpuPct = data.cpu?.usage_pct ?? 0;
    updateMetric(
      'cpu-value', 'cpu-bar', 'cpu-cores',
      cpuPct.toFixed(1) + '%',
      cpuPct,
      `${data.cpu?.cores ?? '--'} nucleos logicos`
    );
    pushChartData('cpu', cpuPct);

    const ramPct = data.ram?.usage_pct ?? 0;
    const ramUsed = formatBytes(data.ram?.used_bytes ?? 0);
    const ramTotal = formatBytes(data.ram?.total_bytes ?? 0);
    updateMetric(
      'ram-value', 'ram-bar', 'ram-total',
      ramUsed,
      ramPct,
      `Total: ${ramTotal}`
    );
    pushChartData('ram', ramPct);

    const diskPct = data.disk?.usage_pct ?? 0;
    updateMetric(
      'disk-value', 'disk-bar', 'disk-info',
      diskPct.toFixed(1) + '%',
      diskPct,
      `Livre: ${formatBytes(data.disk?.free_bytes ?? 0)}`
    );

    const { send: netSend, recv: netRecv } = getNetworkSpeeds(data.network);
    const netTotal = netSend + netRecv;
    const netPct = netTotal > 0 ? Math.max(Math.min((netTotal / 1048576) * 10, 100), 1) : 0;
    updateMetric(
      'net-value', 'net-bar', 'net-info',
      formatBytes(netTotal) + '/s',
      netPct,
      `Up ${formatBytes(netSend)}/s  Down ${formatBytes(netRecv)}/s`
    );

    updateProcessTable(data.processes ?? []);

    const timeEl = document.getElementById('update-time');
    if (timeEl) {
      timeEl.textContent = `Atualizado as ${new Date().toLocaleTimeString('pt-BR')}`;
    }
  } catch (err) {
    const timeEl = document.getElementById('update-time');
    if (timeEl) timeEl.textContent = `Erro ao ler dados: ${err.message}`;
    console.warn('[SysMonitor] Falha ao buscar dados:', err.message);
  }
}

document.addEventListener('DOMContentLoaded', () => {
  fetchAndUpdate();
  setInterval(fetchAndUpdate, UPDATE_INTERVAL_MS);
});
