let mineLayout = null;
const svgNS = "http://www.w3.org/2000/svg";
const initialMapView = {x: 0, y: 0, width: 1000, height: 560};
let mapView = {...initialMapView};
let mapDrag = null;
const mapPointers = new Map();
let pinchState = null;
let selectedTagId = null;
const stateColors = {
  peligro: "#e24335",
  precaucion: "#f2b705",
  proximo: "#3182bd",
  seguro: "#18a06f",
  sin_senal: "#7f8984"
};

function svgElement(name, attributes = {}) {
  const element = document.createElementNS(svgNS, name);
  for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, value);
  return element;
}

function applyMapView() {
  $("mineSvg").setAttribute("viewBox", `${mapView.x} ${mapView.y} ${mapView.width} ${mapView.height}`);
}

function constrainMapView() {
  // Permite un pequeño margen exterior, pero evita que el plano se pierda fuera del visor.
  const marginX = mapView.width * .12;
  const marginY = mapView.height * .12;
  const minX = initialMapView.x - marginX;
  const maxX = initialMapView.x + initialMapView.width - mapView.width + marginX;
  const minY = initialMapView.y - marginY;
  const maxY = initialMapView.y + initialMapView.height - mapView.height + marginY;
  mapView.x = Math.min(maxX, Math.max(minX, mapView.x));
  mapView.y = Math.min(maxY, Math.max(minY, mapView.y));
}

function zoomMap(factor, clientX = null, clientY = null) {
  const svg = $("mineSvg");
  const rect = svg.getBoundingClientRect();
  const ratioX = clientX == null ? .5 : Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
  const ratioY = clientY == null ? .5 : Math.max(0, Math.min(1, (clientY - rect.top) / rect.height));
  const nextWidth = Math.max(260, Math.min(initialMapView.width, mapView.width * factor));
  const nextHeight = nextWidth * initialMapView.height / initialMapView.width;
  const anchorX = mapView.x + mapView.width * ratioX;
  const anchorY = mapView.y + mapView.height * ratioY;
  mapView.x = anchorX - nextWidth * ratioX;
  mapView.y = anchorY - nextHeight * ratioY;
  mapView.width = nextWidth;
  mapView.height = nextHeight;
  constrainMapView();
  applyMapView();
}

function boundsOf(layout) {
  const points = [];
  (layout.segmentos || []).forEach(segment => points.push(segment.a, segment.b));
  (layout.readers || []).forEach(reader => points.push([reader.x, reader.y, reader.z]));
  const xs = points.map(point => Number(point[0]) || 0);
  const ys = points.map(point => Number(point[1]) || 0);
  const zs = points.map(point => Number(point[2]) || 0);
  return {minX: Math.min(...xs), maxX: Math.max(...xs), minY: Math.min(...ys), maxY: Math.max(...ys), minZ: Math.min(...zs), maxZ: Math.max(...zs)};
}

function projector(layout) {
  const bounds = boundsOf(layout);
  const rangeX = Math.max(1, bounds.maxX - bounds.minX);
  const rangeY = Math.max(1, bounds.maxY - bounds.minY);
  const rangeZ = Math.max(1, bounds.maxZ - bounds.minZ);
  if (layout.vista === "superior") return point => {
    const x = (Number(point[0]) - bounds.minX) / rangeX;
    const y = (Number(point[1]) - bounds.minY) / rangeY;
    return [60 + x * 880, 520 - y * 460];
  };
  return point => {
    const x = (Number(point[0]) - bounds.minX) / rangeX;
    const y = (Number(point[1]) - bounds.minY) / rangeY;
    const z = (Number(point[2]) - bounds.minZ) / rangeZ;
    return [500 + (x - y) * 360, 70 + (x + y) * 155 + (1 - z) * 145];
  };
}

function drawText(group, x, y, text, className = "reader-label") {
  const node = svgElement("text", {x, y, class: className, "text-anchor": "middle"});
  node.textContent = text;
  group.appendChild(node);
}

function tagNumber(tag) {
  const match = String(tag.id || "").match(/(\d+)$/);
  return match ? match[1].padStart(2, "0") : "--";
}

function parkingPlacement(tag, index, project) {
  const zone = mineLayout?.zonas_operativas?.[tag.reader_id];
  if (!zone?.puntos?.length) return null;
  const point = zone.puntos[index % zone.puntos.length];
  const projected = project(point);
  const plaza = tag.reader_id === "RX-01" ? mineLayout.plazas?.[index] : null;
  return {
    x: projected[0],
    y: projected[1],
    estado: plaza ? `${zone.estado} · Plaza ${plaza.id}` : zone.estado,
    clase: tag.reader_id === "RX-01" ? "estacionado" : tag.reader_id === "RX-02" ? "ingresando" : "saliendo"
  };
}

function drawTagLabel(group, x, y, tag, placeBelow, pendingMessage = null, operationalState = "") {
  const width = 178;
  const height = 44;
  const top = placeBelow ? y + 25 : y - height - 25;
  group.appendChild(svgElement("rect", {x: x - width / 2, y: top, width, height, rx: 8, class: "tag-label-card"}));
  const title = svgElement("text", {x, y: top + 17, class: "tag-label-title", "text-anchor": "middle"});
  title.textContent = `${tag.id} · ${tag.nombre}`;
  group.appendChild(title);
  const detail = svgElement("text", {x, y: top + 33, class: "tag-label-detail", "text-anchor": "middle"});
  const distance = tag.distancia == null ? "sin distancia" : `${tag.distancia} m`;
  detail.textContent = operationalState || `${labels[tag.estado] || tag.estado} · ${distance}`;
  group.appendChild(detail);
  if (pendingMessage) {
    const action = svgElement("g", {class: "tag-message-link", role: "button", tabindex: "0", "data-message-id": pendingMessage.id, "aria-label": `Abrir mensaje: ${pendingMessage.titulo}`});
    action.appendChild(svgElement("rect", {x: x - 95, y: top + 47, width: 190, height: 19, rx: 5}));
    const title = String(pendingMessage.titulo || "Mensaje pendiente");
    const compactTitle = title.length > 27 ? `${title.slice(0, 26)}…` : title;
    const actionText = svgElement("text", {x, y: top + 60, "text-anchor": "middle"});
    actionText.textContent = `! ${compactTitle}`;
    action.appendChild(actionText);
    const openMessage = event => {
      event.preventDefault();
      event.stopPropagation();
      if (typeof window.focusSupervisorMessage === "function") window.focusSupervisorMessage(pendingMessage.id);
    };
    action.addEventListener("click", openMessage);
    action.addEventListener("keydown", event => { if (event.key === "Enter" || event.key === " ") openMessage(event); });
    group.appendChild(action);
  }
}

function selectMapTag(svg, group, tagId) {
  selectedTagId = tagId;
  svg.querySelectorAll(".tag-node").forEach(node => node.classList.toggle("selected", node === group));
  svg.classList.add("has-tag-selection");
  svg.appendChild(group);
  group.focus({preventScroll: true});
}

function clearMapTagSelection() {
  selectedTagId = null;
  const svg = $("mineSvg");
  svg.classList.remove("has-tag-selection");
  svg.querySelectorAll(".tag-node.selected").forEach(node => node.classList.remove("selected"));
}

function renderMine() {
  if (!mineLayout) return;
  const svg = $("mineSvg");
  const project = projector(mineLayout);
  svg.replaceChildren();
  applyMapView();
  const grid = svgElement("g", {opacity: ".16"});
  for (let y = 80; y < 540; y += 70) grid.appendChild(svgElement("line", {x1: 20, y1: y, x2: 980, y2: y, stroke: "#91a098", "stroke-width": "1"}));
  svg.appendChild(grid);
  const tunnels = svgElement("g");
  for (const segment of mineLayout.segmentos || []) {
    const a = project(segment.a), b = project(segment.b);
    const layer = String(segment.capa || "general").toLowerCase().replace(/[^a-z0-9]+/g, "-");
    tunnels.appendChild(svgElement("line", {x1: a[0], y1: a[1], x2: b[0], y2: b[1], class: `tunnel layer-${layer}`}));
    tunnels.appendChild(svgElement("line", {x1: a[0], y1: a[1], x2: b[0], y2: b[1], class: `tunnel-core layer-${layer}`}));
  }
  svg.appendChild(tunnels);
  for (const plaza of mineLayout.plazas || []) {
    const point = project([plaza.x, plaza.y, plaza.z || 0]);
    drawText(svg, point[0], point[1] + 4, plaza.id, "parking-slot-label");
  }
  for (const reader of mineLayout.readers || []) {
    const point = project([reader.x, reader.y, reader.z]);
    const group = svgElement("g", {class: `reader-node ${reader.disponible ? "active" : "pending"}`});
    group.appendChild(svgElement("circle", {cx: point[0], cy: point[1], r: 16}));
    drawText(group, point[0], point[1] + 4, "R");
    drawText(group, point[0], point[1] + 34, `${reader.id} · ${reader.nombre}`);
    svg.appendChild(group);
  }
  const tags = state.beacons
    .filter(item => Array.isArray(item.coordenadas) && item.estado !== "sin_senal")
    .sort((a, b) => String(a.id).localeCompare(String(b.id)));
  const offsets = new Map();
  const positions = [[46, -42], [52, 54], [-52, -45], [-56, 55], [88, 0], [-88, 0]];
  let selectedGroup = null;
  for (const tag of tags) {
    const count = offsets.get(tag.reader_id) || 0;
    offsets.set(tag.reader_id, count + 1);
    const base = project(tag.coordenadas);
    const offset = positions[count % positions.length];
    const ring = Math.floor(count / positions.length) * 30;
    const parking = parkingPlacement(tag, count, project);
    const x = parking?.x ?? base[0] + offset[0] + Math.sign(offset[0]) * ring;
    const y = parking?.y ?? base[1] + offset[1] + Math.sign(offset[1] || 1) * ring;
    const operationalColors = {estacionado: "#18a06f", ingresando: "#3182bd", saliendo: "#f2b705"};
    const color = operationalColors[parking?.clase] || stateColors[tag.estado] || stateColors.sin_senal;
    const pendingMessage = window.pendingTagAlerts?.get(tag.id) || null;
    const pendingText = pendingMessage ? `, mensaje pendiente: ${pendingMessage.titulo}` : "";
    const group = svgElement("g", {class: `tag-node ${tag.estado} ${parking?.clase || ""}${pendingMessage ? " has-pending-message" : ""}${selectedTagId === tag.id ? " selected" : ""}`, tabindex: "0", role: "button", "data-tag-id": tag.id, "aria-label": `${tag.id}, ${tag.nombre}, ${parking?.estado || labels[tag.estado] || tag.estado}${pendingText}`});
    const tooltip = svgElement("title");
    tooltip.textContent = `${tag.id} · ${tag.nombre}\n${tag.tipo} · ${tag.persona}\nEstado operativo: ${parking?.estado || labels[tag.estado] || tag.estado}\nDistancia aproximada: ${tag.distancia == null ? "sin datos" : `${tag.distancia} m`}\nÚltimo reader: ${tag.reader_nombre || "sin reader"}`;
    group.appendChild(tooltip);
    group.appendChild(svgElement("line", {x1: base[0], y1: base[1], x2: x, y2: y, stroke: color, "stroke-width": "2", "stroke-dasharray": "4 3"}));
    group.appendChild(svgElement("circle", {cx: x, cy: y, r: 19, class: "tag-marker", style: `fill:${color}`}));
    drawText(group, x, y + 4, tagNumber(tag), "tag-marker-number");
    group.appendChild(svgElement("circle", {cx: x + 15, cy: y - 14, r: 9, class: "tag-kind-marker"}));
    drawText(group, x + 15, y - 11, tag.categoria === "maquinaria" ? "M" : "C", "tag-kind-letter");
    if (pendingMessage) {
      group.appendChild(svgElement("circle", {cx: x - 15, cy: y - 14, r: 7, class: "tag-message-alert"}));
      drawText(group, x - 15, y - 11.5, "!", "tag-message-alert-symbol");
    }
    drawTagLabel(group, x, y, tag, parking ? tag.reader_id !== "RX-01" : offset[1] > 0, pendingMessage, parking?.estado || "");
    group.addEventListener("click", event => { event.stopPropagation(); selectMapTag(svg, group, tag.id); });
    group.addEventListener("keydown", event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); selectMapTag(svg, group, tag.id); } });
    svg.appendChild(group);
    if (selectedTagId === tag.id) selectedGroup = group;
  }
  svg.classList.toggle("has-tag-selection", Boolean(selectedGroup));
  if (selectedGroup) svg.appendChild(selectedGroup);
  $("layoutStatus").textContent = `${mineLayout.nombre} · ${tags.length} vehículo${tags.length === 1 ? "" : "s"} detectado${tags.length === 1 ? "" : "s"}`;
  $("readerList").innerHTML = (mineLayout.readers || []).map(reader => `<article class="reader-card ${reader.disponible ? "active" : "pending"}"><strong><i></i>${esc(reader.id)} · ${esc(reader.nombre)}</strong><span>${esc(reader.sector)} · ${esc(reader.transporte)}</span><span>${reader.disponible ? "Disponible ahora" : "Pendiente de instalación"}</span></article>`).join("");
}

window.renderMineTracking = renderMine;
async function loadLayout() {
  const response = await fetch("/api/layout", {cache: "no-store"});
  if (!response.ok) throw new Error(await response.text());
  mineLayout = await response.json();
  renderMine();
}

$("dxfFile").addEventListener("change", async event => {
  const file = event.target.files[0];
  if (!file) return;
  $("layoutStatus").textContent = "Importando layout…";
  const form = new FormData();
  form.append("archivo", file);
  try {
    const response = await fetch("/api/layout/importar", {method: "POST", body: form});
    if (!response.ok) throw new Error(await response.text());
    mineLayout = await response.json();
    renderMine();
  } catch (error) {
    $("layoutStatus").textContent = `No se pudo importar: ${error.message}`;
  } finally {
    event.target.value = "";
  }
});
$("mapZoomIn").addEventListener("click", () => zoomMap(.78));
$("mapZoomOut").addEventListener("click", () => zoomMap(1.28));
$("resetView").addEventListener("click", () => { mapView = {...initialMapView}; applyMapView(); });

$("mineSvg").addEventListener("wheel", event => {
  event.preventDefault();
  zoomMap(event.deltaY < 0 ? .86 : 1.16, event.clientX, event.clientY);
}, {passive: false});

$("mineSvg").addEventListener("pointerdown", event => {
  const messageLink = event.target.closest(".tag-message-link");
  if (messageLink) {
    event.preventDefault();
    event.stopPropagation();
    if (typeof window.focusSupervisorMessage === "function") {
      window.focusSupervisorMessage(messageLink.dataset.messageId);
    }
    return;
  }
  const tagGroup = event.target.closest(".tag-node");
  if (tagGroup) selectMapTag($("mineSvg"), tagGroup, tagGroup.dataset.tagId);
  else clearMapTagSelection();
  mapPointers.set(event.pointerId, {x: event.clientX, y: event.clientY});
  $("mineSvg").setPointerCapture(event.pointerId);
  $("mineSvg").classList.add("dragging");
  if (mapPointers.size === 1) {
    mapDrag = {pointerId: event.pointerId, x: event.clientX, y: event.clientY, viewX: mapView.x, viewY: mapView.y};
  } else if (mapPointers.size === 2) {
    const [a, b] = [...mapPointers.values()];
    pinchState = {distance: Math.hypot(b.x - a.x, b.y - a.y), x: (a.x + b.x) / 2, y: (a.y + b.y) / 2};
    mapDrag = null;
  }
});

$("mineSvg").addEventListener("pointermove", event => {
  if (!mapPointers.has(event.pointerId)) return;
  mapPointers.set(event.pointerId, {x: event.clientX, y: event.clientY});
  if (mapPointers.size >= 2) {
    const [a, b] = [...mapPointers.values()];
    const distance = Math.max(1, Math.hypot(b.x - a.x, b.y - a.y));
    const midpointX = (a.x + b.x) / 2;
    const midpointY = (a.y + b.y) / 2;
    if (pinchState) {
      const rect = $("mineSvg").getBoundingClientRect();
      zoomMap(pinchState.distance / distance, midpointX, midpointY);
      mapView.x -= (midpointX - pinchState.x) * mapView.width / rect.width;
      mapView.y -= (midpointY - pinchState.y) * mapView.height / rect.height;
      constrainMapView();
      applyMapView();
    }
    pinchState = {distance, x: midpointX, y: midpointY};
    return;
  }
  if (!mapDrag || mapDrag.pointerId !== event.pointerId) return;
  const rect = $("mineSvg").getBoundingClientRect();
  mapView.x = mapDrag.viewX - (event.clientX - mapDrag.x) * mapView.width / rect.width;
  mapView.y = mapDrag.viewY - (event.clientY - mapDrag.y) * mapView.height / rect.height;
  constrainMapView();
  applyMapView();
});

function stopMapDrag(event) {
  mapPointers.delete(event.pointerId);
  pinchState = null;
  if (mapPointers.size === 1) {
    const [pointerId, point] = [...mapPointers.entries()][0];
    mapDrag = {pointerId, x: point.x, y: point.y, viewX: mapView.x, viewY: mapView.y};
  } else {
    mapDrag = null;
    $("mineSvg").classList.remove("dragging");
  }
}

$("mineSvg").addEventListener("pointerup", stopMapDrag);
$("mineSvg").addEventListener("pointercancel", stopMapDrag);
loadLayout().catch(error => $("layoutStatus").textContent = `Error de layout: ${error.message}`);
setInterval(() => loadLayout().catch(error => console.error("No fue posible actualizar el layout", error)), 3000);
