const gpsEl = id => document.getElementById(id);
const GPS_BEACON_ID = "TAG-001";
let gpsAuthorized = false;
let gpsTrackerData = null;
let gpsSelectedTagId = null;
let gpsOfflineMap = null;
let gpsOfflineMapPromise = null;
let gpsMapZoom = 2.0;

function pairedBeacon() {
  return (state.beacons || []).find(item => item.id === GPS_BEACON_ID) || null;
}

function formatGpsAge(timestamp) {
  if (!timestamp) return "sin fecha";
  const elapsed = Math.max(0, Date.now() - Number(timestamp));
  const minutes = Math.floor(elapsed / 60000);
  if (minutes < 1) return "hace menos de 1 min";
  if (minutes < 60) return `hace ${minutes} min`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `hace ${hours} h`;
  return `hace ${Math.floor(hours / 24)} día${hours < 48 ? "" : "s"}`;
}

function formatGpsDate(timestamp) {
  if (!timestamp) return "Sin fecha disponible";
  return new Intl.DateTimeFormat("es-CL", {dateStyle: "short", timeStyle: "medium"}).format(new Date(Number(timestamp)));
}

function renderBeaconGpsLink() {
  const beacon = pairedBeacon();
  const active = Boolean(beacon?.activo && beacon.estado !== "sin_senal");
  gpsEl("gpsBleState").textContent = active
    ? `BLE: ${beacon.reader_id} · ${beacon.sector}`
    : "Fuera del alcance BLE del DXF · GPS independiente";

  const selection = gpsEl("tagGpsSelection");
  if (gpsSelectedTagId !== GPS_BEACON_ID) {
    selection.hidden = true;
    return;
  }
  selection.hidden = false;
  gpsEl("tagGpsSelectionName").textContent = beacon?.nombre || "Trabajador 01 - Supervisor";
  gpsEl("tagGpsSelectionStatus").textContent = active
    ? `Detectado en ${beacon.sector} por ${beacon.reader_id}. El T1000-E aporta la vista exterior.`
    : "El beacon ya no está visible en el DXF. Consulta la última posición recibida del T1000-E.";
}

function setGpsSummaryState(kind, title, detail) {
  const card = gpsEl("gpsFreshness").closest("article");
  card.classList.remove("fresh", "stale", "offline");
  card.classList.add(kind);
  gpsEl("gpsFreshness").textContent = title;
  gpsEl("gpsLastFix").textContent = detail;
}

function renderGpsSummary() {
  const position = gpsTrackerData?.position;
  gpsEl("gpsSource").textContent = gpsTrackerData?.hardware || "SenseCAP T1000-E";
  if (!gpsAuthorized) {
    setGpsSummaryState("offline", "Acceso protegido", "Inicia sesión para consultar coordenadas");
    gpsEl("gpsBattery").textContent = "Batería reservada";
    gpsEl("gpsAccessNote").textContent = "Las coordenadas exactas solo son visibles para supervisor o administrador.";
    return;
  }
  if (!position) {
    setGpsSummaryState("offline", "Sin posición recibida", "El gateway Meshtastic aún no entregó un punto GNSS");
    gpsEl("gpsBattery").textContent = "Sin telemetría";
    return;
  }
  const age = Math.max(0, Date.now() - Number(position.timestamp));
  if (age <= 20 * 60000) setGpsSummaryState("fresh", "Posición vigente", `${formatGpsDate(position.timestamp)} · ${formatGpsAge(position.timestamp)}`);
  else if (age <= 2 * 60 * 60000) setGpsSummaryState("stale", "Última posición", `${formatGpsDate(position.timestamp)} · ${formatGpsAge(position.timestamp)}`);
  else setGpsSummaryState("offline", "Posición histórica", `${formatGpsDate(position.timestamp)} · ${formatGpsAge(position.timestamp)}`);
  const battery = Number(position.battery_level);
  gpsEl("gpsBattery").textContent = Number.isFinite(battery)
    ? `${battery >= 101 ? "Cargando" : `${battery}%`}${position.voltage ? ` · ${Number(position.voltage).toFixed(2)} V` : ""}`
    : "Sin telemetría";
  gpsEl("gpsAccessNote").textContent = "Ubicación obtenida del T1000-E y servida dentro de MINA-LOCAL, sin mapa de Internet.";
}

function gpsSvgElement(name, attributes = {}) {
  const node = document.createElementNS("http://www.w3.org/2000/svg", name);
  Object.entries(attributes).forEach(([key, value]) => node.setAttribute(key, value));
  return node;
}

function gpsSvgText(parent, x, y, value, className = "") {
  const text = gpsSvgElement("text", {x, y, class: className});
  text.textContent = value;
  parent.appendChild(text);
  return text;
}

function mapPath(coordinates, project, close = false) {
  const path = (coordinates || []).map((coordinate, index) => {
    const [x, y] = project(coordinate);
    return `${index ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`;
  }).join(" ");
  return close && path ? `${path} Z` : path;
}

function roadClass(value) {
  if (value === "motorway" || value === "trunk" || value === "primary" || value === "secondary" || value === "tertiary") return value;
  return "local";
}

function distanceToSegment(point, start, end) {
  const dx = end[0] - start[0], dy = end[1] - start[1];
  if (!dx && !dy) return Math.hypot(point[0] - start[0], point[1] - start[1]);
  const factor = Math.max(0, Math.min(1, ((point[0] - start[0]) * dx + (point[1] - start[1]) * dy) / (dx * dx + dy * dy)));
  return Math.hypot(point[0] - start[0] - factor * dx, point[1] - start[1] - factor * dy);
}

function closestIquiqueStreet(lat, lon) {
  if (!gpsOfflineMap) return null;
  const lonFactor = 111320 * Math.cos(lat * Math.PI / 180);
  const local = coordinate => [(Number(coordinate[0]) - lon) * lonFactor, (Number(coordinate[1]) - lat) * 111320];
  let closest = null;
  for (const road of gpsOfflineMap.roads || []) {
    if (!road[1] || !Array.isArray(road[2])) continue;
    for (let index = 1; index < road[2].length; index += 1) {
      const distance = distanceToSegment([0, 0], local(road[2][index - 1]), local(road[2][index]));
      if (!closest || distance < closest.distance) closest = {name: road[1], distance};
    }
  }
  return closest;
}

function pointInsideIquique(point) {
  if (!gpsOfflineMap?.bounds) return false;
  const [south, west, north, east] = gpsOfflineMap.bounds;
  const lat = Number(point.latitude), lon = Number(point.longitude);
  return lat >= south && lat <= north && lon >= west && lon <= east;
}

function createIquiqueProjection(latest) {
  const [south, west, north, east] = gpsOfflineMap.bounds;
  const centerLat = (south + north) / 2;
  const lonFactor = 111320 * Math.cos(centerLat * Math.PI / 180);
  const fullWidth = (east - west) * lonFactor;
  const fullHeight = (north - south) * 111320;
  const baseScale = Math.min(900 / fullWidth, 450 / fullHeight);
  const scale = baseScale * gpsMapZoom;
  const toLocal = coordinate => [(Number(coordinate[0]) - west) * lonFactor, (Number(coordinate[1]) - south) * 111320];
  const desired = toLocal([latest.longitude, latest.latitude]);
  const halfWidth = 470 / scale, halfHeight = 245 / scale;
  const centered = (value, maximum, half) => maximum <= half * 2 ? maximum / 2 : Math.max(half, Math.min(maximum - half, value));
  const centerX = centered(desired[0], fullWidth, halfWidth);
  const centerY = centered(desired[1], fullHeight, halfHeight);
  const project = coordinate => {
    const [x, y] = toLocal(coordinate);
    return [500 + (x - centerX) * scale, 260 - (y - centerY) * scale];
  };
  return {project, visible: coordinate => {
    const [x, y] = project(coordinate);
    return x >= -20 && x <= 1020 && y >= -20 && y <= 540;
  }};
}

function drawIquiqueMap(svg, latest) {
  const projection = createIquiqueProjection(latest);
  const {project, visible} = projection;
  svg.appendChild(gpsSvgElement("rect", {x: 0, y: 0, width: 1000, height: 520, class: "gps-map-land"}));

  for (const area of gpsOfflineMap.areas || []) {
    const path = mapPath(area[2], project, true);
    if (path) svg.appendChild(gpsSvgElement("path", {d: path, class: `gps-map-area ${String(area[0]).replace(/[^a-z_]/g, "")}`}));
  }
  const coastPath = (gpsOfflineMap.coast || []).map(line => mapPath(line, project)).join(" ");
  if (coastPath) svg.appendChild(gpsSvgElement("path", {d: coastPath, class: "gps-coast"}));

  const roadPaths = new Map();
  for (const road of gpsOfflineMap.roads || []) {
    const kind = roadClass(road[0]);
    const path = mapPath(road[2], project);
    if (path) roadPaths.set(kind, `${roadPaths.get(kind) || ""} ${path}`);
  }
  ["local", "tertiary", "secondary", "primary", "trunk", "motorway"].forEach(kind => {
    if (roadPaths.has(kind)) svg.appendChild(gpsSvgElement("path", {d: roadPaths.get(kind), class: `gps-road ${kind}`}));
  });

  const priority = {motorway: 0, trunk: 1, primary: 2, secondary: 3, tertiary: 4, residential: 5};
  const roadLabels = (gpsOfflineMap.roads || []).filter(road => road[1] && road[2]?.length).map(road => {
    const coordinate = road[2][Math.floor(road[2].length / 2)];
    const screen = project(coordinate);
    return {name: road[1], coordinate, screen, priority: priority[road[0]] ?? 6, distance: Math.hypot(screen[0] - 500, screen[1] - 260)};
  }).filter(item => visible(item.coordinate)).sort((a, b) => a.priority - b.priority || a.distance - b.distance);
  const usedNames = new Set();
  for (const item of roadLabels) {
    if (usedNames.has(item.name) || usedNames.size >= 18) continue;
    usedNames.add(item.name);
    const text = gpsSvgText(svg, item.screen[0], item.screen[1] - 4, item.name, "gps-street-label");
    text.setAttribute("text-anchor", "middle");
  }
  for (const label of gpsOfflineMap.labels || []) {
    const coordinate = [label[2], label[3]];
    if (!visible(coordinate)) continue;
    const [x, y] = project(coordinate);
    const text = gpsSvgText(svg, x, y, label[1], `gps-place-label ${label[0]}`);
    text.setAttribute("text-anchor", "middle");
  }

  const closest = closestIquiqueStreet(Number(latest.latitude), Number(latest.longitude));
  gpsEl("gpsArea").textContent = closest
    ? `Iquique · cerca de ${closest.name} (${Math.max(10, Math.round(closest.distance / 10) * 10)} m)`
    : "Sector: Iquique";
  gpsEl("gpsMapCaption").textContent = gpsOfflineMap.attribution || "© OpenStreetMap contributors · ODbL";
  return point => project([Number(point.longitude), Number(point.latitude)]);
}

function drawGpsGrid(svg, points) {
  const lat0 = points.reduce((sum, point) => sum + Number(point.latitude), 0) / points.length;
  const lon0 = points.reduce((sum, point) => sum + Number(point.longitude), 0) / points.length;
  const lonFactor = 111320 * Math.max(.2, Math.cos(lat0 * Math.PI / 180));
  const local = points.map(point => ({x: (Number(point.longitude) - lon0) * lonFactor, y: (Number(point.latitude) - lat0) * 111320, source: point}));
  const xs = local.map(point => point.x), ys = local.map(point => point.y);
  const centerX = (Math.min(...xs) + Math.max(...xs)) / 2, centerY = (Math.min(...ys) + Math.max(...ys)) / 2;
  const spanX = Math.max(300, Math.max(...xs) - Math.min(...xs)), spanY = Math.max(220, Math.max(...ys) - Math.min(...ys));
  const scale = Math.min(820 / spanX, 380 / spanY);
  const projectLocal = point => [500 + (point.x - centerX) * scale, 260 - (point.y - centerY) * scale];
  const grid = gpsSvgElement("g");
  for (let x = 80; x <= 920; x += 70) grid.appendChild(gpsSvgElement("line", {x1: x, y1: 30, x2: x, y2: 490, class: x === 500 ? "gps-axis" : "gps-grid"}));
  for (let y = 50; y <= 470; y += 70) grid.appendChild(gpsSvgElement("line", {x1: 40, y1: y, x2: 960, y2: y, class: y === 260 ? "gps-axis" : "gps-grid"}));
  svg.appendChild(grid);
  gpsEl("gpsArea").textContent = gpsOfflineMap ? "Fuera del mapa offline de Iquique" : "Cartografía offline no disponible";
  gpsEl("gpsMapCaption").textContent = "Cuadrícula GNSS local · fuera de cobertura cartográfica";
  return point => projectLocal({x: (Number(point.longitude) - lon0) * lonFactor, y: (Number(point.latitude) - lat0) * 111320});
}

async function loadOfflineGpsMap() {
  if (gpsOfflineMap) return gpsOfflineMap;
  if (!gpsOfflineMapPromise) gpsOfflineMapPromise = fetch("/static/iquique_map.json", {cache: "force-cache"})
    .then(response => {
      if (!response.ok) throw new Error(`Mapa offline: HTTP ${response.status}`);
      return response.json();
    })
    .then(document => {
      gpsOfflineMap = document;
      if (!gpsEl("gpsPanel").hidden) renderGpsMap();
      return document;
    })
    .catch(error => { console.error("No fue posible cargar el mapa offline de Iquique", error); return null; });
  return gpsOfflineMapPromise;
}

function renderGpsMap() {
  const svg = gpsEl("gpsSvg");
  svg.replaceChildren();
  const points = (gpsTrackerData?.history || []).filter(point => Number.isFinite(Number(point.latitude)) && Number.isFinite(Number(point.longitude)));
  if (!gpsAuthorized || !points.length) {
    const message = gpsSvgText(svg, 500, 250, gpsAuthorized ? "Sin puntos GNSS recibidos" : "Acceso de supervisor requerido", "gps-map-outside");
    message.setAttribute("text-anchor", "middle");
    return;
  }

  const latest = points[0];
  const project = pointInsideIquique(latest) ? drawIquiqueMap(svg, latest) : drawGpsGrid(svg, points);
  const chronological = [...points].reverse();
  if (chronological.length > 1) svg.appendChild(gpsSvgElement("path", {d: chronological.map((point, index) => {
    const [x, y] = project(point);
    return `${index ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`;
  }).join(" "), class: "gps-track"}));
  chronological.slice(0, -1).forEach(point => {
    const [x, y] = project(point);
    svg.appendChild(gpsSvgElement("circle", {cx: x, cy: y, r: 7, class: "gps-history-point"}));
  });
  const [currentX, currentY] = project(latest);
  svg.appendChild(gpsSvgElement("circle", {cx: currentX, cy: currentY, r: 42, class: "gps-location-ring"}));
  svg.appendChild(gpsSvgElement("circle", {cx: currentX, cy: currentY, r: 31, class: "gps-current-halo"}));
  svg.appendChild(gpsSvgElement("circle", {cx: currentX, cy: currentY, r: 13, class: "gps-current-point"}));
  const labelX = Math.min(735, Math.max(25, currentX + 28)), labelY = Math.min(430, Math.max(35, currentY - 68));
  const label = gpsSvgElement("g", {class: "gps-worker-label"});
  label.appendChild(gpsSvgElement("rect", {x: labelX, y: labelY, width: 240, height: 58, rx: 9}));
  gpsSvgText(label, labelX + 14, labelY + 23, "Trabajador 01 · Supervisor");
  gpsSvgText(label, labelX + 14, labelY + 45, formatGpsAge(latest.timestamp), "gps-label-detail");
  svg.appendChild(label);

  gpsEl("gpsCoordinates").textContent = `${Number(latest.latitude).toFixed(7)}, ${Number(latest.longitude).toFixed(7)} · ${formatGpsDate(latest.timestamp)}`;
  gpsEl("gpsAltitude").textContent = `Altitud: ${Number(latest.altitude || 0).toFixed(1)} m`;
  gpsEl("gpsPrecision").textContent = Number(latest.precision_bits) >= 30 ? "Coordenada GNSS completa" : `Precisión transmitida: ${latest.precision_bits || "--"} bits`;
  gpsEl("gpsPointCount").textContent = `Puntos: ${points.length}`;
}

async function loadGpsTracker() {
  try {
    const response = await fetch("/api/gps", {cache: "no-store"});
    const payload = await response.json();
    gpsAuthorized = response.ok && Boolean(payload.autorizado);
    gpsTrackerData = payload;
  } catch (error) {
    gpsAuthorized = false;
    gpsTrackerData = null;
    console.error("No fue posible consultar el geotracker", error);
  }
  renderGpsSummary();
  if (!gpsEl("gpsPanel").hidden) renderGpsMap();
  return gpsAuthorized;
}

async function openGpsPanel() {
  // Primero valida la sesión: el selector debe abrir de inmediato y no esperar
  // la descarga del mapa offline de Iquique (aprox. 360 KB).
  const authorized = await loadGpsTracker();
  if (!authorized) {
    gpsEl("gpsAccessNote").textContent = "Elige Supervisor o Administrador e inicia sesión para mostrar la ubicación exacta.";
    gpsEl("gpsRoleModal").hidden = false;
    gpsEl("gpsRoleSupervisor").focus();
    return;
  }
  await loadOfflineGpsMap();
  gpsEl("gpsPanel").hidden = false;
  renderGpsMap();
  gpsEl("gpsPanel").scrollIntoView({behavior: "smooth", block: "start"});
}

window.addEventListener("mina:tag-selected", event => {
  gpsSelectedTagId = event.detail?.tagId || null;
  renderBeaconGpsLink();
});
window.addEventListener("mina:access-change", () => loadGpsTracker());
window.addEventListener("mina:gps-access-granted", openGpsPanel);
gpsEl("gpsRoleClose").addEventListener("click", () => { gpsEl("gpsRoleModal").hidden = true; });
gpsEl("gpsRoleModal").addEventListener("click", event => {
  if (event.target === gpsEl("gpsRoleModal")) gpsEl("gpsRoleModal").hidden = true;
});
gpsEl("gpsRoleSupervisor").addEventListener("click", () => {
  gpsEl("gpsRoleModal").hidden = true;
  if (typeof window.openSupervisorForGps === "function") window.openSupervisorForGps();
});
gpsEl("gpsRoleAdmin").addEventListener("click", () => {
  gpsEl("gpsRoleModal").hidden = true;
  if (typeof window.openAdminForGps === "function") window.openAdminForGps();
});
gpsEl("tagGpsSelectionOpen").addEventListener("click", openGpsPanel);
gpsEl("gpsOpen").addEventListener("click", openGpsPanel);
gpsEl("gpsRefresh").addEventListener("click", loadGpsTracker);
gpsEl("gpsClose").addEventListener("click", () => { gpsEl("gpsPanel").hidden = true; });
gpsEl("gpsMapZoomIn").addEventListener("click", () => { gpsMapZoom = Math.min(5, gpsMapZoom + .5); renderGpsMap(); });
gpsEl("gpsMapZoomOut").addEventListener("click", () => { gpsMapZoom = Math.max(1, gpsMapZoom - .5); renderGpsMap(); });
gpsEl("gpsMapCenter").addEventListener("click", () => { gpsMapZoom = 2; renderGpsMap(); });

renderBeaconGpsLink();
renderGpsSummary();
loadGpsTracker();
// El mapa de Iquique pesa cerca de 360 KB. Se descarga solamente cuando el
// usuario abre la vista GPS (openGpsPanel), no durante la carga del portal.
setInterval(renderBeaconGpsLink, 1500);
setInterval(loadGpsTracker, 30000);
