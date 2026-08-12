const state = { beacons: [], events: [], coordination: null, socket: null, showAll: false };
const $ = id => document.getElementById(id);
const esc = value => String(value ?? "").replace(/[&<>'"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" }[c]));
const labels = { peligro: "Peligro", precaucion: "Precaución", proximo: "Próximo", seguro: "Seguro", sin_senal: "Sin señal" };
const trends = { acercandose: "↓ Acercándose", alejandose: "↑ Alejándose", estable: "— Estable", sin_datos: "Sin tendencia" };

function formatTime(value) {
  if (!value) return "Nunca";
  return new Intl.DateTimeFormat("es-CL", { hour: "2-digit", minute: "2-digit", second: "2-digit" }).format(new Date(value));
}

function render(data) {
  state.beacons = data.beacons || [];
  state.events = data.eventos || [];
  state.coordination = data.coordinacion || null;
  $("receiverName").textContent = data.receptor?.nombre || "Receptor BLE";
  const roleLabels = {
    coordinador_preferido: "Coordinador preferido",
    coordinador_respaldo: "Coordinador de respaldo activo",
    reader_respaldo: "Reader de respaldo sincronizado"
  };
  const localRole = roleLabels[state.coordination?.rol_local] || "Reader autÃ³nomo";
  $("receiverLocation").textContent = `${data.receptor?.ubicacion || "Punto de referencia"} Â· ${localRole}`;
  $("countDanger").textContent = data.conteos?.peligro || 0;
  $("countWarning").textContent = data.conteos?.precaucion || 0;
  $("countNear").textContent = data.conteos?.proximo || 0;
  $("countOffline").textContent = data.conteos?.sin_senal || 0;
  $("updated").textContent = `Actualizado ${formatTime(data.actualizado)}`;
  renderAssets();
  renderEvents();
  if (typeof updatePendingTagAlerts === "function") updatePendingTagAlerts(false);
  if (typeof window.renderMineTracking === "function") window.renderMineTracking();
}

function renderAssets() {
  const canManageHistory = typeof adminAuthenticated !== "undefined" && adminAuthenticated;
  $("assetGrid").innerHTML = state.beacons.map(item => {
    const counters = item.contadores || {};
    return `<article class="asset ${esc(item.estado)}">
      <div class="asset-top"><span class="asset-id">${esc(item.id)} · ${esc(item.tipo)}</span><span class="state-pill">${esc(labels[item.estado] || item.estado)}</span></div>
      <h3>${esc(item.nombre)}</h3><div class="asset-person">${esc(item.persona)} · ${esc(item.instalacion)}</div>
      <div class="distance"><strong>${item.distancia == null ? "—" : esc(item.distancia)} <small>m aprox.</small></strong><span class="trend">${esc(trends[item.tendencia] || item.tendencia)}</span></div>
      <div class="asset-history-head"><div><span>Historial de cambios del TAG</span><small>Veces que ingresó a cada estado</small></div>${canManageHistory ? `<button class="clear-asset-history" data-beacon-id="${esc(item.id)}" type="button">Limpiar historial</button>` : ""}</div>
      <div class="asset-counts" aria-label="Historial de estados de ${esc(item.nombre)}">
        <span class="danger"><i></i><small>Peligro</small><strong>${esc(counters.peligro || 0)}</strong></span>
        <span class="near"><i></i><small>Próximo</small><strong>${esc(counters.proximo || 0)}</strong></span>
        <span class="offline"><i></i><small>Sin señal</small><strong>${esc(counters.sin_senal || 0)}</strong></span>
      </div>
      <div class="asset-meta"><div><span>Último punto conocido</span><strong>${esc(item.reader_nombre || "Sin reader")}</strong></div><div><span>Última recepción</span><strong>${esc(formatTime(item.ultima_senal))}</strong></div></div>
    </article>`;
  }).join("");
  document.querySelectorAll(".clear-asset-history").forEach(button => button.addEventListener("click", () => clearAssetHistory(button.dataset.beaconId)));
}

async function clearAssetHistory(beaconId) {
  if (!confirm(`¿Limpiar el historial de ${beaconId}? Los otros TAG no serán modificados.`)) return;
  const response = await fetch(`/api/historial-proximidad/${encodeURIComponent(beaconId)}`, { method: "DELETE" });
  if (!response.ok) { alert(await response.text()); return; }
  await fetchState();
}

function renderEvents() {
  const items = state.showAll ? state.events : state.events.slice(0, 6);
  $("events").innerHTML = items.length ? items.map(event => `<div class="event ${esc(event.estado)}"><i class="event-mark"></i><div><strong>${esc(event.nombre)} · ${esc(labels[event.estado] || event.estado)}</strong><small>${esc(labels[event.anterior] || event.anterior)} → ${esc(labels[event.estado] || event.estado)}${event.distancia == null ? "" : ` · ${esc(event.distancia)} m`}</small></div><time>${esc(formatTime(event.fecha))}</time></div>`).join("") : '<div class="empty">Aún no hay cambios de estado.</div>';
}

async function fetchState() {
  const response = await fetch("/api/estado", { cache: "no-store" });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  render(await response.json());
}

function setConnection(online) {
  $("connection").classList.toggle("online", online);
  const coordinator = state.coordination?.coordinador_activo;
  $("connection").querySelector("span").textContent = online
    ? (coordinator ? `Coordinador ${coordinator}` : "Receptor conectado")
    : "Reconectando";
}

async function refreshLocalState() {
  try {
    await fetchState();
    setConnection(true);
  } catch (error) {
    setConnection(false);
    console.error(error);
  }
}

$("refresh").addEventListener("click", refreshLocalState);
$("eventsToggle").addEventListener("click", event => { state.showAll = !state.showAll; event.currentTarget.textContent = state.showAll ? "Ver resumen" : "Ver todos"; renderEvents(); });
$("dismissAlarm").addEventListener("click", () => {
  const messageId = $("alarm").dataset.messageId;
  $("alarm").hidden = true;
  delete $("alarm").dataset.messageId;
  if (messageId && typeof window.focusSupervisorMessage === "function") {
    window.focusSupervisorMessage(messageId);
  }
});
fetch("/api/reloj", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ epoch: Date.now() })
}).catch(console.error).finally(refreshLocalState);
setInterval(refreshLocalState, 2000);
