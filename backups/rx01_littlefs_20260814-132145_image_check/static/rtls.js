let mineLayout = null;
const svgNS = "http://www.w3.org/2000/svg";
const initialMapView = {x: 0, y: 0, width: 1000, height: 560};
const minimumMapViewWidth = 82;
let mapView = {...initialMapView};
let mapDrag = null;
const mapPointers = new Map();
let pinchState = null;
let selectedTagId = null;
let mapYaw = -35 * Math.PI / 180;
let selectedMineLevel = "ALL";
let selectedMineArea = "";
let selectedMineAreaLabel = "";
let navigationBoundsCache = null;
const mineLevelDepths = {N1: 250, N2: 450, N3: 650};
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
  updateMapLod();
}

function updateMapLod() {
  const svg = $("mineSvg");
  if (!svg) return;
  const focused = selectedMineLevel !== "ALL";
  const detail = focused || mapView.width <= 650;
  const close = mapView.width <= 390;
  const ultra = mapView.width <= 155;
  svg.classList.toggle("map-level-focused", focused);
  svg.classList.toggle("map-lod-overview", !detail);
  svg.classList.toggle("map-lod-detail", detail && !close);
  svg.classList.toggle("map-lod-close", close);
  svg.classList.toggle("map-lod-ultra", ultra);
  const hint = $("mineLevelHint");
  if (!hint) return;
  const zoom = (initialMapView.width / mapView.width).toFixed(1).replace(".0", "");
  hint.textContent = selectedMineAreaLabel
    ? `${selectedMineAreaLabel} · zoom ${zoom}× · toca un RX o TAG para identificarlo.`
    : focused
      ? `${selectedMineLevel.replace("N", "Nivel ")} seleccionado · zoom ${zoom}× · toca un punto para identificarlo.`
      : `Vista general · zoom ${zoom}× · selecciona un nivel o amplía para revelar RX y TAG.`;
  const areaHint = $("mineAreaHint");
  if (areaHint && selectedMineAreaLabel) {
    areaHint.textContent = `${selectedMineAreaLabel} centrada automáticamente. Usa + o la pinza para ampliar hasta 12×.`;
  }
}

function constrainMapView() {
  // El desplazamiento queda limitado al nivel visible. Así el usuario puede
  // recorrer sus galerías sin perder el plano en zonas vacías de otros niveles.
  const bounds = navigationBoundsForSelectedLevel();
  const marginX = Math.min(65, mapView.width * .16);
  const marginY = Math.min(48, mapView.height * .16);
  const minX = bounds.minX - marginX;
  const maxX = bounds.maxX - mapView.width + marginX;
  const minY = bounds.minY - marginY;
  const maxY = bounds.maxY - mapView.height + marginY;
  mapView.x = minX > maxX
    ? (bounds.minX + bounds.maxX - mapView.width) / 2
    : Math.min(maxX, Math.max(minX, mapView.x));
  mapView.y = minY > maxY
    ? (bounds.minY + bounds.maxY - mapView.height) / 2
    : Math.min(maxY, Math.max(minY, mapView.y));
}

function zoomMap(factor, clientX = null, clientY = null) {
  const svg = $("mineSvg");
  const rect = svg.getBoundingClientRect();
  const ratioX = clientX == null ? .5 : Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
  const ratioY = clientY == null ? .5 : Math.max(0, Math.min(1, (clientY - rect.top) / rect.height));
  const nextWidth = Math.max(minimumMapViewWidth, Math.min(maximumMapViewWidth(), mapView.width * factor));
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
  navigationBoundsCache = null;
  renderMine();
  if (selectedMineArea) fitSelectedMineArea();
  else if (selectedMineLevel === "ALL") fitMineOverview();
  else fitSelectedMineLevel();
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

function readerLevel(readerId, reader = null) {
  const configured = mineLayout?.zonas_operativas?.[readerId]?.nivel;
  if (configured) return String(configured).toUpperCase();
  const depth = Math.abs(Number(reader?.z) || 0);
  if (depth >= 550) return "N3";
  if (depth >= 350) return "N2";
  return "N1";
}

function segmentVisibleAtLevel(segment, level = selectedMineLevel) {
  if (level === "ALL") return true;
  const segmentLevel = String(segment.nivel || "CONEXION").toUpperCase();
  if (segmentLevel === level) return true;
  if (segmentLevel !== "CONEXION") return false;
  const layer = String(segment.capa || "").toUpperCase();
  if (!['RAMPA_CARACOL', 'ENLACE_RAMPA', 'RAMPA_ACCESO'].includes(layer)) return false;
  const middleDepth = Math.abs((Number(segment.a?.[2]) + Number(segment.b?.[2])) / 2);
  return Math.abs(middleDepth - mineLevelDepths[level]) <= 125;
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

function projectedNavigationPoints(level = selectedMineLevel) {
  if (!mineLayout) return [];
  const project = projector(mineLayout);
  const points = [];
  expandedLayoutSegments(mineLayout)
    .filter(segment => level === "ALL" || segmentVisibleAtLevel(segment, level))
    .forEach(segment => points.push(project(segment.a), project(segment.b)));
  (mineLayout.planos_nivel || [])
    .filter(plane => level === "ALL" || String(plane.id).toUpperCase() === level)
    .forEach(plane => (plane.puntos || []).forEach(point => points.push(project(point))));
  (mineLayout.readers || [])
    .filter(reader => level === "ALL" || readerLevel(reader.id, reader) === level)
    .forEach(reader => points.push(project([reader.x, reader.y, reader.z])));
  (mineLayout.etiquetas || [])
    .filter(label => level === "ALL" || String(label.nivel || "").toUpperCase() === level)
    .forEach(label => points.push(project([label.x, label.y, label.z || 0])));
  return points.filter(point => point.every(Number.isFinite));
}

function navigationBoundsForSelectedLevel() {
  if (navigationBoundsCache) return navigationBoundsCache;
  const points = projectedNavigationPoints(selectedMineLevel);
  if (!points.length) {
    navigationBoundsCache = {
      minX: initialMapView.x,
      maxX: initialMapView.x + initialMapView.width,
      minY: initialMapView.y,
      maxY: initialMapView.y + initialMapView.height
    };
    return navigationBoundsCache;
  }
  navigationBoundsCache = {
    minX: Math.min(...points.map(point => point[0])),
    maxX: Math.max(...points.map(point => point[0])),
    minY: Math.min(...points.map(point => point[1])),
    maxY: Math.max(...points.map(point => point[1]))
  };
  return navigationBoundsCache;
}

function maximumMapViewWidth() {
  if (!mineLayout || selectedMineLevel === "ALL") return initialMapView.width;
  const bounds = navigationBoundsForSelectedLevel();
  const contentWidth = bounds.maxX - bounds.minX;
  const contentHeightAsWidth = (bounds.maxY - bounds.minY) * initialMapView.width / initialMapView.height;
  return Math.max(360, Math.min(760, Math.max(contentWidth, contentHeightAsWidth) * 1.38));
}

function fitMineOverview() {
  const points = projectedNavigationPoints("ALL");
  if (!points.length) {
    centerMapView(true);
    return;
  }
  const xs = points.map(point => point[0]);
  const ys = points.map(point => point[1]);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const widthForX = (maxX - minX) * 1.08;
  const widthForY = (maxY - minY) * initialMapView.width / initialMapView.height * 1.06;
  mapView.width = Math.max(760, Math.min(initialMapView.width, Math.max(widthForX, widthForY)));
  mapView.height = mapView.width * initialMapView.height / initialMapView.width;
  mapView.x = (minX + maxX - mapView.width) / 2;
  mapView.y = (minY + maxY - mapView.height) / 2;
  constrainMapView();
  applyMapView();
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
  [...(mineLayout.planos_nivel || [])].reverse().filter(plane => selectedMineLevel === "ALL" || String(plane.id).toUpperCase() === selectedMineLevel).forEach(plane => {
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
    .filter(block => selectedMineLevel === "ALL" || String(block.nivel).toUpperCase() === selectedMineLevel)
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
 * Distribuye los TAG dentro del poligono operativo de su RX. La posicion es
 * estable y escalable: en vista general manda el agregado y, al elegir nivel
 * o acercar, aparecen los puntos individuales dentro de la zona real del plano.
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
    const worldArea = Array.isArray(zone?.area) && zone.area.length >= 3 ? zone.area : null;
    const count = readerTags.length;
    const dense = count > 24;
    const veryDense = count > 80;
    const columns = Math.max(1, Math.min(count, veryDense ? 14 : dense ? 10 : Math.ceil(Math.sqrt(count * 1.5))));
    const rows = Math.ceil(count / columns);
    const worldPoints = [];

    if (worldArea) {
      const xs = worldArea.map(point => Number(point[0]));
      const ys = worldArea.map(point => Number(point[1]));
      const minX = Math.min(...xs), maxX = Math.max(...xs);
      const minY = Math.min(...ys), maxY = Math.max(...ys);
      const z = Number(worldArea[0][2]) || Number(reader?.z) || 0;
      readerTags.forEach((tag, index) => {
        const row = Math.floor(index / columns);
        const rowCount = Math.min(columns, count - row * columns);
        const column = index - row * columns;
        const xRatio = (column + 1) / (rowCount + 1);
        const yRatio = (row + 1) / (rows + 1);
        worldPoints.push([minX + (maxX - minX) * xRatio, minY + (maxY - minY) * yRatio, z]);
      });
    } else {
      const fallback = zone?.puntos?.length ? zone.puntos : readerTags.map(tag => tag.coordenadas);
      readerTags.forEach((tag, index) => worldPoints.push(fallback[index % Math.max(1, fallback.length)] || tag.coordenadas));
    }

    const projectedZone = (worldArea || worldPoints).map(point => project(point));
    const projectedTags = worldPoints.map(point => project(point));
    const zoneXs = projectedZone.map(point => point[0]);
    const zoneYs = projectedZone.map(point => point[1]);
    const minScreenX = Math.min(...zoneXs), maxScreenX = Math.max(...zoneXs);
    const minScreenY = Math.min(...zoneYs), maxScreenY = Math.max(...zoneYs);
    const clusterWidth = Math.max(58, maxScreenX - minScreenX + 18);
    const clusterHeight = Math.max(42, maxScreenY - minScreenY + 18);
    const centerX = (minScreenX + maxScreenX) / 2;
    const centerY = (minScreenY + maxScreenY) / 2;
    const readerPoint = reader ? project([reader.x, reader.y, reader.z]) : [centerX, centerY];

    readerTags.forEach((tag, index) => {
      const [x, y] = projectedTags[index];
      positions.set(tag.id, {
        x,
        y,
        estado: zone?.estado || tag.estado_turno || "Ubicacion estimada",
        clase: zone?.clase || (readerId === "RX-01" ? "en-turno" : readerId === "RX-02" ? "ingresando" : "saliendo"),
        markerRadius: veryDense ? 3.2 : dense ? 4 : selectedMineLevel === "ALL" ? 5 : 6,
        overviewAggregate: count > 12,
        placeBelow: y < 115
      });
    });

    clusters.push({
      readerId,
      count,
      x: centerX - clusterWidth / 2,
      y: centerY - clusterHeight / 2,
      width: clusterWidth,
      height: clusterHeight,
      centerX,
      centerY,
      readerX: readerPoint[0],
      readerY: readerPoint[1]
    });
  }
  return {positions, clusters};
}

function drawTagCluster(svg, cluster) {
  const group = svgElement("g", {class: `tag-cluster${cluster.count > 12 ? " aggregate" : ""}`, "aria-hidden": "true"});
  group.appendChild(svgElement("path", {
    d: `M ${cluster.readerX} ${cluster.readerY} L ${cluster.centerX} ${cluster.centerY}`,
    class: "tag-cluster-stem"
  }));
  group.appendChild(svgElement("rect", {x: cluster.x, y: cluster.y, width: cluster.width, height: cluster.height, rx: 6, class: "tag-cluster-area"}));
  group.appendChild(svgElement("rect", {x: cluster.centerX - 38, y: cluster.y - 13, width: 76, height: 17, rx: 6, class: "tag-cluster-badge"}));
  const count = svgElement("text", {x: cluster.centerX, y: cluster.y - 1, class: "tag-cluster-title", "text-anchor": "middle"});
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
  const axisLayers = new Set(["galeria-produccion", "galeria-retorno", "galeria-servicio", "galeria-perimetral", "galeria-secundaria", "galeria-ventilacion", "via-evacuacion", "drenaje", "rampa-acceso", "rampa-caracol", "enlace-rampa", "calle-hundimiento"]);
  for (const segment of expandedLayoutSegments(mineLayout).filter(item => segmentVisibleAtLevel(item))) {
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
    if (selectedMineLevel !== "ALL" && String(label.nivel || "").toUpperCase() !== selectedMineLevel) continue;
    const point = project([label.x, label.y, label.z || 0]);
    drawText(svg, point[0], point[1] + 4, label.id, `office-area-label label-${String(label.nivel || "general").toLowerCase()}`);
  }
  for (const reader of mineLayout.readers || []) {
    if (selectedMineLevel !== "ALL" && readerLevel(reader.id, reader) !== selectedMineLevel) continue;
    const point = project([reader.x, reader.y, reader.z]);
    const group = svgElement("g", {class: `reader-node ${reader.disponible ? "active" : "pending"}`});
    group.appendChild(svgElement("circle", {cx: point[0], cy: point[1], r: 15, class: "reader-hit-area"}));
    group.appendChild(svgElement("circle", {cx: point[0], cy: point[1], r: 8, class: "reader-marker"}));
    drawText(group, point[0], point[1] + 2.5, "R");
    const compactDepth = readerDepth(reader.id);
    const readerCaption = compactDepth == null ? reader.id : `${reader.id} · Cota -${compactDepth} m`;
    const captionWidth = Math.max(88, readerCaption.length * 5.8 + 18);
    group.appendChild(svgElement("rect", {x: point[0] - captionWidth / 2, y: point[1] + 13, width: captionWidth, height: 18, rx: 6, class: "reader-label-plate"}));
    drawText(group, point[0], point[1] + 26, readerCaption);
    svg.appendChild(group);
  }
  const tags = state.beacons
    .filter(item => Array.isArray(item.coordenadas) && item.estado !== "sin_senal")
    .filter(item => selectedMineLevel === "ALL" || readerLevel(item.reader_id) === selectedMineLevel)
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
    const group = svgElement("g", {class: `tag-node ${tag.estado} ${placement?.clase || ""}${placement?.overviewAggregate ? " overview-aggregate" : ""}${pendingMessage ? " has-pending-message" : ""}${tag.gps_asociado ? " has-gps" : ""}${selectedTagId === tag.id ? " selected" : ""}`, tabindex: "0", role: "button", "data-tag-id": tag.id, "aria-label": `${tag.id}, ${tag.nombre}, ${placement?.estado || labels[tag.estado] || tag.estado}, ${tagDepth}${tag.gps_asociado ? ", con geotracker GPS" : ""}${pendingText}`});
    const tooltip = svgElement("title");
    tooltip.textContent = `${tag.id} · ${tag.nombre}\n${tag.codigo_personal || tag.persona} · ${tag.cargo || tag.tipo}\nSector: ${placement?.estado || tag.estado_turno || labels[tag.estado] || tag.estado}\nProfundidad estimada: ${tagDepth}\nDistancia al RX: ${tag.distancia == null ? "sin datos" : `${tag.distancia} m`}\nÚltimo reader: ${tag.reader_nombre || "sin reader"}`;
    group.appendChild(tooltip);
    group.appendChild(svgElement("line", {x1: base[0], y1: base[1], x2: x, y2: y, class: "tag-reader-link", stroke: color}));
    const markerRadius = placement?.markerRadius || 5;
    const badgeOffset = markerRadius + 3.5;
    const auxiliaryRadius = 3.2;
    group.appendChild(svgElement("circle", {cx: x, cy: y, r: 15, class: "tag-hit-area"}));
    group.appendChild(svgElement("circle", {cx: x, cy: y, r: markerRadius, class: "tag-marker", style: `fill:${color}`}));
    drawText(group, x, y + 2.2, tagNumber(tag), "tag-marker-number");
    group.appendChild(svgElement("circle", {cx: x + badgeOffset, cy: y - badgeOffset, r: auxiliaryRadius, class: "tag-kind-marker"}));
    drawText(group, x + badgeOffset, y - badgeOffset + 1.5, tag.categoria === "persona" ? "P" : "T", "tag-kind-letter");
    if (tag.gps_asociado) {
      group.appendChild(svgElement("circle", {cx: x - badgeOffset, cy: y + badgeOffset, r: auxiliaryRadius, class: "tag-gps-marker"}));
      drawText(group, x - badgeOffset, y + badgeOffset + 1.5, "G", "tag-gps-letter");
    }
    if (pendingMessage) {
      group.appendChild(svgElement("circle", {cx: x - badgeOffset, cy: y - badgeOffset, r: auxiliaryRadius, class: "tag-message-alert"}));
      drawText(group, x - badgeOffset, y - badgeOffset + 1.5, "!", "tag-message-alert-symbol");
    }
    drawTagLabel(group, x, y, tag, placement?.placeBelow ?? y < 310, pendingMessage, placement?.estado || "");
    group.addEventListener("click", event => { event.stopPropagation(); selectMapTag(svg, group, tag.id); });
    group.addEventListener("keydown", event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); selectMapTag(svg, group, tag.id); } });
    svg.appendChild(group);
    if (selectedTagId === tag.id) selectedGroup = group;
  }
  svg.classList.toggle("has-tag-selection", Boolean(selectedGroup));
  if (selectedGroup) svg.appendChild(selectedGroup);
  const levelText = selectedMineLevel === "ALL" ? "vista general" : selectedMineLevel.replace("N", "nivel ");
  $("layoutStatus").textContent = `${mineLayout.nombre} · ${levelText} · ${tags.length} beacon${tags.length === 1 ? "" : "s"}`;
  $("readerList").innerHTML = (mineLayout.readers || []).filter(reader => selectedMineLevel === "ALL" || readerLevel(reader.id, reader) === selectedMineLevel).map(reader => `<article class="reader-card ${reader.disponible ? "active" : "pending"}"><strong><i></i>${esc(reader.id)} · ${esc(reader.nombre)}</strong><span>${esc(reader.sector)}</span><span>${esc(depthText(reader.id))} · ${esc(reader.transporte)}</span><span>${reader.disponible ? "Disponible ahora" : "Sin enlace de lectura"}</span></article>`).join("");
}

window.renderMineTracking = renderMine;

function updateMineLevelButtons() {
  document.querySelectorAll("[data-mine-level]").forEach(button => {
    const active = button.dataset.mineLevel === selectedMineLevel;
    button.classList.toggle("active", active);
    button.setAttribute("aria-pressed", String(active));
  });
}

function appendMineAreaGroup(select, title, areas) {
  if (!areas.length) return;
  const group = document.createElement("optgroup");
  group.label = title;
  areas.forEach(area => {
    const option = document.createElement("option");
    option.value = area.value;
    option.textContent = area.label;
    group.appendChild(option);
  });
  select.appendChild(group);
}

function populateMineAreaSelector() {
  const select = $("mineAreaSelect");
  if (!select || !mineLayout) return;
  const previous = selectedMineArea;
  select.replaceChildren();
  const defaultOption = document.createElement("option");
  defaultOption.value = "";
  defaultOption.textContent = "Nivel completo / vista general";
  select.appendChild(defaultOption);

  const levelNames = {N1: "Nivel 1 · −250 m", N2: "Nivel 2 · −450 m", N3: "Nivel 3 · −650 m", CONEXION: "Conexiones principales"};
  const readerAreas = Object.entries(mineLayout.zonas_operativas || {}).map(([readerId, zone]) => ({
    value: `zone:${readerId}`,
    label: `${levelNames[String(zone.nivel).toUpperCase()] || zone.nivel} · ${readerId}`
  }));
  appendMineAreaGroup(select, "Zonas de lectores y beacons", readerAreas);

  const labelsByLevel = {N1: [], N2: [], N3: [], CONEXION: []};
  (mineLayout.etiquetas || []).forEach((label, index) => {
    const level = String(label.nivel || "CONEXION").toUpperCase();
    if (!labelsByLevel[level] || /^(COTA|NIVEL)\b/i.test(String(label.id || ""))) return;
    labelsByLevel[level].push({value: `label:${index}`, label: String(label.id || `Zona ${index + 1}`)});
  });
  ["N1", "N2", "N3", "CONEXION"].forEach(level => appendMineAreaGroup(select, levelNames[level], labelsByLevel[level]));
  select.disabled = false;
  select.value = previous && [...select.options].some(option => option.value === previous) ? previous : "";
}

function resolveMineArea(value = selectedMineArea) {
  if (!mineLayout || !value) return null;
  if (value.startsWith("zone:")) {
    const readerId = value.slice(5);
    const zone = mineLayout.zonas_operativas?.[readerId];
    if (!zone) return null;
    const points = [...(zone.area || []), ...(zone.puntos || [])];
    const reader = (mineLayout.readers || []).find(item => item.id === readerId);
    if (reader) points.push([reader.x, reader.y, reader.z]);
    (state.beacons || []).filter(tag => tag.reader_id === readerId && Array.isArray(tag.coordenadas)).forEach(tag => points.push(tag.coordenadas));
    return {
      level: String(zone.nivel || "ALL").toUpperCase(),
      label: `${readerId} · ${String(zone.estado || "zona operativa").replace(/^Nivel\s+\d+\s*·\s*/i, "")}`,
      points,
      targetWidth: 112
    };
  }
  if (value.startsWith("label:")) {
    const index = Number(value.slice(6));
    const label = mineLayout.etiquetas?.[index];
    if (!label) return null;
    return {
      level: String(label.nivel || "ALL").toUpperCase(),
      label: String(label.id || "Zona seleccionada"),
      points: [[label.x, label.y, label.z || 0]],
      targetWidth: 132
    };
  }
  return null;
}

function focusProjectedPoints(points, targetWidth = 118) {
  const valid = points.filter(point => point.every(Number.isFinite));
  if (!valid.length) return;
  const xs = valid.map(point => point[0]);
  const ys = valid.map(point => point[1]);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const widthForX = (maxX - minX) * 1.8;
  const widthForY = (maxY - minY) * initialMapView.width / initialMapView.height * 1.95;
  mapView.width = Math.max(minimumMapViewWidth, Math.min(220, Math.max(targetWidth, widthForX, widthForY)));
  mapView.height = mapView.width * initialMapView.height / initialMapView.width;
  mapView.x = (minX + maxX - mapView.width) / 2;
  mapView.y = (minY + maxY - mapView.height) / 2;
  constrainMapView();
  applyMapView();
}

function fitSelectedMineArea(value = selectedMineArea) {
  const area = resolveMineArea(value);
  if (!area) return false;
  const project = projector(mineLayout);
  focusProjectedPoints(area.points.map(point => project(point)), area.targetWidth);
  return true;
}

function focusMineArea(value) {
  if (!value) {
    selectedMineArea = "";
    selectedMineAreaLabel = "";
    selectedMineLevel = "ALL";
    selectedTagId = null;
    navigationBoundsCache = null;
    updateMineLevelButtons();
    const select = $("mineAreaSelect");
    if (select) select.value = "";
    const hint = $("mineAreaHint");
    if (hint) hint.textContent = "Solo este menú cambia de zona. Los gestos desplazan o amplían la vista actual hasta 12×.";
    renderMine();
    fitMineOverview();
    return;
  }
  const area = resolveMineArea(value);
  if (!area) return;
  selectedMineArea = value;
  selectedMineAreaLabel = area.label;
  selectedMineLevel = mineLevelDepths[area.level] ? area.level : "ALL";
  selectedTagId = null;
  navigationBoundsCache = null;
  updateMineLevelButtons();
  const select = $("mineAreaSelect");
  if (select && select.value !== value) select.value = value;
  renderMine();
  fitSelectedMineArea(value);
}

function fitSelectedMineLevel() {
  if (!mineLayout || selectedMineLevel === "ALL") {
    centerMapView(true);
    return;
  }
  const project = projector(mineLayout);
  const points = [];
  expandedLayoutSegments(mineLayout)
    .filter(segment => segmentVisibleAtLevel(segment, selectedMineLevel))
    .forEach(segment => points.push(project(segment.a), project(segment.b)));
  (mineLayout.planos_nivel || [])
    .filter(plane => String(plane.id).toUpperCase() === selectedMineLevel)
    .forEach(plane => plane.puntos.forEach(point => points.push(project(point))));
  if (!points.length) {
    centerMapView(true);
    return;
  }
  const xs = points.map(point => point[0]);
  const ys = points.map(point => point[1]);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const centerX = (minX + maxX) / 2;
  const centerY = (minY + maxY) / 2;
  const widthForX = (maxX - minX) * 1.22;
  const widthForY = (maxY - minY) * initialMapView.width / initialMapView.height * 1.28;
  mapView.width = Math.max(320, Math.min(680, Math.max(widthForX, widthForY)));
  mapView.height = mapView.width * initialMapView.height / initialMapView.width;
  mapView.x = centerX - mapView.width / 2;
  mapView.y = centerY - mapView.height / 2;
  constrainMapView();
  applyMapView();
}

function selectMineLevel(level) {
  selectedMineLevel = mineLevelDepths[level] ? level : "ALL";
  selectedMineArea = "";
  selectedMineAreaLabel = "";
  selectedTagId = null;
  navigationBoundsCache = null;
  const areaSelect = $("mineAreaSelect");
  if (areaSelect) areaSelect.value = "";
  const areaHint = $("mineAreaHint");
  if (areaHint) areaHint.textContent = "Solo este menú cambia de zona. Los gestos desplazan o amplían la vista actual hasta 12×.";
  updateMineLevelButtons();
  renderMine();
  if (selectedMineLevel === "ALL") fitMineOverview();
  else fitSelectedMineLevel();
}

async function loadLayout() {
  const initialLoad = !mineLayout;
  const response = await fetch("/api/layout", {cache: "no-store"});
  if (!response.ok) throw new Error(await response.text());
  mineLayout = await response.json();
  navigationBoundsCache = null;
  populateMineAreaSelector();
  renderMine();
  if (initialLoad && selectedMineLevel === "ALL" && !selectedMineArea) fitMineOverview();
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
    navigationBoundsCache = null;
    populateMineAreaSelector();
    renderMine();
    if (selectedMineArea) fitSelectedMineArea();
    else if (selectedMineLevel === "ALL") fitMineOverview();
    else fitSelectedMineLevel();
  } catch (error) {
    $("layoutStatus").textContent = `No se pudo importar: ${error.message}`;
  } finally {
    event.target.value = "";
  }
});
$("mapZoomIn").addEventListener("click", () => zoomMap(.7));
$("mapZoomOut").addEventListener("click", () => zoomMap(1.35));
$("resetView").addEventListener("click", () => {
  if (selectedMineArea) fitSelectedMineArea();
  else if (selectedMineLevel === "ALL") fitMineOverview();
  else fitSelectedMineLevel();
});
$("mapRotateLeft").addEventListener("click", () => rotateMap(-1));
$("mapRotateRight").addEventListener("click", () => rotateMap(1));
document.querySelectorAll("[data-mine-level]").forEach(button => button.addEventListener("click", () => selectMineLevel(button.dataset.mineLevel)));
$("mineAreaSelect").addEventListener("change", event => focusMineArea(event.target.value));

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
