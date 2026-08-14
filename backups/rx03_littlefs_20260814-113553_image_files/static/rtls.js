let mineLayout = null;
const svgNS = "http://www.w3.org/2000/svg";
const initialMapView = {x: 0, y: 0, width: 1000, height: 560};
let mapView = {...initialMapView};
let mapDrag = null;
const mapPointers = new Map();
let pinchState = null;
let selectedTagId = null;
let mapYaw = -35 * Math.PI / 180;
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

function centerMapView(resetZoom = false) {
  if (resetZoom) mapView = {...initialMapView};
  else {
    mapView.x = initialMapView.x + (initialMapView.width - mapView.width) / 2;
    mapView.y = initialMapView.y + (initialMapView.height - mapView.height) / 2;
  }
  constrainMapView();
  applyMapView();
}

function rotateMap(direction) {
  mapYaw += direction * Math.PI / 8;
  if (Math.abs(mapYaw) > Math.PI * 2) mapYaw %= Math.PI * 2;
  centerMapView(false);
  renderMine();
}

function expandedLayoutSegments(layout) {
  const segments = [...(layout.segmentos || [])];
  for (const ramp of layout.rampas_helicoidales || []) {
    const centerX = Number(ramp.centro?.[0]) || 0;
    const centerY = Number(ramp.centro?.[1]) || 0;
    const radius = Math.max(1, Number(ramp.radio_m) || 1);
    const startAngle = (Number(ramp.angulo_inicial_deg) || 0) * Math.PI / 180;
    const turns = Number(ramp.vueltas) || 1;
    const startZ = Number(ramp.z_inicio) || 0;
    const endZ = Number(ramp.z_fin) || 0;
    const steps = Math.max(16, Math.min(120, Number(ramp.resolucion) || 64));
    const pointAt = index => {
      const progress = index / steps;
      const angle = startAngle + turns * Math.PI * 2 * progress;
      return [
        centerX + Math.cos(angle) * radius,
        centerY + Math.sin(angle) * radius,
        startZ + (endZ - startZ) * progress
      ];
    };
    for (let index = 0; index < steps; index += 1) {
      segments.push({
        a: pointAt(index),
        b: pointAt(index + 1),
        capa: ramp.capa || "RAMPA_CARACOL",
        nivel: ramp.nivel || "CONEXION",
        trazado_id: ramp.id || "RAMPA-CARACOL"
      });
    }
  }
  return segments;
}

function boundsOf(layout) {
  const points = [];
  expandedLayoutSegments(layout).forEach(segment => points.push(segment.a, segment.b));
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
  if (layout.vista === "perfil") return point => {
    const x = (Number(point[0]) - bounds.minX) / rangeX;
    const z = (Number(point[2]) - bounds.minZ) / rangeZ;
    return [60 + x * 880, 500 - z * 420];
  };
  if (layout.vista === "subterranea_3d") {
    const centerX = (bounds.minX + bounds.maxX) / 2;
    const centerY = (bounds.minY + bounds.maxY) / 2;
    const horizontalSpan = Math.max(rangeX, rangeY * 1.75, 1);
    const cosine = Math.cos(mapYaw);
    const sine = Math.sin(mapYaw);
    return point => {
      const x = (Number(point[0]) - centerX) / horizontalSpan;
      const y = (Number(point[1]) - centerY) / horizontalSpan;
      const depth = (bounds.maxZ - Number(point[2])) / rangeZ;
      const rotatedX = x * cosine - y * sine;
      const rotatedY = x * sine + y * cosine;
      return [500 + rotatedX * 790, 72 + depth * 405 + rotatedY * 205];
    };
  }
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

function readerDepth(readerId) {
  const zoneDepth = Number(mineLayout?.zonas_operativas?.[readerId]?.profundidad_m);
  if (Number.isFinite(zoneDepth) && zoneDepth > 0) return zoneDepth;
  const reader = (mineLayout?.readers || []).find(item => item.id === readerId);
  const z = Number(reader?.z);
  return Number.isFinite(z) && z !== 0 ? Math.abs(z) : null;
}

function depthText(readerId) {
  const depth = readerDepth(readerId);
  return depth == null ? "Profundidad sin confirmar" : `Cota -${depth} m · ${depth} m bajo referencia`;
}

function projectedPoints(points, project) {
  return points.map(point => project(point).map(value => value.toFixed(1)).join(",")).join(" ");
}

function drawUndergroundVolume(svg, project) {
  if (mineLayout?.vista !== "subterranea_3d") return;

  const planes = svgElement("g", {class: "level-planes", "aria-hidden": "true"});
  [...(mineLayout.planos_nivel || [])].reverse().forEach(plane => {
    const level = String(plane.id || "nivel").toLowerCase();
    planes.appendChild(svgElement("polygon", {
      points: projectedPoints(plane.puntos, project),
      class: `level-plane plane-${level}`
    }));
    const outline = [...plane.puntos, plane.puntos[0]];
    planes.appendChild(svgElement("polyline", {
      points: projectedPoints(outline, project),
      class: `level-plane-outline plane-${level}`
    }));
  });
  svg.appendChild(planes);

  const blocks = svgElement("g", {class: "macroblocks", "aria-hidden": "true"});
  [...(mineLayout.macrobloques || [])]
    .sort((a, b) => Number(a.z_inferior) - Number(b.z_inferior))
    .forEach(block => {
      const halfX = Number(block.ancho) / 2;
      const halfY = Number(block.fondo) / 2;
      const top = Number(block.z_superior);
      const bottom = Number(block.z_inferior);
      const x = Number(block.x), y = Number(block.y);
      const topFace = [[x-halfX,y-halfY,top],[x+halfX,y-halfY,top],[x+halfX,y+halfY,top],[x-halfX,y+halfY,top]];
      const frontFace = [[x-halfX,y-halfY,top],[x+halfX,y-halfY,top],[x+halfX,y-halfY,bottom],[x-halfX,y-halfY,bottom]];
      const sideFace = [[x+halfX,y-halfY,top],[x+halfX,y+halfY,top],[x+halfX,y+halfY,bottom],[x+halfX,y-halfY,bottom]];
      const level = String(block.nivel || "nivel").toLowerCase();
      blocks.appendChild(svgElement("polygon", {points: projectedPoints(frontFace, project), class: `macroblock-face front ${level}`}));
      blocks.appendChild(svgElement("polygon", {points: projectedPoints(sideFace, project), class: `macroblock-face side ${level}`}));
      blocks.appendChild(svgElement("polygon", {points: projectedPoints(topFace, project), class: `macroblock-face top ${level}`}));
      const labelPoint = project([x, y, top]);
      drawText(blocks, labelPoint[0], labelPoint[1] - 5, block.id, "macroblock-label");
    });
  svg.appendChild(blocks);
}

function clampMapValue(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

/*
 * Ordena todos los TAG de un reader en una grilla estable. Antes se usaban
 * solo tres puntos de zona y, desde el cuarto TAG, las posiciones se repetian.
 * La grilla crece por filas, centra la ultima fila y reduce levemente los
 * marcadores cuando el grupo es grande para conservar legibilidad.
 */
function groupedTagPlacements(tags, project) {
  const grouped = new Map();
  tags.forEach(tag => {
    const readerId = tag.reader_id || "SIN-RX";
    if (!grouped.has(readerId)) grouped.set(readerId, []);
    grouped.get(readerId).push(tag);
  });

  const positions = new Map();
  const clusters = [];
  for (const [readerId, readerTags] of grouped.entries()) {
    const zone = mineLayout?.zonas_operativas?.[readerId];
    const reader = (mineLayout?.readers || []).find(item => item.id === readerId);
    const zonePoints = (zone?.puntos || []).map(point => project(point));
    const fallbackPoints = readerTags.map(tag => project(tag.coordenadas));
    const anchorPoints = zonePoints.length ? zonePoints : fallbackPoints;
    const anchor = anchorPoints.reduce((total, point) => [total[0] + point[0], total[1] + point[1]], [0, 0]);
    anchor[0] /= Math.max(1, anchorPoints.length);
    anchor[1] /= Math.max(1, anchorPoints.length);
    const readerPoint = reader
      ? project([reader.x, reader.y, reader.z])
      : anchor;

    const count = readerTags.length;
    const dense = count > 24;
    const veryDense = count > 80;
    const spacingX = veryDense ? 34 : dense ? 46 : 72;
    const spacingY = veryDense ? 34 : dense ? 48 : 62;
    const preferredColumns = count <= 3 ? count : count <= 8 ? 4 :
        Math.min(veryDense ? 14 : 10, Math.ceil(Math.sqrt(count * 1.35)));
    const columns = Math.max(1, Math.min(count, preferredColumns));
    const rows = Math.ceil(count / columns);
    const contentWidth = Math.max(0, (columns - 1) * spacingX);
    const contentHeight = Math.max(0, (rows - 1) * spacingY);
    // La grilla queda siempre sobre el circulo del RX. El margen inferior evita
    // que los marcadores oculten el lector, incluso cuando solo hay un TAG.
    const clusterWidth = Math.max(132, contentWidth + 54);
    const clusterHeight = contentHeight + 64;
    const centerX = clampMapValue(readerPoint[0], 18 + clusterWidth / 2, 982 - clusterWidth / 2);
    const desiredCenterY = readerPoint[1] - 54 - contentHeight / 2;
    const centerY = clampMapValue(desiredCenterY, 42 + contentHeight / 2, 485 - contentHeight / 2);

    readerTags.forEach((tag, index) => {
      const row = Math.floor(index / columns);
      const rowStart = row * columns;
      const rowCount = Math.min(columns, count - rowStart);
      const column = index - rowStart;
      const x = centerX + (column - (rowCount - 1) / 2) * spacingX;
      const y = centerY + (row - (rows - 1) / 2) * spacingY;
      positions.set(tag.id, {
        x,
        y,
        estado: zone?.estado || tag.estado_turno || "Ubicacion estimada",
        clase: zone?.clase || (readerId === "RX-01" ? "en-turno" : readerId === "RX-02" ? "ingresando" : "saliendo"),
        markerRadius: veryDense ? 11 : dense ? 14 : 17,
        // La ficha se abre hacia arriba para mantener visible el RX inferior.
        // Solo cambia de lado si el TAG queda muy cerca del borde superior.
        placeBelow: y < 95
      });
    });

    clusters.push({
      readerId,
      count,
      x: centerX - clusterWidth / 2,
      y: centerY - contentHeight / 2 - 31,
      width: clusterWidth,
      height: clusterHeight,
      centerX,
      readerX: readerPoint[0],
      readerY: readerPoint[1]
    });
  }
  return {positions, clusters};
}

function drawTagCluster(svg, cluster) {
  const group = svgElement("g", {class: "tag-cluster", "aria-hidden": "true"});
  group.appendChild(svgElement("path", {
    d: `M ${cluster.readerX} ${cluster.readerY - 18} L ${cluster.centerX} ${cluster.y + cluster.height}`,
    class: "tag-cluster-stem"
  }));
  group.appendChild(svgElement("rect", {x: cluster.x, y: cluster.y, width: cluster.width, height: cluster.height, rx: 15, class: "tag-cluster-area"}));
  group.appendChild(svgElement("rect", {x: cluster.centerX - 58, y: cluster.y - 10, width: 116, height: 24, rx: 9, class: "tag-cluster-badge"}));
  const count = svgElement("text", {x: cluster.centerX, y: cluster.y + 7, class: "tag-cluster-title", "text-anchor": "middle"});
  count.textContent = `${cluster.readerId} · ${cluster.count} TAG`;
  group.appendChild(count);
  const firstReader = svg.querySelector(".reader-node");
  if (firstReader) svg.insertBefore(group, firstReader);
  else svg.appendChild(group);
}

function drawTagLabel(group, x, y, tag, placeBelow, pendingMessage = null, operationalState = "") {
  const width = 216;
  const height = 59;
  const top = placeBelow ? y + 25 : y - height - 25;
  group.appendChild(svgElement("rect", {x: x - width / 2, y: top, width, height, rx: 8, class: "tag-label-card"}));
  const title = svgElement("text", {x, y: top + 17, class: "tag-label-title", "text-anchor": "middle"});
  title.textContent = `${tag.id} · ${tag.nombre}`;
  group.appendChild(title);
  const detail = svgElement("text", {x, y: top + 33, class: "tag-label-detail", "text-anchor": "middle"});
  const distance = tag.distancia == null ? "sin distancia" : `${tag.distancia} m`;
  detail.textContent = operationalState || `${labels[tag.estado] || tag.estado} · ${distance}`;
  group.appendChild(detail);
  const depth = svgElement("text", {x, y: top + 49, class: "tag-label-depth", "text-anchor": "middle"});
  depth.textContent = depthText(tag.reader_id);
  group.appendChild(depth);
  if (pendingMessage) {
    const action = svgElement("g", {class: "tag-message-link", role: "button", tabindex: "0", "data-message-id": pendingMessage.id, "aria-label": `Abrir mensaje: ${pendingMessage.titulo}`});
    action.appendChild(svgElement("rect", {x: x - 105, y: top + 62, width: 210, height: 19, rx: 5}));
    const title = String(pendingMessage.titulo || "Mensaje pendiente");
    const compactTitle = title.length > 27 ? `${title.slice(0, 26)}…` : title;
    const actionText = svgElement("text", {x, y: top + 75, "text-anchor": "middle"});
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
  window.dispatchEvent(new CustomEvent("mina:tag-selected", {detail: {tagId}}));
}

function clearMapTagSelection() {
  selectedTagId = null;
  const svg = $("mineSvg");
  svg.classList.remove("has-tag-selection");
  svg.querySelectorAll(".tag-node.selected").forEach(node => node.classList.remove("selected"));
  window.dispatchEvent(new CustomEvent("mina:tag-selected", {detail: {tagId: null}}));
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
  drawUndergroundVolume(svg, project);
  const tunnels = svgElement("g", {class: "mine-tunnels", "aria-label": "Galerias y servicios subterraneos"});
  const axisLayers = new Set(["galeria-produccion", "galeria-retorno", "galeria-servicio", "galeria-perimetral", "galeria-secundaria", "rampa-acceso", "rampa-caracol", "enlace-rampa", "calle-hundimiento"]);
  for (const segment of expandedLayoutSegments(mineLayout)) {
    const a = project(segment.a), b = project(segment.b);
    const layer = String(segment.capa || "general").toLowerCase().replace(/[^a-z0-9]+/g, "-");
    const level = String(segment.nivel || "conexion").toLowerCase().replace(/[^a-z0-9]+/g, "-");
    tunnels.appendChild(svgElement("line", {x1: a[0], y1: a[1], x2: b[0], y2: b[1], class: `tunnel layer-${layer} level-${level}`}));
    tunnels.appendChild(svgElement("line", {x1: a[0], y1: a[1], x2: b[0], y2: b[1], class: `tunnel-core layer-${layer} level-${level}`}));
    if (axisLayers.has(layer)) {
      tunnels.appendChild(svgElement("line", {x1: a[0], y1: a[1], x2: b[0], y2: b[1], class: `tunnel-axis layer-${layer} level-${level}`}));
    }
    if (layer === "galeria-produccion" || layer === "galeria-retorno") {
      tunnels.appendChild(svgElement("circle", {cx: a[0], cy: a[1], r: 2.8, class: `gallery-terminal level-${level}`}));
      tunnels.appendChild(svgElement("circle", {cx: b[0], cy: b[1], r: 2.8, class: `gallery-terminal level-${level}`}));
    }
  }
  svg.appendChild(tunnels);
  for (const label of mineLayout.etiquetas || []) {
    const point = project([label.x, label.y, label.z || 0]);
    drawText(svg, point[0], point[1] + 4, label.id, `office-area-label label-${String(label.nivel || "general").toLowerCase()}`);
  }
  for (const reader of mineLayout.readers || []) {
    const point = project([reader.x, reader.y, reader.z]);
    const group = svgElement("g", {class: `reader-node ${reader.disponible ? "active" : "pending"}`});
    group.appendChild(svgElement("circle", {cx: point[0], cy: point[1], r: 16}));
    drawText(group, point[0], point[1] + 4, "R");
    const compactDepth = readerDepth(reader.id);
    const readerCaption = compactDepth == null ? reader.id : `${reader.id} · Cota -${compactDepth} m`;
    const captionWidth = Math.max(88, readerCaption.length * 5.8 + 18);
    group.appendChild(svgElement("rect", {x: point[0] - captionWidth / 2, y: point[1] + 22, width: captionWidth, height: 20, rx: 7, class: "reader-label-plate"}));
    drawText(group, point[0], point[1] + 36, readerCaption);
    svg.appendChild(group);
  }
  const tags = state.beacons
    .filter(item => Array.isArray(item.coordenadas) && item.estado !== "sin_senal")
    .sort((a, b) => String(a.id).localeCompare(String(b.id)));
  const groupedPlacements = groupedTagPlacements(tags, project);
  groupedPlacements.clusters.forEach(cluster => drawTagCluster(svg, cluster));
  let selectedGroup = null;
  for (const tag of tags) {
    const base = project(tag.coordenadas);
    const placement = groupedPlacements.positions.get(tag.id);
    const x = placement?.x ?? base[0];
    const y = placement?.y ?? base[1];
    const operationalColors = {"en-turno": "#18a06f", ingresando: "#3182bd", saliendo: "#f2b705", "nivel-1": "#9cbf36", "nivel-2": "#d99818", "nivel-3": "#c84235"};
    const color = operationalColors[placement?.clase] || stateColors[tag.estado] || stateColors.sin_senal;
    const pendingMessage = window.pendingTagAlerts?.get(tag.id) || null;
    const tagDepth = depthText(tag.reader_id);
    const pendingText = pendingMessage ? `, mensaje pendiente: ${pendingMessage.titulo}` : "";
    const group = svgElement("g", {class: `tag-node ${tag.estado} ${placement?.clase || ""}${pendingMessage ? " has-pending-message" : ""}${tag.gps_asociado ? " has-gps" : ""}${selectedTagId === tag.id ? " selected" : ""}`, tabindex: "0", role: "button", "data-tag-id": tag.id, "aria-label": `${tag.id}, ${tag.nombre}, ${placement?.estado || labels[tag.estado] || tag.estado}, ${tagDepth}${tag.gps_asociado ? ", con geotracker GPS" : ""}${pendingText}`});
    const tooltip = svgElement("title");
    tooltip.textContent = `${tag.id} · ${tag.nombre}\n${tag.codigo_personal || tag.persona} · ${tag.cargo || tag.tipo}\nSector: ${placement?.estado || tag.estado_turno || labels[tag.estado] || tag.estado}\nProfundidad estimada: ${tagDepth}\nDistancia al RX: ${tag.distancia == null ? "sin datos" : `${tag.distancia} m`}\nÚltimo reader: ${tag.reader_nombre || "sin reader"}`;
    group.appendChild(tooltip);
    group.appendChild(svgElement("line", {x1: base[0], y1: base[1], x2: x, y2: y, class: "tag-reader-link", stroke: color}));
    group.appendChild(svgElement("circle", {cx: x, cy: y, r: placement?.markerRadius || 18, class: "tag-marker", style: `fill:${color}`}));
    drawText(group, x, y + 4, tagNumber(tag), "tag-marker-number");
    group.appendChild(svgElement("circle", {cx: x + 15, cy: y - 14, r: 9, class: "tag-kind-marker"}));
    drawText(group, x + 15, y - 11, tag.categoria === "persona" ? "P" : "T", "tag-kind-letter");
    if (tag.gps_asociado) {
      group.appendChild(svgElement("circle", {cx: x - 15, cy: y + 14, r: 9, class: "tag-gps-marker"}));
      drawText(group, x - 15, y + 17, "G", "tag-gps-letter");
    }
    if (pendingMessage) {
      group.appendChild(svgElement("circle", {cx: x - 15, cy: y - 14, r: 7, class: "tag-message-alert"}));
      drawText(group, x - 15, y - 11.5, "!", "tag-message-alert-symbol");
    }
    drawTagLabel(group, x, y, tag, placement?.placeBelow ?? y < 310, pendingMessage, placement?.estado || "");
    group.addEventListener("click", event => { event.stopPropagation(); selectMapTag(svg, group, tag.id); });
    group.addEventListener("keydown", event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); selectMapTag(svg, group, tag.id); } });
    svg.appendChild(group);
    if (selectedTagId === tag.id) selectedGroup = group;
  }
  svg.classList.toggle("has-tag-selection", Boolean(selectedGroup));
  if (selectedGroup) svg.appendChild(selectedGroup);
  $("layoutStatus").textContent = `${mineLayout.nombre} · ${tags.length} beacon${tags.length === 1 ? "" : "s"} con nivel confirmado`;
  $("readerList").innerHTML = (mineLayout.readers || []).map(reader => `<article class="reader-card ${reader.disponible ? "active" : "pending"}"><strong><i></i>${esc(reader.id)} · ${esc(reader.nombre)}</strong><span>${esc(reader.sector)}</span><span>${esc(depthText(reader.id))} · ${esc(reader.transporte)}</span><span>${reader.disponible ? "Disponible ahora" : "Sin enlace de lectura"}</span></article>`).join("");
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
$("resetView").addEventListener("click", () => centerMapView(true));
$("mapRotateLeft").addEventListener("click", () => rotateMap(-1));
$("mapRotateRight").addEventListener("click", () => rotateMap(1));

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
// El plano cambia solo al importar un DXF; consultar 10 KB cada tres segundos
// competia innecesariamente con el estado en vivo de los beacons.
setInterval(() => loadLayout().catch(error => console.error("No fue posible actualizar el layout", error)), 15000);
