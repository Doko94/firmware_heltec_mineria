let meshMessages = [];
let meshAuthorized = false;

const meshStatusLabels = {
  en_cola: "En cola para RX02",
  transmitiendo: "Transmitiendo",
  transmitido: "Transmitido por RX02",
  confirmado: "Confirmado por T1000-E",
  error: "Pendiente de reintento",
  recibido: "Recibido por RX02"
};

function meshDate(value) {
  if (!value) return "Hora local pendiente";
  return new Intl.DateTimeFormat("es-CL", {dateStyle:"short",timeStyle:"short"}).format(new Date(value));
}

function renderMeshMessages() {
  $("meshLocked").hidden = meshAuthorized;
  $("meshConversation").hidden = !meshAuthorized;
  if (!meshAuthorized) return;
  const list = $("meshMessageList");
  list.innerHTML = meshMessages.length ? meshMessages.slice().reverse().map(item => `<article class="mesh-bubble ${esc(item.direccion)}">
    <header><span>${esc(item.direccion === "entrante" ? "Trabajador 01" : item.autor || "Supervisión")}</span><span>${esc(item.direccion === "entrante" ? "T1000-E → Página" : "Página → T1000-E")}</span></header>
    <p>${esc(item.mensaje)}</p>
    <footer><time>${esc(meshDate(item.fecha))}</time><strong class="mesh-message-state ${esc(item.estado)}">${esc(meshStatusLabels[item.estado] || item.estado)}</strong></footer>
  </article>`).join("") : '<div class="empty">Aún no hay mensajes Meshtastic.</div>';
  list.scrollTop = list.scrollHeight;
}

async function loadMeshMessages() {
  try {
    const response = await fetch("/api/meshtastic/mensajes", {cache:"no-store"});
    if (response.status === 403) {
      meshAuthorized = false;
      meshMessages = [];
      renderMeshMessages();
      return;
    }
    if (!response.ok) throw new Error(await response.text());
    const data = await response.json();
    meshAuthorized = Boolean(data.autorizado);
    meshMessages = data.mensajes || [];
    $("meshGatewayState").textContent = data.gateway_disponible ? "RX02 Meshtastic activo" : "RX02 pendiente de actualización";
    $("meshGatewayState").classList.toggle("offline", !data.gateway_disponible);
    renderMeshMessages();
  } catch (error) {
    $("meshGatewayState").textContent = "Sin conexión con RX01";
    $("meshGatewayState").classList.add("offline");
  }
}

function updateMeshCounter() {
  const bytes = new TextEncoder().encode($("meshMessageBody").value).length;
  $("meshMessageCount").textContent = `${bytes} / 180 bytes`;
  $("meshMessageSend").disabled = bytes === 0 || bytes > 180;
}

$("meshLogin").addEventListener("click", () => {
  if (typeof window.openSupervisorForMesh === "function") window.openSupervisorForMesh();
});

$("meshMessageBody").addEventListener("input", updateMeshCounter);
$("meshMessageForm").addEventListener("submit", async event => {
  event.preventDefault();
  const button = $("meshMessageSend");
  const text = $("meshMessageBody").value.trim();
  if (!text || new TextEncoder().encode(text).length > 180) return;
  setButtonLoading(button, true, "Guardando…");
  $("meshMessageStatus").textContent = "Guardando mensaje en RX01…";
  try {
    const response = await fetch("/api/meshtastic/mensajes", {method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({mensaje:text})});
    if (!response.ok) throw new Error(await response.text());
    const result = await response.json();
    $("meshMessageBody").value = "";
    $("meshMessageStatus").textContent = result.gateway_disponible ? "Mensaje en cola para transmisión" : "Guardado; se enviará cuando RX02 sea actualizado";
    updateMeshCounter();
    await loadMeshMessages();
  } catch (error) {
    $("meshMessageStatus").textContent = error.message || "No fue posible guardar el mensaje";
  } finally {
    setButtonLoading(button, false);
    updateMeshCounter();
  }
});

$("meshClearHistory").addEventListener("click", async () => {
  const accepted = window.confirm(
    "¿Limpiar el historial visible de esta página?\n\n" +
    "Los mensajes ya recibidos por el T1000-E no se borrarán. " +
    "Los envíos todavía pendientes se conservarán."
  );
  if (!accepted) return;

  const button = $("meshClearHistory");
  setButtonLoading(button, true, "Limpiando…");
  $("meshMessageStatus").textContent = "Limpiando solamente el historial de RX01…";
  try {
    const response = await fetch("/api/meshtastic/mensajes", {method:"DELETE"});
    if (!response.ok) throw new Error(await response.text());
    const result = await response.json();
    const pending = Number(result.pendientes_conservados || 0);
    $("meshMessageStatus").textContent = `${result.eliminados || 0} mensaje(s) retirado(s) de la página${pending ? ` · ${pending} pendiente(s) conservado(s)` : ""}. T1000-E sin cambios.`;
    await loadMeshMessages();
  } catch (error) {
    $("meshMessageStatus").textContent = error.message || "No fue posible limpiar el historial";
  } finally {
    setButtonLoading(button, false);
  }
});

window.addEventListener("mina:access-change", event => {
  if (!event.detail.authorized) {
    meshAuthorized = false;
    renderMeshMessages();
  } else loadMeshMessages();
});
window.addEventListener("mina:mesh-access-granted", loadMeshMessages);

updateMeshCounter();
loadMeshMessages();
setInterval(loadMeshMessages, 5000);
