const state = { beacons: [], events: [], coordination: null, socket: null, showAll: false, reportEvents: [], reportSummary: {} };
const $ = id => document.getElementById(id);
const esc = value => String(value ?? "").replace(/[&<>'"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" }[c]));
const labels = { peligro: "Peligro", precaucion: "Precaución", proximo: "Próximo", seguro: "Seguro", sin_senal: "Sin señal" };
const trends = { acercandose: "↓ Acercándose", alejandose: "↑ Alejándose", estable: "— Estable", sin_datos: "Sin tendencia" };
const shiftLabels = { ingresando: "Registrando ingreso", en_turno: "En turno", saliendo: "Registrando salida", fuera: "Fuera del recinto", ausente: "Pendiente de ingreso", sin_senal: "Sin señal" };

let actionConfirmationResolver = null;

function closeActionConfirmation(accepted) {
  const modal = $("actionConfirmModal");
  if (!modal || modal.hidden) return;
  modal.hidden = true;
  const resolve = actionConfirmationResolver;
  actionConfirmationResolver = null;
  if (resolve) resolve(Boolean(accepted));
}

window.requestMinaConfirmation = ({ title, message, confirmText = "Sí, eliminar" }) => {
  if (actionConfirmationResolver) closeActionConfirmation(false);
  $("actionConfirmTitle").textContent = title || "¿Eliminar información?";
  $("actionConfirmMessage").textContent = message || "Esta acción no se puede deshacer.";
  $("actionConfirmAccept").textContent = confirmText;
  $("actionConfirmModal").hidden = false;
  window.setTimeout(() => $("actionConfirmAccept").focus(), 0);
  return new Promise(resolve => { actionConfirmationResolver = resolve; });
};

$("actionConfirmCancel").addEventListener("click", () => closeActionConfirmation(false));
$("actionConfirmAccept").addEventListener("click", () => closeActionConfirmation(true));
$("actionConfirmModal").addEventListener("click", event => {
  if (event.target === $("actionConfirmModal")) closeActionConfirmation(false);
});
document.addEventListener("keydown", event => {
  if (event.key === "Escape" && !$("actionConfirmModal").hidden) closeActionConfirmation(false);
});

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
  renderShiftRoster(data.actualizado);
  renderEvents();
  if (state.reportEvents.length) renderReports();
  if (typeof updatePendingTagAlerts === "function") updatePendingTagAlerts(false);
  if (typeof window.renderMineTracking === "function") window.renderMineTracking();
}

function renderAssets() {
  const canManageHistory = typeof adminAuthenticated !== "undefined" && adminAuthenticated;
  $("assetGrid").innerHTML = state.beacons.map(item => {
    const counters = item.contadores || {};
    const precision = item.precision || {};
    const qualityLabels = { alta: "Señal estable", media: "Señal moderada", variable: "Señal variable", inicializando: "Calibrando", sin_senal: "Sin señal" };
    return `<article class="asset ${esc(item.estado)}">
      <div class="asset-top"><span class="asset-id">${esc(item.id)} · ${esc(item.tipo)}</span><span class="state-pill">${esc(labels[item.estado] || item.estado)}</span></div>
      <h3>${esc(item.nombre)}</h3><div class="asset-person">${esc(item.codigo_personal || item.persona)} · ${esc(item.cargo || "Personal minero")} · ${esc(item.cuadrilla || "Sin cuadrilla")}</div>
      <div class="worker-shift-state ${esc(item.estado_turno || "sin_senal")}"><strong>${esc(shiftLabels[item.estado_turno] || item.estado_turno || "Sin señal")}</strong><small>${esc(item.turno?.nombre || "Turno sin asignar")} · ${esc(item.turno?.inicio || "--:--")}–${esc(item.turno?.fin || "--:--")}</small></div>
      <div class="distance"><strong>${item.distancia == null ? "—" : esc(item.distancia)} <small>m aprox.</small></strong><span class="trend">${esc(trends[item.tendencia] || item.tendencia)}</span></div>
      <div class="precision-line"><span class="precision-quality ${esc(precision.calidad || "sin_senal")}">${esc(qualityLabels[precision.calidad] || precision.calidad || "Sin señal")}</span><small>${precision.rssi_filtrado == null ? "Esperando muestras" : `RSSI filtrado ${esc(precision.rssi_filtrado)} dBm · ventana ${esc(precision.ventana || 0)}/${esc(precision.objetivo_ventana || 7)}`}</small></div>
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
  const accepted = await window.requestMinaConfirmation({
    title: `¿Limpiar el historial de ${beaconId}?`,
    message: "Se borrarán sus cambios de proximidad y contadores. Los otros TAG no serán modificados.",
    confirmText: "Sí, limpiar historial"
  });
  if (!accepted) return;
  try {
    const response = await fetch(`/api/historial-proximidad/${encodeURIComponent(beaconId)}`, { method: "DELETE" });
    if (!response.ok) throw new Error(await response.text());
    await Promise.all([fetchState(), loadReports()]);
  } catch (error) {
    alert(error.message || "No fue posible limpiar el historial del TAG.");
  }
}

function renderEvents() {
  const items = state.showAll ? state.events : state.events.slice(0, 6);
  $("events").innerHTML = items.length ? items.map(event => {
    const movement = event.tipo === "sector";
    const title = movement ? `${event.nombre} · Cambio de sector` : `${event.nombre} · ${labels[event.estado] || event.estado}`;
    const detail = movement
      ? `${event.reader_anterior || "Fuera de cobertura"} → ${event.reader_id || "Sin reader"} · ${event.sector || "Sector sin nombre"}`
      : `${labels[event.anterior] || event.anterior} → ${labels[event.estado] || event.estado}${event.distancia == null ? "" : ` · ${event.distancia} m`}`;
    return `<div class="event ${esc(movement ? "proximo" : event.estado)}"><i class="event-mark"></i><div><strong>${esc(title)}</strong><small>${esc(detail)}</small></div><time>${esc(formatTime(event.fecha))}</time></div>`;
  }).join("") : '<div class="empty">Aún no hay cambios confirmados.</div>';
}

function arrivalStatus(item) {
  const arrival = Number(item.turno?.ultimo_ingreso || 0);
  if (!arrival || new Date(arrival).toDateString() !== new Date().toDateString()) return { label: "Sin marcación hoy", className: "pending" };
  const [hour, minute] = String(item.turno?.inicio || "08:00").split(":").map(Number);
  const moment = new Date(arrival);
  const difference = moment.getHours() * 60 + moment.getMinutes() - (hour * 60 + minute);
  if (difference <= 10) return { label: "Ingreso a tiempo", className: "ontime" };
  return { label: `Atraso ${difference} min`, className: "late" };
}

function todayShiftTime(value) {
  if (!value) return "—";
  const moment = new Date(Number(value));
  return moment.toDateString() === new Date().toDateString() ? formatTime(value) : "—";
}

function renderShiftRoster(updatedAt = null) {
  const workers = state.beacons || [];
  const count = status => workers.filter(item => item.estado_turno === status).length;
  const missing = workers.filter(item => ["fuera", "ausente", "sin_senal"].includes(item.estado_turno)).length;
  $("shiftExpected").textContent = workers.length;
  $("shiftEntering").textContent = count("ingresando");
  $("shiftPresent").textContent = count("en_turno");
  $("shiftLeaving").textContent = count("saliendo");
  $("shiftMissing").textContent = missing;
  $("shiftUpdated").textContent = updatedAt ? `Actualizado ${formatTime(updatedAt)}` : "Esperando detecciones";
  $("shiftRows").innerHTML = workers.length ? workers.map(item => {
    const attendance = arrivalStatus(item);
    const entry = todayShiftTime(item.turno?.ultimo_ingreso);
    const exit = todayShiftTime(item.turno?.ultima_salida);
    return `<tr>
      <td><strong>${esc(item.nombre)}</strong><small>${esc(item.codigo_personal || item.id)} · ${esc(item.id)}</small></td>
      <td><strong>${esc(item.cargo || "Sin cargo")}</strong><small>${esc(item.cuadrilla || "Sin cuadrilla")}</small></td>
      <td><strong>${esc(item.turno?.nombre || "Sin turno")}</strong><small>${esc(item.turno?.inicio || "--:--")}–${esc(item.turno?.fin || "--:--")}</small></td>
      <td><span class="shift-status ${esc(item.estado_turno || "sin_senal")}">${esc(shiftLabels[item.estado_turno] || item.estado_turno || "Sin señal")}</span><small class="attendance ${attendance.className}">${esc(attendance.label)}</small></td>
      <td><strong>${esc(item.sector || "Sin ubicación")}</strong><small>${item.ultima_senal ? `Señal ${esc(formatTime(item.ultima_senal))}` : "Sin recepción vigente"}</small></td>
      <td><strong>Entrada ${esc(entry)}</strong><small>Salida ${esc(exit)}</small></td>
    </tr>`;
  }).join("") : '<tr><td colspan="6">No hay personas configuradas.</td></tr>';
}

function filteredReportEvents() {
  const tag = $("reportTagFilter").value;
  const reader = $("reportReaderFilter").value;
  const type = $("reportTypeFilter").value;
  const hours = Number($("reportPeriodFilter").value || 0);
  const cutoff = hours ? Date.now() - hours * 60 * 60 * 1000 : 0;
  return state.reportEvents.filter(event =>
    (tag === "todos" || event.beacon_id === tag) &&
    (reader === "todos" || event.reader_id === reader) &&
    (type === "todos" || event.tipo === type) &&
    (!cutoff || (event.fecha && Number(event.fecha) >= cutoff))
  );
}

function reportEventLabel(event) {
  if (event.tipo === "sector") return `${event.reader_anterior || "Fuera"} → ${event.reader_id || "Sin reader"}`;
  return `${labels[event.anterior] || event.anterior} → ${labels[event.estado] || event.estado}`;
}

function renderReports() {
  const summary = state.reportSummary || {};
  const events = filteredReportEvents();
  $("reportEventCount").textContent = summary.eventos || 0;
  $("reportActiveCount").textContent = summary.personas_detectadas || 0;
  $("reportParkedCount").textContent = summary.en_turno || 0;
  $("reportMovementCount").textContent = summary.cambios_sector || 0;
  $("reportCapacity").textContent = `Capacidad: ${summary.capacidad_historial || 120}`;
  $("reportRows").innerHTML = events.length ? events.map(event => `<tr>
    <td>${esc(event.fecha ? new Intl.DateTimeFormat("es-CL", { dateStyle: "short", timeStyle: "medium" }).format(new Date(event.fecha)) : "Hora no sincronizada")}</td>
    <td><strong>${esc(event.beacon_id)}</strong><small>${esc(event.nombre)}</small></td>
    <td><span class="report-type ${esc(event.tipo || "estado")}">${event.tipo === "sector" ? "Sector" : "Proximidad"}</span><small>${esc(reportEventLabel(event))}</small></td>
    <td><strong>${esc(event.reader_id || "—")}</strong><small>${esc(event.sector || "Sin sector")}</small></td>
    <td>${event.distancia == null ? "—" : `${esc(event.distancia)} m`}</td>
    <td>${event.rssi == null || event.rssi <= -127 ? "—" : `${esc(event.rssi)} dBm`}</td>
  </tr>`).join("") : '<tr><td colspan="6">No existen eventos para los filtros seleccionados.</td></tr>';
  $("reportStatus").textContent = `${events.length} evento${events.length === 1 ? "" : "s"} en el reporte filtrado`;
}

async function loadReports() {
  try {
    const response = await fetch("/api/reportes", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const report = await response.json();
    state.reportEvents = report.historial || [];
    state.reportSummary = report.resumen || {};
    renderReports();
  } catch (error) {
    $("reportStatus").textContent = "No fue posible actualizar el historial.";
    console.error(error);
  }
}

function csvCell(value) {
  return `"${String(value ?? "").replace(/"/g, '""')}"`;
}

function exportReportCsv() {
  const events = filteredReportEvents();
  if (!events.length) {
    $("reportStatus").textContent = "No hay eventos para exportar con estos filtros.";
    return;
  }
  const rows = [["Fecha ISO", "TAG", "Código personal", "Persona", "Tipo", "Evento", "Reader", "Sector", "Distancia m", "RSSI dBm"]];
  events.forEach(event => rows.push([
    event.fecha ? new Date(event.fecha).toISOString() : "Hora no sincronizada",
    event.beacon_id, state.beacons.find(item => item.id === event.beacon_id)?.codigo_personal || "", event.nombre,
    event.tipo === "sector" ? "Cambio de sector" : "Cambio de proximidad",
    reportEventLabel(event), event.reader_id, event.sector,
    event.distancia == null ? "" : event.distancia, event.rssi <= -127 ? "" : event.rssi
  ]));
  const csv = "\ufeff" + rows.map(row => row.map(csvCell).join(";")).join("\r\n");
  const url = URL.createObjectURL(new Blob([csv], { type: "text/csv;charset=utf-8" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = `control_turno_mina_${new Date().toISOString().slice(0, 10)}.csv`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  $("reportStatus").textContent = `${events.length} eventos exportados para Excel.`;
}

async function fetchState() {
  // Un cambio de BSSID invalida la conexion TCP anterior. Acotar la espera
  // permite que el siguiente intento llegue de inmediato al Heltec que ahora
  // atiende 192.168.4.1, en vez de depender del timeout largo del navegador.
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), 1800);
  let response;
  try {
    response = await fetch("/api/estado", {
      cache: "no-store",
      signal: controller.signal
    });
  } finally {
    window.clearTimeout(timeout);
  }
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

async function refreshAll() {
  await Promise.all([refreshLocalState(), loadReports()]);
}

async function runFriendlyRefresh() {
  const button = $("refresh");
  if (button.classList.contains("is-refreshing")) return;
  const startedAt = performance.now();
  button.classList.add("is-refreshing");
  button.disabled = true;
  button.setAttribute("aria-label", "Actualizando estados");
  button.querySelector(".refresh-accessible-label").textContent = "Actualizando estados";
  try {
    await refreshAll();
    const remaining = 650 - (performance.now() - startedAt);
    if (remaining > 0) await new Promise(resolve => window.setTimeout(resolve, remaining));
  } finally {
    button.classList.remove("is-refreshing");
    button.disabled = false;
    button.setAttribute("aria-label", "Actualizar estados");
    button.querySelector(".refresh-accessible-label").textContent = "Actualizar estados";
  }
}

$("refresh").addEventListener("click", runFriendlyRefresh);
$("refreshReports").addEventListener("click", loadReports);
$("exportReport").addEventListener("click", exportReportCsv);
[$("reportTagFilter"), $("reportReaderFilter"), $("reportTypeFilter"), $("reportPeriodFilter")]
  .forEach(filter => filter.addEventListener("change", renderReports));
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
}).catch(console.error);

// Programa la siguiente consulta solo cuando la anterior termino. setInterval
// acumulaba peticiones si BLE/Wi-Fi demoraban una respuesta y hacia que el
// portal pareciera congelado, especialmente dentro del navegador cautivo iOS.
let stateRefreshTimer = 0;
let stateRefreshRunning = false;
async function scheduleLocalStateRefresh() {
  if (stateRefreshRunning) return;
  stateRefreshRunning = true;
  try {
    await refreshLocalState();
  } finally {
    stateRefreshRunning = false;
    stateRefreshTimer = window.setTimeout(
      scheduleLocalStateRefresh,
      document.hidden ? 3500 : 1500
    );
  }
}
document.addEventListener("visibilitychange", () => {
  if (document.hidden) return;
  window.clearTimeout(stateRefreshTimer);
  scheduleLocalStateRefresh();
});
// iOS puede conservar abierto el portal cautivo mientras cambia de BSSID. Los
// eventos de regreso/online adelantan la consulta sin recargar toda la pagina.
["online", "pageshow", "focus"].forEach(eventName => {
  window.addEventListener(eventName, () => {
    window.clearTimeout(stateRefreshTimer);
    scheduleLocalStateRefresh();
  });
});
scheduleLocalStateRefresh();
loadReports();
setInterval(loadReports, 10000);
