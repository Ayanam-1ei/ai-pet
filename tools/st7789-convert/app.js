(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);

  const els = {
    fileInput: $("file-input"),
    dropZone: $("drop-zone"),
    sourceCanvas: $("source-canvas"),
    emptyState: $("empty-state"),
    previewCanvas: $("preview-canvas"),
    zoomCanvas: $("zoom-canvas"),
    gridOverlay: $("pixel-grid-overlay"),
    screenPreset: $("screen-preset"),
    outW: $("out-w"),
    outH: $("out-h"),
    scaleMode: $("scale-mode"),
    ditherMode: $("dither-mode"),
    colorDepth: $("color-depth"),
    showGrid: $("show-grid"),
    previewZoom: $("preview-zoom"),
    zoomLabel: $("zoom-label"),
    lockSquare: $("lock-square"),
    cropInfo: $("crop-info"),
    previewBadge: $("preview-badge"),
    deviceLabel: $("device-label"),
    usageSnippet: $("usage-snippet"),
    statSize: $("stat-size"),
    statPixels: $("stat-pixels"),
    statBytes: $("stat-bytes"),
    btnExportPng: $("btn-export-png"),
    btnExportC: $("btn-export-c"),
    btnExportBin: $("btn-export-bin"),
    btnFit: $("btn-fit"),
    btnResetCrop: $("btn-reset-crop"),
  };

  const state = {
    image: null,
    imageWidth: 0,
    imageHeight: 0,
    // crop rect in image coordinates
    crop: { x: 0, y: 0, w: 0, h: 0 },
    // view transform
    scale: 1,
    offsetX: 0,
    offsetY: 0,
    // interaction
    dragMode: null, // move | resize-nw | resize-ne | resize-sw | resize-se | pan | null
    dragStart: null,
    spaceDown: false,
    outW: 240,
    outH: 240,
    dirty: true,
  };

  const sourceCtx = els.sourceCanvas.getContext("2d");
  const previewCtx = els.previewCanvas.getContext("2d", { willReadFrequently: true });
  const zoomCtx = els.zoomCanvas.getContext("2d");
  const workCanvas = document.createElement("canvas");
  const workCtx = workCanvas.getContext("2d", { willReadFrequently: true });
  const cropCanvas = document.createElement("canvas");
  const cropCtx = cropCanvas.getContext("2d", { willReadFrequently: true });

  const BAYER4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
  ];

  const BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
  ];

  function clamp(v, min, max) {
    return Math.max(min, Math.min(max, v));
  }

  function formatNum(n) {
    return Number(n).toLocaleString("zh-CN");
  }

  function setEnabled(enabled) {
    [els.btnExportPng, els.btnExportC, els.btnExportBin, els.btnFit, els.btnResetCrop].forEach((btn) => {
      btn.disabled = !enabled;
    });
  }

  function loadImageFile(file) {
    if (!file || !file.type.startsWith("image/")) return;
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      URL.revokeObjectURL(url);
      state.image = img;
      state.imageWidth = img.naturalWidth;
      state.imageHeight = img.naturalHeight;
      resetCropToSquare();
      fitView();
      els.dropZone.classList.add("has-image");
      els.emptyState.classList.add("hidden");
      setEnabled(true);
      requestProcess();
    };
    img.onerror = () => {
      URL.revokeObjectURL(url);
      alert("无法读取该图片，请换一张文件试试。");
    };
    img.src = url;
  }

  function resetCropToSquare() {
    if (!state.image) return;
    const { imageWidth: iw, imageHeight: ih } = state;
    const side = Math.min(iw, ih);
    state.crop = {
      x: (iw - side) / 2,
      y: (ih - side) / 2,
      w: side,
      h: side,
    };
    // For non-square target, crop can be non-square
    applyTargetAspectToCrop();
  }

  function applyTargetAspectToCrop() {
    if (!state.image) return;
    const targetAspect = state.outW / state.outH;
    const iw = state.imageWidth;
    const ih = state.imageHeight;
    let { x, y, w, h } = state.crop;

    // Fit a max crop of target aspect inside image, centered on current crop center
    const cx = x + w / 2;
    const cy = y + h / 2;

    let cw;
    let ch;
    if (iw / ih > targetAspect) {
      // image wider than target
      ch = ih;
      cw = ch * targetAspect;
    } else {
      cw = iw;
      ch = cw / targetAspect;
    }

    x = clamp(cx - cw / 2, 0, iw - cw);
    y = clamp(cy - ch / 2, 0, ih - ch);
    state.crop = { x, y, w: cw, h: ch };
  }

  function fitView() {
    if (!state.image) return;
    const rect = els.dropZone.getBoundingClientRect();
    const cw = rect.width;
    const ch = rect.height;
    const iw = state.imageWidth;
    const ih = state.imageHeight;
    const scale = Math.min(cw / iw, ch / ih) * 0.92;
    state.scale = scale;
    state.offsetX = (cw - iw * scale) / 2;
    state.offsetY = (ch - ih * scale) / 2;
    resizeSourceCanvas();
  }

  function resizeSourceCanvas() {
    const rect = els.dropZone.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    els.sourceCanvas.width = Math.max(1, Math.floor(rect.width * dpr));
    els.sourceCanvas.height = Math.max(1, Math.floor(rect.height * dpr));
    els.sourceCanvas.style.width = `${rect.width}px`;
    els.sourceCanvas.style.height = `${rect.height}px`;
    sourceCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
    drawSource();
  }

  function imageToView(x, y) {
    return {
      x: state.offsetX + x * state.scale,
      y: state.offsetY + y * state.scale,
    };
  }

  function viewToImage(x, y) {
    return {
      x: (x - state.offsetX) / state.scale,
      y: (y - state.offsetY) / state.scale,
    };
  }

  function getHandleHit(viewX, viewY) {
    const c = state.crop;
    const p1 = imageToView(c.x, c.y);
    const p2 = imageToView(c.x + c.w, c.y + c.h);
    const tol = 10;
    const handles = {
      "resize-nw": { x: p1.x, y: p1.y },
      "resize-ne": { x: p2.x, y: p1.y },
      "resize-sw": { x: p1.x, y: p2.y },
      "resize-se": { x: p2.x, y: p2.y },
    };
    for (const [name, h] of Object.entries(handles)) {
      if (Math.abs(viewX - h.x) <= tol && Math.abs(viewY - h.y) <= tol) return name;
    }
    if (viewX >= p1.x && viewX <= p2.x && viewY >= p1.y && viewY <= p2.y) return "move";
    return null;
  }

  function drawSource() {
    const rect = els.dropZone.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;
    sourceCtx.clearRect(0, 0, w, h);

    // checker background
    sourceCtx.fillStyle = "#0a0d11";
    sourceCtx.fillRect(0, 0, w, h);
    const tile = 12;
    sourceCtx.fillStyle = "rgba(255,255,255,0.02)";
    for (let y = 0; y < h; y += tile) {
      for (let x = 0; x < w; x += tile) {
        if (((x / tile) + (y / tile)) % 2 === 0) sourceCtx.fillRect(x, y, tile, tile);
      }
    }

    if (!state.image) return;

    sourceCtx.imageSmoothingEnabled = true;
    sourceCtx.drawImage(
      state.image,
      state.offsetX,
      state.offsetY,
      state.imageWidth * state.scale,
      state.imageHeight * state.scale
    );

    // dim outside crop
    const c = state.crop;
    const p1 = imageToView(c.x, c.y);
    const p2 = imageToView(c.x + c.w, c.y + c.h);
    sourceCtx.fillStyle = "rgba(5, 8, 12, 0.58)";
    sourceCtx.fillRect(0, 0, w, p1.y);
    sourceCtx.fillRect(0, p2.y, w, h - p2.y);
    sourceCtx.fillRect(0, p1.y, p1.x, p2.y - p1.y);
    sourceCtx.fillRect(p2.x, p1.y, w - p2.x, p2.y - p1.y);

    // crop border
    sourceCtx.strokeStyle = "#3dd6c6";
    sourceCtx.lineWidth = 2;
    sourceCtx.strokeRect(p1.x, p1.y, p2.x - p1.x, p2.y - p1.y);

    // handles
    const hs = [
      [p1.x, p1.y],
      [p2.x, p1.y],
      [p1.x, p2.y],
      [p2.x, p2.y],
    ];
    sourceCtx.fillStyle = "#3dd6c6";
    for (const [hx, hy] of hs) {
      sourceCtx.fillRect(hx - 5, hy - 5, 10, 10);
    }

    // rule of thirds
    sourceCtx.strokeStyle = "rgba(61, 214, 198, 0.25)";
    sourceCtx.lineWidth = 1;
    for (let i = 1; i < 3; i++) {
      const x = p1.x + ((p2.x - p1.x) * i) / 3;
      const y = p1.y + ((p2.y - p1.y) * i) / 3;
      sourceCtx.beginPath();
      sourceCtx.moveTo(x, p1.y);
      sourceCtx.lineTo(x, p2.y);
      sourceCtx.stroke();
      sourceCtx.beginPath();
      sourceCtx.moveTo(p1.x, y);
      sourceCtx.lineTo(p2.x, y);
      sourceCtx.stroke();
    }

    // labels
    sourceCtx.fillStyle = "rgba(232, 238, 246, 0.85)";
    sourceCtx.font = "11px SF Mono, Consolas, monospace";
    const label = `${Math.round(c.w)}×${Math.round(c.h)}`;
    const lw = sourceCtx.measureText(label).width + 10;
    sourceCtx.fillStyle = "rgba(10, 13, 17, 0.75)";
    sourceCtx.fillRect(p1.x + 6, p1.y + 6, lw, 18);
    sourceCtx.fillStyle = "#3dd6c6";
    sourceCtx.fillText(label, p1.x + 11, p1.y + 19);
  }

  function quantizeChannel(v, bits) {
    if (bits >= 8) return clamp(Math.round(v), 0, 255);
    const levels = 1 << bits;
    const step = 255 / (levels - 1);
    return clamp(Math.round(Math.round(v / step) * step), 0, 255);
  }

  function toRGB565(r, g, b) {
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
  }

  function applyColorDepth(r, g, b, depth, x, y, dither) {
    let ar = r;
    let ag = g;
    let ab = b;

    if (depth === 1) {
      const thr = dither === "bayer4" || dither === "bayer8"
        ? bayerThreshold(x, y, dither)
        : 128;
      const gray = 0.299 * r + 0.587 * g + 0.114 * b;
      const on = gray >= thr ? 255 : 0;
      return [on, on, on];
    }

    const rBits = depth === 16 ? 5 : depth === 15 ? 5 : depth === 12 ? 4 : 3;
    const gBits = depth === 16 ? 6 : depth === 15 ? 5 : depth === 12 ? 4 : 3;
    const bBits = depth === 16 ? 5 : depth === 15 ? 5 : depth === 12 ? 4 : 2;

    if (dither === "bayer4" || dither === "bayer8") {
      const bias = bayerBias(x, y, dither);
      ar = quantizeChannel(r + bias, rBits);
      ag = quantizeChannel(g + bias, gBits);
      ab = quantizeChannel(b + bias, bBits);
    } else {
      ar = quantizeChannel(r, rBits);
      ag = quantizeChannel(g, gBits);
      ab = quantizeChannel(b, bBits);
    }

    return [ar, ag, ab];
  }

  function bayerBias(x, y, mode) {
    const m = mode === "bayer4" ? BAYER4 : BAYER8;
    const n = m.length;
    const t = m[y % n][x % n];
    // map [0, n*n) to roughly -threshold .. +threshold
    const max = n * n;
    return ((t + 0.5) / max - 0.5) * (255 / Math.max(2, 1 << 3));
  }

  function bayerThreshold(x, y, mode) {
    const m = mode === "bayer4" ? BAYER4 : BAYER8;
    const n = m.length;
    const t = m[y % n][x % n];
    return ((t + 0.5) / (n * n)) * 255;
  }

  function processImage() {
    if (!state.image) return;

    const ow = clamp(Math.round(state.outW) || 240, 1, 512);
    const oh = clamp(Math.round(state.outH) || 240, 1, 512);
    state.outW = ow;
    state.outH = oh;

    const c = state.crop;
    cropCanvas.width = Math.max(1, Math.round(c.w));
    cropCanvas.height = Math.max(1, Math.round(c.h));
    cropCtx.clearRect(0, 0, cropCanvas.width, cropCanvas.height);
    cropCtx.imageSmoothingEnabled = true;
    cropCtx.imageSmoothingQuality = "high";
    cropCtx.drawImage(
      state.image,
      c.x,
      c.y,
      c.w,
      c.h,
      0,
      0,
      cropCanvas.width,
      cropCanvas.height
    );

    workCanvas.width = ow;
    workCanvas.height = oh;
    workCtx.clearRect(0, 0, ow, oh);
    workCtx.imageSmoothingEnabled = els.scaleMode.value === "smooth";
    workCtx.imageSmoothingQuality = "high";
    workCtx.drawImage(cropCanvas, 0, 0, ow, oh);

    const imageData = workCtx.getImageData(0, 0, ow, oh);
    const data = imageData.data;
    const dither = els.ditherMode.value;
    const depth = Number(els.colorDepth.value);

    if (dither === "floyd" && depth > 1) {
      floydSteinberg(data, ow, oh, depth);
    } else {
      for (let y = 0; y < oh; y++) {
        for (let x = 0; x < ow; x++) {
          const i = (y * ow + x) * 4;
          const [r, g, b] = applyColorDepth(data[i], data[i + 1], data[i + 2], depth, x, y, dither);
          data[i] = r;
          data[i + 1] = g;
          data[i + 2] = b;
        }
      }
    }

    workCtx.putImageData(imageData, 0, 0);

    // preview (1:1 device)
    els.previewCanvas.width = ow;
    els.previewCanvas.height = oh;
    // CSS size follows aspect, max side ~240px like a real 1.54" panel
    const maxSide = Math.max(ow, oh);
    const cssScale = 240 / maxSide;
    els.previewCanvas.style.width = `${Math.round(ow * cssScale)}px`;
    els.previewCanvas.style.height = `${Math.round(oh * cssScale)}px`;
    previewCtx.imageSmoothingEnabled = false;
    previewCtx.clearRect(0, 0, ow, oh);
    previewCtx.drawImage(workCanvas, 0, 0);

    // zoom preview
    const zoom = Number(els.previewZoom.value) || 2;
    const zw = ow;
    const zh = oh;
    els.zoomCanvas.width = zw * zoom;
    els.zoomCanvas.height = zh * zoom;
    zoomCtx.imageSmoothingEnabled = false;
    zoomCtx.clearRect(0, 0, els.zoomCanvas.width, els.zoomCanvas.height);
    zoomCtx.drawImage(workCanvas, 0, 0, ow, oh, 0, 0, ow * zoom, zh * zoom);

    // keep CSS size reasonable on small screens
    const displayZoomCss = Math.min(zoom, 3);
    els.zoomCanvas.style.width = `${Math.min(360, ow * displayZoomCss)}px`;
    els.zoomCanvas.style.height = `${Math.min(360, oh * displayZoomCss)}px`;

    updateStats(ow, oh);
    updateCropInfo();
    state.dirty = false;
  }

  function floydSteinberg(data, w, h, depth) {
    // work in float RGB
    const buf = new Float32Array(w * h * 3);
    for (let i = 0, p = 0; i < data.length; i += 4, p += 3) {
      buf[p] = data[i];
      buf[p + 1] = data[i + 1];
      buf[p + 2] = data[i + 2];
    }

    const bits =
      depth === 16 ? [5, 6, 5] :
      depth === 15 ? [5, 5, 5] :
      depth === 12 ? [4, 4, 4] :
      depth === 8 ? [3, 3, 2] :
      [8, 8, 8];

    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const p = (y * w + x) * 3;
        const oldR = buf[p];
        const oldG = buf[p + 1];
        const oldB = buf[p + 2];
        const nR = quantizeChannel(oldR, bits[0]);
        const nG = quantizeChannel(oldG, bits[1]);
        const nB = quantizeChannel(oldB, bits[2]);
        buf[p] = nR;
        buf[p + 1] = nG;
        buf[p + 2] = nB;
        const errR = oldR - nR;
        const errG = oldG - nG;
        const errB = oldB - nB;

        spread(buf, w, h, x + 1, y, errR, errG, errB, 7 / 16);
        spread(buf, w, h, x - 1, y + 1, errR, errG, errB, 3 / 16);
        spread(buf, w, h, x, y + 1, errR, errG, errB, 5 / 16);
        spread(buf, w, h, x + 1, y + 1, errR, errG, errB, 1 / 16);
      }
    }

    for (let i = 0, p = 0; i < data.length; i += 4, p += 3) {
      data[i] = clamp(Math.round(buf[p]), 0, 255);
      data[i + 1] = clamp(Math.round(buf[p + 1]), 0, 255);
      data[i + 2] = clamp(Math.round(buf[p + 2]), 0, 255);
    }
  }

  function spread(buf, w, h, x, y, er, eg, eb, factor) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    const p = (y * w + x) * 3;
    buf[p] += er * factor;
    buf[p + 1] += eg * factor;
    buf[p + 2] += eb * factor;
  }

  function updateStats(ow, oh) {
    const pixels = ow * oh;
    const bytes = pixels * 2; // RGB565
    els.statSize.textContent = `${ow}×${oh}`;
    els.statPixels.textContent = formatNum(pixels);
    els.statBytes.textContent = formatNum(bytes);
    els.previewBadge.textContent = `${ow}×${oh} · RGB565`;
    els.zoomLabel.textContent = `${els.previewZoom.value}×`;

    const labelOpt = els.screenPreset.selectedOptions?.[0]?.text;
    const screenName =
      els.screenPreset.value === "custom" || !labelOpt
        ? `自定义 ${ow}×${oh}`
        : `${labelOpt.replace(/·.*/, "").trim()} · ${ow}×${oh}`;
    els.deviceLabel.textContent = `${screenName} SPI`;

    const arrName = `img_${ow}x${oh}`;
    els.usageSnippet.textContent = [
      `// Arduino + TFT_eSPI / Adafruit_ST7789`,
      `// 将导出的 ${arrName}.h 放到 sketch 同目录，再：`,
      `// tft.pushImage(0, 0, ${ow}, ${oh}, ${arrName});`,
      ``,
      `// ESP-IDF / LVGL 可直接用 .bin：`,
      `// esp_lcd_panel_draw_bitmap(panel, 0, 0, ${ow}, ${oh}, buf);`,
    ].join("\n");
  }

  function updateCropInfo() {
    if (!state.image) {
      els.cropInfo.textContent = "—";
      return;
    }
    const c = state.crop;
    els.cropInfo.textContent =
      `源 ${state.imageWidth}×${state.imageHeight} · 裁剪 ${Math.round(c.w)}×${Math.round(c.h)}`;
  }

  let processScheduled = false;
  function requestProcess() {
    if (processScheduled) return;
    processScheduled = true;
    requestAnimationFrame(() => {
      processScheduled = false;
      processImage();
      drawSource();
    });
  }

  // ---- interaction ----

  function pointerPos(e) {
    const rect = els.sourceCanvas.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }

  function onPointerDown(e) {
    if (!state.image) return;
    els.sourceCanvas.setPointerCapture?.(e.pointerId);
    const p = pointerPos(e);

    if (state.spaceDown || e.button === 1) {
      state.dragMode = "pan";
      state.dragStart = {
        x: p.x,
        y: p.y,
        offsetX: state.offsetX,
        offsetY: state.offsetY,
      };
      return;
    }

    const hit = getHandleHit(p.x, p.y);
    state.dragMode = hit;
    if (!hit) return;

    const c = state.crop;
    state.dragStart = {
      x: p.x,
      y: p.y,
      crop: { ...c },
    };
  }

  function onPointerMove(e) {
    if (!state.image) return;
    const p = pointerPos(e);

    if (!state.dragMode) {
      const hit = getHandleHit(p.x, p.y);
      const cursors = {
        move: "move",
        "resize-nw": "nwse-resize",
        "resize-ne": "nesw-resize",
        "resize-sw": "nesw-resize",
        "resize-se": "nwse-resize",
      };
      els.dropZone.style.cursor = state.spaceDown
        ? "grab"
        : cursors[hit] || "crosshair";
      return;
    }

    if (state.dragMode === "pan" && state.dragStart) {
      state.offsetX = state.dragStart.offsetX + (p.x - state.dragStart.x);
      state.offsetY = state.dragStart.offsetY + (p.y - state.dragStart.y);
      drawSource();
      return;
    }

    if (!state.dragStart) return;
    const start = state.dragStart;
    const dxImg = (p.x - start.x) / state.scale;
    const dyImg = (p.y - start.y) / state.scale;
    const iw = state.imageWidth;
    const ih = state.imageHeight;
    // 勾选「锁定输出比例」时，裁剪框始终贴合屏幕宽高比
    const lock = els.lockSquare.checked
      ? state.outW / state.outH
      : null;

    let { x, y, w, h } = start.crop;

    if (state.dragMode === "move") {
      x = clamp(x + dxImg, 0, iw - w);
      y = clamp(y + dyImg, 0, ih - h);
    } else {
      // resize: optionally enforce target aspect ratio
      const minSide = 8;
      let anchorX;
      let anchorY;
      let newW;
      let newH;

      if (state.dragMode === "resize-se") {
        anchorX = start.crop.x;
        anchorY = start.crop.y;
        newW = start.crop.w + dxImg;
        newH = start.crop.h + dyImg;
      } else if (state.dragMode === "resize-ne") {
        anchorX = start.crop.x + start.crop.w;
        anchorY = start.crop.y;
        newW = start.crop.w - dxImg;
        newH = start.crop.h + dyImg;
      } else if (state.dragMode === "resize-sw") {
        anchorX = start.crop.x + start.crop.w;
        anchorY = start.crop.y + start.crop.h;
        newW = start.crop.w - dxImg;
        newH = start.crop.h - dyImg;
      } else {
        // nw
        anchorX = start.crop.x + start.crop.w;
        anchorY = start.crop.y + start.crop.h;
        newW = start.crop.w - dxImg;
        newH = start.crop.h - dyImg;
      }

      newW = Math.max(minSide, newW);
      newH = Math.max(minSide, newH);

      if (lock) {
        // use dominant axis, enforce aspect
        const sizeFromW = newW;
        const sizeFromH = newH * lock;
        let sideW = Math.max(sizeFromW, sizeFromH);
        sideW = Math.max(minSide, sideW);
        newW = sideW;
        newH = sideW / lock;
      }

      // clamp inside image
      const fit = (wLim, hLim) => {
        newW = Math.min(newW, wLim);
        newH = Math.min(newH, hLim);
        if (lock) {
          if (newW / lock <= newH) newH = newW / lock;
          else newW = newH * lock;
        }
      };

      if (state.dragMode === "resize-se") {
        fit(iw - anchorX, ih - anchorY);
        x = anchorX;
        y = anchorY;
      } else if (state.dragMode === "resize-ne") {
        fit(anchorX, ih - anchorY);
        x = anchorX - newW;
        y = anchorY;
      } else if (state.dragMode === "resize-sw") {
        fit(anchorX, anchorY);
        x = anchorX - newW;
        y = anchorY - newH;
      } else {
        fit(anchorX, anchorY);
        x = anchorX - newW;
        y = anchorY - newH;
      }

      w = newW;
      h = newH;
    }

    state.crop = { x, y, w, h };
    drawSource();
    requestProcess();
  }

  function onPointerUp(e) {
    state.dragMode = null;
    state.dragStart = null;
    try {
      els.sourceCanvas.releasePointerCapture?.(e.pointerId);
    } catch (_) {
      /* ignore */
    }
  }

  function onWheel(e) {
    if (!state.image) return;
    e.preventDefault();
    const p = pointerPos(e);
    const img = viewToImage(p.x, p.y);
    const factor = e.deltaY < 0 ? 1.1 : 1 / 1.1;
    state.scale = clamp(state.scale * factor, 0.05, 16);
    state.offsetX = p.x - img.x * state.scale;
    state.offsetY = p.y - img.y * state.scale;
    drawSource();
  }

  // ---- exports ----

  function getProcessedCanvas() {
    return workCanvas;
  }

  function downloadBlob(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
  }

  function exportPng() {
    const canvas = getProcessedCanvas();
    canvas.toBlob((blob) => {
      if (!blob) return;
      downloadBlob(blob, `st7789_${state.outW}x${state.outH}.png`);
    }, "image/png");
  }

  function exportBin() {
    const canvas = getProcessedCanvas();
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    const { width: w, height: h } = canvas;
    const data = ctx.getImageData(0, 0, w, h).data;
    const out = new Uint16Array(w * h);
    for (let i = 0, p = 0; i < data.length; i += 4, p++) {
      out[p] = toRGB565(data[i], data[i + 1], data[i + 2]);
    }
    // little-endian raw
    const bytes = new Uint8Array(out.buffer);
    downloadBlob(new Blob([bytes], { type: "application/octet-stream" }), `st7789_${w}x${h}.bin`);
  }

  function exportCArray() {
    const canvas = getProcessedCanvas();
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    const { width: w, height: h } = canvas;
    const data = ctx.getImageData(0, 0, w, h).data;
    const name = `img_${w}x${h}`;
    const lines = [];
    lines.push(`// Generated by Pixel ST7789`);
    lines.push(`// ${w}x${h} RGB565, ${w * h * 2} bytes`);
    lines.push(`// pushImage(0, 0, ${w}, ${h}, ${name});`);
    lines.push(`#include <stdint.h>`);
    lines.push(``);
    lines.push(`const uint16_t ${name}[${w * h}] PROGMEM = {`);

    let row = [];
    for (let y = 0; y < h; y++) {
      row = [];
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const v = toRGB565(data[i], data[i + 1], data[i + 2]);
        row.push(`0x${v.toString(16).padStart(4, "0")}`);
      }
      lines.push(`  ${row.join(", ")},`);
    }
    lines.push(`};`);
    lines.push(``);

    const text = lines.join("\n");
    downloadBlob(new Blob([text], { type: "text/plain;charset=utf-8" }), `${name}.h`);
  }

  // ---- events ----

  els.fileInput.addEventListener("change", (e) => {
    const file = e.target.files?.[0];
    loadImageFile(file);
    e.target.value = "";
  });

  ["dragenter", "dragover"].forEach((type) => {
    els.dropZone.addEventListener(type, (e) => {
      e.preventDefault();
      els.dropZone.classList.add("dragover");
    });
  });

  ["dragleave", "drop"].forEach((type) => {
    els.dropZone.addEventListener(type, (e) => {
      e.preventDefault();
      els.dropZone.classList.remove("dragover");
    });
  });

  els.dropZone.addEventListener("drop", (e) => {
    const file = e.dataTransfer?.files?.[0];
    loadImageFile(file);
  });

  els.sourceCanvas.addEventListener("pointerdown", onPointerDown);
  els.sourceCanvas.addEventListener("pointermove", onPointerMove);
  els.sourceCanvas.addEventListener("pointerup", onPointerUp);
  els.sourceCanvas.addEventListener("pointercancel", onPointerUp);
  els.sourceCanvas.addEventListener("wheel", onWheel, { passive: false });

  window.addEventListener("keydown", (e) => {
    if (e.code === "Space" && e.target === document.body) {
      state.spaceDown = true;
      els.dropZone.style.cursor = "grab";
      e.preventDefault();
    }
  });
  window.addEventListener("keyup", (e) => {
    if (e.code === "Space") {
      state.spaceDown = false;
      els.dropZone.style.cursor = "crosshair";
    }
  });

  els.screenPreset.addEventListener("change", () => {
    const v = els.screenPreset.value;
    if (v === "custom") {
      els.outW.focus();
      return;
    }
    const [w, h] = v.split("x").map(Number);
    els.outW.value = w;
    els.outH.value = h;
    state.outW = w;
    state.outH = h;
    if (state.image) applyTargetAspectToCrop();
    requestProcess();
  });

  function syncOutputsFromInputs() {
    state.outW = clamp(Math.round(Number(els.outW.value) || 240), 1, 512);
    state.outH = clamp(Math.round(Number(els.outH.value) || 240), 1, 512);
    els.outW.value = state.outW;
    els.outH.value = state.outH;
    if (state.image) applyTargetAspectToCrop();
    requestProcess();
  }

  els.outW.addEventListener("change", () => {
    els.screenPreset.value = "custom";
    const key = `${els.outW.value}x${els.outH.value}`;
    const option = [...els.screenPreset.options].find((o) => o.value === key);
    if (option) els.screenPreset.value = key;
    syncOutputsFromInputs();
  });

  els.outH.addEventListener("change", () => {
    els.screenPreset.value = "custom";
    const key = `${els.outW.value}x${els.outH.value}`;
    const option = [...els.screenPreset.options].find((o) => o.value === key);
    if (option) els.screenPreset.value = key;
    syncOutputsFromInputs();
  });

  ["scaleMode", "ditherMode", "colorDepth"].forEach((id) => {
    els[id].addEventListener("change", requestProcess);
  });

  els.previewZoom.addEventListener("input", () => {
    els.zoomLabel.textContent = `${els.previewZoom.value}×`;
    requestProcess();
  });

  els.showGrid.addEventListener("change", () => {
    els.gridOverlay.hidden = !els.showGrid.checked;
  });

  els.lockSquare.addEventListener("change", () => {
    if (state.image) applyTargetAspectToCrop();
    requestProcess();
  });

  els.btnFit.addEventListener("click", () => {
    fitView();
    requestProcess();
  });

  els.btnResetCrop.addEventListener("click", () => {
    resetCropToSquare();
    fitView();
    requestProcess();
  });

  els.btnExportPng.addEventListener("click", exportPng);
  els.btnExportC.addEventListener("click", exportCArray);
  els.btnExportBin.addEventListener("click", exportBin);

  window.addEventListener("resize", () => {
    resizeSourceCanvas();
  });

  // init
  setEnabled(false);
  updateStats(240, 240);
})();
