let supervisorAuthenticated = false;
let adminAuthenticated = false;
let currentSupervisorName = "";
let supervisorMessages = [];

function initials(value, fallback = "--") {
  const words = String(value || "").trim().split(/\s+/).filter(Boolean);
  if (!words.length) return fallback;
  return (words.length > 1 ? words[0][0] + words[1][0] : words[0].slice(0, 2)).toUpperCase();
}

function updatePendingTagAlerts(redraw = true) {
  const priorities = { informacion: 1, atencion: 2, emergencia: 3 };
  const alerts = new Map();
  const tagIds = (state.beacons || []).map(item => item.id);
  supervisorMessages
    .filter(item => item.vigente && !item.confirmado_por)
    .forEach(item => {
      const targets = item.destino === "todos" ? tagIds : [item.destino];
      targets.forEach(tagId => {
        const previous = alerts.get(tagId);
        if (!previous || priorities[item.nivel] > priorities[previous.nivel]) alerts.set(tagId, item);
      });
    });
  window.pendingTagAlerts = alerts;
  if (redraw && typeof window.renderMineTracking === "function") window.renderMineTracking();
}

function updateMessageCounters() {
  $("messageTitleCount").textContent = `${$("messageTitle").value.length} / 70 caracteres · revisión ortográfica solicitada al navegador`;
  $("messageBodyCount").textContent = `${$("messageBody").value.length} / 500 caracteres · revisión ortográfica solicitada al navegador`;
}

function renderAccessRole() {
  const badge = $("accessRole");
  if (adminAuthenticated) {
    badge.className = "role-indicator admin";
    $("accessRoleIcon").textContent = "AD";
    $("accessRoleTitle").textContent = "Modo administrador";
    $("accessRoleDetail").textContent = "Control total del sistema";
  } else if (supervisorAuthenticated) {
    badge.className = "role-indicator supervisor";
    $("accessRoleIcon").textContent = initials(currentSupervisorName, "SU");
    $("accessRoleTitle").textContent = currentSupervisorName || "Modo supervisor";
    $("accessRoleDetail").textContent = "Supervisor autenticado";
  } else {
    badge.className = "role-indicator public";
    $("accessRoleIcon").textContent = "VP";
    $("accessRoleTitle").textContent = "Vista pública";
    $("accessRoleDetail").textContent = "Sin sesión iniciada";
  }
}

function messageTime(value) {
  if (!value) return "";
  return new Intl.DateTimeFormat("es-CL", { dateStyle: "short", timeStyle: "short" }).format(new Date(value));
}

function setButtonLoading(button, loading, loadingText = "Procesando…") {
  if (!button) return;
  if (loading) {
    button.dataset.originalText = button.textContent.trim();
    button.disabled = true;
    button.classList.add("is-loading");
    button.textContent = loadingText;
  } else {
    button.disabled = false;
    button.classList.remove("is-loading");
    if (button.dataset.originalText) button.textContent = button.dataset.originalText;
  }
}

function syncSupervisorLoginButton() {
  $("supervisorPin").value = $("supervisorPin").value.replace(/\D/g, "").slice(0, 6);
  $("supervisorLoginSubmit").disabled = !$("supervisorLoginName").value.trim() || !/^\d{6}$/.test($("supervisorPin").value);
}

function syncAdminLoginButton() {
  $("adminPin").value = $("adminPin").value.replace(/\D/g, "").slice(0, 5);
  $("adminLoginSubmit").disabled = !/^\d{5}$/.test($("adminPin").value);
}

function messageLevelIcon(level) {
  if (level === "informacion") return '<svg viewBox="0 0 20 20" aria-hidden="true"><circle cx="10" cy="10" r="8"></circle><path d="M10 8.4v5M10 5.8v.2"></path></svg>';
  if (level === "atencion") return '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M9 3.4 2.3 15a1.2 1.2 0 0 0 1 1.8h13.4a1.2 1.2 0 0 0 1-1.8L11 3.4a1.15 1.15 0 0 0-2 0Z"></path><path d="M10 7.3v4.5M10 14.3v.1"></path></svg>';
  return '<svg viewBox="0 0 20 20" aria-hidden="true"><circle cx="10" cy="10" r="8"></circle><path d="M10 5.8v5.6M10 14.2v.1"></path></svg>';
}

const checkIcon = '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="m4 10.3 3.6 3.6L16 5.8"></path></svg>';
const trashIcon = '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M4.8 6.3h10.4M8 3.8h4M6.3 6.3l.6 10h6.2l.6-10M8.5 9v4.5M11.5 9v4.5"></path></svg>';

function renderSupervisorMessages() {
  const current = supervisorMessages.filter(item => item.vigente && !item.confirmado_por);
  const resolved = supervisorMessages.filter(item => !item.vigente || item.confirmado_por);
  const items = [...current, ...(adminAuthenticated ? resolved : resolved.slice(0, 4))];
  $("messageCount").textContent = `${current.length} pendiente${current.length === 1 ? "" : "s"}`;
  $("messageList").innerHTML = items.length ? items.map(item => {
    const confirmed = Boolean(item.confirmado_por);
    const status = confirmed ? "Confirmado" : item.vigente ? "Pendiente" : "Finalizado";
    return `<article class="supervisor-message ${esc(item.nivel)} ${confirmed ? "confirmed" : ""}" data-message-id="${esc(item.id)}" tabindex="-1">
      <div class="message-meta"><span class="message-context"><span class="message-level-icon">${messageLevelIcon(item.nivel)}</span><span>${esc(item.destino === "todos" ? "Todos los TAG" : item.destino)} · ${esc(item.nivel)}</span></span><span class="message-status">${status}</span></div>
      <h3>${esc(item.titulo)}</h3><p>${esc(item.mensaje)}</p>
      <div class="message-foot"><span>Emitido por ${esc(item.autor)}</span><time>${esc(messageTime(item.fecha))}</time></div>
      ${confirmed ? `<div class="message-ack"><span>✓ Caso tomado</span><strong>Confirmado por ${esc(item.confirmado_por)}</strong><time>${esc(messageTime(item.confirmado_fecha))}</time></div>` : ""}
      <div class="message-actions">
        ${supervisorAuthenticated && item.vigente && !confirmed ? `<button class="confirm-message" data-message-id="${esc(item.id)}">${checkIcon}<span>Confirmar y tomar caso</span></button>` : ""}
        ${adminAuthenticated ? `<button class="delete-message" data-message-id="${esc(item.id)}">${trashIcon}<span>Eliminar mensaje</span></button>` : ""}
      </div>
    </article>`;
  }).join("") : '<div class="empty">No hay mensajes publicados.</div>';
  document.querySelectorAll(".confirm-message").forEach(button => button.addEventListener("click", () => confirmMessage(button.dataset.messageId)));
  document.querySelectorAll(".delete-message").forEach(button => button.addEventListener("click", () => deleteMessage(button.dataset.messageId)));
  updatePendingTagAlerts();
}

window.focusSupervisorMessage = function focusSupervisorMessage(id) {
  const card = [...document.querySelectorAll(".supervisor-message[data-message-id]")]
    .find(item => item.dataset.messageId === String(id));
  if (!card) return;
  document.querySelectorAll(".supervisor-message.message-focus").forEach(item => item.classList.remove("message-focus"));
  card.scrollIntoView({ behavior: "smooth", block: "center" });
  card.classList.add("message-focus");
  card.focus({ preventScroll: true });
  window.setTimeout(() => card.classList.remove("message-focus"), 4200);
};

async function loadSupervisorMessages() {
  try {
    const response = await fetch("/api/mensajes", { cache: "no-store" });
    supervisorMessages = (await response.json()).mensajes || [];
    renderSupervisorMessages();
  } catch (error) { console.error(error); }
}

async function checkSupervisorSession() {
  const response = await fetch("/api/supervisor/estado", { cache: "no-store" });
  const session = await response.json();
  supervisorAuthenticated = session.autenticado;
  currentSupervisorName = session.nombre || "";
  $("messageAuthor").value = currentSupervisorName;
  $("loginForm").hidden = supervisorAuthenticated;
  $("messageForm").hidden = !supervisorAuthenticated;
  renderSupervisorMessages();
  renderAccessRole();
}

function openSupervisor() {
  $("supervisorModal").hidden = false;
  $("loginError").textContent = "";
  $("publishStatus").textContent = "";
  checkSupervisorSession().then(() => { if (!supervisorAuthenticated) $("supervisorLoginName").focus(); });
}

function closeSupervisor() { $("supervisorModal").hidden = true; }

$("supervisorOpen").addEventListener("click", openSupervisor);
$("supervisorClose").addEventListener("click", closeSupervisor);
$("supervisorModal").addEventListener("click", event => { if (event.target === $("supervisorModal")) closeSupervisor(); });

document.querySelectorAll(".pin-toggle").forEach(button => button.addEventListener("click", () => {
  const input = $(button.dataset.pinTarget);
  const showing = input.type === "text";
  input.type = showing ? "password" : "text";
  button.classList.toggle("showing", !showing);
  button.setAttribute("aria-label", showing ? "Mostrar PIN" : "Ocultar PIN");
  button.title = showing ? "Mostrar PIN" : "Ocultar PIN";
  input.focus();
}));
$("supervisorLoginName").addEventListener("input", syncSupervisorLoginButton);
$("supervisorPin").addEventListener("input", syncSupervisorLoginButton);
$("adminPin").addEventListener("input", syncAdminLoginButton);

$("loginForm").addEventListener("submit", async event => {
  event.preventDefault();
  const submit = $("supervisorLoginSubmit");
  if (submit.disabled) return;
  $("loginError").textContent = "";
  setButtonLoading(submit, true, "Verificando…");
  let response;
  try {
    response = await fetch("/api/supervisor/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ nombre: $("supervisorLoginName").value.trim(), pin: $("supervisorPin").value })
    });
  } catch (error) {
    $("loginError").textContent = "No fue posible contactar al servidor.";
    setButtonLoading(submit, false);
    syncSupervisorLoginButton();
    return;
  }
  if (!response.ok) { $("loginError").textContent = await response.text(); setButtonLoading(submit, false); syncSupervisorLoginButton(); return; }
  const session = await response.json();
  $("supervisorPin").value = "";
  supervisorAuthenticated = true;
  currentSupervisorName = session.nombre;
  $("messageAuthor").value = currentSupervisorName;
  $("loginForm").hidden = true;
  $("messageForm").hidden = false;
  renderSupervisorMessages();
  renderAccessRole();
  $("messageTitle").focus();
  setButtonLoading(submit, false);
  syncSupervisorLoginButton();
});

$("messageForm").addEventListener("submit", async event => {
  event.preventDefault();
  const submit = $("publishMessage");
  $("publishStatus").textContent = "Publicando…";
  setButtonLoading(submit, true, "Publicando…");
  const payload = { destino: $("messageTarget").value, nivel: $("messageLevel").value, titulo: $("messageTitle").value, mensaje: $("messageBody").value };
  let response;
  try { response = await fetch("/api/mensajes", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload) }); }
  catch (error) { $("publishStatus").textContent = "No fue posible contactar al servidor."; setButtonLoading(submit, false); return; }
  if (!response.ok) { $("publishStatus").textContent = await response.text(); setButtonLoading(submit, false); return; }
  $("messageTitle").value = "";
  $("messageBody").value = "";
  updateMessageCounters();
  $("publishStatus").textContent = "Mensaje publicado correctamente";
  await loadSupervisorMessages();
  setButtonLoading(submit, false);
});

$("clearMessageForm").addEventListener("click", () => {
  $("messageTitle").value = "";
  $("messageBody").value = "";
  $("messageLevel").value = "informacion";
  $("messageTarget").value = "todos";
  $("publishStatus").textContent = "Formulario limpio";
  updateMessageCounters();
  $("messageTitle").focus();
});

async function confirmMessage(id) {
  const button = [...document.querySelectorAll(".confirm-message")].find(item => item.dataset.messageId === String(id));
  setButtonLoading(button, true, "Confirmando…");
  let response;
  try { response = await fetch(`/api/mensajes/${encodeURIComponent(id)}/confirmar`, { method: "POST" }); }
  catch (error) { setButtonLoading(button, false); alert("No fue posible contactar al servidor."); return; }
  if (!response.ok) { setButtonLoading(button, false); alert(await response.text()); return; }
  await loadSupervisorMessages();
}

async function deleteMessage(id) {
  if (!confirm("¿Eliminar definitivamente este mensaje?")) return;
  const response = await fetch(`/api/mensajes/${encodeURIComponent(id)}`, { method: "DELETE" });
  if (!response.ok) { alert(await response.text()); return; }
  await loadSupervisorMessages();
}

$("logoutSupervisor").addEventListener("click", async () => {
  await fetch("/api/supervisor/logout", { method: "POST" });
  supervisorAuthenticated = false;
  currentSupervisorName = "";
  $("messageAuthor").value = "";
  $("messageForm").hidden = true;
  $("loginForm").hidden = false;
  syncSupervisorLoginButton();
  renderSupervisorMessages();
  renderAccessRole();
});

async function checkAdminSession() {
  const response = await fetch("/api/administrador/estado", { cache: "no-store" });
  adminAuthenticated = (await response.json()).autenticado;
  $("adminLoginForm").hidden = adminAuthenticated;
  $("adminControls").hidden = !adminAuthenticated;
  document.querySelectorAll(".admin-layout-control").forEach(control => control.hidden = !adminAuthenticated);
  renderAssets();
  renderSupervisorMessages();
  renderAccessRole();
}

function openAdmin() {
  $("adminModal").hidden = false;
  $("adminLoginError").textContent = "";
  $("adminStatus").textContent = "";
  checkAdminSession().then(() => { if (!adminAuthenticated) $("adminPin").focus(); });
}

function closeAdmin() { $("adminModal").hidden = true; }
$("adminOpen").addEventListener("click", openAdmin);
$("adminClose").addEventListener("click", closeAdmin);
$("adminModal").addEventListener("click", event => { if (event.target === $("adminModal")) closeAdmin(); });

$("adminLoginForm").addEventListener("submit", async event => {
  event.preventDefault();
  const submit = $("adminLoginSubmit");
  if (submit.disabled) return;
  $("adminLoginError").textContent = "";
  setButtonLoading(submit, true, "Verificando…");
  let response;
  try { response = await fetch("/api/administrador/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ pin: $("adminPin").value }) }); }
  catch (error) { $("adminLoginError").textContent = "No fue posible contactar al servidor."; setButtonLoading(submit, false); syncAdminLoginButton(); return; }
  if (!response.ok) { $("adminLoginError").textContent = await response.text(); setButtonLoading(submit, false); syncAdminLoginButton(); return; }
  $("adminPin").value = "";
  adminAuthenticated = true;
  $("adminLoginForm").hidden = true;
  $("adminControls").hidden = false;
  document.querySelectorAll(".admin-layout-control").forEach(control => control.hidden = false);
  renderAssets();
  renderSupervisorMessages();
  renderAccessRole();
  setButtonLoading(submit, false);
  syncAdminLoginButton();
});

$("logoutAdmin").addEventListener("click", async () => {
  await fetch("/api/administrador/logout", { method: "POST" });
  adminAuthenticated = false;
  $("adminControls").hidden = true;
  $("adminLoginForm").hidden = false;
  syncAdminLoginButton();
  document.querySelectorAll(".admin-layout-control").forEach(control => control.hidden = true);
  $("adminStatus").textContent = "";
  renderAssets();
  renderSupervisorMessages();
  renderAccessRole();
});

$("clearMessages").addEventListener("click", async () => {
  if (!confirm("¿Eliminar definitivamente todos los mensajes del supervisor?")) return;
  const response = await fetch("/api/mensajes", { method: "DELETE" });
  if (response.ok) { $("adminStatus").textContent = "Mensajes del supervisor eliminados"; await loadSupervisorMessages(); }
  else $("adminStatus").textContent = await response.text();
});

$("clearEvents").addEventListener("click", async () => {
  if (!confirm("¿Eliminar definitivamente todo el historial de proximidad?")) return;
  const response = await fetch("/api/historial-proximidad", { method: "DELETE" });
  $("adminStatus").textContent = response.ok ? "Historial de proximidad eliminado" : await response.text();
});

loadSupervisorMessages();
[$("messageTitle"), $("messageBody")].forEach(field => field.addEventListener("input", updateMessageCounters));
updateMessageCounters();
syncSupervisorLoginButton();
syncAdminLoginButton();
Promise.all([checkSupervisorSession(), checkAdminSession()]).catch(error => console.error("No fue posible verificar la sesión", error));
setInterval(loadSupervisorMessages, 5000);
