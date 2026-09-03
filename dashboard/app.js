(() => {
  const WS_URL = `ws://${location.hostname}:8081`;
  const STORE = "xraywifi.v1";

  const $ = (id) => document.getElementById(id);
  const connEl = $("conn");
  const nodesEl = $("nodes");
  const nodeToggles = $("nodeToggles");
  const rowsEl = $("rows");
  const mapRowsEl = $("mapRows");
  const countEl = $("count");
  const kindEl = $("kind");
  const rssiNear = $("rssiNear");
  const rssiFar = $("rssiFar");
  const rssiNearOut = $("rssiNearOut");
  const rssiFarOut = $("rssiFarOut");
  const rssiHeads = $("rssiHeads");
  const ringLabelModeEl = $("ringLabelMode");
  const scope = $("scope");
  const map = $("map");
  const sctx = scope.getContext("2d");
  const mctx = map.getContext("2d");
  const p0 = $("p0");
  const d0 = $("d0");
  const nExp = $("nExp");
  const roomW = $("roomW");
  const roomH = $("roomH");
  const p0Out = $("p0Out");
  const d0Out = $("d0Out");
  const nExpOut = $("nExpOut");
  const roomWOut = $("roomWOut");
  const roomHOut = $("roomHOut");
  const trackMeta = $("trackMeta");
  const fixEl = $("fix");

  const COL = {
    bg: "#020503",
    grid: "#0d3a1c",
    ring: "#1c7a3a",
    text: "#60dc82",
    hot: "#28ff5a",
    open: "#ff4030",
    ble: "#ffa820",
    hotDim: "#106e2c",
    bleDim: "#784c0c",
    stale: "#16351e",
    node: ["#28ff5a", "#6ad0ff", "#ffa820"],
  };

  let state = { nodes: [], contacts: [] };
  let enabled = {};
  let selected = null;
  let sweep = 0;
  let lastTs = performance.now();
  let ws = null;
  let floorImg = null;
  let drag = null;
  let ringLabelMode = "metres";

  const loadStore = () => {
    try {
      return JSON.parse(localStorage.getItem(STORE) || "{}");
    } catch {
      return {};
    }
  };
  const saveStore = (patch) => {
    const cur = loadStore();
    localStorage.setItem(STORE, JSON.stringify({ ...cur, ...patch }));
  };

  const saved = loadStore();
  if (saved.rssiNear != null) rssiNear.value = saved.rssiNear;
  if (saved.rssiFar != null) rssiFar.value = saved.rssiFar;
  if (saved.p0 != null) p0.value = saved.p0;
  if (saved.d0 != null) d0.value = saved.d0;
  if (saved.nExp != null) nExp.value = saved.nExp;
  if (saved.roomW != null) roomW.value = saved.roomW;
  if (saved.roomH != null) roomH.value = saved.roomH;
  if (saved.kind) kindEl.value = saved.kind;
  if (saved.ringLabelMode) ringLabelMode = saved.ringLabelMode;
  if (saved.floor) {
    floorImg = new Image();
    floorImg.src = saved.floor;
  }

  const nodeLayout = saved.layout || {};

  function rssiToDist(rssi) {
    const P0 = Number(p0.value);
    const D0 = Number(d0.value);
    const n = Number(nExp.value);
    return D0 * Math.pow(10, (P0 - rssi) / (10 * n));
  }

  function formatDist(m) {
    if (!Number.isFinite(m) || m < 0) return "–";
    if (m < 10) return `${m.toFixed(1)} m`;
    return `${Math.round(m)} m`;
  }

  function maxRingMetres() {
    return rssiToDist(Number(rssiFar.value));
  }

  function ringRssi(i) {
    const near = Number(rssiNear.value);
    const far = Number(rssiFar.value);
    const t = i / 4;
    return near + (far - near) * t;
  }

  function ringMetres(i) {
    return rssiToDist(ringRssi(i));
  }

  function distToRadius(dist, R, maxDist) {
    const clamped = Math.max(0, Math.min(maxDist || 1, dist));
    return (clamped / Math.max(0.1, maxDist)) * (R - 8) + 10;
  }

  const bindOut = (input, out, fmt) => {
    const sync = () => {
      out.textContent = fmt(Number(input.value));
      saveStore({
        rssiNear: Number(rssiNear.value),
        rssiFar: Number(rssiFar.value),
        p0: Number(p0.value),
        d0: Number(d0.value),
        nExp: Number(nExp.value),
        roomW: Number(roomW.value),
        roomH: Number(roomH.value),
        kind: kindEl.value,
        ringLabelMode,
      });
      tableSig = "";
      renderTables();
    };
    input.addEventListener("input", sync);
    sync();
  };
  bindOut(rssiNear, rssiNearOut, (v) => `${v} dBm`);
  bindOut(rssiFar, rssiFarOut, (v) => `${v} dBm`);
  bindOut(p0, p0Out, (v) => `${v} dBm`);
  bindOut(d0, d0Out, (v) => `${v.toFixed(1)} m`);
  bindOut(nExp, nExpOut, (v) => v.toFixed(1));
  bindOut(roomW, roomWOut, (v) => `${v} m`);
  bindOut(roomH, roomHOut, (v) => `${v} m`);
  kindEl.addEventListener("change", () => {
    saveStore({ kind: kindEl.value });
    tableSig = "";
    renderTables();
  });

  if (ringLabelModeEl) {
    ringLabelModeEl.value = ringLabelMode;
    ringLabelModeEl.addEventListener("change", () => {
      ringLabelMode = ringLabelModeEl.value;
      saveStore({ ringLabelMode });
    });
  }

  document.querySelectorAll(".tab").forEach((btn) => {
    btn.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach((b) => b.classList.toggle("on", b === btn));
      $("view-radar").classList.toggle("hidden", btn.dataset.tab !== "radar");
      $("view-map").classList.toggle("hidden", btn.dataset.tab !== "map");
    });
  });

  function switchTab(tab) {
    document.querySelectorAll(".tab").forEach((b) => {
      b.classList.toggle("on", b.dataset.tab === tab);
    });
    $("view-radar").classList.toggle("hidden", tab !== "radar");
    $("view-map").classList.toggle("hidden", tab !== "map");
  }

  $("floor").addEventListener("change", (e) => {
    const file = e.target.files && e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      floorImg = new Image();
      floorImg.src = reader.result;
      saveStore({ floor: reader.result });
    };
    reader.readAsDataURL(file);
  });
  $("clearFloor").addEventListener("click", () => {
    floorImg = null;
    saveStore({ floor: null });
  });
  $("resetNodes").addEventListener("click", () => {
    Object.keys(nodeLayout).forEach((k) => delete nodeLayout[k]);
    saveStore({ layout: nodeLayout });
  });

  const colorFor = (c, dim) => {
    if (c.type === "BLE") return dim ? COL.bleDim : COL.ble;
    if (c.open || c.type === "OPEN") return dim ? "#7a2018" : COL.open;
    return dim ? COL.hotDim : COL.hot;
  };

  const keepContact = (c) => {
    const kind = kindEl.value;
    if (kind === "WIFI" && c.type === "BLE") return false;
    if (kind === "BLE" && c.type !== "BLE") return false;
    const ids = Object.keys(c.nodes || {});
    if (!ids.length) return false;
    return ids.some((id) => enabled[id] !== false);
  };

  const bestRssi = (c) => {
    const vals = Object.entries(c.nodes || {})
      .filter(([id]) => enabled[id] !== false)
      .map(([, info]) => info.rssi);
    return vals.length ? Math.max(...vals) : c.best;
  };

  const contactDist = (c) => {
    const rssi = bestRssi(c);
    if (rssi == null || !Number.isFinite(rssi)) return null;
    return rssiToDist(rssi);
  };

  const distTooltip = (c) => {
    const rssi = bestRssi(c);
    if (rssi == null) return "";
    const dist = rssiToDist(rssi);
    return `${rssi} dBm → ≈ ${formatDist(dist)} (n=${Number(nExp.value).toFixed(1)})`;
  };

  const rssiRadius = (rssi, R) => {
    const near = Number(rssiNear.value);
    const far = Number(rssiFar.value);
    const lo = Math.min(near, far);
    const hi = Math.max(near, far);
    const t = (Math.max(lo, Math.min(hi, rssi)) - lo) / Math.max(1, hi - lo);
    return (1 - t) * (R - 8) + 10;
  };

  const blipRadius = (rssi, R) => {
    if (ringLabelMode === "metres") {
      return distToRadius(rssiToDist(rssi), R, maxRingMetres());
    }
    return rssiRadius(rssi, R);
  };

  let nodeSig = "";
  let tableSig = "";

  function renderNodes() {
    const list = state.nodes || [];
    const sig = list.map((n) => `${n.id}|${n.ok}|${n.ap}|${n.ble}|${n.tracked}|${n.heap}|${n.mode}|${n.via}|${n.ip}`).join(";");
    if (sig === nodeSig) return;
    nodeSig = sig;
    nodesEl.innerHTML = list.length
      ? list
          .map((n, i) => {
            const age = n.last_seen ? ((Date.now() / 1000 - n.last_seen) * 1).toFixed(0) : "?";
            const via = n.via || (n.port && String(n.port).startsWith("wifi") ? "wifi" : "usb");
            const ip = n.ip ? ` · ${n.ip}` : "";
            return `<article class="node ${n.ok ? "" : "dead"}">
              <div>
                <div><span class="k">${n.label || "•"}</span>${n.id}</div>
                <div class="id">${via}${ip} · ${n.port || ""} · ${n.mode || ""}</div>
              </div>
              <div class="meta">
                AP ${n.ap ?? "–"} · BLE ${n.ble ?? "–"} · TRK ${n.tracked ?? "–"}<br/>
                heap ${n.heap ?? "–"} · ${n.ok ? "live" : "stale " + age + "s"}
              </div>
            </article>`;
          })
          .join("")
      : `<article class="node dead"><div>No boards yet</div><div class="meta">waiting for Wi-Fi or USB</div></article>`;

    const known = new Set(list.map((n) => n.id));
    nodeToggles.innerHTML = list
      .map(
        (n) => `<label><input type="checkbox" data-id="${n.id}" ${enabled[n.id] === false ? "" : "checked"} /> ${n.label || n.id.slice(-5)}</label>`
      )
      .join("");
    nodeToggles.querySelectorAll("input").forEach((inp) => {
      inp.addEventListener("change", () => {
        enabled[inp.dataset.id] = inp.checked;
        tableSig = "";
        renderTables();
      });
    });
    Object.keys(enabled).forEach((id) => {
      if (!known.has(id)) delete enabled[id];
    });

    rssiHeads.innerHTML = list.map((n) => n.label || "?").join(" / ");
  }

  function sortContacts(list) {
    return [...list].sort((a, b) => {
      const staleA = a.age > 20 ? 1 : 0;
      const staleB = b.age > 20 ? 1 : 0;
      if (staleA !== staleB) return staleA - staleB;
      const da = contactDist(a);
      const db = contactDist(b);
      if (da == null && db == null) return 0;
      if (da == null) return 1;
      if (db == null) return -1;
      return da - db;
    });
  }

  function renderTables() {
    const list = sortContacts((state.contacts || []).filter(keepContact));
    const nodeIds = (state.nodes || []).map((n) => n.id);
    const sig = selected + "|" + kindEl.value + "|" + ringLabelMode + "|" + list.map((c) => `${c.addr}:${bestRssi(c)}:${Object.keys(c.nodes || {}).length}`).join(",");
    if (sig === tableSig) return;
    tableSig = sig;
    countEl.textContent = String(list.length);

    const rowHtml = (c, extra) => {
      const rssiCells = nodeIds
        .map((id) => {
          const info = (c.nodes || {})[id];
          return info ? String(info.rssi) : "–";
        })
        .join(" / ");
      const seen = Object.keys(c.nodes || {}).length;
      const dist = contactDist(c);
      const distCell = dist != null
        ? `<td class="dist" title="${escapeHtml(distTooltip(c))}">${formatDist(dist)}</td>`
        : `<td class="dist">–</td>`;
      return `<tr data-addr="${c.addr}" class="${selected === c.addr ? "sel" : ""}">
        <td><span class="dot" style="background:${colorFor(c, false)}"></span></td>
        <td class="name" title="${c.addr}">${escapeHtml(c.name || c.addr)}</td>
        ${extra === "map"
          ? `<td>${seen}</td><td>${bestRssi(c)}</td>${distCell}`
          : `<td>${c.type}</td><td>${c.brg}°</td><td>${bestRssi(c)}</td>${distCell}
        <td>${rssiCells}</td><td>${Number(c.age).toFixed(0)}s</td>`}
      </tr>`;
    };

    rowsEl.innerHTML = list.slice(0, 80).map((c) => rowHtml(c, "radar")).join("");
    mapRowsEl.innerHTML = list
      .filter((c) => Object.keys(c.nodes || {}).length >= 1)
      .slice(0, 80)
      .map((c) => rowHtml(c, "map"))
      .join("");

    const pick = (tbody) => {
      tbody.querySelectorAll("tr").forEach((tr) => {
        tr.addEventListener("click", () => {
          selected = tr.dataset.addr;
          renderTables();
        });
      });
    };
    pick(rowsEl);
    pick(mapRowsEl);
  }

  function escapeHtml(s) {
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;");
  }

  function drawScope(now) {
    const w = scope.width;
    const h = scope.height;
    const cx = w / 2;
    const cy = h / 2 - 10;
    const R = Math.min(w, h) * 0.42;
    sctx.fillStyle = COL.bg;
    sctx.fillRect(0, 0, w, h);

    sctx.strokeStyle = COL.grid;
    sctx.lineWidth = 1;
    for (let i = 1; i <= 4; i++) {
      sctx.beginPath();
      sctx.arc(cx, cy, (R * i) / 4, 0, Math.PI * 2);
      sctx.strokeStyle = i === 4 ? COL.ring : COL.grid;
      sctx.stroke();
    }
    sctx.beginPath();
    sctx.moveTo(cx - R, cy);
    sctx.lineTo(cx + R, cy);
    sctx.moveTo(cx, cy - R);
    sctx.lineTo(cx, cy + R);
    sctx.strokeStyle = COL.grid;
    sctx.stroke();

    const near = Number(rssiNear.value);
    const far = Number(rssiFar.value);
    sctx.fillStyle = COL.text;
    sctx.font = "12px IBM Plex Mono, Menlo, monospace";

    if (ringLabelMode === "metres") {
      sctx.fillText("≤" + formatDist(Number(d0.value)), cx + 6, cy - 8);
      for (let i = 1; i <= 4; i++) {
        const rad = (R * i) / 4;
        const label = formatDist(ringMetres(i));
        const lx = cx + rad - 18;
        const ly = cy - 8;
        sctx.fillText(label, lx, ly);
      }
    } else {
      sctx.fillText(String(near), cx + 6, cy - 8);
      sctx.fillText(String(far), cx + R - 28, cy - 8);
    }
    sctx.fillText("RADAR", 18, 28);

    const dt = Math.min(0.05, (now - lastTs) / 1000);
    lastTs = now;
    sweep = (sweep + 62 * dt) % 360;

    for (let k = 0; k < 28; k++) {
      const a = ((sweep - k * 2.2) * Math.PI) / 180;
      const f = 1 - k / 28;
      const g = Math.floor(235 * f * f * 0.85);
      sctx.strokeStyle = `rgb(${Math.floor(30 * f)},${g},${Math.floor(70 * f)})`;
      sctx.beginPath();
      sctx.moveTo(cx, cy);
      sctx.lineTo(cx + Math.cos(a) * R, cy + Math.sin(a) * R);
      sctx.stroke();
    }

    for (const c of (state.contacts || []).filter(keepContact)) {
      const rssi = bestRssi(c);
      const rad = blipRadius(rssi, R);
      const a = (c.brg * Math.PI) / 180;
      const x = cx + Math.cos(a) * rad;
      const y = cy + Math.sin(a) * rad;
      let delta = (sweep - c.brg + 720) % 360;
      const lit = delta < 90 ? 1 - delta / 90 : 0;
      const stale = c.age > 20;
      const dim = lit < 0.25;
      sctx.fillStyle = stale && dim ? COL.stale : colorFor(c, dim);
      const sz = 3 + lit * 4;
      sctx.fillRect(x - sz / 2, y - sz / 2, sz, sz);
      if (lit > 0.55) {
        sctx.strokeStyle = sctx.fillStyle;
        sctx.beginPath();
        sctx.arc(x, y, 4 + lit * 4, 0, Math.PI * 2);
        sctx.stroke();
      }
      if (selected === c.addr) {
        sctx.strokeStyle = "#e8ffe8";
        sctx.beginPath();
        sctx.arc(x, y, 10, 0, Math.PI * 2);
        sctx.stroke();
      }
    }

    sctx.fillStyle = COL.text;
    sctx.fillRect(cx - 2, cy - 2, 4, 4);
  }

  function defaultLayout(i, n) {
    const cx = 0.2 + (i / Math.max(1, n - 1)) * 0.6;
    return { x: n === 1 ? 0.5 : cx, y: 0.18 + (i % 2) * 0.55 };
  }

  function placedNodes() {
    const list = state.nodes || [];
    return list.map((n, i) => {
      const pos = nodeLayout[n.id] || defaultLayout(i, list.length);
      return { ...n, x: pos.x, y: pos.y, color: COL.node[i % COL.node.length] };
    });
  }

  function twoCircle(a, b) {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const d = Math.hypot(dx, dy) || 1e-6;
    if (d > a.r + b.r || d < Math.abs(a.r - b.r)) {
      const t = a.r / Math.max(0.2, a.r + b.r);
      return {
        x: a.x + dx * t,
        y: a.y + dy * t,
        rms: Math.abs(d - a.r - b.r),
        n: 2,
      };
    }
    const aa = (a.r * a.r - b.r * b.r + d * d) / (2 * d);
    const h = Math.sqrt(Math.max(0, a.r * a.r - aa * aa));
    const mx = a.x + (aa * dx) / d;
    const my = a.y + (aa * dy) / d;
    const px = -dy / d;
    const py = dx / d;
    return {
      x: mx,
      y: my,
      rms: h,
      n: 2,
      alt: [
        { x: mx + h * px, y: my + h * py },
        { x: mx - h * px, y: my - h * py },
      ],
    };
  }

  function laterate(circles) {
    if (circles.length < 2) return null;
    if (circles.length === 2) return twoCircle(circles[0], circles[1]);
    let x = 0;
    let y = 0;
    let w = 0;
    for (const c of circles) {
      const wi = 1 / Math.max(c.r, 0.4);
      x += c.x * wi;
      y += c.y * wi;
      w += wi;
    }
    x /= w;
    y /= w;
    for (let iter = 0; iter < 24; iter++) {
      let j11 = 0,
        j12 = 0,
        j22 = 0,
        b1 = 0,
        b2 = 0;
      for (const c of circles) {
        const dx = x - c.x;
        const dy = y - c.y;
        const d = Math.hypot(dx, dy) || 1e-6;
        const e = d - c.r;
        const g1 = dx / d;
        const g2 = dy / d;
        j11 += g1 * g1;
        j12 += g1 * g2;
        j22 += g2 * g2;
        b1 += g1 * e;
        b2 += g2 * e;
      }
      const det = j11 * j22 - j12 * j12;
      if (Math.abs(det) < 1e-12) break;
      const sx = (j22 * b1 - j12 * b2) / det;
      const sy = (-j12 * b1 + j11 * b2) / det;
      x -= sx;
      y -= sy;
      if (sx * sx + sy * sy < 1e-10) break;
    }
    let res = 0;
    for (const c of circles) res += (Math.hypot(x - c.x, y - c.y) - c.r) ** 2;
    return { x, y, rms: Math.sqrt(res / circles.length), n: circles.length };
  }

  function mapMetrics() {
    const pad = 36;
    const w = map.width - pad * 2;
    const h = map.height - pad * 2;
    return { pad, w, h, metresW: Number(roomW.value), metresH: Number(roomH.value) };
  }

  function toPx(nx, ny) {
    const m = mapMetrics();
    return { x: m.pad + nx * m.w, y: m.pad + ny * m.h };
  }

  function toNorm(px, py) {
    const m = mapMetrics();
    return {
      x: Math.max(0, Math.min(1, (px - m.pad) / m.w)),
      y: Math.max(0, Math.min(1, (py - m.pad) / m.h)),
    };
  }

  function contactFix(c) {
    const placed = placedNodes();
    const m = mapMetrics();
    const circles = [];
    for (const n of placed) {
      const info = (c.nodes || {})[n.id];
      if (!info || enabled[n.id] === false) continue;
      const dist = rssiToDist(info.rssi);
      circles.push({
        x: n.x * m.metresW,
        y: n.y * m.metresH,
        r: dist,
        label: n.label,
        rssi: info.rssi,
      });
    }
    const fix = laterate(circles);
    return { circles, fix, metres: m };
  }

  function drawMap() {
    const w = map.width;
    const h = map.height;
    mctx.fillStyle = COL.bg;
    mctx.fillRect(0, 0, w, h);
    if (floorImg && floorImg.complete && floorImg.naturalWidth) {
      mctx.globalAlpha = 0.55;
      mctx.drawImage(floorImg, 0, 0, w, h);
      mctx.globalAlpha = 1;
    }
    const m = mapMetrics();
    mctx.strokeStyle = COL.grid;
    mctx.strokeRect(m.pad, m.pad, m.w, m.h);
    mctx.fillStyle = COL.text;
    mctx.font = "12px IBM Plex Mono, Menlo, monospace";
    mctx.fillText(`${m.metresW} m`, m.pad + m.w - 48, m.pad + m.h + 18);
    mctx.fillText(`${m.metresH} m`, 8, m.pad + 12);

    const c = (state.contacts || []).find((x) => x.addr === selected);
    const placed = placedNodes();
    let info = { circles: [], fix: null, metres: m };
    if (c) info = contactFix(c);

    if (c) {
      for (const circ of info.circles) {
        const p = toPx(circ.x / m.metresW, circ.y / m.metresH);
        const rx = (circ.r / m.metresW) * m.w;
        const ry = (circ.r / m.metresH) * m.h;
        mctx.strokeStyle = colorFor(c, true);
        mctx.beginPath();
        mctx.ellipse(p.x, p.y, rx, ry, 0, 0, Math.PI * 2);
        mctx.stroke();
      }
      if (info.fix) {
        const p = toPx(info.fix.x / m.metresW, info.fix.y / m.metresH);
        mctx.fillStyle = colorFor(c, false);
        mctx.beginPath();
        mctx.arc(p.x, p.y, 7, 0, Math.PI * 2);
        mctx.fill();
        const urx = (info.fix.rms / m.metresW) * m.w;
        const ury = (info.fix.rms / m.metresH) * m.h;
        mctx.strokeStyle = "rgba(232,255,232,0.7)";
        mctx.beginPath();
        mctx.ellipse(p.x, p.y, Math.max(10, urx), Math.max(10, ury), 0, 0, Math.PI * 2);
        mctx.stroke();
        if (info.fix.alt) {
          mctx.fillStyle = "rgba(232,255,232,0.35)";
          for (const a of info.fix.alt) {
            const q = toPx(a.x / m.metresW, a.y / m.metresH);
            mctx.beginPath();
            mctx.arc(q.x, q.y, 4, 0, Math.PI * 2);
            mctx.fill();
          }
        }
      }
    }

    for (const n of placed) {
      const p = toPx(n.x, n.y);
      mctx.fillStyle = n.color;
      mctx.beginPath();
      mctx.arc(p.x, p.y, 8, 0, Math.PI * 2);
      mctx.fill();
      mctx.fillStyle = "#e8ffe8";
      mctx.fillText(n.label || n.id.slice(-4), p.x + 10, p.y - 8);
    }

    if (c && info.fix) {
      trackMeta.textContent = c.name || c.addr;
      fixEl.innerHTML = `<strong>${escapeHtml(c.name || c.addr)}</strong><br/>
        ${info.circles.length} receiver${info.circles.length === 1 ? "" : "s"} ·
        est (${info.fix.x.toFixed(1)}, ${info.fix.y.toFixed(1)}) m ·
        rms ${info.fix.rms.toFixed(1)} m<br/>
        ${info.circles.map((x) => `${x.label} ${x.rssi} dBm ≈ ${x.r.toFixed(1)} m`).join(" · ")}`;
    } else if (c) {
      trackMeta.textContent = c.name || c.addr;
      fixEl.textContent =
        Object.keys(c.nodes || {}).length < 2
          ? "Need RSSI from two or more nodes to estimate a position."
          : "Waiting for a fix.";
    } else {
      trackMeta.textContent = "none selected";
      fixEl.textContent = "Select a contact seen by two or more nodes.";
    }
  }

  function hitNode(mx, my) {
    for (const n of placedNodes()) {
      const p = toPx(n.x, n.y);
      if (Math.hypot(mx - p.x, my - p.y) < 14) return n.id;
    }
    return null;
  }

  map.addEventListener("pointerdown", (e) => {
    const r = map.getBoundingClientRect();
    const mx = ((e.clientX - r.left) / r.width) * map.width;
    const my = ((e.clientY - r.top) / r.height) * map.height;
    const id = hitNode(mx, my);
    if (id) {
      drag = id;
      map.setPointerCapture(e.pointerId);
    }
  });
  map.addEventListener("pointermove", (e) => {
    if (!drag) return;
    const r = map.getBoundingClientRect();
    const mx = ((e.clientX - r.left) / r.width) * map.width;
    const my = ((e.clientY - r.top) / r.height) * map.height;
    nodeLayout[drag] = toNorm(mx, my);
    saveStore({ layout: nodeLayout });
  });
  map.addEventListener("pointerup", () => {
    drag = null;
  });

  function frame(now) {
    drawScope(now);
    drawMap();
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);

  function applyState(msg) {
    if (!msg || msg.t !== "state") return;
    state = msg;
    renderNodes();
    renderTables();
  }

  function connect() {
    connEl.textContent = "connecting";
    connEl.className = "pill off";
    ws = new WebSocket(WS_URL);
    ws.onopen = () => {
      connEl.textContent = "live";
      connEl.className = "pill on";
    };
    ws.onclose = () => {
      connEl.textContent = "offline";
      connEl.className = "pill off";
      setTimeout(connect, 1500);
    };
    ws.onerror = () => ws.close();
    ws.onmessage = (ev) => {
      try {
        applyState(JSON.parse(ev.data));
      } catch {
        /* ignore */
      }
    };
  }

  function setCalValue(key, value) {
    const el = { p0, d0, nExp, roomW, roomH, rssiNear, rssiFar }[key];
    if (!el) return;
    el.value = value;
    el.dispatchEvent(new Event("input"));
  }

  function setRingLabelMode(mode) {
    ringLabelMode = mode;
    if (ringLabelModeEl) ringLabelModeEl.value = mode;
    saveStore({ ringLabelMode: mode });
  }

  window.xrayCal = {
    getState: () => state,
    getSelected: () => selected,
    setSelected: (addr) => {
      selected = addr;
      renderTables();
    },
    getContacts: () => (state.contacts || []).filter(keepContact),
    bestRssi,
    contactDist,
    formatDist,
    rssiToDist,
    setCalValue,
    setRingLabelMode,
    switchTab,
    saveStore,
    getCal: () => ({
      p0: Number(p0.value),
      d0: Number(d0.value),
      nExp: Number(nExp.value),
      roomW: Number(roomW.value),
      roomH: Number(roomH.value),
    }),
  };

  fetch("/api/state")
    .then((r) => r.json())
    .then(applyState)
    .catch(() => {});
  connect();
})();
