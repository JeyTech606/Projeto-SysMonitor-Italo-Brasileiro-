/* =============================================
   SysMonitor — charts.js
   Motor de gráficos via Canvas (sem libs externas)
   Estilo: linha em tempo real, tema bolsa de valores
   ============================================= */

// Histórico de pontos para cada gráfico
const chartHistory = {
  cpu: [],
  ram: []
};

const MAX_POINTS = 30; // 30 leituras = 30 segundos (atualiza a cada 1s)

// Cor de linha conforme valor
function lineColor(value) {
  if (value >= 85) return '#f85149'; // vermelho
  if (value >= 60) return '#d29922'; // amarelo
  return '#3fb950';                  // verde
}

// Desenha um gráfico de linha no canvas
function drawLineChart(canvasId, historyArray, label, unit) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;

  const ctx    = canvas.getContext('2d');
  const width  = canvas.offsetWidth  || canvas.width;
  const height = canvas.offsetHeight || canvas.height;

  // Ajusta resolução para telas retina
  const dpr = window.devicePixelRatio || 1;
  if (canvas.width !== width * dpr) {
    canvas.width  = width  * dpr;
    canvas.height = height * dpr;
    ctx.scale(dpr, dpr);
  }

  // Limpa
  ctx.clearRect(0, 0, width, height);

  // Fundo do gráfico
  ctx.fillStyle = '#0d1117';
  ctx.fillRect(0, 0, width, height);

  // Grade horizontal
  ctx.strokeStyle = '#21262d';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = (height / 4) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }

  // Nada para desenhar ainda
  if (historyArray.length < 2) {
    ctx.fillStyle = '#8b949e';
    ctx.font = '11px Consolas';
    ctx.textAlign = 'center';
    ctx.fillText('Aguardando dados...', width / 2, height / 2);
    return;
  }

  const data  = historyArray;
  const len   = data.length;
  const stepX = width / (MAX_POINTS - 1);

  // Offset X para animar da direita para esquerda
  const offsetX = (MAX_POINTS - len) * stepX;

  // Área preenchida (gradiente)
  const lastVal = data[data.length - 1];
  const grad = ctx.createLinearGradient(0, 0, 0, height);
  grad.addColorStop(0,   hexWithAlpha(lineColor(lastVal), 0.25));
  grad.addColorStop(1,   hexWithAlpha(lineColor(lastVal), 0.0));

  ctx.beginPath();
  for (let i = 0; i < len; i++) {
    const x = offsetX + i * stepX;
    const y = height - (data[i] / 100) * (height - 8) - 4;
    if (i === 0) ctx.moveTo(x, y);
    else         ctx.lineTo(x, y);
  }
  // Fecha a área
  ctx.lineTo(offsetX + (len - 1) * stepX, height);
  ctx.lineTo(offsetX, height);
  ctx.closePath();
  ctx.fillStyle = grad;
  ctx.fill();

  // Linha principal
  ctx.beginPath();
  ctx.strokeStyle = lineColor(lastVal);
  ctx.lineWidth   = 2;
  ctx.lineJoin    = 'round';
  for (let i = 0; i < len; i++) {
    const x = offsetX + i * stepX;
    const y = height - (data[i] / 100) * (height - 8) - 4;
    if (i === 0) ctx.moveTo(x, y);
    else         ctx.lineTo(x, y);
  }
  ctx.stroke();

  // Ponto atual (último valor)
  const lastX = offsetX + (len - 1) * stepX;
  const lastY = height - (lastVal / 100) * (height - 8) - 4;
  ctx.beginPath();
  ctx.arc(lastX, lastY, 4, 0, Math.PI * 2);
  ctx.fillStyle = lineColor(lastVal);
  ctx.fill();

  // Label do valor atual
  ctx.fillStyle   = lineColor(lastVal);
  ctx.font        = 'bold 13px Consolas';
  ctx.textAlign   = 'right';
  ctx.fillText(`${lastVal.toFixed(1)}${unit}`, width - 4, 16);

  // Escala Y (0%, 50%, 100%)
  ctx.fillStyle  = '#4a5568';
  ctx.font       = '10px Consolas';
  ctx.textAlign  = 'left';
  ctx.fillText('100%', 4, 14);
  ctx.fillText(' 50%', 4, height / 2 + 4);
  ctx.fillText('  0%', 4, height - 4);
}

// Adiciona um novo ponto ao histórico e redesenha
function pushChartData(type, value) {
  chartHistory[type].push(value);
  if (chartHistory[type].length > MAX_POINTS) {
    chartHistory[type].shift();
  }
  if (type === 'cpu') drawLineChart('cpu-chart', chartHistory.cpu, 'CPU', '%');
  if (type === 'ram') drawLineChart('ram-chart', chartHistory.ram, 'RAM', '%');
}

// Helper: hex color com alpha
function hexWithAlpha(hex, alpha) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r},${g},${b},${alpha})`;
}

// Redesenha ao redimensionar a janela
window.addEventListener('resize', () => {
  drawLineChart('cpu-chart', chartHistory.cpu, 'CPU', '%');
  drawLineChart('ram-chart', chartHistory.ram, 'RAM', '%');
});