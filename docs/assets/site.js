(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const desk = $('desk'), box = $('screenBox'), root = document.documentElement;
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const W = 1280, HH = 800;
  const clamp = (v, a, b) => v < a ? a : v > b ? b : v;
  const smooth = (a, b, x) => { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
  const svgNS = 'http://www.w3.org/2000/svg';
  const el = (tag, cls, parent, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; if (parent) parent.appendChild(e); return e; };

  /* ── escala: el monitor virtual de 1280×800 cabe en la caja ── */
  let S = 1;
  const fit = () => { S = box.clientWidth / W; desk.style.transform = `scale(${S})`; };
  new ResizeObserver(fit).observe(box); fit();

  /* ── el muelle de la app: a = (t − x)·k − v·2√k·ζ, Euler semiimplícito en 2 subpasos ── */
  const ZETA = { soft: .85, normal: .62, bouncy: .40 };
  const st = { mat: 'oled', bounce: 'normal', hide: 'half', sound: false, vol: 55, sIdx: 0, obf: false, wifi: true, bt: true, bar: true, muted: false, volume: .7, bright: 1, charging: false, batt: 64 };
  const Z = () => ZETA[st.bounce];
  const spring = (o, key, target, k, z, dt, eps = .001) => {
    const vk = key + 'V'; let x = o[key], v = o[vk] || 0; const h = dt / 2, c = 2 * Math.sqrt(k) * z;
    for (let i = 0; i < 2; i++) { v += ((target - x) * k - v * c) * h; x += v * h; }
    if (Math.abs(target - x) < eps && Math.abs(v) < eps * 10) { x = target; v = 0; }
    o[key] = x; o[vk] = v; return x !== target;
  };
  const approach = (x, t, rate, dt) => { const n = x + (t - x) * (1 - Math.exp(-rate * dt)); return Math.abs(n - t) < .002 ? t : n; };

  /* ── puntero en coordenadas del monitor ── */
  const P = { x: -1, y: -1, in: false, still: 0, lx: -1, ly: -1 };
  desk.addEventListener('pointermove', e => {
    const r = desk.getBoundingClientRect(); P.x = (e.clientX - r.left) / S; P.y = (e.clientY - r.top) / S; P.in = true;
    if (Math.hypot(P.x - P.lx, P.y - P.ly) > 4) { P.still = performance.now(); P.lx = P.x; P.ly = P.y; }
    wake();
  });
  desk.addEventListener('pointerleave', () => { P.in = false; P.x = P.y = -1; wake(); });
  const barH = () => st.bar ? 28 : 0;

  /* ── apps de ejemplo ── */
  const APPS = [
    { n: 'Explorador', c: ['#FFD25E', '#E08A1E'], g: '' , run: true },
    { n: 'Navegador', c: ['#5AB0FF', '#1D5FD1'], g: 'N' },
    { n: 'Mensajes', c: ['#45D483', '#118057'], g: 'M' },
    { n: 'Correo', c: ['#6FA8FF', '#3157D6'], g: 'C' },
    { n: 'Calendario', c: ['#FF8A65', '#D9452B'], g: '2' },
    { n: 'Música', c: ['#FF6B95', '#B6245A'], g: '♪' },
    { n: 'Editor', c: ['#A98BFF', '#5B3BD6'], g: '{ }' },
    { n: 'Fotos', c: ['#FFB547', '#E0602B'], g: '✿' },
    { n: 'Terminal', c: ['#4A4F5C', '#1E2129'], g: '>_' },
  ];
  const grad = a => `linear-gradient(160deg,${a.c[0]},${a.c[1]})`;
  let front = 0;

  /* ═════════════ barra superior ═════════════ */
  const barRight = $('barRight');
  const mkSvg = (w, h, vb = '0 0 24 24') => { const s = document.createElementNS(svgNS, 'svg'); s.setAttribute('viewBox', vb); s.setAttribute('width', w); s.setAttribute('height', h); return s; };
  const sv = (parent, tag, attrs) => { const e = document.createElementNS(svgNS, tag); for (const k in attrs) e.setAttribute(k, attrs[k]); parent.appendChild(e); return e; };
  const arcPath = (cx, cy, r, a0, a1) => { const p = a => [cx + r * Math.cos(a * Math.PI / 180), cy + r * Math.sin(a * Math.PI / 180)];
    const [x0, y0] = p(a0), [x1, y1] = p(a1); return `M${x0.toFixed(2)} ${y0.toFixed(2)}A${r} ${r} 0 0 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`; };
  // cada icono: ancho de su tinta, con la caja de 24 unidades centrada (los valores de la app)
  const icons = {};
  const mkIcon = (name, inkW, boxPx, gapLeft) => {
    const b = el('button', 'bi', barRight); b.style.width = inkW + 'px'; b.style.height = '28px'; b.style.position = 'relative'; b.style.marginLeft = gapLeft + 'px';
    const s = mkSvg(boxPx, boxPx); s.style.position = 'absolute'; s.style.left = (inkW - boxPx) / 2 + 'px'; s.style.top = (28 - boxPx) / 2 + 'px'; b.appendChild(s);
    const o = { b, s, sc: 1, scV: 0, hot: false, down: false };
    b.addEventListener('pointerenter', () => { o.hot = true; wake(); });
    b.addEventListener('pointerleave', () => { o.hot = false; o.down = false; wake(); });
    b.addEventListener('pointerdown', () => { o.down = true; wake(); });
    b.addEventListener('pointerup', () => { o.down = false; wake(); });
    icons[name] = o; return o;
  };
  // izquierda a derecha: chevrón 16 sonido 16 wifi 16 batería 18 hora 18 engranaje
  const chev = mkIcon('chev', 14.5, 25.4, 0);
  sv(chev.s, 'path', { d: 'M6.5 14.5L12 9l5.5 5.5', fill: 'none', stroke: 'var(--barfg2)', 'stroke-width': 2.7, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' });
  const snd = mkIcon('snd', 17.1, 21.9, 16);
  const sndG = sv(snd.s, 'g', { fill: 'currentColor' });
  sv(sndG, 'path', { d: 'M2.4 9.6Q2.4 8.4 3.6 8.4H6.3L10.2 4.9Q11.4 3.9 11.4 5.4V18.6Q11.4 20.1 10.2 19.1L6.3 15.6H3.6Q2.4 15.6 2.4 14.4Z' });
  const arc1 = sv(snd.s, 'path', { fill: 'none', stroke: 'currentColor', 'stroke-width': 2.6, 'stroke-linecap': 'round' });
  const arc2 = sv(snd.s, 'path', { fill: 'none', stroke: 'currentColor', 'stroke-width': 2.6, 'stroke-linecap': 'round' });
  const xg = sv(snd.s, 'g', { stroke: 'currentColor', 'stroke-width': 2.2, 'stroke-linecap': 'round' });
  sv(xg, 'path', { d: 'M14.8 9.6L19.6 14.4M19.6 9.6L14.8 14.4' });
  const wifi = mkIcon('wifi', 12.8, 25.3, 16);
  const wg = sv(wifi.s, 'g', { transform: 'rotate(35 12 12) rotate(15 12 15)' });
  const wdot = sv(wg, 'circle', { cx: 12, cy: 19.4, r: 1.7, fill: 'currentColor' });
  const wa1 = sv(wg, 'path', { d: arcPath(12, 20, 5.2, -135, -45), fill: 'none', stroke: 'currentColor', 'stroke-width': 2.6, 'stroke-linecap': 'round' });
  const wa2 = sv(wg, 'path', { d: arcPath(12, 20, 9.6, -135, -45), fill: 'none', stroke: 'currentColor', 'stroke-width': 2.6, 'stroke-linecap': 'round' });
  const batt = mkIcon('batt', 21.8, 22.8, 16);
  sv(batt.s, 'rect', { x: 1.5, y: 6.5, width: 19, height: 11, rx: 3.8, fill: 'none', stroke: 'currentColor', 'stroke-width': 1.7 });
  sv(batt.s, 'path', { d: 'M22.6 10.3V13.7', stroke: 'currentColor', 'stroke-width': 2, 'stroke-linecap': 'round', opacity: .9 });
  const bfill = sv(batt.s, 'rect', { x: 3.35, y: 8.35, height: 7.3, rx: 2, fill: 'currentColor' });
  const bolt = sv(batt.s, 'path', { d: 'M12.6 7.2L8.4 12.6H11.2L10.2 16.8L14.4 11.2H11.6Z', fill: '#fff', stroke: 'var(--barbg)', 'stroke-width': .7 });
  const clock = el('span', 'clock', barRight); clock.style.marginLeft = '18px';
  const gear = mkIcon('gear', 13.9, 16.3, 18);
  (() => { // engranaje de 6 dientes, hueco de radio 3, girado −30°
    let d = ''; const n = 6, ro = 10.6, ri = 8.0;
    for (let i = 0; i < n * 4; i++) { const a = (i / (n * 4)) * Math.PI * 2 - Math.PI / 2 - Math.PI / 6 + Math.PI / (n * 4);
      const r = (i % 4 === 0 || i % 4 === 1) ? ro : ri; d += (i ? 'L' : 'M') + (12 + r * Math.cos(a)).toFixed(2) + ' ' + (12 + r * Math.sin(a)).toFixed(2); }
    d += 'ZM15 12A3 3 0 1 0 9 12A3 3 0 1 0 15 12Z';
    sv(gear.s, 'path', { d, fill: 'currentColor', 'fill-rule': 'evenodd', 'stroke-linejoin': 'round' });
  })();
  const B = { am: 0, amV: 0, vl: 2, vlV: 0, rip: 0, ripV: 0, wl: 3, wlV: 0, bc: 0, bcV: 0, bf: 64, bfV: 0 };
  const bump = (o, v) => { o.scV = (o.scV || 0) + v; wake(); };
  const tickClock = () => { const d = new Date();
    let day = d.toLocaleDateString('es', { weekday: 'short', day: 'numeric', month: 'short' }).replace(/\./g, '').replace(',', '');
    day = day.charAt(0).toUpperCase() + day.slice(1);
    clock.textContent = day + '  ' + d.toLocaleTimeString('es', { hour: '2-digit', minute: '2-digit' }); };
  tickClock(); setInterval(tickClock, 15000);
  const setFrontIcon = () => { const a = APPS[front]; $('barIco').style.background = grad(a); $('barApp').textContent = a.n; };
  setFrontIcon();

  snd.b.onclick = () => { setMute(!st.muted); };
  const setMute = m => { st.muted = m; bump(snd, 2.6); if (typeof POP !== 'undefined') POP.vV += 2.4; syncCC(); wake(); };
  wifi.b.onclick = () => toggleCC();
  batt.b.onclick = () => toggleCC();
  gear.b.onclick = () => toggleCC();
  chev.b.onclick = () => bump(chev, 1.6);
  clock.onclick = () => openCenter(N.mode !== 'center');
  const setWifi = on => { st.wifi = on; bump(wifi, on ? 2.2 : -1.6); syncCC(); };
  const setCharging = on => { st.charging = on; bump(batt, on ? 2.6 : -1.4); };
  setInterval(() => { st.batt = clamp(st.batt + (st.charging ? .6 : -.04), 5, 100); wake(); }, 1000);

  const frameBar = dt => {
    let m = false; const z = Z();
    m |= spring(B, 'am', st.muted ? 1 : 0, 380, z, dt);
    m |= spring(B, 'vl', st.volume < .5 ? 1 : 2, 200, Math.max(z, .8), dt);
    m |= spring(B, 'rip', 0, 330, .3, dt);
    m |= spring(B, 'wl', st.wifi ? 3 : 0, 70, Math.max(z, .85), dt);
    m |= spring(B, 'bc', st.charging ? 1 : 0, 360, z, dt);
    m |= spring(B, 'bf', st.batt, 40, 1, dt, .05);
    // sonido: arcos que se encogen al silenciar y una X que entra girando
    const r1 = (4.6 + B.rip * .55) * (1 - .45 * B.am), r2 = (9.0 + B.rip) * (1 - .45 * B.am);
    arc1.setAttribute('d', arcPath(9.6, 12, Math.max(.1, r1), -45, 45)); arc2.setAttribute('d', arcPath(9.6, 12, Math.max(.1, r2), -45, 45));
    arc1.setAttribute('opacity', clamp(1 - B.am, 0, 1)); arc2.setAttribute('opacity', clamp((1 - B.am) * clamp(B.vl - 1, 0, 1), 0, 1));
    const xs = clamp(B.am, 0, 1.3); xg.setAttribute('transform', `translate(17.2 12) rotate(${-90 * (1 - B.am)}) scale(${xs}) translate(-17.2 -12)`); xg.setAttribute('opacity', clamp(B.am * 1.5, 0, 1));
    // wifi: punto y dos arcos que se encienden en cascada
    [wdot, wa1, wa2].forEach((p, i) => p.setAttribute('opacity', .28 + .72 * clamp(B.wl - i, 0, 1)));
    // batería
    const pct = B.bf, low = pct <= 20;
    bfill.setAttribute('width', Math.max(0, 4.75 + 13.9 * pct / 100 - 3.35).toFixed(2));
    bfill.style.fill = low && !st.charging ? '#FF453A' : `color-mix(in srgb, #30D158 ${Math.round(clamp(B.bc, 0, 1) * 100)}%, var(--barfg))`;
    bolt.setAttribute('transform', `translate(11.4 12) scale(${clamp(B.bc, 0, 1.3)}) translate(-11.4 -12)`); bolt.setAttribute('opacity', clamp(B.bc * 2, 0, 1));
    // pasar el cursor: 1,16; pulsar: 0,88 (k 520)
    for (const k in icons) { const o = icons[k]; m |= spring(o, 'sc', o.down ? .88 : o.hot ? 1.16 : 1, 520, z, dt);
      o.b.style.transform = `scale(${clamp(o.sc, .8, 1.3).toFixed(4)})`; }
    $('dbar').style.transform = `translateY(${(st.bar ? 0 : -28)}px)`;
    return m;
  };

  /* ═════════════ notch ═════════════ */
  const NOTES = [
    { app: 'Mensajes', t: 'Lucía', b: '¿Comemos a las 2? Hay mesa en el de siempre y luego vamos por café.', c: ['#45D483', '#118057'], g: 'L', photo: true, acts: ['Responder', 'Marcar como leído'] },
    { app: 'Calendario', t: 'Revisión de diseño', b: 'Hoy a las 15:00 · Sala 2', c: ['#FF8A65', '#D9452B'], g: '2', acts: ['Posponer'] },
    { app: 'foro.dev', t: 'Marcos respondió a tu hilo', b: '“Probé tu build en mi laptop y va finísimo, ¿lo subes a la Store?”', c: ['#A98BFF', '#5B3BD6'], g: 'M', photo: true, site: '#2F80ED', acts: ['Responder'] },
    { app: 'Captura copiada', t: 'Captura copiada', b: '1920 × 1080 · clic para abrir', c: ['#5AB0FF', '#1D5FD1'], g: '✓' },
    { app: 'Correo', t: 'Factura de octubre', b: 'Tu factura ya está disponible para descargar.', c: ['#6FA8FF', '#3157D6'], g: 'C', acts: ['Archivar'] },
  ];
  const nNotch = $('nNotch'), nContent = $('nContent');
  const N = { mode: 'hidden', w: 0, h: 0, wV: 0, hV: 0, x: 640, xV: 0, alpha: 0, until: 0, cards: [], scroll: 0, unseen: 0, hoverCard: -1, hoverSince: 0, mini: 0 };
  const OLDER = [
    { app: 'Correo', t: 'Reunión movida', b: 'La revisión trimestral pasa al jueves a las 10:00.', c: ['#6FA8FF', '#3157D6'], g: 'C', m: 6, acts: ['Archivar'] },
    { app: 'Música', t: 'Tu mezcla de la semana', b: '30 canciones nuevas para ti', c: ['#FF6B95', '#B6245A'], g: '♪', m: 14 },
    { app: 'Mensajes', t: 'Ana', b: 'Te mandé las fotos del viaje, están en la carpeta compartida.', c: ['#45D483', '#118057'], g: 'A', photo: true, m: 27, acts: ['Responder'] },
    { app: 'Calendario', t: 'Cumpleaños de Pablo', b: 'Mañana · todo el día', c: ['#FF8A65', '#D9452B'], g: '2', m: 52 },
    { app: 'tienda.mx', t: 'Tu pedido va en camino', b: 'Llega hoy entre las 14:00 y las 18:00.', c: ['#FFB547', '#E0602B'], g: 'T', site: '#E0602B', m: 95 },
    { app: 'Fotos', t: 'Recuerdos de hace un año', b: 'Mira lo que hacías un día como hoy.', c: ['#FFB547', '#E0602B'], g: '✿', m: 140 },
    { app: 'Correo', t: 'Boletín de diseño', b: 'Las tendencias de interfaz de este mes, en cinco minutos.', c: ['#6FA8FF', '#3157D6'], g: 'C', m: 210 },
    { app: 'Mensajes', t: 'Grupo Familia', b: 'Papá: ¿quién trae el postre el domingo?', c: ['#45D483', '#118057'], g: 'F', photo: true, m: 300, acts: ['Responder'] },
  ];
  let k = 0, notes = OLDER.map((n, i) => ({ ...n, id: 'o' + i, at: Date.now() - n.m * 60000 }));
  const icon18 = n => `<span class="ic18${n.photo ? ' photo' : ''}" style="background:${grad(n)}">${n.g}${n.site ? `<i class="bdg" style="background:${n.site}"></i>` : ''}</span>`;
  const target = () => {
    switch (N.mode) {
      case 'peek': return [360, 54]; case 'bell': return [112, 34]; case 'mini': return [110, 9]; case 'quick': return [272, 44];
      case 'center': return [384, 56 + listH() + 14];
    }
    return [N.w > 0 ? N.w : 360 * .45, 0];
  };
  const listH = () => { const n = N.cards.length; if (!n) return 120;
    const all = N.cards.reduce((a, c) => a + 70 + c.ex + 12, 0) - 12 + 18; return Math.min(all, 6 * 82 - 12 + 18); };
  const contentH = () => N.cards.reduce((a, c) => a + 70 + c.ex + 12, 0) - 12 + 18;
  const setContent = html => { nContent.innerHTML = html; N.contentAlpha = 0; };
  const show = (mode, html) => {
    const was = N.mode; N.mode = mode;
    if (html != null) setContent(html);
    if (was !== 'hidden' && was !== mode) N.wV += target()[0] * 1.2;     // cambia el contenido abierto: un empujón de anchura
    if (was === 'hidden') { N.w = target()[0] * .45; N.h = 0; N.wV = N.hV = 0; }
    wake();
  };
  const hide = () => { N.mode = 'hidden'; wake(); };
  const arrive = () => {
    if (N.mode === 'center' || N.mode === 'quick') return;
    const n = { ...NOTES[k % NOTES.length], id: Date.now() + Math.random(), at: Date.now() }; k++;
    notes.unshift(n); if (notes.length > 12) notes.pop(); N.unseen++;
    if (st.sound) play();
    if (st.obf) {
      show('bell', `<div class="bellv"><svg viewBox="0 0 20 20" fill="currentColor"><path d="M10 2.4a5.2 5.2 0 0 0-5.2 5.2v3L3.3 13.6h13.4L15.2 10.6v-3A5.2 5.2 0 0 0 10 2.4zM8 15.2a2 2 0 0 0 4 0z"/></svg><span class="cnt">${N.unseen}</span></div>`);
      N.bellT = performance.now() + 260; N.until = performance.now() + 3500;
    } else {
      show('peek', `<div class="peek"><span class="ib">${icon18(n)}</span><span class="t">${n.t}</span><span class="tm">ahora</span><span class="b">${n.b}</span></div>`);
      N.until = performance.now() + 4500;
    }
  };
  nNotch.addEventListener('pointerleave', () => { if (N.mode === 'peek' || N.mode === 'bell') N.until = Math.max(N.until, performance.now() + 1200); });
  nNotch.addEventListener('click', e => {
    if (N.mode === 'peek' || N.mode === 'bell' || N.mode === 'quick') { openCenter(true); e.stopPropagation(); }
  });
  const ago = t => { const m = Math.floor((Date.now() - t) / 60000); return m < 1 ? 'ahora' : m < 60 ? m + ' min' : Math.floor(m / 60) + ' h'; };

  /* centro de notificaciones */
  const openCenter = on => {
    if (!on) { hide(); return; }
    closeCC(); N.unseen = 0;
    N.cards = notes.map(n => ({ n, ex: 0, exV: 0, sc: 1, scV: 0, tw: 0, twV: 0, out: 0, rise: 0, riseV: 0 })); N.scroll = N.scrollT = 0; N.scrollV = 0;
    const html = `<div class="center"><div class="hd"><span class="t">Notificaciones</span>
        <button class="hb txt" id="cClear" style="right:${18 + 32 + 8 + 32 + 8}px">Borrar</button>
        <button class="hb" id="cMute" style="right:${18 + 32 + 8}px" aria-label="Silenciar"><svg viewBox="0 0 20 20" fill="currentColor"><path d="M10 2.4a5.2 5.2 0 0 0-5.2 5.2v3L3.3 13.6h13.4L15.2 10.6v-3A5.2 5.2 0 0 0 10 2.4zM8 15.2a2 2 0 0 0 4 0z"/></svg></button>
        <button class="hb" id="cGear" style="right:18px" aria-label="Ajustes"><svg viewBox="0 0 24 24" fill="currentColor"><path d="M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zm9.4 5.5-2-.4a7.6 7.6 0 0 0 0-2.2l2-.4-1-3.4-2 .7a7.7 7.7 0 0 0-1.6-1.6l.7-2-3.4-1-.4 2a7.6 7.6 0 0 0-2.2 0l-.4-2-3.4 1 .7 2A7.7 7.7 0 0 0 5 6.4l-2-.7-1 3.4 2 .4a7.6 7.6 0 0 0 0 2.2l-2 .4 1 3.4 2-.7a7.7 7.7 0 0 0 1.6 1.6l-.7 2 3.4 1 .4-2a7.6 7.6 0 0 0 2.2 0l.4 2 3.4-1-.7-2a7.7 7.7 0 0 0 1.6-1.6l2 .7z"/></svg></button></div>
      <div class="list" id="cList"></div></div>`;
    show('center', html);
    const list = $('cList');
    if (!N.cards.length) el('div', 'empty', list, 'Sin notificaciones');
    N.cards.forEach((c, i) => {
      const n = c.n, d = el('div', 'ncard', list, `<span class="ib">${icon18(n)}</span><span class="ap">${n.app}</span><span class="tm">${ago(n.at)}</span>
        <span class="t">${n.t}</span><span class="b" style="height:16px;white-space:nowrap;text-overflow:ellipsis">${n.b}</span>
        <div class="acts" style="opacity:0">${(n.acts || []).map(a => `<span>${a}</span>`).join('')}</div>
        <div class="trash"><svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M4 6h12M8 6V4h4v2M6 6l.8 10h6.4L14 6"/></svg></div>`);
      c.el = d; d.style.height = '70px';
      // cuánto crece: el cuerpo completo (hasta 4 líneas) + la fila de botones
      const meas = el('div', '', null, n.b); meas.style.cssText = 'position:absolute;visibility:hidden;width:280px;font-size:12px;line-height:16px;font-family:inherit';
      desk.appendChild(meas); const bodyH = meas.offsetHeight; meas.remove();
      c.full = Math.max(0, Math.min(bodyH, 64) - 16) + (n.acts ? 40 : 0);
      d.addEventListener('pointerenter', () => { N.hoverCard = i; N.hoverSince = performance.now(); wake(); });
      d.addEventListener('pointerleave', () => { if (N.hoverCard === i) N.hoverCard = -1; wake(); });
      d.querySelector('.trash').addEventListener('click', ev => { ev.stopPropagation(); removeCard(c); });
    });
    $('cClear').onclick = ev => { ev.stopPropagation(); const t0 = performance.now(); N.cards.forEach((c, i) => c.outAt = t0 + i * 0.12 / 1.6 * 550); N.clearing = t0; wake(); };
    $('cMute').onclick = ev => { ev.stopPropagation(); st.obf = !st.obf; $('cMute').style.background = st.obf ? 'var(--accent)' : ''; $('cMute').style.color = st.obf ? '#fff' : ''; syncCC(); };
    $('cGear').onclick = ev => { ev.stopPropagation(); hide(); setTimeout(() => { openCC(); ccSection(1); }, 180); };
    if (st.obf) { $('cMute').style.background = 'var(--accent)'; $('cMute').style.color = '#fff'; }
  };
  const removeCard = c => { c.outAt = performance.now(); wake(); };
  desk.addEventListener('pointerdown', e => {
    if (N.mode === 'center' && !nNotch.contains(e.target) && e.target !== clock) hide();
    if (C.open && !$('nCC').contains(e.target) && !Object.values(icons).some(o => o.b.contains(e.target))) closeCC();
  });

  const frameNotch = (dt, now) => {
    let m = false; const z = Z(); const bh = barH();
    // mini notch: tocar la barra (o el borde) dentro de la zona central; quieto 450 ms → vista rápida
    const zone = Math.max(220, W * .18), inZone = P.in && Math.abs(P.x - 640) <= zone && P.x >= 260 && P.x <= W - 260;
    if (N.mode === 'hidden' && inZone && P.y <= bh + 10 && P.y >= 0) show('mini', '');
    if (N.mode === 'mini') {
      if (!inZone || P.y > bh + 26) hide();
      else if (now - P.still > 450 && P.y <= bh + 10) {
        const n = notes.length;
        show('quick', `<div class="quick"><span class="t">Notificaciones</span><span class="mute" style="right:${14 + Math.max(24, 7 * 2 + 8 * String(n).length) + 8}px"><svg viewBox="0 0 20 20" fill="currentColor"><path d="M10 2.4a5.2 5.2 0 0 0-5.2 5.2v3L3.3 13.6h13.4L15.2 10.6v-3A5.2 5.2 0 0 0 10 2.4zM8 15.2a2 2 0 0 0 4 0z"/></svg></span><span class="cnt${n ? '' : ' zero'}">${n}</span></div>`);
      }
    }
    if (N.mode === 'quick') { const l = N.x - N.w / 2 - 28, r = N.x + N.w / 2 + 28; if (!P.in || P.x < l || P.x > r || P.y > bh + N.h + 28) hide(); }
    if ((N.mode === 'peek' || N.mode === 'bell') && now > N.until && !nNotch.matches(':hover')) hide();
    // tamaño y posición
    const [tw, th] = target(); const open = N.mode !== 'hidden';
    if (open) { m |= spring(N, 'w', tw, 380, z, dt, .05); m |= spring(N, 'h', th, 420, Math.min(1, z + .1), dt, .05); }
    else { m |= spring(N, 'w', (N.lastW || 360) * .45, 300, 1, dt, .05); m |= spring(N, 'h', 0, 300, 1, dt, .05); }
    if (open) N.lastW = tw;
    let tx = 640;
    if (N.mode === 'mini' && P.in) { tx = clamp(P.x, 260, W - 260); if (Math.abs(tx - 640) < 64) tx = 640; }
    m |= spring(N, 'x', tx, N.mode === 'mini' ? 240 : 320, z, dt, .05);
    // contenido: entra cuando la altura pasa del 55 %
    const want = open && N.h > .55 * th ? 1 : 0;
    N.contentAlpha = approach(N.contentAlpha || 0, want, want ? 16 : 26, dt); if (N.contentAlpha !== want) m = true;
    const h = Math.max(0, N.h), w = Math.max(0, N.w);
    nNotch.style.display = h < .5 && !open ? 'none' : '';
    nNotch.style.top = bh + 'px';
    nNotch.style.left = (N.x - w / 2).toFixed(2) + 'px'; nNotch.style.width = w.toFixed(2) + 'px'; nNotch.style.height = h.toFixed(2) + 'px';
    const r = Math.min(N.mode === 'center' ? 26 : 16, h / 2), ear = Math.min(7, h / 2);
    nNotch.style.setProperty('--r', r.toFixed(2) + 'px'); nNotch.style.setProperty('--ear', ear.toFixed(2) + 'px');
    nNotch.classList.toggle('live', open && N.mode !== 'mini');
    nContent.style.opacity = N.contentAlpha.toFixed(3);
    nContent.style.left = ((w - tw) / 2).toFixed(2) + 'px';
    // campanita: 0,55·sin(21t)·e^(−3,2t), y un segundo toque a 1,1 s
    if (N.mode === 'bell' && N.bellT) { const t = (now - N.bellT) / 1000, bell = nContent.querySelector('svg');
      if (bell && t > 0) { const tt = t > 1.1 ? t - 1.1 : t, a = t < 2.3 ? .55 * Math.sin(21 * tt) * Math.exp(-3.2 * tt) : 0; bell.style.transform = `rotate(${a}rad)`; m = true; } }
    // tarjetas del centro
    if (N.mode === 'center' && N.cards.length) {
      const lh = listH(), maxS = Math.max(0, contentH() - lh); N.scrollT = clamp(N.scrollT, 0, maxS);
      m |= spring(N, 'scroll', N.scrollT, 260, Math.max(.75, z), dt, .05);
      const scrolling = Math.abs(N.scroll - N.scrollT) > .5 || Math.abs(N.scrollV || 0) > 4;
      let y = 0; const anyHot = N.hoverCard >= 0 && !scrolling;
      N.cards.forEach((c, i) => {
        if (c.outAt) { c.out = clamp((now - c.outAt) / 260, 0, 1); m = true; }
        const hot = N.hoverCard === i && !c.outAt, expand = hot && now - N.hoverSince > 260;
        if (hot && now - N.hoverSince <= 260) m = true;
        m |= spring(c, 'ex', expand ? c.full : 0, 340, Math.max(.7, z), dt, .05);
        const ccY = y - N.scroll + (70 + c.ex) / 2;
        const scT = scrolling ? 1.035 - .075 * Math.min(1, Math.abs(ccY - lh / 2) / (lh / 2)) : anyHot ? (hot ? 1.035 : .965) : 1;
        m |= spring(c, 'sc', scT, 420, z, dt);
        const nearR = hot && P.x > (N.x - 192 + 18 + 348 - 70);
        const overT = nearR && P.x > (N.x - 192 + 18 + 348 - c.tw);
        m |= spring(c, 'tw', nearR ? (overT ? 58 : 40) : 0, 520, .78, dt, .05);
        m |= spring(c, 'rise', 0, 300, z, dt, .05);
        const e = smooth(0, 1, c.out), d = c.el, hh = 70 + c.ex;
        d.style.top = (y + c.rise - N.scroll).toFixed(2) + 'px'; d.style.height = hh.toFixed(2) + 'px';
        d.style.transform = `translateX(${(e * 366).toFixed(2)}px) scale(${c.sc.toFixed(4)})`; d.style.opacity = (1 - e).toFixed(3);
        const b = d.querySelector('.b'), extraBody = Math.max(0, c.ex - (c.n.acts ? 40 : 0));
        b.style.height = (16 + extraBody).toFixed(1) + 'px'; b.style.whiteSpace = c.ex > 2 ? 'normal' : 'nowrap';
        const acts = d.querySelector('.acts'); acts.style.top = (hh - 40 + 4).toFixed(1) + 'px';
        acts.style.opacity = c.n.acts ? clamp((c.ex - c.full * .5) / (c.full * .5 || 1), 0, 1).toFixed(3) : 0;
        const tr = d.querySelector('.trash'); tr.style.width = Math.max(0, c.tw).toFixed(1) + 'px'; tr.style.opacity = clamp(c.tw / 14, 0, 1);
        tr.style.background = overT ? '#FF453A' : '#E5443C'; tr.querySelector('svg').style.opacity = clamp((c.tw - 20) / 12, 0, 1);
        y += hh + 12;
      });
      // quitar las que ya salieron: las de abajo suben desde +82
      const gone = N.cards.filter(c => c.out >= 1);
      if (gone.length) {
        gone.forEach(c => { c.el.remove(); notes = notes.filter(n => n !== c.n); });
        const first = N.cards.findIndex(c => c.out >= 1);
        N.cards = N.cards.filter(c => c.out < 1); N.cards.forEach((c, i) => { if (i >= first) c.rise += 82 * gone.length; });
        N.hoverCard = -1;
        if (!N.cards.length) el('div', 'empty', $('cList'), 'Sin notificaciones');
      }
      $('cList').style.height = Math.max(0, h - 56 - 14).toFixed(1) + 'px';
    } else if (N.mode === 'center') { const l = $('cList'); if (l) l.style.height = '120px'; }
    return m || open;
  };

  /* ═════════════ centro de control ═════════════ */
  const nCC = $('nCC'), ccContent = $('ccContent');
  const C = { open: false, w: 0, h: 0, wV: 0, hV: 0, alpha: 0, sec: 0, secT: 0, secTV: 0, fillB: 1, fillV: .7 };
  const ICON = {
    wifi: '<svg viewBox="0 0 24 24"><g transform="rotate(35 12 12) rotate(15 12 15)"><circle cx="12" cy="19.4" r="1.7" fill="currentColor"/><path d="' + arcPath(12, 20, 5.2, -135, -45) + '" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round"/><path d="' + arcPath(12, 20, 9.6, -135, -45) + '" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round"/></g></svg>',
    bt: '<svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M6 6.5l8 7-4 3.5V3l4 3.5-8 7"/></svg>',
    moon: '<svg viewBox="0 0 20 20" fill="currentColor"><path d="M11.5 2.6A7.5 7.5 0 1 0 17.4 12 6 6 0 0 1 11.5 2.6z"/></svg>',
    bell: '<svg viewBox="0 0 20 20" fill="currentColor"><path d="M10 2.4a5.2 5.2 0 0 0-5.2 5.2v3L3.3 13.6h13.4L15.2 10.6v-3A5.2 5.2 0 0 0 10 2.4zM8 15.2a2 2 0 0 0 4 0z"/></svg>',
    sun: '<svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><circle cx="10" cy="10" r="3.4"/><path d="M10 1.8v2M10 16.2v2M1.8 10h2M16.2 10h2M4.2 4.2l1.4 1.4M14.4 14.4l1.4 1.4M4.2 15.8l1.4-1.4M14.4 5.6l1.4-1.4"/></svg>',
    vol: '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M2.4 9.6Q2.4 8.4 3.6 8.4H6.3L10.2 4.9Q11.4 3.9 11.4 5.4V18.6Q11.4 20.1 10.2 19.1L6.3 15.6H3.6Q2.4 15.6 2.4 14.4Z"/><path d="' + arcPath(9.6, 12, 4.6, -45, 45) + '" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round"/></svg>',
    mute: '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M2.4 9.6Q2.4 8.4 3.6 8.4H6.3L10.2 4.9Q11.4 3.9 11.4 5.4V18.6Q11.4 20.1 10.2 19.1L6.3 15.6H3.6Q2.4 15.6 2.4 14.4Z"/><path d="M14.8 9.6L19.6 14.4M19.6 9.6L14.8 14.4" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"/></svg>',
  };
  const ccHTML = () => `
    <div class="ccw">
      <div class="ccsec" id="ccS0">
        <div class="cc-card" style="left:14px;top:14px;width:151px;height:126px">
          <div class="cc-row cc-hit" id="ccWifi" style="top:2px"><span class="bub" id="bWifi" style="left:8px;top:16.5px">${ICON.wifi}</span><span class="two" style="left:48px;top:13.5px"><b>Wi‑Fi</b><span id="sWifi"></span></span></div>
          <div class="cc-row cc-hit" id="ccBt" style="top:65px"><span class="bub" id="bBt" style="left:8px;top:16.5px">${ICON.bt}</span><span class="two" style="left:48px;top:13.5px"><b>Bluetooth</b><span id="sBt"></span></span></div>
        </div>
        <div class="cc-card cc-hit" id="ccDnd" style="left:175px;top:14px;width:151px;height:58px"><span class="bub" style="left:12px;top:14px">${ICON.moon}</span><span class="two" style="left:52px;top:11px"><b>No molestar</b><span>Ajustes</span></span></div>
        <div class="cc-card cc-hit" id="ccNot" style="left:175px;top:82px;width:151px;height:58px"><span class="bub" id="bNot" style="left:12px;top:14px">${ICON.bell}</span><span class="two" style="left:52px;top:11px"><b>Notificaciones</b><span id="sNot"></span></span></div>
        <div class="cc-card cc-hit" id="ccBatt" style="left:14px;top:150px;width:312px;height:60px">
          <span style="position:absolute;left:14px;top:10px;width:40px;height:40px;display:grid;place-items:center" id="ccBattIco"></span>
          <span class="two" style="left:66px;top:12px"><b>Batería</b><span id="sBatt"></span></span>
          <span id="pBatt" style="position:absolute;right:16px;top:0;line-height:60px;font:700 20px/60px 'Segoe UI',sans-serif"></span></div>
        <div class="cc-card" style="left:14px;top:220px;width:312px;height:118px">
          <div class="sl-row" style="top:10px"><span class="lb">Pantalla</span><span class="vl" id="vB"></span><div class="tr" id="trB"><div class="fl" id="flB"></div><span class="gl" id="gB">${ICON.sun}</span></div></div>
          <div class="sl-row" style="top:64px"><span class="lb">Sonido</span><span class="vl" id="vV"></span><div class="tr" id="trV"><div class="fl" id="flV"></div><span class="gl" id="gV"></span></div></div>
        </div>
        <div class="cc-pill cc-hit" id="ccHide" style="left:14px;top:348px;width:151px;background:var(--cccard);color:var(--fg)">Ocultar barra</div>
        <div class="cc-pill cc-hit" id="ccSet" style="left:175px;top:348px;width:151px;background:var(--accent);color:#fff">Ajustes  ›</div>
      </div>
      <div class="ccsec" id="ccS1" style="opacity:0">
        <span class="bub cc-hit" id="ccBack" style="left:14px;top:14px;width:32px;height:32px;background:var(--cccard)"><svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4.5L6.5 10l5.5 5.5"/></svg></span>
        <span style="position:absolute;left:58px;top:14px;line-height:32px;font-size:17px;font-weight:700">Ajustes</span>
        <div class="cc-card" style="left:14px;top:60px;width:312px;height:248px">
          <div class="set-row" style="top:4px;left:14px;right:14px">Material<div class="dseg" data-bind="mat"><button data-v="oled">OLED</button><button data-v="glass">Vidrio</button><button data-v="system">Sistema</button></div></div>
          <div class="set-row" style="top:52px;left:14px;right:14px">Rebote<div class="dseg" data-bind="bounce"><button data-v="soft">Suave</button><button data-v="normal">Normal</button><button data-v="bouncy">Bouncy</button></div></div>
          <div class="set-row" style="top:100px;left:14px;right:14px">Ocultar el dock<div class="dseg" data-bind="hide"><button data-v="no">No</button><button data-v="half">Mitad</button><button data-v="full">Todo</button></div></div>
          <div class="set-row" style="top:148px;left:14px;right:14px">Avisos discretos<div class="dseg" data-bind="obf"><button data-v="off">No</button><button data-v="on">Sí</button></div></div>
          <div class="set-row" style="top:196px;left:14px;right:14px">Barra superior<div class="dseg" data-bind="bar"><button data-v="off">No</button><button data-v="on">Sí</button></div></div>
        </div>
      </div>
    </div>`;
  ccContent.innerHTML = ccHTML();
  const ccH = [396, 322];
  const battSvg = () => `<svg viewBox="0 0 24 24" width="${22.8 * 40 / 24}" height="${22.8 * 40 / 24}" style="color:var(--fg)"><rect x="1.5" y="6.5" width="19" height="11" rx="3.8" fill="none" stroke="currentColor" stroke-width="1.7"/><path d="M22.6 10.3V13.7" stroke="currentColor" stroke-width="2" stroke-linecap="round"/><rect x="3.35" y="8.35" height="7.3" rx="2" width="${(4.75 + 13.9 * st.batt / 100 - 3.35).toFixed(2)}" fill="${st.charging ? '#30D158' : st.batt <= 20 ? '#FF453A' : 'currentColor'}"/></svg>`;
  const syncCC = () => {
    $('bWifi').classList.toggle('on', st.wifi); $('sWifi').textContent = st.wifi ? 'Casa' : 'Sin conexión';
    $('bBt').classList.toggle('on', st.bt); $('sBt').textContent = st.bt ? 'Activado' : 'Desactivado';
    const n = notes.length; $('bNot').classList.toggle('on', n > 0); $('sNot').textContent = n + ' en el notch';
    $('sBatt').textContent = st.charging ? 'Cargando' : 'Con batería'; $('pBatt').textContent = Math.round(st.batt) + ' %';
    $('ccBattIco').innerHTML = battSvg();
    $('vB').textContent = Math.round(st.bright * 100) + ' %'; $('vV').textContent = (st.muted ? 0 : Math.round(st.volume * 100)) + ' %';
    $('gV').innerHTML = st.muted ? ICON.mute : ICON.vol;
    document.querySelectorAll('[data-bind] button').forEach(b => { const key = b.parentElement.dataset.bind;
      const v = key === 'obf' ? (st.obf ? 'on' : 'off') : key === 'bar' ? (st.bar ? 'on' : 'off') : st[key]; b.setAttribute('aria-pressed', String(b.dataset.v === v)); });
  };
  const openCC = () => { if (N.mode === 'center') hide(); if (!C.open) { C.open = true; C.w = 150; C.h = 0; C.alpha = .4; ccSection(0, true); } syncCC(); wake(); };
  const closeCC = () => { C.open = false; wake(); };
  const toggleCC = () => C.open ? closeCC() : openCC();
  const ccSection = (s, now) => { C.sec = s; if (now) { C.secT = s; C.secTV = 0; } wake(); };
  $('ccWifi').onclick = () => setWifi(!st.wifi);
  $('ccBt').onclick = () => { st.bt = !st.bt; syncCC(); };
  $('ccNot').onclick = () => { closeCC(); setTimeout(() => openCenter(true), 120); };
  $('ccBatt').onclick = () => { setCharging(!st.charging); syncCC(); };
  $('ccDnd').onclick = () => { const d = $('ccDnd'); d.animate([{ transform: 'scale(1)' }, { transform: 'scale(.97)' }, { transform: 'scale(1)' }], { duration: 260 }); };
  $('ccSet').onclick = () => ccSection(1);
  $('ccBack').onclick = () => ccSection(0);
  $('ccHide').onclick = () => { closeCC(); setBar(false); };
  const slider = (tr, get, set) => {
    const at = e => { const r = tr.getBoundingClientRect(); set(clamp((e.clientX - r.left) / r.width, 0, 1)); syncCC(); wake(); };
    tr.addEventListener('pointerdown', e => { tr.setPointerCapture(e.pointerId); if (tr === $('trV') && (e.clientX - tr.getBoundingClientRect().left) / S < 32) { setMute(!st.muted); return; } at(e); tr.onpointermove = at; });
    tr.addEventListener('pointerup', () => { tr.onpointermove = null; });
  };
  slider($('trB'), () => st.bright, v => { const was = st.bright; st.bright = Math.max(.2, v); if (Math.abs(v - was) > .02) POP.bV += v > was ? 2.2 : -1.4; });
  slider($('trV'), () => st.volume, v => { const was = st.volume; st.volume = v; st.muted = v < .01; B.ripV += v > was ? 20 : -15; bump(snd, v > was ? 1.6 : -1.1); if (Math.abs(v - was) > .02) POP.vV += v > was ? 2.2 : -1.4; });
  desk.addEventListener('wheel', e => {
    if (N.mode === 'center' && nNotch.contains(e.target)) { e.preventDefault(); N.scrollT += e.deltaY > 0 ? 60 : -60; N.hoverCard = -1; wake(); return; }
    if (!C.open && !($('dbar').contains(e.target))) return; e.preventDefault(); st.volume = clamp(st.volume + (e.deltaY < 0 ? .05 : -.05), 0, 1); st.muted = st.volume < .01; bump(snd, e.deltaY < 0 ? 1.6 : -1.1); syncCC(); }, { passive: false });

  // pasar el cursor: botones 1,03 · filas Wi‑Fi/BT 1,12 (en la burbuja) · fichas y batería 1,02 · deslizadores 1,012
  const HOV = [];
  const hov = (e, target, hover, press) => { const o = { e, target: target || e, hover, press, s: 1, sV: 0, hot: false, down: false };
    e.addEventListener('pointerenter', () => { o.hot = true; wake(); }); e.addEventListener('pointerleave', () => { o.hot = o.down = false; wake(); });
    e.addEventListener('pointerdown', () => { o.down = true; wake(); }); e.addEventListener('pointerup', () => { o.down = false; wake(); });
    HOV.push(o); return o; };
  hov($('ccHide'), null, 1.03, .97); hov($('ccSet'), null, 1.03, .97); hov($('ccBack'), null, 1.03, .97);
  hov($('ccWifi'), $('bWifi'), 1.12, .9); hov($('ccBt'), $('bBt'), 1.12, .9);
  hov($('ccDnd'), null, 1.02, .97); hov($('ccNot'), null, 1.02, .97); hov($('ccBatt'), null, 1.02, .97);
  const slB = hov($('trB'), null, 1.012, 1.02), slV = hov($('trV'), null, 1.012, 1.02);
  // el icono del deslizador "salta" al cambiar el valor (+2,2 al subir, −1,4 al bajar, +2,4 al silenciar)
  const POP = { b: 1, bV: 0, v: 1, vV: 0 };
  const frameCC = dt => {
    let m = false; const z = Z();
    HOV.forEach(o => { m |= spring(o, 's', o.down ? o.press : o.hot ? o.hover : 1, 420, z, dt); o.target.style.transform = o.s === 1 ? '' : `scale(${o.s.toFixed(4)})`; });
    m |= spring(POP, 'b', 1, 380, z, dt); m |= spring(POP, 'v', 1, 380, z, dt);
    $('gB').style.transform = `scale(${clamp(POP.b, .85, 1.25).toFixed(4)})`; $('gV').style.transform = `scale(${clamp(POP.v, .85, 1.25).toFixed(4)})`;
    const tw = C.open ? 340 : 150, th = C.open ? ccH[C.sec] : 0;
    m |= spring(C, 'w', tw, C.open ? 220 : 300, C.open ? Math.min(1, z + .22) : 1, dt, .05);
    m |= spring(C, 'h', th, C.open ? 250 : 300, C.open ? Math.min(1, z + .28) : 1, dt, .05);
    const want = C.open && C.h > .42 * th ? 1 : 0; C.alpha = approach(C.alpha, want, want ? 11 : 26, dt); if (C.alpha !== want) m = true;
    m |= spring(C, 'secT', C.sec, 210, .84, dt);
    m |= spring(C, 'fillB', st.bright, 320, Math.max(.55, z), dt); m |= spring(C, 'fillV', st.muted ? 0 : st.volume, 320, Math.max(.55, z), dt);
    const w = Math.max(0, C.w), h = Math.max(0, C.h), anchor = (W - 14 - 340) + .95 * 340, bh = barH();
    nCC.style.display = h < .5 && !C.open ? 'none' : '';
    nCC.style.top = bh + 'px';
    nCC.style.left = (anchor - .95 * w).toFixed(2) + 'px'; nCC.style.width = w.toFixed(2) + 'px'; nCC.style.height = h.toFixed(2) + 'px';
    nCC.style.setProperty('--r', Math.min(24, h / 2).toFixed(2) + 'px'); nCC.style.setProperty('--ear', Math.min(8, h / 2).toFixed(2) + 'px');
    nCC.classList.toggle('live', C.open);
    ccContent.style.opacity = C.alpha.toFixed(3); ccContent.style.left = (w - 340).toFixed(2) + 'px';
    // cambio de sección: la de controles se aleja (0,93) y la nueva entra desde la derecha
    const e = clamp(C.secT, -.04, 1.04), t = clamp(e, 0, 1);
    const s0 = $('ccS0'), s1 = $('ccS1');
    s0.style.transform = `translateX(${(-e * 340 * .12).toFixed(2)}px) scale(${(1 - .07 * t).toFixed(4)})`; s0.style.opacity = (1 - smooth(0, .6, t)).toFixed(3);
    s1.style.transform = `translateX(${((1 - e) * 340 * .34).toFixed(2)}px)`; s1.style.opacity = smooth(.18, .85, t).toFixed(3);
    s0.style.pointerEvents = t < .5 ? '' : 'none'; s1.style.pointerEvents = t >= .5 ? '' : 'none';
    $('flB').style.width = (C.fillB * 100).toFixed(2) + '%'; $('flV').style.width = (C.fillV * 100).toFixed(2) + '%';
    const ink = 'var(--ink)'; $('gB').style.color = ink; $('gV').style.color = C.fillV > .04 ? ink : 'var(--fg)';
    $('dimmer').style.opacity = ((1 - st.bright) * .75).toFixed(3);
    return m || C.open;
  };

  /* ═════════════ dock ═════════════ */
  const ddock = $('ddock');
  const panel = el('div', 'panel', ddock);
  const D = { sink: 42, sinkV: 0, up: false, leaveAt: 0, items: [] };
  APPS.forEach((a, i) => {
    const e = el('div', 'app', ddock, `<span class="glyph">${a.g}</span>`); e.style.background = grad(a);
    const run = el('i', 'run', ddock);
    const it = { a, e, run, s: 1, sV: 0, hop: null, rw: a.run ? (i === front ? 16 : 6) : 0, ra: 0, running: !!a.run };
    e.addEventListener('click', () => launch(i)); D.items.push(it);
  });
  const sinkFor = () => st.hide === 'no' ? 0 : st.hide === 'half' ? 12 + 30 : 12 + 60 + 8;
  const launch = i => {
    const it = D.items[i], w = winOf(i);
    if (it.running && front === i && w && w.open && !w.min) { minimize(w); return; }      // clic de nuevo: minimiza
    const was = it.running && w && w.open; it.hop = { t0: performance.now(), n: was ? 1 : 3, h: was ? .38 * 38 : .75 * 38 };
    setTimeout(() => {
      it.running = true;
      if (i === 0) { openWin(WINS[0], 0); return; }
      $('wAppT').textContent = it.a.n; $('wAppIco').style.background = grad(it.a);
      if (WINS[1].app !== i && WINS[1].app > 0 && WINS[1].open) D.items[WINS[1].app].running = true;
      openWin(WINS[1], i);
    }, was ? 200 : 1200);
    wake();
  };

  /* ═════════════ ventanas: mover, maximizar entre la barra y el dock, minimizar ═════════════ */
  // el dock reserva su franja (12 + la mitad del panel = 42 px): las maximizadas terminan encima
  const work = () => ({ x: 0, y: barH(), w: W, h: HH - barH() - 42 });
  const WINS = [];
  const order = [];
  const place = (w, anim) => {
    if (anim) { w.el.classList.add('anim'); clearTimeout(w.animT); w.animT = setTimeout(() => w.el.classList.remove('anim'), 260); }
    const r = w.max ? work() : w;
    w.el.style.left = r.x + 'px'; w.el.style.top = r.y + 'px'; w.el.style.width = r.w + 'px'; w.el.style.height = r.h + 'px';
    w.el.classList.toggle('maxed', w.max);
  };
  const raise = w => { const i = order.indexOf(w); if (i >= 0) order.splice(i, 1); order.push(w); order.forEach((q, k) => q.el.style.zIndex = 2 + k); };
  const focusWin = w => { raise(w); front = w.app; setFrontIcon(); wake(); };
  const winOf = i => WINS.find(w => w.app === i);
  const openWin = (w, app) => {
    w.app = app; w.open = true; const wasMin = w.min; w.min = false;
    w.el.classList.add('anim'); w.el.classList.remove('gone');
    w.el.style.transform = wasMin ? 'scale(.25)' : 'scale(.92)'; w.el.style.opacity = '0'; void w.el.offsetWidth;
    w.el.style.transform = ''; w.el.style.opacity = ''; place(w, true); focusWin(w);
  };
  const minimize = w => {
    w.min = true; const it = D.items[w.app], r = it.e.getBoundingClientRect(), d = desk.getBoundingClientRect();
    const tx = (r.left - d.left) / S + 19 - (w.max ? W / 2 : w.x + w.w / 2), ty = (r.top - d.top) / S + 19 - (w.max ? (barH() + HH - 42) / 2 : w.y + w.h / 2);
    w.el.classList.add('anim'); w.el.style.transform = `translate(${tx}px, ${ty}px) scale(.08)`; w.el.style.opacity = '0';
    setTimeout(() => { w.el.classList.add('gone'); w.el.style.transform = ''; w.el.style.opacity = ''; }, 260);
    const next = order.filter(q => q !== w && q.open && !q.min).pop(); front = next ? next.app : 0; setFrontIcon(); wake();
  };
  const closeWin = w => {
    w.open = false; w.el.classList.add('anim'); w.el.style.transform = 'scale(.94)'; w.el.style.opacity = '0';
    setTimeout(() => { w.el.classList.add('gone'); w.el.style.transform = ''; w.el.style.opacity = ''; }, 200);
    if (w.app > 0) D.items[w.app].running = false;
    const next = order.filter(q => q !== w && q.open && !q.min).pop(); front = next ? next.app : 0; setFrontIcon(); wake();
  };
  const toggleMax = w => { w.max = !w.max; place(w, true); focusWin(w); };
  const mkWin = (e, app, open) => {
    const w = { el: e, app, x: e.offsetLeft, y: e.offsetTop, w: e.offsetWidth, h: e.offsetHeight, max: false, open, min: false };
    WINS.push(w); order.push(w); place(w);
    const tb = e.querySelector('.tb');
    e.addEventListener('pointerdown', () => { if (w.open) focusWin(w); });
    tb.addEventListener('dblclick', ev => { if (!ev.target.closest('.ctl')) toggleMax(w); });
    e.querySelectorAll('.ctl b').forEach(b => b.addEventListener('click', ev => { ev.stopPropagation();
      const a = b.dataset.a; if (a === 'min') minimize(w); else if (a === 'max') toggleMax(w); else closeWin(w); }));
    tb.addEventListener('pointerdown', ev => {
      if (ev.target.closest('.ctl')) return;
      tb.setPointerCapture(ev.pointerId);
      const d = desk.getBoundingClientRect(), px = (ev.clientX - d.left) / S, py = (ev.clientY - d.top) / S;
      let gx = px - w.x, gy = py - w.y, moved = false;
      tb.onpointermove = mv => {
        const qx = (mv.clientX - d.left) / S, qy = (mv.clientY - d.top) / S;
        if (!moved && Math.hypot(qx - px, qy - py) < 4) return;
        if (w.max) { // arrastrar una maximizada la restaura bajo el cursor, como Windows
          const ratio = clamp(px / W, .1, .9); w.max = false; gx = w.w * ratio; gy = 16; w.el.classList.remove('maxed');
        }
        moved = true;
        w.x = clamp(qx - gx, -w.w + 80, W - 80); w.y = clamp(qy - gy, barH(), HH - 40); place(w);
      };
      tb.onpointerup = up => { tb.onpointermove = tb.onpointerup = null;
        const qy = (up.clientY - d.top) / S; if (moved && qy <= barH() + 2) { w.y = Math.max(w.y, barH() + 40); toggleMax(w); } };   // al borde de arriba: maximiza
    });
    return w;
  };
  mkWin($('wFiles'), 0, true); mkWin($('wApp'), 2, false);
  const frameDock = (dt, now) => {
    let m = false; const z = Z(), n = D.items.length, base = 38, gap = 12, padX = 16;
    const panelH = 60, panelTopBase = HH - 12 - panelH;
    // ¿el cursor está en el dock? (el hueco de abajo también cuenta)
    const baseW = n * base + (n - 1) * gap + padX * 2, baseL = (W - baseW) / 2;
    const top = panelTopBase + D.sink;
    const overDock = P.in && P.x >= baseL - 10 && P.x <= baseL + baseW + 10 && P.y >= top - 30 && P.y <= HH;
    const inZone = P.in && P.y >= HH - 90 && P.x >= baseL - 40 && P.x <= baseL + baseW + 40;
    if (overDock || inZone || C.open && false) { D.up = true; D.leaveAt = 0; } else if (D.up) { if (!D.leaveAt) D.leaveAt = now; if (now - D.leaveAt > 450) D.up = false; else m = true; }
    const sinkT = D.up ? 0 : sinkFor();
    m |= spring(D, 'sink', sinkT, sinkT > D.sink ? 140 : 260, sinkT > D.sink ? .95 : Math.min(1, z + .12), dt, .05);
    // lupa gaussiana: 1 + 0,5·exp(−((i − f)/1,55)²), con k 520 y ζ 0,66 fijos
    const f = (P.x - baseL - padX - base / 2) / (base + gap), mag = overDock && P.y >= top - 10;
    D.items.forEach((it, i) => { m |= spring(it, 's', mag ? 1 + .5 * Math.exp(-(((i - f) / 1.55) ** 2)) : 1, 520, .66, dt); });
    const total = D.items.reduce((a, it) => a + base * it.s, 0) + (n - 1) * gap + padX * 2, left = (W - total) / 2;
    const pTop = panelTopBase + D.sink, pBottom = pTop + panelH, baseline = pBottom - 11;
    panel.style.left = left.toFixed(2) + 'px'; panel.style.top = pTop.toFixed(2) + 'px'; panel.style.width = total.toFixed(2) + 'px';
    let x = left + padX;
    D.items.forEach((it, i) => {
      const s = base * it.s; let hop = 0;
      if (it.hop) { const t = (now - it.hop.t0) / 1000, k = Math.floor(t / .4), p = (t % .4) / .4;
        if (k >= it.hop.n) it.hop = null; else { hop = it.hop.h * Math.pow(.62, k) * 4 * p * (1 - p); m = true; } }
      it.e.style.transform = `translate(${x.toFixed(2)}px, ${(baseline - s - hop).toFixed(2)}px) scale(${it.s.toFixed(4)})`;
      it.e.style.left = '0'; it.e.style.top = '0'; it.e.style.transformOrigin = '0 0';
      const rw = it.running ? (front === i ? 16 : 6) : 0, ra = it.running ? (front === i ? .95 : .55) : 0;
      it.rw = approach(it.rw, rw, 14, dt); it.ra = approach(it.ra, ra, 14, dt); if (it.rw !== rw || it.ra !== ra) m = true;
      it.run.style.left = (x + s / 2 - it.rw / 2).toFixed(2) + 'px'; it.run.style.top = (pBottom - 5.5 - 1.5).toFixed(2) + 'px';
      it.run.style.width = it.rw.toFixed(2) + 'px'; it.run.style.opacity = it.ra.toFixed(3);
      x += s + gap;
    });
    return m;
  };

  /* ═════════════ barra on/off ═════════════ */
  const setBar = on => { st.bar = on; setTimeout(() => WINS.forEach(w => { if (w.max) place(w, true); }), 0); desk.style.setProperty('--bh', on ? '28px' : '0px'); document.querySelectorAll('.dcorner.k-tl,.dcorner.k-tr').forEach(c => { c.style.top = on ? '28px' : '0'; c.style.background = on ? '' : '#000'; c.style.backdropFilter = on ? '' : 'none'; }); syncCC(); wake(); };

  /* ═════════════ bucle ═════════════ */
  let raf = 0, last = 0, visible = true;
  const loop = now => {
    raf = 0; const dt = Math.min(.05, last ? (now - last) / 1000 : .016); last = now;
    let m = frameBar(dt); m = frameNotch(dt, now) || m; m = frameCC(dt) || m; m = frameDock(dt, now) || m;
    if (m && visible) raf = requestAnimationFrame(loop); else last = 0;
  };
  const wake = () => { if (!raf && visible) raf = requestAnimationFrame(loop); };
  new IntersectionObserver(es => { visible = es[0].isIntersecting; if (visible) wake(); }).observe(box);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) wake(); });

  /* ═════════════ ajustes compartidos ═════════════ */
  const apply = (key, v) => {
    if (key === 'mat') { st.mat = v; desk.dataset.mat = v; }
    if (key === 'bounce') { st.bounce = v; root.dataset.bounce = v; }
    if (key === 'hide') st.hide = v;
    if (key === 'obf') st.obf = v === 'on';
    if (key === 'bar') setBar(v === 'on');
    document.querySelectorAll(`[data-bind="${key}"] button`).forEach(b => b.setAttribute('aria-pressed', String(b.dataset.v === v)));
    syncCC(); wake();
  };
  document.querySelectorAll('[data-bind] button').forEach(b => b.addEventListener('click', e => { e.stopPropagation(); apply(b.parentElement.dataset.bind, b.dataset.v); }));
  syncCC();

  /* ═════════════ avisos automáticos ═════════════ */
  let auto = 0;
  const autoLoop = () => { clearInterval(auto); if (!reduce) auto = setInterval(() => { if (!document.hidden && visible) arrive(); }, 8000); };
  setTimeout(arrive, 1200); autoLoop();

  /* ═════════════ sonidos (mismo selector y deslizador que la app) ═════════════ */
  const SOUNDS = [
    { n: 'Eco suave', f: 'Avisos · Material', d: 'assets/sounds/eco-suave.wav' },
    { n: 'Nota', f: 'Avisos · Material', d: 'assets/sounds/nota.wav' },
    { n: 'Destello', f: 'Avisos · Material', d: 'assets/sounds/destello.wav' },
    { n: 'Ambiente', f: 'Avisos · Material', d: 'assets/sounds/ambiente.wav' },
    { n: 'Logro', f: 'Celebración · Material', d: 'assets/sounds/logro.wav' },
  ];
  let actx = null; const buffers = {};
  const bufferFor = async (s, c) => {
    const r = await fetch(s.d); return await c.decodeAudioData(await r.arrayBuffer());
  };
  const getBuf = async (i, c) => { const key = i + ':' + c.sampleRate; if (!buffers[key]) buffers[key] = await bufferFor(SOUNDS[i], c); return buffers[key]; };
  const waveEl = $('sWave'), BARS = 72; for (let i = 0; i < BARS; i++) waveEl.appendChild(document.createElement('i'));
  let sweepT = 0;
  const sweep = dur => { const bars = [...waveEl.children], t0 = performance.now(); cancelAnimationFrame(sweepT);
    const f = now => { const p = (now - t0) / 1000 / dur; bars.forEach((b, k) => b.classList.toggle('hot', k / BARS < p && p < 1)); if (p < 1) sweepT = requestAnimationFrame(f); else bars.forEach(b => b.classList.remove('hot')); };
    sweepT = requestAnimationFrame(f); };
  async function play(i = st.sIdx) {
    try { actx = actx || new (window.AudioContext || window.webkitAudioContext)(); if (actx.state === 'suspended') await actx.resume();
      const b = await getBuf(i, actx), src = actx.createBufferSource(), g = actx.createGain(); g.gain.value = Math.pow(st.vol / 100, 1.2);
      src.buffer = b; src.connect(g).connect(actx.destination); src.start(); sweep(b.duration); } catch (e) { }
  }
  const drawWave = async i => { try { const off = new (window.OfflineAudioContext || window.webkitOfflineAudioContext)(1, 1, 24000);
      const b = await getBuf(i, off), d = b.getChannelData(0), step = Math.floor(d.length / BARS); let max = 1e-4; const peaks = [];
      for (let q = 0; q < BARS; q++) { let mm = 0; for (let j = q * step; j < (q + 1) * step; j++) mm = Math.max(mm, Math.abs(d[j] || 0)); peaks.push(mm); max = Math.max(max, mm); }
      [...waveEl.children].forEach((e2, q) => e2.style.height = Math.max(3, peaks[q] / max * 100) + '%'); } catch (e) { } };
  const pick = d => { st.sIdx = (st.sIdx + d + SOUNDS.length) % SOUNDS.length; $('sName').textContent = SOUNDS[st.sIdx].n; $('sFam').textContent = SOUNDS[st.sIdx].f; drawWave(st.sIdx); play(); };
  $('prevS').onclick = () => pick(-1); $('nextS').onclick = () => pick(1); $('playS').onclick = () => play();
  const setVol = v => { st.vol = v; $('sVol').value = v; $('sVolOut').textContent = v + ' %'; $('vw1').style.opacity = v > 0 ? 1 : .15; $('vw2').style.opacity = v > 50 ? 1 : .15; };
  $('sVol').addEventListener('input', e => setVol(+e.target.value)); $('sVol').addEventListener('change', () => play());
  drawWave(0);
  $('sndSwitch').onclick = () => { st.sound = !st.sound; $('sndSwitch').setAttribute('aria-pressed', String(st.sound)); if (st.sound) play(); };

  /* ═════════════ botones "pruébalo" y esquinas ═════════════ */
  document.querySelectorAll('[data-try]').forEach(b => b.addEventListener('click', () => {
    box.scrollIntoView({ behavior: reduce ? 'auto' : 'smooth', block: 'center' });
    const t = b.dataset.try;
    setTimeout(() => {
      if (t === 'note') { if (N.mode === 'center') hide(); setTimeout(arrive, N.mode === 'hidden' ? 0 : 300); autoLoop(); }
      if (t === 'cc') openCC();
      if (t === 'dock') { D.up = true; D.leaveAt = performance.now() + 2500; launch(4); }
      if (t === 'mat') { const order = ['oled', 'glass', 'system']; apply('mat', order[(order.indexOf(st.mat) + 1) % 3]); }
    }, reduce ? 0 : 450);
  }));
  $('radius').addEventListener('input', e => { const r = +e.target.value; $('rOut').textContent = r + ' px'; $('vCorners').style.setProperty('--cr', r); desk.style.setProperty('--rr', Math.max(.01, r) + 'px'); });
  wake();
})();

/* ── formularios: Netlify Forms por AJAX ── */
(() => {
  const tabs = [['tabBug', 'panelBug'], ['tabIdea', 'panelIdea']];
  tabs.forEach(([t, p]) => document.getElementById(t).addEventListener('click', () => {
    tabs.forEach(([t2, p2]) => { document.getElementById(t2).setAttribute('aria-selected', String(t2 === t)); document.getElementById(p2).hidden = p2 !== p; });
  }));
  const MSG = { 'que-paso': 'Cuéntame qué pasó (al menos 10 caracteres).', idea: 'Escribe tu idea en una frase.', detalle: 'Cuéntame un poco más (al menos 10 caracteres).', correo: 'Ese correo no parece válido.' };
  const MAX = 8 * 1024 * 1024, TYPES = ['image/png', 'image/jpeg', 'image/gif', 'image/webp'];
  const check = f => {
    let ok = true;
    f.querySelectorAll('input[type="file"]').forEach(el => {
      const box = el.closest('.field'), err = box.querySelector(':scope > .err'), file = el.files[0];
      const msg = !file ? '' : !TYPES.includes(file.type) ? 'Solo imágenes PNG, JPG, GIF o WebP.' : file.size > MAX ? 'La imagen pesa más de 8 MB.' : '';
      box.classList.toggle('bad', !!msg); if (err) err.textContent = msg;
      if (msg) ok = false;
    });
    f.querySelectorAll('input[required], textarea[required], input[type="email"]').forEach(el => {
      const box = el.closest('.field'), err = box.querySelector('.err');
      const bad = el.type === 'email' ? el.value.trim() !== '' && !el.checkValidity() : !el.checkValidity() || el.value.trim().length < (el.minLength || 1);
      box.classList.toggle('bad', bad); if (err) err.textContent = bad ? (MSG[el.name] || 'Revisa este campo.') : '';
      if (bad && ok) { el.focus(); ok = false; }
    });
    return ok;
  };
  document.querySelectorAll('form[data-netlify]').forEach(f => {
    const btn = f.querySelector('.send'), lbl = btn.querySelector('.lbl'), label = lbl.textContent, status = f.querySelector('.fstatus');
    f.addEventListener('input', e => { const box = e.target.closest('.field'); if (box && box.classList.contains('bad')) { box.classList.remove('bad'); const er = box.querySelector('.err'); if (er) er.textContent = ''; } });
    f.addEventListener('submit', async e => {
      e.preventDefault(); status.textContent = '';
      if (!check(f)) return;
      btn.disabled = true; lbl.textContent = 'Enviando…';
      try {
        const data = new FormData(f);
        const file = f.querySelector('input[type="file"]');
        if (file && !file.files.length) data.delete(file.name);           /* sin imagen: sin campo vacío */
        const r = await fetch('/', { method: 'POST', body: data });       /* multipart: así viaja la imagen */
        if (!r.ok) throw new Error(String(r.status));
        const card = f.parentElement, bug = f.name === 'reportar-error';
        f.hidden = true;
        const done = document.createElement('div'); done.className = 'done'; done.setAttribute('role', 'status');
        done.innerHTML = '<span class="ok"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg></span>'
          + '<h3>' + (bug ? 'Gracias, ya me llegó el reporte' : 'Gracias, ya me llegó tu idea') + '</h3>'
          + '<p>' + (f.querySelector('[name="correo"]').value.trim() ? 'Te escribo a tu correo en cuanto tenga novedades.' : 'Lo reviso en los próximos días.') + '</p>'
          + '<button type="button">' + (bug ? 'Reportar otro error' : 'Enviar otra idea') + '</button>';
        done.querySelector('button').onclick = () => { done.remove(); f.reset(); f.querySelectorAll('[data-drop]').forEach(d => d.clear && d.clear()); f.hidden = false; f.querySelector('input:not([type="hidden"]), textarea').focus(); };
        card.appendChild(done); done.querySelector('h3').focus?.();
      } catch (err) {
        status.textContent = /^\d+$/.test(err.message)
          ? 'No se pudo enviar (error ' + err.message + ' del servidor). Inténtalo de nuevo en un momento.'
          : 'No se pudo enviar. Revisa tu conexión e inténtalo de nuevo.';
      } finally { btn.disabled = false; lbl.textContent = label; }
    });
  });
})();

/* ── movimiento: cabecera, aparición al bajar, contadores, copiar y versiones ── */
(() => {
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const head = document.querySelector('header.top');
  const onScroll = () => head.classList.toggle('scrolled', scrollY > 12);
  addEventListener('scroll', onScroll, { passive: true }); onScroll();

  // aparición: cada bloque entra al acercarse; si algo falla, todo se ve igual
  const els = [...document.querySelectorAll('[data-reveal]')];
  const show = e => e.classList.add('in');
  if (!reduce && 'IntersectionObserver' in window) {
    let fired = false;      /* si el navegador nunca avisa, todo se muestra igual */
    const io = new IntersectionObserver(es => { fired = true; es.forEach(e => { if (e.isIntersecting) { show(e.target); io.unobserve(e.target); } }); }, { rootMargin: '0px 0px -8% 0px', threshold: .08 });
    els.forEach(e => io.observe(e));
    setTimeout(() => { if (!fired) els.forEach(show); }, 1500);
    setTimeout(() => els.forEach(e => { if (e.getBoundingClientRect().top < innerHeight) show(e); }), 60);
  } else els.forEach(show);

  // contadores de la franja de cifras
  const fmt = (v, d) => v.toFixed(d).replace('.', ',');
  const count = el => {
    const to = parseFloat(el.dataset.count), d = +el.dataset.dec || 0, t0 = performance.now(), dur = 1100;
    const f = now => { const p = Math.min(1, (now - t0) / dur), e = 1 - Math.pow(1 - p, 4); el.textContent = fmt(to * e, d); if (p < 1) requestAnimationFrame(f); };
    requestAnimationFrame(f);
  };
  const nums = [...document.querySelectorAll('[data-count]')];
  if (!reduce && 'IntersectionObserver' in window) {
    const io2 = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) { count(e.target); io2.unobserve(e.target); } }), { threshold: .6 });
    nums.forEach(n => io2.observe(n));
  }

  // instalar: pestañas Windows / Linux; desde Linux (no Android ni ChromeOS) se abre la de Linux
  const osTabs = [['tabWin', 'panelWin'], ['tabLin', 'panelLin']].map(([t, p]) => [document.getElementById(t), document.getElementById(p)]);
  const showOs = i => osTabs.forEach(([t, p], k) => { t.setAttribute('aria-selected', String(k === i)); t.tabIndex = k === i ? 0 : -1; p.hidden = k !== i; });
  osTabs.forEach(([t], i) => {
    t.addEventListener('click', () => showOs(i));
    t.addEventListener('keydown', e => { if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { const j = 1 - i; showOs(j); osTabs[j][0].focus(); } });
  });
  const ua = navigator.userAgent;
  const onLinux = /Linux/i.test(ua + ' ' + (navigator.userAgentData ? navigator.userAgentData.platform : '') + ' ' + (navigator.platform || '')) && !/Android|CrOS/.test(ua);
  showOs(onLinux ? 1 : 0);
  // #linux (menú, aviso del hero o un enlace compartido): la pestaña de Linux, a la vista
  const goLinux = smooth => { showOs(1); document.getElementById('instalar').scrollIntoView({ behavior: smooth && !reduce ? 'smooth' : 'auto', block: 'start' }); };
  document.addEventListener('click', e => {
    const a = e.target.closest('a[href="#linux"]'); if (!a) return;
    e.preventDefault(); history.replaceState(null, '', '#linux'); goLinux(true);
  });
  if (location.hash === '#linux') requestAnimationFrame(() => goLinux(false));
  if (onLinux) { const al = document.getElementById('alsoLinux'); if (al) al.hidden = true; }
  // Linux: el paquete de GNOME para la distribución que se adivina por el navegador (Fedora y
  // openSUSE lo dicen en su Firefox; si no, el .deb de Ubuntu y Debian, lo más común)
  // (si el navegador no lo dice, como en Manjaro o con Chrome, se pide elegir: un .deb en
  // Arch no hace nada)
  const linBtn = document.getElementById('linBtn'), linFor = document.getElementById('linFor'), linHow = document.getElementById('linHow');
  const LIN = {
    deb:  ['Para Ubuntu y Debian con GNOME', 'Se abre en el Centro de software: pulsa <b>Instalar</b>, cierra sesión y vuelve a entrar. OpenDock se activa solo.'],
    rpm:  ['Para Fedora y openSUSE con GNOME', 'Se abre en Software: pulsa <b>Instalar</b>, cierra sesión y vuelve a entrar. OpenDock se activa solo.'],
    arch: ['Para Arch y Manjaro con GNOME', 'Ábrelo con Pamac (Añadir/quitar software) o en una terminal: <code>sudo pacman -U opendock-gnome-any.pkg.tar.zst</code>. Cierra sesión y vuelve a entrar: OpenDock se activa solo.'],
  };
  const setLin = kind => {
    if (!linBtn) return;
    const box = linBtn.closest('.lininst');
    document.querySelectorAll('[data-lin]').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.lin === kind)));
    if (!kind) {                    /* sin saber el sistema: el botón pide elegir */
      box.classList.add('pick');
      linBtn.removeAttribute('href');
      linBtn.lastChild.textContent = 'Elige tu sistema';
      linFor.textContent = '¿Qué sistema usas?';
      linHow.innerHTML = 'Elige abajo y el botón descargará el paquete que toca.';
      return;
    }
    box.classList.remove('pick');
    linBtn.href = linBtn.dataset[kind];
    linBtn.lastChild.textContent = 'Instalar';
    linFor.textContent = LIN[kind][0];
    linHow.innerHTML = LIN[kind][1];
  };
  setLin(/Ubuntu|Debian|Mint|Pop!?_?OS/i.test(ua) ? 'deb' : /Fedora|SUSE/i.test(ua) ? 'rpm' : /Manjaro|Arch|Endeavour/i.test(ua) ? 'arch' : null);
  if (linBtn) linBtn.addEventListener('click', e => { if (!linBtn.getAttribute('href')) { e.preventDefault(); document.getElementById('linAlt').querySelector('button').focus(); } });
  document.querySelectorAll('[data-lin]').forEach(b => b.addEventListener('click', () => setLin(b.dataset.lin)));
  if (onLinux) {
    const hero = document.querySelector('.hero .btn-primary');
    if (hero && hero.firstChild) { hero.href = '#instalar'; hero.firstChild.textContent = 'Instalar en Linux '; }
  }

  // copiar con confirmación
  document.addEventListener('click', async e => {
    const b = e.target.closest('.copy'); if (!b) return;
    const text = b.dataset.copy;
    try { await navigator.clipboard.writeText(text); }
    catch { const r = document.createRange(); r.selectNodeContents(b.previousElementSibling); const s = getSelection(); s.removeAllRanges(); s.addRange(r); }
    b.classList.add('done'); b.setAttribute('aria-label', 'Copiado');
    setTimeout(() => { b.classList.remove('done'); b.setAttribute('aria-label', 'Copiar'); }, 1600);
  });

  // versiones: la lista real de GitHub (la de la página queda si no hay red)
  const box = document.getElementById('rels'); if (!box || !window.fetch) return;
  const REPO = 'JosueGM24/OpenDock';
  const esc = t => String(t).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const date = s => new Date(s).toLocaleDateString('es', { day: 'numeric', month: 'short', year: 'numeric' }).replace('.', '');
  const mb = n => (n / 1048576).toFixed(1).replace('.', ',') + ' MB';
  const tpl = box.querySelector('.rel');
  fetch('https://api.github.com/repos/' + REPO + '/releases?per_page=6', { headers: { Accept: 'application/vnd.github+json' } })
    .then(r => r.ok ? r.json() : Promise.reject(r.status))
    .then(list => {
      list = list.filter(r => !r.draft);
      if (!list.length) return;
      const keep = tpl.cloneNode(true);
      box.innerHTML = '';
      list.forEach((r, i) => {
        const exe = (r.assets || []).find(a => /\.exe$/i.test(a.name));
        let el;
        if (r.tag_name === 'v2.0.0') { el = keep.cloneNode(true); el.classList.toggle('latest', i === 0); const b = el.querySelector('.badge'); if (b && i) b.remove(); }
        else {
          el = document.createElement('article'); el.className = 'rel' + (i === 0 ? ' latest' : '');
          const notes = (r.body || '').split('\n').map(l => l.replace(/^[-*]\s+/, '').trim()).filter(l => l && !/^#|^\*\*Full Changelog/.test(l)).slice(0, 4);
          el.innerHTML = '<div><h3>' + esc(r.name || r.tag_name) + (i === 0 ? ' <span class="badge">Más reciente</span>' : '') + '</h3>'
            + '<div class="meta2"><span>' + date(r.published_at) + '</span>' + (exe ? '<span>' + mb(exe.size) + '</span>' : '') + '<span>Windows 10 y 11 · x64</span></div>'
            + (notes.length ? '<ul>' + notes.map(n => '<li>' + esc(n) + '</li>').join('') + '</ul>' : '') + '</div>'
            + '<div class="side">' + (exe ? '<a class="dl" href="' + esc(exe.browser_download_url) + '">' + tpl.querySelector('.dl svg').outerHTML + 'Descargar .exe</a>' : '')
            + '<a class="dl ghost" href="' + esc(r.html_url) + '">Notas completas</a></div>';
        }
        el.style.animation = reduce ? '' : 'rise .6s ' + (i * 0.07) + 's cubic-bezier(.2,.9,.25,1) both';
        box.appendChild(el);
      });
    })
    .catch(() => { /* sin red o sin cuota de la API: se queda la versión escrita en la página */ });
})();

/* ── imagen adjunta: arrastrar, pegar (Ctrl+V) o elegir, con vista previa ── */
(() => {
  document.querySelectorAll('[data-drop]').forEach(d => {
    const input = d.querySelector('input'), pv = d.querySelector('.pv'), img = pv.querySelector('img'), nm = pv.querySelector('.nm');
    let url = '';
    const kb = n => n < 1048576 ? Math.max(1, Math.round(n / 1024)) + ' KB' : (n / 1048576).toFixed(1).replace('.', ',') + ' MB';
    const render = () => {
      if (url) URL.revokeObjectURL(url), url = '';
      const f = input.files[0];
      d.classList.toggle('has', !!f); pv.hidden = !f;
      if (f) { url = URL.createObjectURL(f); img.src = url; nm.textContent = f.name + ' · ' + kb(f.size); }
      const box = d.closest('.field'); box.classList.remove('bad'); const er = box.querySelector(':scope > .err'); if (er) er.textContent = '';
    };
    const set = file => { try { const dt = new DataTransfer(); dt.items.add(file); input.files = dt.files; } catch { return; } render(); };
    d.clear = () => { input.value = ''; render(); };
    input.addEventListener('change', render);
    pv.querySelector('.rm').addEventListener('click', e => { e.preventDefault(); d.clear(); });
    ['dragenter', 'dragover'].forEach(t => d.addEventListener(t, e => { e.preventDefault(); d.classList.add('over'); }));
    ['dragleave', 'drop'].forEach(t => d.addEventListener(t, () => d.classList.remove('over')));
    d.addEventListener('drop', e => { e.preventDefault(); const f = [...(e.dataTransfer?.files || [])].find(x => x.type.startsWith('image/')); if (f) set(f); });
    d.paste = e => { const f = [...(e.clipboardData?.files || [])].find(x => x.type.startsWith('image/')); if (f) { e.preventDefault(); set(f); return true; } return false; };
  });
  // Ctrl+V con una captura (Win+Shift+S): va al formulario visible
  document.addEventListener('paste', e => {
    const panel = [...document.querySelectorAll('.fcard')].find(p => !p.hidden && p.querySelector('form:not([hidden])'));
    const d = panel && panel.querySelector('[data-drop]');
    if (!d) return;
    const r = panel.getBoundingClientRect();
    if (r.bottom < 0 || r.top > innerHeight) return;      /* solo si el formulario está a la vista */
    d.paste(e);
  });
})();
