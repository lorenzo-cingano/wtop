/* wtop landing page interactions
   - builds a live terminal recreation of the wtop process view
   - copy-to-clipboard for install commands
   - scroll reveals and nav scroll-spy
   Everything degrades to a static, readable state when motion is reduced
   or when JavaScript is unavailable (a screenshot fallback ships in <noscript>).
*/
(function () {
  'use strict';

  var reduceMotion = window.matchMedia &&
    window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* ---------------------------------------------------------------- helpers */
  function el(tag, cls, html) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (html != null) node.innerHTML = html;
    return node;
  }
  function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }
  function loadClass(v) { return v >= 80 ? 'm--hi' : v >= 45 ? 'm--mid' : 'm--lo'; }

  /* ---------------------------------------------------- live terminal build */
  var body = document.getElementById('term-body');

  // Eight logical cores, each with a resting load it drifts around.
  var cores = [62, 18, 9, 41, 4, 27, 12, 7];

  // A believable slice of a Windows process list.
  var procs = [
    { pid: 7264, cpu: 9.4,  mem: '212M', thr: 38, name: 'chrome.exe' },
    { pid: 1180, cpu: 6.1,  mem: '148M', thr: 51, name: 'MsMpEng.exe' },
    { pid: 9032, cpu: 4.8,  mem: '96.4M', thr: 24, name: 'WindowsTerminal.exe' },
    { pid: 4408, cpu: 3.2,  mem: '74.1M', thr: 19, name: 'explorer.exe' },
    { pid: 6720, cpu: 2.0,  mem: '52.8M', thr: 12, name: 'pwsh.exe' },
    { pid: 2884, cpu: 1.3,  mem: '38.2M', thr: 9,  name: 'svchost.exe' },
    { pid: 512,  cpu: 0.9,  mem: '21.0M', thr: 7,  name: 'dwm.exe' },
    { pid: 3140, cpu: 0.6,  mem: '14.7M', thr: 6,  name: 'svchost.exe' },
    { pid: 8816, cpu: 0.4,  mem: '9.80M', thr: 5,  name: 'RuntimeBroker.exe' },
    { pid: 1024, cpu: 0.2,  mem: '6.20M', thr: 4,  name: 'wtop.exe' },
    { pid: 4,    cpu: 0.0,  mem: '1.10M', thr: 2,  name: 'System' }
  ];

  var meters = [];   // { fill, pct, cell } per core
  var memFill, memPct;
  var rowEls = [];
  var sel = 0;

  function meterCell(id) {
    var cell = el('div', 'm m--lo');
    var idEl = el('span', 'm__id', String(id));
    var track = el('span', 'm__track');
    var bar = el('span', 'm__bar');
    var fill = el('span', 'm__fill');
    var pct = el('span', 'm__pct', '0%');
    bar.appendChild(fill);
    track.appendChild(bar);
    track.appendChild(pct);
    cell.appendChild(idEl);
    cell.appendChild(track);
    return { cell: cell, fill: fill, pct: pct };
  }

  function setMeter(m, v) {
    v = clamp(Math.round(v), 0, 100);
    m.fill.style.width = v + '%';
    m.pct.textContent = v + '%';
    m.cell.className = 'm ' + loadClass(v);
  }

  function buildTerminal() {
    if (!body) return;
    body.innerHTML = '';

    // CPU meter grid
    var grid = el('div', 'cpu-grid');
    for (var i = 0; i < cores.length; i++) {
      var m = meterCell(i + 1);
      meters.push(m);
      grid.appendChild(m.cell);
    }
    body.appendChild(grid);

    // Memory meter
    var memrow = el('div', 'memrow');
    var memId = el('span', 'm__id', 'Mem');
    var memTrack = el('span', 'm__track');
    var memBar = el('span', 'm__bar');
    memFill = el('span', 'm__fill');
    memPct = el('span', 'm__pct', '7.4G/16G');
    memBar.appendChild(memFill);
    memTrack.appendChild(memBar);
    memTrack.appendChild(memPct);
    memrow.appendChild(memId);
    memrow.appendChild(memTrack);
    body.appendChild(memrow);

    // Summary line
    body.appendChild(el('div', 'summary',
      '<b>233</b> tasks, sorted by <span class="badge">CPU%</span>'));

    // Process table
    var table = el('div', 'ptable');
    var head = el('div', 'prow prow--head',
      '<span class="r">PID</span>' +
      '<span class="r on">CPU%</span>' +
      '<span class="r">MEM</span>' +
      '<span class="r">THR</span>' +
      '<span class="l">COMMAND</span>');
    table.appendChild(head);

    for (var p = 0; p < procs.length; p++) {
      var d = procs[p];
      var row = el('div', 'prow');
      row.appendChild(rowCells(d));
      rowEls.push(row);
      table.appendChild(row);
    }
    body.appendChild(table);

    applySelection();
  }

  function cpuClass(v) { return v >= 5 ? 'cpuv-hi' : v > 0 ? 'cpuv-top' : 'dimv'; }

  function rowCells(d) {
    var frag = document.createDocumentFragment();
    frag.appendChild(el('span', 'r', String(d.pid)));
    frag.appendChild(el('span', 'r ' + cpuClass(d.cpu), d.cpu.toFixed(1)));
    frag.appendChild(el('span', 'r dimv', d.mem));
    frag.appendChild(el('span', 'r dimv', String(d.thr)));
    frag.appendChild(el('span', 'l', d.name));
    return frag;
  }

  function applySelection() {
    for (var i = 0; i < rowEls.length; i++) {
      if (i === sel) rowEls[i].classList.add('prow--sel');
      else rowEls[i].classList.remove('prow--sel');
    }
  }

  /* ---------------------------------------------------------- animation loop */
  function snapToValues() {
    for (var i = 0; i < meters.length; i++) setMeter(meters[i], cores[i]);
    if (memFill) memFill.style.width = '46%';
  }

  var memBase = 46;
  var timer = null;
  var ticks = 0;

  // One refresh: re-sample loads and nudge memory every tick (the live part),
  // and drift the selection only occasionally so it reads as gentle, not jumpy.
  function tick() {
    for (var i = 0; i < meters.length; i++) {
      var next = clamp(cores[i] + (Math.random() * 22 - 11), 0, 100);
      cores[i] = cores[i] * 0.6 + next * 0.4; // smooth toward the new sample
      setMeter(meters[i], cores[i]);
    }
    memBase = clamp(memBase + (Math.random() * 6 - 3), 38, 58);
    if (memFill) memFill.style.width = memBase.toFixed(0) + '%';
    if (memPct) memPct.textContent = (memBase / 100 * 16).toFixed(1) + 'G/16G';
    if (++ticks % 3 === 0) { sel = (sel + 1) % rowEls.length; applySelection(); }
  }

  function startLoop() { if (!timer) timer = setInterval(tick, 1600); }
  function stopLoop()  { if (timer) { clearInterval(timer); timer = null; } }

  function bootAndRun() {
    if (!body) return;

    if (reduceMotion) { snapToValues(); return; }

    // Start empty, then let the CSS transitions fill the meters in.
    requestAnimationFrame(function () {
      requestAnimationFrame(function () {
        for (var i = 0; i < meters.length; i++) setMeter(meters[i], cores[i]);
        if (memFill) memFill.style.width = '46%';
      });
    });

    // Run the live drift only while the terminal is on screen and the tab is
    // focused. No reason to animate pixels nobody can see.
    var onScreen = true;
    var term = document.getElementById('hero-term');
    function sync() {
      if (onScreen && !document.hidden) startLoop();
      else stopLoop();
    }
    if ('IntersectionObserver' in window && term) {
      new IntersectionObserver(function (entries) {
        onScreen = entries[0].isIntersecting;
        sync();
      }, { threshold: 0.08 }).observe(term);
    }
    document.addEventListener('visibilitychange', sync);
    sync();
  }

  buildTerminal();
  bootAndRun();

  /* ----------------------------------------------------------------- copy */
  var copyButtons = document.querySelectorAll('.copy');
  var status = document.getElementById('copy-status');

  function flash(btn) {
    var label = btn.querySelector('.copy__text');
    var original = label ? label.textContent : '';
    btn.classList.add('is-copied');
    if (label) label.textContent = 'Copied';
    if (status) status.textContent = 'Command copied to clipboard';
    setTimeout(function () {
      btn.classList.remove('is-copied');
      if (label) label.textContent = original || 'Copy';
    }, 1400);
  }

  for (var b = 0; b < copyButtons.length; b++) {
    copyButtons[b].addEventListener('click', function () {
      var text = this.getAttribute('data-copy') || '';
      var btn = this;
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () { flash(btn); });
      } else {
        var ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        try { document.execCommand('copy'); flash(btn); } catch (e) {}
        document.body.removeChild(ta);
      }
    });
  }

  /* ------------------------------------------------------- reveal on scroll */
  var revealTargets = document.querySelectorAll(
    '.block, .card, .iblock, .io__shot, .divider, .keyrow');
  for (var r = 0; r < revealTargets.length; r++) {
    revealTargets[r].setAttribute('data-reveal', '');
  }

  if (reduceMotion || !('IntersectionObserver' in window)) {
    for (var k = 0; k < revealTargets.length; k++) revealTargets[k].classList.add('in');
  } else {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add('in');
          io.unobserve(entry.target);
        }
      });
    }, { rootMargin: '0px 0px -8% 0px', threshold: 0.12 });
    for (var t = 0; t < revealTargets.length; t++) io.observe(revealTargets[t]);
  }

  /* --------------------------------------------------------- nav scroll-spy */
  var tabs = document.querySelectorAll('.tab');
  var spyMap = [
    { id: 'keys',    tab: 'Keys' },
    { id: 'install', tab: 'Install' },
    { id: 'io',      tab: 'I/O' },
    { id: 'what',    tab: 'Processes' }
  ];
  function setActiveTab(name) {
    for (var i = 0; i < tabs.length; i++) {
      var on = tabs[i].textContent.trim() === name;
      tabs[i].classList.toggle('tab--active', on);
      if (on) tabs[i].setAttribute('aria-current', 'true');
      else tabs[i].removeAttribute('aria-current');
    }
  }
  function onScroll() {
    var pos = window.scrollY + window.innerHeight * 0.34;
    var active = 'Processes';
    for (var i = 0; i < spyMap.length; i++) {
      var node = document.getElementById(spyMap[i].id);
      if (node && pos >= node.offsetTop) { active = spyMap[i].tab; break; }
    }
    setActiveTab(active);
  }
  var ticking = false;
  window.addEventListener('scroll', function () {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(function () { onScroll(); ticking = false; });
  }, { passive: true });
  onScroll();
})();
