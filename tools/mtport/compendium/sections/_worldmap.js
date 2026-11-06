/* The compendium's interactive world map (#232): world/map.html.
 *
 * Hand-written, no libraries. The data is the page's <script id="wm-data"> (the same JSON as
 * world/worldmap.json; links are relative to world/). Three levels: the world (the game's
 * world-map art with the areas pinned at its spots), an area (its maps as tiles joined by
 * their warps) and a map (its render with markers). Pointer drag, touch pinch, wheel, keys
 * and buttons pan and zoom; a panel (a bottom sheet on phones) shows what to do at the
 * selected place. The pure functions at the top are exported for tests under Node.
 */
(function () {
  "use strict";

  // --- pure functions ------------------------------------------------------------------------

  /** "none" (no monsters), "hard", "easy" or "good" for a map's monster levels [lo, hi]. */
  function levelFit(lv, hero, fit) {
    if (!lv || hero == null || isNaN(hero)) return "none";
    var above = fit ? fit.above : 5, below = fit ? fit.below : 10;
    if (lv[0] > hero + above) return "hard";
    if (lv[1] < hero - below) return "easy";
    return "good";
  }

  /** Whether something of chapters `ch` (a number, a list, or null/[] for any) shows in
   *  `chapter` (null: every chapter). */
  function inChapter(ch, chapter) {
    if (chapter == null) return true;
    if (ch == null) return true;
    if (typeof ch === "number") return ch === chapter;
    return ch.length === 0 || ch.indexOf(chapter) >= 0;
  }

  /** A warp is open in `chapter` when its chapters include it (null: any warp). */
  function edgeOpen(e, chapter) {
    return chapter == null || (e.ch || []).indexOf(chapter) >= 0;
  }

  /** Directed adjacency {map: [{b, c}]} of the open warps; `c`: every warp of the step has a
   *  condition. */
  function graph(edges, chapter) {
    var adj = {};
    edges.forEach(function (e) {
      if (!edgeOpen(e, chapter)) return;
      var list = adj[e.a] || (adj[e.a] = []);
      for (var i = 0; i < list.length; i++) {
        if (list[i].b === e.b) {
          list[i].c = list[i].c && e.c;
          return;
        }
      }
      list.push({ b: e.b, c: !!e.c });
    });
    return adj;
  }

  /** The fewest map changes from `from` to `to` over the open warps: {path, steps} or null.
   *  Breadth first, neighbours in edge order, so the answer is stable. */
  function route(edges, from, to, chapter) {
    if (from == null || to == null) return null;
    if (from === to) return { path: [from], steps: [] };
    var adj = graph(edges, chapter), prev = {}, q = [from], seen = {};
    seen[from] = true;
    while (q.length) {
      var a = q.shift(), next = adj[a] || [];
      for (var i = 0; i < next.length; i++) {
        var b = next[i].b;
        if (seen[b]) continue;
        seen[b] = true;
        prev[b] = { a: a, c: next[i].c };
        if (b === to) {
          var path = [to], steps = [], x = to;
          while (x !== from) {
            steps.unshift({ a: prev[x].a, b: x, c: prev[x].c });
            x = prev[x].a;
            path.unshift(x);
          }
          return { path: path, steps: steps };
        }
        q.push(b);
      }
    }
    return null;
  }

  function norm(s) {
    s = String(s == null ? "" : s);
    if (s.normalize) s = s.normalize("NFKC");
    return s.toLowerCase().trim();
  }

  /** Places matching `q`: exact, then prefix, then substring of a name (English or Korean),
   *  or the map number ("2.3" or "2-3"); areas before maps on a tie, then data order. */
  function searchPlaces(data, q, limit) {
    q = norm(q);
    if (!q) return [];
    var hits = [], n = 0;
    function score(names, extra) {
      var best = 9;
      [names.en, names.ko].concat(extra || []).forEach(function (h) {
        h = norm(h);
        if (!h) return;
        var s = h === q ? 0 : h.indexOf(q) === 0 ? 1 : h.indexOf(q) >= 0 ? 2 : 9;
        if (s < best) best = s;
      });
      return best;
    }
    data.areas.forEach(function (a) {
      var s = score(a.n, ["area " + a.id]);
      if (s < 9) hits.push({ kind: "area", id: a.id, s: s, o: n });
      n++;
    });
    data.maps.forEach(function (m) {
      var s = score(m.n, [m.num, m.id]);
      if (s < 9) hits.push({ kind: "map", id: m.id, s: s, o: n });
      n++;
    });
    hits.sort(function (x, y) { return x.s - y.s || x.o - y.o; });
    return hits.slice(0, limit || 8);
  }

  /** The deep link of a hash: {map, area, open (the map level), from, to, ch, lv}. */
  function parseHash(hash) {
    var out = {};
    String(hash || "").replace(/^#/, "").split("&").forEach(function (part) {
      var i = part.indexOf("=");
      if (i < 0) return;
      var k = part.slice(0, i), v = decodeURIComponent(part.slice(i + 1));
      if (k === "map" && /^\d+-\d+$/.test(v)) out.map = v;
      else if (k === "area" && /^\d+$/.test(v)) out.area = v;
      else if (k === "route") {
        var ends = v.split("~");
        if (ends.length === 2 && /^\d+-\d+$/.test(ends[0]) && /^\d+-\d+$/.test(ends[1])) {
          out.from = ends[0];
          out.to = ends[1];
        }
      } else if (k === "ch" && /^\d+$/.test(v)) out.ch = +v;
      else if (k === "lv" && /^\d+$/.test(v)) out.lv = +v;
      else if (k === "level" && v === "map") out.open = true;
    });
    return out;
  }

  function formatHash(st) {
    var parts = [];
    if (st.map) parts.push("map=" + st.map + (st.open ? "&level=map" : ""));
    else if (st.area) parts.push("area=" + st.area);
    if (st.from && st.to) parts.push("route=" + st.from + "~" + st.to);
    if (st.ch != null) parts.push("ch=" + st.ch);
    if (st.lv != null) parts.push("lv=" + st.lv);
    return parts.length ? "#" + parts.join("&") : "";
  }

  /** The transform {x, y, k} that shows a `b` (w, h) box whole in a view `v` (w, h). */
  function fitView(b, v, pad) {
    pad = pad == null ? 24 : pad;
    var k = Math.min((v.w - 2 * pad) / b.w, (v.h - 2 * pad) / b.h);
    if (!(k > 0)) k = 1;
    return { x: (v.w - b.w * k) / 2, y: (v.h - b.h * k) / 2, k: k };
  }

  /** Zoom by `f` about the view point (px, py), keeping k within [kMin, kMax]. */
  function zoomAt(t, f, px, py, kMin, kMax) {
    var k = Math.max(kMin, Math.min(kMax, t.k * f));
    var r = k / t.k;
    return { x: px - (px - t.x) * r, y: py - (py - t.y) * r, k: k };
  }

  /** Keep part of the content in view: at least `keep` (0 to 1) of the smaller of the view
   *  and the content overlaps on each axis. */
  function clampView(t, b, v, keep) {
    keep = keep == null ? 0.3 : keep;
    var w = b.w * t.k, h = b.h * t.k;
    var mx = keep * Math.min(v.w, w), my = keep * Math.min(v.h, h);
    return {
      x: Math.min(v.w - mx, Math.max(mx - w, t.x)),
      y: Math.min(v.h - my, Math.max(my - h, t.y)),
      k: t.k
    };
  }

  /** What a map offers, as counts per layer, in `chapter` (null: every chapter). */
  function mapCounts(m, chapter) {
    function n(list) {
      return (list || []).filter(function (x) { return inChapter(x.ch, chapter); }).length;
    }
    var bosses = n(m.events) + (m.mobs || []).filter(function (x) { return x.boss; }).length;
    return {
      train: m.train ? 1 : 0,
      shops: n(m.shops),
      quests: n(m.quests),
      craft: n(m.recipes) + n(m.trades),
      bosses: bosses,
      secrets: (m.portals || []).filter(function (p) { return p.hidden; }).length
    };
  }

  /** The best maps to train on at `hero` level: good fit, most EXP per HP first. */
  function trainingPicks(data, hero, limit) {
    if (hero == null || isNaN(hero)) return [];
    return data.maps
      .filter(function (m) { return m.train && levelFit(m.lv, hero, data.fit) === "good"; })
      .sort(function (a, b) { return b.train.eph - a.train.eph || (a.id < b.id ? -1 : 1); })
      .slice(0, limit || 5);
  }

  /** The areas a route passes, in order, without repeats in a row. */
  function routeAreas(path, mapsById) {
    var out = [];
    path.forEach(function (id) {
      var a = mapsById[id] && mapsById[id].area;
      if (a != null && out[out.length - 1] !== a) out.push(a);
    });
    return out;
  }

  function ease(t) {
    return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
  }

  var api = {
    levelFit: levelFit, inChapter: inChapter, edgeOpen: edgeOpen, graph: graph, route: route,
    searchPlaces: searchPlaces, parseHash: parseHash, formatHash: formatHash, fitView: fitView,
    zoomAt: zoomAt, clampView: clampView, mapCounts: mapCounts, trainingPicks: trainingPicks,
    routeAreas: routeAreas, ease: ease
  };
  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
    return;
  }
  if (typeof document === "undefined") return;

  // --- the viewer ------------------------------------------------------------------------------

  var dataEl = document.getElementById("wm-data"), wm = document.getElementById("wm");
  if (!dataEl || !wm) return;
  var D;
  try {
    D = JSON.parse(dataEl.textContent);
  } catch (err) {
    return;
  }
  var $ = function (id) { return document.getElementById(id); };
  var view = $("wm-view"), stage = $("wm-stage"), panel = $("wm-panel"), body = $("wm-panel-body");
  var crumbs = $("wm-crumbs"), status = $("wm-status");
  var mapsById = {}, areasById = {}, spotsById = {};
  D.maps.forEach(function (m) { mapsById[m.id] = m; });
  D.areas.forEach(function (a) { areasById[a.id] = a; });
  D.spots.forEach(function (s) { spotsById[s.id] = s; });
  var reduced = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)");
  function still() { return reduced && reduced.matches; }

  var LAYERS = ["train", "shops", "quests", "craft", "bosses", "secrets"];
  var LAYER_WORDS = {
    train: "training spot", shops: "shop", quests: "quest", craft: "recipe or trade",
    bosses: "boss or event", secrets: "hidden portal"
  };
  // Small icons of our own, 16 by 16.
  var ICONS = {
    train: '<circle cx="8" cy="8" r="5.2" fill="none" stroke="currentColor" stroke-width="2"/>' +
      '<circle cx="8" cy="8" r="2" fill="currentColor"/>',
    shops: '<path d="M3.5 6.5h9l-.8 7h-7.4z" fill="currentColor"/><path d="M6 6.5V5a2 2 0 0 1 4 ' +
      '0v1.5" fill="none" stroke="currentColor" stroke-width="1.6"/>',
    quests: '<path d="M8 2.5v7" stroke="currentColor" stroke-width="2.6" ' +
      'stroke-linecap="round"/><circle cx="8" cy="13" r="1.5" fill="currentColor"/>',
    craft: '<path d="M2.5 13.5l6-6" stroke="currentColor" stroke-width="2.2" ' +
      'stroke-linecap="round"/><path d="M7 4.2l2.6-1.7 4 4-1.7 2.6z" fill="currentColor"/>',
    bosses: '<path d="M2 12.5l1-7.5 3 3.2L8 3l2 5.2 3-3.2 1 7.5z" fill="currentColor"/>',
    secrets: '<path d="M8 1.5l1.6 4.9L14.5 8l-4.9 1.6L8 14.5l-1.6-4.9L1.5 8l4.9-1.6z" ' +
      'fill="currentColor"/>'
  };
  function icon(layer) {
    return '<svg class="wm-ico" viewBox="0 0 16 16" aria-hidden="true" focusable="false">' +
      ICONS[layer] + "</svg>";
  }

  var S = {
    level: "world", area: null, map: null, hero: null, ch: null,
    layers: { train: true, shops: true, quests: true, craft: true, bosses: true, secrets: true },
    from: null, to: null, routeOn: false, result: null, marks: true
  };
  var T = { x: 0, y: 0, k: 1 }, B = { w: 1, h: 1 }, K = { min: 0.1, max: 8 };
  // Below this zoom the world's spot cards would overlap: they show the levels only.
  var COMPACT = 2.2;

  // --- small DOM helpers
  function h(tag, attrs, kids) {
    var el = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        var v = attrs[k];
        if (v == null || v === false) return;
        if (k === "text") el.textContent = v;
        else if (k === "html") el.innerHTML = v; // only our own markup (icons)
        else if (k === "on") Object.keys(v).forEach(function (e) { el.addEventListener(e, v[e]); });
        else el.setAttribute(k, v === true ? "" : v);
      });
    }
    (kids || []).forEach(function (c) {
      if (c == null || c === false) return;
      el.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
    });
    return el;
  }
  /** Game text: the English name, marked as such, with the Korean as its title. */
  function gname(n, cls) {
    return h("span", { "data-verbatim": true, lang: "en", class: cls || null,
      title: n.ko && n.ko !== n.en ? n.ko : null, text: n.en || n.ko || "" });
  }
  function link(u, kids, cls) { return h("a", { href: u, class: cls || null }, kids); }
  function lvText(lv) {
    if (!lv) return "";
    return lv[0] === lv[1] ? "Lv. " + lv[0] : "Lv. " + lv[0] + " to " + lv[1];
  }
  function chText(ch) {
    if (ch == null || (ch.length === 0)) return "";
    if (typeof ch === "number") return "Chapter " + ch;
    return (ch.length === 1 ? "Chapter " : "Chapters ") + ch.join(", ");
  }
  function say(text) { status.textContent = text; }
  function mapLabel(m) { return (m.n.en || m.n.ko) + " (" + m.num + ")"; }

  // --- transform
  var frame = 0;
  function apply() {
    frame = 0;
    stage.style.transform = "translate(" + T.x.toFixed(2) + "px," + T.y.toFixed(2) + "px) scale(" +
      T.k.toFixed(5) + ")";
    stage.style.setProperty("--k", T.k.toFixed(5));
    stage.style.setProperty("--inv", (1 / T.k).toFixed(5));
    if (S.level === "area") {
      upgradeTiles();
      stage.classList.toggle("wm-small", T.k * D.tile.w < 72);
      stage.classList.toggle("wm-mid", T.k * D.tile.w < 150);
    }
    if (S.level === "world") stage.classList.toggle("wm-compact", T.k < COMPACT);
  }
  function schedule() { if (!frame) frame = requestAnimationFrame(apply); }
  function viewSize() {
    var r = view.getBoundingClientRect();
    return { w: r.width || 1, h: r.height || 1 };
  }
  /** The part of the view the bottom sheet leaves free (phones). */
  function freeView() {
    var v = viewSize();
    if (sheetMode()) {
      var top = panel.getBoundingClientRect().top - view.getBoundingClientRect().top;
      if (top > 80 && top < v.h) v.h = top;
    }
    return v;
  }
  function set(t) {
    T = clampView(t, B, viewSize(), 0.3);
    schedule();
  }
  var anim = 0, flights = 0;
  function stop() { if (anim) cancelAnimationFrame(anim); anim = 0; flights++; }
  /** Fly to transform `to`: the view centre moves in content space and the zoom in log
   *  space, so a long flight does not swoop; instant when motion is reduced. */
  function fly(to, ms) {
    stop();
    to = clampView(to, B, viewSize(), 0.3);
    if (still() || ms === 0) { T = to; schedule(); return; }
    var v = viewSize(), from = T, t0 = performance.now(), dur = ms || 450;
    var c0 = { x: (v.w / 2 - from.x) / from.k, y: (v.h / 2 - from.y) / from.k };
    var c1 = { x: (v.w / 2 - to.x) / to.k, y: (v.h / 2 - to.y) / to.k };
    var l0 = Math.log(from.k), l1 = Math.log(to.k), id = ++flights;
    // land even when frames do not come (a hidden tab, a headless snapshot)
    setTimeout(function () {
      if (id === flights && anim) { stop(); T = to; apply(); }
    }, dur + 120);
    function step(now) {
      var p = Math.min(1, (now - t0) / dur), e = ease(p);
      var k = Math.exp(l0 + (l1 - l0) * e);
      var cx = c0.x + (c1.x - c0.x) * e, cy = c0.y + (c1.y - c0.y) * e;
      T = { x: v.w / 2 - cx * k, y: v.h / 2 - cy * k, k: k };
      apply();
      anim = p < 1 ? requestAnimationFrame(step) : 0;
    }
    anim = requestAnimationFrame(step);
  }
  function fitAll(ms) {
    var v = freeView(), t = fitView(B, v, 20);
    K.min = t.k * 0.6;
    if (S.level === "world" && v.h > v.w * 1.2) {
      // a phone held upright: fill more of the height and let the reader pan sideways
      // centred on the spots, not on the picture
      var k = Math.min(v.h / B.h * 0.9, t.k * 1.7), xs = [], ys = [];
      D.spots.forEach(function (s) { xs.push(s.x); ys.push(s.y); });
      var cx = (Math.min.apply(null, xs) + Math.max.apply(null, xs)) / 2;
      var cy = (Math.min.apply(null, ys) + Math.max.apply(null, ys)) / 2;
      t = { x: v.w / 2 - cx * k, y: v.h / 2 - cy * k, k: k };
    }
    fly(t, ms);
  }
  /** Show the box (x, y, w, h) of content at about `share` of the free view. */
  function focusBox(bx, ms, share) {
    var v = freeView(), k = Math.min(v.w / bx.w, v.h / bx.h) * (share || 0.5);
    k = Math.max(K.min, Math.min(K.max, k));
    fly({ x: v.w / 2 - (bx.x + bx.w / 2) * k, y: v.h / 2 - (bx.y + bx.h / 2) * k, k: k }, ms);
  }
  function ensureVisible(bx) {
    var v = freeView(), x0 = T.x + bx.x * T.k, y0 = T.y + bx.y * T.k;
    var x1 = x0 + bx.w * T.k, y1 = y0 + bx.h * T.k, dx = 0, dy = 0, m = 16;
    if (x0 < m) dx = m - x0; else if (x1 > v.w - m) dx = v.w - m - x1;
    if (y0 < m) dy = m - y0; else if (y1 > v.h - m) dy = v.h - m - y1;
    if (dx || dy) fly({ x: T.x + dx, y: T.y + dy, k: T.k }, 250);
  }

  // --- pointer, wheel and keys
  var pts = {}, gesture = null, suppress = false, velocity = null;
  function count() { return Object.keys(pts).length; }
  function pair() {
    var ids = Object.keys(pts), a = pts[ids[0]], b = pts[ids[1]];
    return { d: Math.hypot(a.x - b.x, a.y - b.y) || 1, x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
  }
  function local(e) {
    var r = view.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }
  function used() { wm.classList.add("wm-used"); }
  ["pointerdown", "wheel", "keydown"].forEach(function (t) {
    view.addEventListener(t, used, { passive: true });
  });
  view.addEventListener("pointerdown", function (e) {
    if (e.pointerType === "mouse" && e.button !== 0) return;
    if (e.target.closest && e.target.closest(".wm-panel, .wm-zoom, .wm-crumbs")) return;
    stop();
    pts[e.pointerId] = local(e);
    if (count() === 1) {
      gesture = { kind: "pan", p0: pts[e.pointerId], t0: T, moved: false, id: e.pointerId };
      velocity = { x: 0, y: 0, t: performance.now(), p: pts[e.pointerId] };
    } else if (count() === 2) {
      var p = pair();
      gesture = { kind: "pinch", d0: p.d, c0: p, t0: T, moved: true };
      Object.keys(pts).forEach(function (id) {
        try { view.setPointerCapture(+id); } catch (err) { /* gone */ }
      });
      view.classList.add("wm-dragging");
    }
  });
  view.addEventListener("pointermove", function (e) {
    if (!(e.pointerId in pts) || !gesture) return;
    pts[e.pointerId] = local(e);
    if (gesture.kind === "pan" && count() === 1) {
      var p = pts[e.pointerId], dx = p.x - gesture.p0.x, dy = p.y - gesture.p0.y;
      if (!gesture.moved && Math.hypot(dx, dy) > 6) {
        gesture.moved = true;
        try { view.setPointerCapture(e.pointerId); } catch (err) { /* gone */ }
        view.classList.add("wm-dragging");
      }
      if (gesture.moved) {
        var now = performance.now(), dt = Math.max(1, now - velocity.t);
        velocity = {
          x: 0.8 * ((p.x - velocity.p.x) / dt) + 0.2 * velocity.x,
          y: 0.8 * ((p.y - velocity.p.y) / dt) + 0.2 * velocity.y, t: now, p: p
        };
        set({ x: gesture.t0.x + dx, y: gesture.t0.y + dy, k: gesture.t0.k });
      }
    } else if (gesture.kind === "pinch" && count() >= 2) {
      var q = pair(), t0 = gesture.t0;
      var k = Math.max(K.min, Math.min(K.max, t0.k * q.d / gesture.d0)), r = k / t0.k;
      set({
        x: q.x - (gesture.c0.x - t0.x) * r, y: q.y - (gesture.c0.y - t0.y) * r, k: k
      });
    }
  });
  function end(e) {
    if (!(e.pointerId in pts)) return;
    delete pts[e.pointerId];
    if (gesture && gesture.moved) {
      suppress = true;
      setTimeout(function () { suppress = false; }, 0);
    }
    if (count() === 0) {
      view.classList.remove("wm-dragging");
      if (gesture && gesture.kind === "pan" && gesture.moved && !still() &&
          performance.now() - velocity.t < 80) glide(velocity.x, velocity.y);
      gesture = null;
    } else if (count() === 1) {
      var id = Object.keys(pts)[0];
      gesture = { kind: "pan", p0: pts[id], t0: T, moved: true, id: +id };
      velocity = { x: 0, y: 0, t: performance.now(), p: pts[id] };
    }
  }
  view.addEventListener("pointerup", end);
  view.addEventListener("pointercancel", end);
  view.addEventListener("click", function (e) {
    if (suppress) { e.stopPropagation(); e.preventDefault(); }
  }, true);
  /** Momentum after a fling: the speed decays by a tenth a frame. */
  function glide(vx, vy) {
    var last = performance.now();
    function step(now) {
      var dt = Math.min(40, now - last);
      last = now;
      vx *= Math.pow(0.9, dt / 16);
      vy *= Math.pow(0.9, dt / 16);
      if (Math.hypot(vx, vy) < 0.02) { anim = 0; return; }
      set({ x: T.x + vx * dt, y: T.y + vy * dt, k: T.k });
      anim = requestAnimationFrame(step);
    }
    if (Math.hypot(vx, vy) > 0.15) anim = requestAnimationFrame(step);
  }
  view.addEventListener("wheel", function (e) {
    if (e.target.closest && e.target.closest(".wm-panel")) return;
    e.preventDefault();
    stop();
    var dy = e.deltaY * (e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 400 : 1);
    var p = local(e);
    set(zoomAt(T, Math.exp(-dy * (e.ctrlKey ? 0.01 : 0.0022)), p.x, p.y, K.min, K.max));
  }, { passive: false });
  view.addEventListener("dblclick", function (e) {
    if (e.target.closest && e.target.closest("button, a, .wm-panel")) return;
    var p = local(e);
    fly(zoomAt(T, 2, p.x, p.y, K.min, K.max), 300);
  });
  function zoomBy(f) {
    var v = viewSize();
    fly(zoomAt(T, f, v.w / 2, v.h / 2, K.min, K.max), 220);
  }
  view.addEventListener("keydown", function (e) {
    var tag = e.target.tagName;
    if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;
    if (e.target.closest && e.target.closest(".wm-panel")) return;
    if (e.altKey || e.ctrlKey || e.metaKey) return;
    var step = e.shiftKey ? 240 : 80, done = true;
    switch (e.key) {
      case "ArrowLeft": fly({ x: T.x + step, y: T.y, k: T.k }, 150); break;
      case "ArrowRight": fly({ x: T.x - step, y: T.y, k: T.k }, 150); break;
      case "ArrowUp": fly({ x: T.x, y: T.y + step, k: T.k }, 150); break;
      case "ArrowDown": fly({ x: T.x, y: T.y - step, k: T.k }, 150); break;
      case "+": case "=": zoomBy(1.5); break;
      case "-": case "_": zoomBy(1 / 1.5); break;
      case "0": fitAll(); break;
      case "Escape": up(); break;
      default: done = false;
    }
    if (done) e.preventDefault();
  });
  $("wm-in").addEventListener("click", function () { zoomBy(1.5); });
  $("wm-out").addEventListener("click", function () { zoomBy(1 / 1.5); });
  $("wm-fit").addEventListener("click", function () { fitAll(); });

  // --- filters
  function fitOf(lv) { return levelFit(lv, S.hero, D.fit); }
  function badges(c, cls) {
    var box = h("span", { class: cls || "wm-badges", "aria-hidden": "true" });
    LAYERS.forEach(function (l) {
      if (!S.layers[l] || !c[l]) return;
      box.appendChild(h("span", { class: "wm-badge wm-l-" + l, html: icon(l) }));
    });
    return box;
  }
  function countsWords(c) {
    var out = [];
    LAYERS.forEach(function (l) {
      if (!c[l]) return;
      if (l === "train") out.push("a training spot");
      else out.push(c[l] + " " + LAYER_WORDS[l] + (c[l] > 1 ? "s" : ""));
    });
    return out.join(", ");
  }
  function areaCounts(a) {
    var sum = { train: 0, shops: 0, quests: 0, craft: 0, bosses: 0, secrets: 0 };
    a.maps.forEach(function (id) {
      var c = mapCounts(mapsById[id], S.ch);
      LAYERS.forEach(function (l) { sum[l] += c[l]; });
    });
    return sum;
  }
  function areaFit(a) {
    var best = "none";
    a.maps.forEach(function (id) {
      var f = fitOf(mapsById[id].lv);
      if (f === "good") best = "good";
      else if (best === "none" || (best === "easy" && f === "hard")) best = f === "none" ? best : f;
    });
    return best;
  }

  // --- the three levels
  var nodes = {};
  function clear() {
    stage.textContent = "";
    nodes = {};
  }
  function svgLayer(w, hgt) {
    var ns = "http://www.w3.org/2000/svg", svg = document.createElementNS(ns, "svg");
    svg.setAttribute("class", "wm-lines");
    svg.setAttribute("viewBox", "0 0 " + w + " " + hgt);
    svg.setAttribute("width", w);
    svg.setAttribute("height", hgt);
    svg.setAttribute("aria-hidden", "true");
    return svg;
  }
  function line(svg, x1, y1, x2, y2, cls) {
    var l = document.createElementNS("http://www.w3.org/2000/svg", "line");
    l.setAttribute("x1", x1);
    l.setAttribute("y1", y1);
    l.setAttribute("x2", x2);
    l.setAttribute("y2", y2);
    l.setAttribute("class", cls);
    l.setAttribute("vector-effect", "non-scaling-stroke");
    svg.appendChild(l);
    return l;
  }

  function renderWorld() {
    clear();
    B = { w: D.world.w, h: D.world.h };
    K.max = 12;
    stage.className = "wm-stage wm-world" + (D.world.art ? " wm-has-art" : "");
    stage.style.width = B.w + "px";
    stage.style.height = B.h + "px";
    if (D.world.art) {
      stage.appendChild(h("img", { class: "wm-art", src: D.world.art.s, width: D.world.art.w,
        height: D.world.art.h, alt: "", draggable: "false" }));
    }
    var svg = svgLayer(B.w, B.h);
    stage.appendChild(svg);
    if (!D.world.art) {
      var seen = {};
      D.edges.forEach(function (e) {
        var a = areasById[mapsById[e.a] && mapsById[e.a].area];
        var b = areasById[mapsById[e.b] && mapsById[e.b].area];
        if (!a || !b || a === b) return;
        var key = [a.spot, b.spot].sort().join("~");
        if (seen[key]) return;
        seen[key] = true;
        var sa = spotsById[a.spot], sb = spotsById[b.spot];
        line(svg, sa.x, sa.y, sb.x, sb.y, "wm-link");
      });
    }
    if (S.result && S.result.path.length > 1) {
      var ar = routeAreas(S.result.path, mapsById), prev = null;
      ar.forEach(function (aid) {
        var s = spotsById[areasById[aid].spot];
        var pnt = { x: s.x, y: s.y };
        if (prev) line(svg, prev.x, prev.y, pnt.x, pnt.y, "wm-route-line");
        prev = pnt;
      });
    }
    D.spots.forEach(function (s) {
      var spot = h("div", { class: "wm-spot", style: "left:" + s.x + "px;top:" + s.y + "px" });
      if (!D.world.art || s.areas.length > 1) {
        spot.appendChild(gname(s.n, "wm-spot-name"));
      }
      s.areas.forEach(function (aid) {
        var a = areasById[aid], c = areaCounts(a), fit = areaFit(a);
        var words = (a.n.en || a.n.ko) + ", area " + a.id + ", " + a.maps.length + " maps" +
          (a.lv ? ", monsters " + lvText(a.lv) : "");
        var btn = h("button", {
          type: "button", class: "wm-pin wm-fit-" + fit + (S.area === aid ? " wm-sel" : ""),
          "aria-label": words + (countsWords(c) ? ". " + countsWords(c) : ""),
          on: { click: function () { goArea(aid, { from: "world" }); } }
        }, [
          h("span", { class: "wm-pin-num", "aria-hidden": "true", text: a.id }),
          h("span", { class: "wm-pin-text" }, [
            s.areas.length > 1 ? h("span", { text: "Area " + a.id }) : gname(a.n),
            a.lv ? h("span", { class: "wm-pin-lv", text: lvText(a.lv) }) : null
          ]),
          badges(c)
        ]);
        nodes["area:" + aid] = btn;
        spot.appendChild(btn);
      });
      stage.appendChild(spot);
    });
  }

  function tileImg(m) {
    var src = m.thumb || m.img;
    if (!src) return h("span", { class: "wm-noimg", "aria-hidden": "true" });
    return h("img", { src: src.s, alt: "", width: src.w, height: src.h, loading: "lazy",
      decoding: "async", draggable: "false" });
  }
  function upgradeTiles() {
    Object.keys(nodes).forEach(function (key) {
      var el = nodes[key];
      if (key.indexOf("map:") !== 0 || el.dataset.big) return;
      var m = mapsById[key.slice(4)];
      if (!m.thumb || !m.img) return;
      if (D.tile.w * T.k > m.thumb.w * 1.4) {
        el.dataset.big = "1";
        var img = el.querySelector("img");
        if (img) img.src = m.img.s;
      }
    });
  }
  function pairKey(a, b) { return a < b ? a + "~" + b : b + "~" + a; }
  function routePairs() {
    var on = {};
    if (S.result) S.result.steps.forEach(function (s) { on[pairKey(s.a, s.b)] = true; });
    return on;
  }
  function renderArea(aid) {
    clear();
    var a = areasById[aid];
    B = { w: a.w, h: a.h };
    K.max = 6;
    stage.className = "wm-stage wm-area";
    stage.style.width = B.w + "px";
    stage.style.height = B.h + "px";
    var svg = svgLayer(B.w, B.h), inside = {}, centers = {}, onRoute = routePairs();
    stage.appendChild(svg);
    a.maps.forEach(function (id) {
      var m = mapsById[id];
      inside[id] = true;
      centers[id] = { x: m.x + m.w / 2, y: m.y + m.h / 2 };
    });
    a.outs.forEach(function (o) { centers["out:" + o.map] = { x: o.x + o.w / 2, y: o.y + o.h / 2 }; });
    var pairs = {};
    D.edges.forEach(function (e) {
      var x = e.a, y = e.b;
      if (!inside[x] && !inside[y]) return;
      var cx = inside[x] ? x : "out:" + x, cy = inside[y] ? y : "out:" + y;
      if (!centers[cx] || !centers[cy] || cx === cy) return;
      var key = pairKey(cx, cy), p = pairs[key];
      if (!p) p = pairs[key] = { a: cx, b: cy, c: true, open: false, route: false };
      p.c = p.c && e.c;
      p.open = p.open || edgeOpen(e, S.ch);
      if (onRoute[pairKey(x, y)]) p.route = true;
    });
    Object.keys(pairs).sort().forEach(function (key) {
      var p = pairs[key], u = centers[p.a], v = centers[p.b];
      line(svg, u.x, u.y, v.x, v.y, "wm-link" + (p.c ? " wm-cond" : "") +
        (p.open ? "" : " wm-closed") + (p.route ? " wm-on-route" : ""));
    });
    var inRoute = {};
    if (S.result) S.result.path.forEach(function (id) { inRoute[id] = true; });
    var order = a.maps.slice().sort(function (x, y) {
      var m = mapsById[x], n = mapsById[y];
      return m.y - n.y || m.x - n.x;
    });
    order.forEach(function (id) {
      var m = mapsById[id], c = mapCounts(m, S.ch), fit = fitOf(m.lv);
      var words = mapLabel(m) + ", " + m.type + (m.lv ? ", monsters " + lvText(m.lv) : "") +
        (countsWords(c) ? ". " + countsWords(c) : "");
      var tile = h("button", {
        type: "button",
        class: "wm-tile wm-fit-" + fit + " wm-type-" + m.type.replace(/\W+/g, "-") +
          (S.map === id ? " wm-sel" : "") + (inRoute[id] ? " wm-in-route" : "") +
          (m.reached ? "" : " wm-unreached"),
        style: "left:" + m.x + "px;top:" + m.y + "px;width:" + m.w + "px;height:" + m.h + "px",
        "aria-label": words, "aria-pressed": S.map === id ? "true" : "false",
        on: {
          click: function () { selectMap(id); },
          dblclick: function (e) { e.stopPropagation(); goMap(id); },
          focus: function () { ensureVisible({ x: m.x, y: m.y, w: m.w, h: m.h + 50 }); }
        }
      }, [
        tileImg(m),
        badges(c),
        h("span", { class: "wm-tile-label" }, [
          gname(m.n, "wm-tile-name"),
          h("span", { class: "wm-tile-meta", text: m.num + (m.lv ? ", " + lvText(m.lv) : "") })
        ])
      ]);
      nodes["map:" + id] = tile;
      stage.appendChild(tile);
    });
    a.outs.forEach(function (o) {
      var m = mapsById[o.map], oa = areasById[o.area];
      if (!m) return;
      var b = h("button", {
        type: "button", class: "wm-stub" + (inRoute[o.map] ? " wm-in-route" : ""),
        style: "left:" + o.x + "px;top:" + o.y + "px;width:" + o.w + "px;height:" + o.h + "px",
        "aria-label": "To " + mapLabel(m) + (oa ? " in " + (oa.n.en || oa.n.ko) : ""),
        on: { click: function () { goArea(o.area, { map: o.map }); } }
      }, [
        h("span", { class: "wm-stub-in" }, [
          h("span", { class: "wm-stub-arrow", "aria-hidden": "true", text: "→" }),
          h("span", {}, [gname(m.n), h("span", { class: "wm-tile-meta",
            text: oa ? " " + (oa.n.en || "") + ", " + m.num : " " + m.num })])
        ])
      ]);
      stage.appendChild(b);
    });
  }

  function renderMap(id) {
    clear();
    var m = mapsById[id];
    var w = m.img ? m.img.w : m.mw || 640, hgt = m.img ? m.img.h : m.mh || 400;
    B = { w: w, h: hgt };
    K.max = 8;
    stage.className = "wm-stage wm-map" + (S.marks ? "" : " wm-nomarks");
    stage.style.width = w + "px";
    stage.style.height = hgt + "px";
    if (m.img) {
      stage.appendChild(h("img", { class: "wm-render", src: m.img.s, width: w, height: hgt,
        alt: "", draggable: "false" }));
    } else {
      stage.appendChild(h("div", { class: "wm-render wm-noimg" }));
    }
    if (m.ov) {
      stage.appendChild(h("img", { class: "wm-ov", src: m.ov.s, width: w, height: hgt, alt: "",
        draggable: "false" }));
    }
    var roleLayer = { shop: "shops", quest: "quests", craft: "craft", trade: "craft" };
    m.npcs.forEach(function (p) {
      if (p.x == null) return;
      var layers = p.roles.map(function (r) { return roleLayer[r]; }).filter(function (l, i, all) {
        return l && all.indexOf(l) === i && S.layers[l];
      });
      if (!layers.length) return;
      stage.appendChild(h("a", {
        class: "wm-marker wm-npc", href: p.u, style: "left:" + p.x + "px;top:" + p.y + "px",
        "aria-label": (p.n.en || p.n.ko) + ": " + p.roles.join(", ")
      }, [
        h("span", { class: "wm-badges" }, layers.map(function (l) {
          return h("span", { class: "wm-badge wm-l-" + l, html: icon(l) });
        })),
        gname(p.n, "wm-marker-name")
      ]));
    });
    m.portals.forEach(function (p) {
      if (p.x == null) return;
      if (p.hidden && !S.layers.secrets) return;
      var dest = p.to.map(function (x) { return mapsById[x]; }).filter(Boolean);
      var label = (p.hidden ? "Hidden portal " : "Portal ") + p.n +
        (dest.length ? " to " + dest.map(mapLabel).join(", ") : ", leads nowhere");
      stage.appendChild(h("button", {
        type: "button", class: "wm-marker wm-portal" + (p.hidden ? " wm-hidden" : "") +
          (p.gated ? " wm-gated" : ""),
        style: "left:" + p.x + "px;top:" + p.y + "px", "aria-label": label, title: label,
        disabled: dest.length ? null : true,
        on: { click: function () { if (dest.length) goMap(dest[0].id); } }
      }, [
        h("span", { class: "wm-portal-arrow", "aria-hidden": "true",
          text: p.hidden ? "✦" : "↑" }),
        dest.length ? gname(dest[0].n, "wm-marker-name") : null
      ]));
    });
  }

  // --- navigation
  function render() {
    stop();
    if (S.level === "world") renderWorld();
    else if (S.level === "area") renderArea(S.area);
    else renderMap(S.map);
    renderCrumbs();
    renderPanel();
    writeHash();
  }
  function transition(fn) {
    if (still()) { fn(); return; }
    stage.classList.add("wm-fade");
    setTimeout(function () {
      fn();
      requestAnimationFrame(function () { stage.classList.remove("wm-fade"); });
    }, 140);
  }
  function goWorld() {
    transition(function () {
      var from = S.area;
      S.level = "world";
      S.map = null;
      render();
      if (from && areasById[from]) {
        var s = spotsById[areasById[from].spot], v = freeView();
        fitAll(0);
        var f = T;
        if (!still()) {
          T = { x: v.w / 2 - s.x * f.k * 3, y: v.h / 2 - s.y * f.k * 3, k: f.k * 3 };
          apply();
          fly(f, 420);
        }
      } else fitAll(0);
      S.area = null;
      renderPanel();
      writeHash();
      say("The world: " + D.areas.length + " areas.");
    });
  }
  function goArea(aid, opts) {
    opts = opts || {};
    if (!areasById[aid]) return;
    transition(function () {
      S.level = "area";
      S.area = aid;
      S.map = opts.map && mapsById[opts.map] && mapsById[opts.map].area === aid ? opts.map : null;
      render();
      var f = fitView(B, freeView(), 20);
      K.min = f.k * 0.6;
      if (S.map) {
        var m = mapsById[S.map];
        T = f;
        apply();
        focusBox({ x: m.x - 60, y: m.y - 60, w: m.w + 120, h: m.h + 140 }, still() ? 0 : 420, 0.9);
      } else if (!still() && opts.from === "world") {
        T = { x: f.x + B.w * f.k * 0.35, y: f.y + B.h * f.k * 0.35, k: f.k * 0.3 };
        apply();
        fly(f, 420);
      } else fly(f, 0);
      var a = areasById[aid];
      say((a.n.en || a.n.ko) + ", area " + a.id + ": " + a.maps.length + " maps." +
        (S.map ? " Selected " + mapLabel(mapsById[S.map]) + "." : ""));
      if (opts.focus !== false && S.map && nodes["map:" + S.map]) {
        nodes["map:" + S.map].focus({ preventScroll: true });
      }
    });
  }
  function selectMap(id) {
    var m = mapsById[id];
    if (!m) return;
    if (S.level !== "area" || S.area !== m.area) { goArea(m.area, { map: id }); return; }
    S.map = id;
    Object.keys(nodes).forEach(function (k) {
      var on = k === "map:" + id;
      nodes[k].classList.toggle("wm-sel", on);
      if (k.indexOf("map:") === 0) nodes[k].setAttribute("aria-pressed", on ? "true" : "false");
    });
    renderPanel();
    writeHash();
    openSheet(true);
    say("Selected " + mapLabel(m) + ".");
  }
  function goMap(id) {
    var m = mapsById[id];
    if (!m) return;
    transition(function () {
      S.level = "map";
      S.area = m.area;
      S.map = id;
      render();
      var f = fitView(B, freeView(), 12);
      K.min = f.k * 0.6;
      K.max = Math.max(4, f.k * 8);
      fly(f, 0);
      say("Map " + mapLabel(m) + ".");
    });
  }
  function up() {
    if (S.level === "map") goArea(S.area, { map: S.map });
    else if (S.level === "area") goWorld();
  }
  function renderCrumbs() {
    crumbs.textContent = "";
    var items = [h("button", { type: "button", class: "wm-crumb", text: "World",
      "aria-current": S.level === "world" ? "true" : null,
      on: { click: function () { if (S.level !== "world") goWorld(); } } })];
    if (S.area) {
      var a = areasById[S.area];
      items.push(h("button", { type: "button", class: "wm-crumb",
        "aria-current": S.level === "area" ? "true" : null,
        on: { click: function () { if (S.level !== "area") goArea(a.id, { map: S.map }); } } },
      [gname(a.n)]));
    }
    if (S.level === "map") {
      items.push(h("span", { class: "wm-crumb", "aria-current": "true" },
        [gname(mapsById[S.map].n)]));
      items.push(h("button", { type: "button", class: "wm-crumb wm-marks",
        "aria-pressed": S.marks ? "true" : "false", text: "Marks",
        title: "Portals, NPCs and spawn points",
        on: { click: function () {
          S.marks = !S.marks;
          stage.classList.toggle("wm-nomarks", !S.marks);
          this.setAttribute("aria-pressed", S.marks ? "true" : "false");
        } } }));
    }
    items.forEach(function (el, i) {
      if (i) crumbs.appendChild(h("span", { class: "wm-sep", "aria-hidden": "true", text: "›" }));
      crumbs.appendChild(el);
    });
  }

  // --- the panel
  var sheetOpen = false;
  function sheetMode() { return window.matchMedia("(max-width: 899px)").matches; }
  function openSheet(on) {
    sheetOpen = on;
    panel.classList.toggle("wm-open", on);
    $("wm-grip").setAttribute("aria-expanded", on || !sheetMode() ? "true" : "false");
  }
  $("wm-grip").addEventListener("click", function () { openSheet(!sheetOpen); });

  function group(title, kids, layer) {
    kids = kids.filter(Boolean);
    if (!kids.length) return null;
    return h("section", { class: "wm-group" + (layer ? " wm-g-" + layer : "") }, [
      h("h3", { html: (layer ? icon(layer) : "") }, [title]),
      h("ul", { class: "wm-list" }, kids)
    ]);
  }
  function pic(p, cls) {
    if (!p) return h("span", { class: "wm-pic wm-pic-none" + (cls ? " " + cls : ""),
      "aria-hidden": "true" });
    return h("span", { class: "wm-pic" + (cls ? " " + cls : "") },
      [h("img", { src: p.s, alt: "", width: p.w, height: p.h, loading: "lazy" })]);
  }
  function who(p) { return p ? [" with ", link(p.u, [gname(p.n)])] : []; }
  function hidden(n) {
    return n ? h("li", { class: "wm-more", text: n + " more in other chapters" }) : null;
  }
  function chapterFilter(list) {
    var shown = list.filter(function (x) { return inChapter(x.ch, S.ch); });
    return { shown: shown, rest: list.length - shown.length };
  }
  function mapButton(id, extra) {
    var m = mapsById[id];
    return h("button", { type: "button", class: "wm-linkbtn",
      on: { click: function () { selectMap(id); } } },
    [gname(m.n), h("span", { class: "wm-muted", text: " " + m.num + (extra || "") })]);
  }

  function panelHead(title, sub, meta) {
    return h("header", { class: "wm-head" }, [
      h("h2", { id: "wm-panel-title" }, title),
      sub ? h("p", { class: "wm-sub", lang: "ko", "data-verbatim": true, text: sub }) : null,
      meta ? h("p", { class: "wm-meta", text: meta }) : null
    ]);
  }

  function routeBlock() {
    if (!S.routeOn || !S.from || !S.to) return null;
    var r = S.result, kids = [];
    if (!r) {
      return h("section", { class: "wm-group wm-routebox" }, [h("h3", { text: "Route" }),
        h("p", { text: "No warp leads from " + mapLabel(mapsById[S.from]) + " to " +
          mapLabel(mapsById[S.to]) + (S.ch != null ? " in chapter " + S.ch : "") + "." })]);
    }
    kids.push(h("li", {}, [mapButton(r.path[0], ", start")]));
    r.steps.forEach(function (s) {
      kids.push(h("li", {}, [mapButton(s.b, s.c ? ", warp with a condition" : "")]));
    });
    return h("section", { class: "wm-group wm-routebox" }, [
      h("h3", { text: "Route: " + r.steps.length + " map change" +
        (r.steps.length === 1 ? "" : "s") }),
      h("ol", { class: "wm-list wm-steps" }, kids)
    ]);
  }

  function panelWorld() {
    var kids = [panelHead(["The world"], null, D.areas.length + " areas, " + D.maps.length +
      " maps")];
    kids.push(h("p", { text: "Choose an area on the map or below. Set a hero level to see" +
      " where to train, and a chapter to see what is open then." }));
    var picks = trainingPicks(D, S.hero, 6);
    if (S.hero != null) {
      kids.push(group("Where to train at level " + S.hero, picks.map(function (m) {
        return h("li", {}, [mapButton(m.id, ", " + lvText(m.lv) + ", " +
          m.train.eph.toFixed(3) + " EXP per HP")]);
      }), "train") || h("p", { class: "wm-muted", text: "No training spot fits level " +
        S.hero + "." }));
    }
    kids.push(group("Areas", D.areas.map(function (a) {
      var c = areaCounts(a);
      return h("li", {}, [h("button", { type: "button", class: "wm-linkbtn",
        on: { click: function () { goArea(a.id, { from: "world" }); } } },
      [gname(a.n), h("span", { class: "wm-muted", text: " area " + a.id +
        (a.lv ? ", " + lvText(a.lv) : "") })]), badges(c, "wm-badges wm-inline")]);
    })));
    return kids;
  }

  function panelArea(a) {
    var c = areaCounts(a);
    var kids = [panelHead([gname(a.n)], a.n.ko !== a.n.en ? a.n.ko : null, "Area " + a.id + ", " +
      a.maps.length + " maps" + (a.lv ? ", monsters " + lvText(a.lv) : ""))];
    kids.push(h("p", { class: "wm-actions" }, [link(a.u, ["Area page"], "wm-btn wm-small")]));
    if (countsWords(c)) kids.push(h("p", { text: "Here: " + countsWords(c) + "." }));
    var best = a.maps.map(function (id) { return mapsById[id]; })
      .filter(function (m) { return m.train && (S.hero == null || fitOf(m.lv) === "good"); })
      .sort(function (x, y) { return y.train.eph - x.train.eph; }).slice(0, 3);
    kids.push(group(S.hero != null ? "Training at level " + S.hero : "Best training",
      best.map(function (m) {
        return h("li", {}, [mapButton(m.id, ", " + lvText(m.lv) + ", " +
          m.train.eph.toFixed(3) + " EXP per HP")]);
      }), "train"));
    var shops = [];
    a.maps.forEach(function (id) {
      chapterFilter(mapsById[id].shops).shown.forEach(function (s) {
        shops.push(h("li", {}, [link(s.u, [cap(s.kind) + " shop"])].concat(who(s.npc),
          [" on ", mapButton(id)])));
      });
    });
    kids.push(group("Shops", shops, "shops"));
    kids.push(group("Maps", a.maps.map(function (id) {
      var m = mapsById[id];
      return h("li", { class: "wm-fit-" + fitOf(m.lv) }, [mapButton(id, m.lv ? ", " +
        lvText(m.lv) : ""), badges(mapCounts(m, S.ch), "wm-badges wm-inline")]);
    })));
    return kids;
  }
  function cap(s) { return s.charAt(0).toUpperCase() + s.slice(1); }

  function panelMap(m) {
    var a = areasById[m.area];
    var meta = "Map " + m.num + ", " + m.type + (m.lv ? ", monsters " + lvText(m.lv) : "") +
      (m.save ? "" : ", " + "saving " + (m.nosave || "no"));
    var kids = [panelHead([gname(m.n)], m.n.ko !== m.n.en ? m.n.ko : null, meta)];
    var acts = [link(m.u, ["Map page"], "wm-btn wm-small")];
    if (S.level !== "map") {
      acts.unshift(h("button", { type: "button", class: "wm-btn wm-small wm-primary",
        text: "Open the map", on: { click: function () { goMap(m.id); } } }));
    }
    acts.push(h("button", { type: "button", class: "wm-btn wm-small", text: "Route from here",
      on: { click: function () { setRoute(m.id, S.to); } } }));
    acts.push(h("button", { type: "button", class: "wm-btn wm-small", text: "Route to here",
      on: { click: function () { setRoute(S.from, m.id); } } }));
    kids.push(h("p", { class: "wm-actions" }, acts));
    kids.push(h("p", { class: "wm-muted" }, ["In ", link(a.u, [gname(a.n)]),
      m.reached ? "." : ". No warp of the game leads here."]));
    if (m.train) {
      var fit = fitOf(m.lv), fitWords = {
        good: "Suits level " + S.hero + ".", hard: "Too hard for level " + S.hero + ".",
        easy: "Too easy for level " + S.hero + "."
      }[fit];
      kids.push(group("Training", [h("li", { text: m.train.exp.toLocaleString("en-US") +
        " EXP for one kill of every spawn, " + m.train.eph.toFixed(3) + " EXP per HP." +
        (fitWords ? " " + fitWords : "") })], "train"));
    }
    kids.push(group("Monsters", m.mobs.map(function (x) {
      return h("li", { class: "wm-row" }, [pic(x.pic), h("span", {}, [link(x.u, [gname(x.n)]),
        h("span", { class: "wm-muted", text: (x.lv ? " Lv. " + x.lv : "") +
          (x.count ? ", " + x.count + " spawn point" + (x.count > 1 ? "s" : "") : "") +
          (x.boss ? ", boss" : "") })])]);
    })));
    var sh = chapterFilter(m.shops);
    kids.push(group("Shops", sh.shown.map(function (s) {
      return h("li", {}, [link(s.u, [cap(s.kind) + " shop"])].concat(who(s.npc),
        s.ch.length ? [h("span", { class: "wm-muted", text: ", " + chText(s.ch).toLowerCase() })]
          : []));
    }).concat([hidden(sh.rest)]), "shops"));
    var qs = chapterFilter(m.quests), byCh = {};
    qs.shown.forEach(function (q) { (byCh[q.ch] = byCh[q.ch] || []).push(q); });
    var qkids = [];
    Object.keys(byCh).sort().forEach(function (ch) {
      qkids.push(h("li", { class: "wm-subhead", text: ch === "null" ? "Any chapter" :
        "Chapter " + ch }));
      byCh[ch].forEach(function (q) {
        qkids.push(h("li", {}, [link(q.u, [gname(q.n)]), h("span", { class: "wm-muted",
          text: (q.main ? " main" : " side") + ", " }), q.npc ? link(q.npc.u, [gname(q.npc.n)])
          : "an NPC", " " + q.role.join(" and ") + " it"]));
      });
    });
    kids.push(group("Quests", qkids.concat([hidden(qs.rest)]), "quests"));
    var rc = chapterFilter(m.recipes), tr = chapterFilter(m.trades);
    kids.push(group("Crafting and trades", rc.shown.map(function (r) {
      return h("li", { class: "wm-row" }, [pic(r.pic, "wm-icon"), h("span", {},
        ["Craft ", link(r.u, [gname(r.n)])].concat(who(r.npc)))]);
    }).concat(tr.shown.map(function (r) {
      return h("li", { class: "wm-row" }, [pic(r.pic, "wm-icon"), h("span", {},
        ["Trade for ", link(r.u, [gname(r.n)])].concat(who(r.npc)))]);
    }), [hidden(rc.rest + tr.rest)]), "craft"));
    var ev = chapterFilter(m.events);
    kids.push(group("Bosses and events", ev.shown.map(function (e) {
      return h("li", {}, [link(e.u, [e.label])]);
    }).concat([hidden(ev.rest)]), "bosses"));
    kids.push(group("Hidden portals", m.portals.filter(function (p) { return p.hidden; })
      .map(function (p) {
        var dest = p.to.filter(function (x) { return mapsById[x]; });
        return h("li", {}, ["Portal " + p.n + (dest.length ? " to " : ", leads nowhere")]
          .concat(dest.map(function (x) { return mapButton(x); })));
      }), "secrets"));
    var outs = [];
    D.edges.forEach(function (e) {
      if (e.a !== m.id || !mapsById[e.b] || !edgeOpen(e, S.ch)) return;
      if (outs.some(function (o) { return o.b === e.b; })) {
        outs.forEach(function (o) { if (o.b === e.b) o.c = o.c && e.c; });
        return;
      }
      outs.push({ b: e.b, c: e.c, npc: e.npc });
    });
    kids.push(group("Ways out", outs.map(function (o) {
      var other = mapsById[o.b], away = other.area !== m.area ? ", " +
        (areasById[other.area].n.en || "") : "";
      return h("li", {}, [mapButton(o.b, away + (o.npc ? ", through an NPC" : "") +
        (o.c ? ", with a condition" : ""))]);
    })));
    return kids;
  }

  function renderPanel() {
    body.textContent = "";
    var kids = [routeBlock()];
    if (S.level === "world" && !S.map) kids = kids.concat(panelWorld());
    else if (S.map) kids = kids.concat(panelMap(mapsById[S.map]));
    else if (S.area) kids = kids.concat(panelArea(areasById[S.area]));
    kids.forEach(function (k) { if (k) body.appendChild(k); });
    body.scrollTop = 0;
  }

  // --- routes, filters, search, hash
  function fillRouteSelects() {
    ["wm-from", "wm-to"].forEach(function (id) {
      var sel = $(id);
      sel.textContent = "";
      sel.appendChild(h("option", { value: "", text: "Choose a map" }));
      D.areas.forEach(function (a) {
        var g = h("optgroup", { label: (a.n.en || a.n.ko) + " (area " + a.id + ")" });
        a.maps.forEach(function (mid) {
          g.appendChild(h("option", { value: mid, text: mapLabel(mapsById[mid]) }));
        });
        sel.appendChild(g);
      });
      sel.addEventListener("change", function () {
        setRoute($("wm-from").value || null, $("wm-to").value || null);
      });
    });
  }
  function setRoute(from, to) {
    S.from = from || null;
    S.to = to || null;
    S.routeOn = true;
    $("wm-route").hidden = false;
    $("wm-route-btn").setAttribute("aria-pressed", "true");
    $("wm-from").value = S.from || "";
    $("wm-to").value = S.to || "";
    computeRoute();
    var out = $("wm-route-out");
    if (!S.from || !S.to) out.textContent = "Choose where to start and where to go.";
    else if (!S.result) out.textContent = "No route" + (S.ch != null ? " in chapter " + S.ch : "") + ".";
    else {
      out.textContent = S.result.steps.length + " map change" +
        (S.result.steps.length === 1 ? "" : "s") + " through " +
        routeAreas(S.result.path, mapsById).length + " area" +
        (routeAreas(S.result.path, mapsById).length === 1 ? "" : "s") + ".";
    }
    render();
    if (S.result && S.result.path.length > 1 && S.level === "world") fitAll(0);
    openSheet(true);
  }
  function computeRoute() {
    S.result = S.from && S.to ? route(D.edges, S.from, S.to, S.ch) : null;
  }
  $("wm-route-btn").addEventListener("click", function () {
    S.routeOn = !S.routeOn;
    $("wm-route").hidden = !S.routeOn;
    this.setAttribute("aria-pressed", S.routeOn ? "true" : "false");
    if (!S.routeOn) { S.from = S.to = null; S.result = null; render(); }
    else {
      if (!S.from && S.map) S.from = S.map;
      setRoute(S.from, S.to);
      $("wm-from").focus();
    }
  });
  $("wm-swap").addEventListener("click", function () { setRoute(S.to, S.from); });

  $("wm-lv").addEventListener("input", function () {
    var v = parseInt(this.value, 10);
    S.hero = isNaN(v) ? null : Math.max(1, Math.min(D.maxLevel, v));
    rerender();
  });
  $("wm-ch").addEventListener("change", function () {
    S.ch = this.value === "" ? null : +this.value;
    computeRoute();
    if (S.routeOn && S.from && S.to) setRoute(S.from, S.to); else rerender();
  });
  Array.prototype.forEach.call(wm.querySelectorAll('input[name="layer"]'), function (box) {
    box.parentNode.insertBefore(h("span", { class: "wm-badge wm-l-" + box.value,
      html: icon(box.value), "aria-hidden": "true" }), box.nextSibling);
    box.addEventListener("change", function () {
      S.layers[box.value] = box.checked;
      rerender();
    });
  });
  /** Redraw the current level in place (filters changed), keeping the view. */
  function rerender() {
    var keep = T;
    render();
    T = keep;
    schedule();
  }

  var q = $("wm-q"), qlist = $("wm-q-list"), hits = [], active = -1;
  function showHits() {
    qlist.textContent = "";
    hits.forEach(function (x, i) {
      var p = x.kind === "map" ? mapsById[x.id] : areasById[x.id];
      var sub = x.kind === "map" ? " map " + p.num + ", " + (areasById[p.area].n.en || "") :
        " area " + p.id;
      qlist.appendChild(h("li", { role: "option", id: "wm-q-" + i,
        "aria-selected": i === active ? "true" : "false",
        on: { mousedown: function (e) { e.preventDefault(); choose(i); } } },
      [gname(p.n), h("span", { class: "wm-muted", text: sub })]));
    });
    qlist.hidden = !hits.length;
    q.setAttribute("aria-expanded", hits.length ? "true" : "false");
    if (active >= 0) q.setAttribute("aria-activedescendant", "wm-q-" + active);
    else q.removeAttribute("aria-activedescendant");
  }
  function choose(i) {
    var x = hits[i];
    if (!x) return;
    hits = [];
    active = -1;
    showHits();
    q.value = "";
    if (x.kind === "map") goArea(mapsById[x.id].area, { map: x.id });
    else goArea(x.id, { from: "world" });
    view.focus({ preventScroll: true });
  }
  q.addEventListener("input", function () {
    hits = searchPlaces(D, q.value, 8);
    active = hits.length ? 0 : -1;
    showHits();
  });
  q.addEventListener("keydown", function (e) {
    if (e.key === "ArrowDown" && hits.length) { active = (active + 1) % hits.length; showHits(); }
    else if (e.key === "ArrowUp" && hits.length) {
      active = (active - 1 + hits.length) % hits.length;
      showHits();
    } else if (e.key === "Enter") choose(active < 0 ? 0 : active);
    else if (e.key === "Escape") { hits = []; active = -1; showHits(); return; }
    else return;
    e.preventDefault();
  });
  q.addEventListener("blur", function () { setTimeout(function () { hits = []; showHits(); }, 120); });

  var lastHash = "";
  function writeHash() {
    var hsh = formatHash({ map: S.map, area: S.area, open: S.level === "map", from: S.routeOn ? S.from : null,
      to: S.routeOn ? S.to : null, ch: S.ch, lv: S.hero });
    if (hsh === lastHash) return;
    lastHash = hsh;
    try {
      history.replaceState(null, "", hsh || location.pathname + location.search);
    } catch (err) { /* file:// in some browsers */ }
  }
  function readHash(first) {
    var st = parseHash(location.hash);
    if (st.ch != null && D.chapters.indexOf(st.ch) >= 0) { S.ch = st.ch; $("wm-ch").value = st.ch; }
    if (st.lv != null) { S.hero = st.lv; $("wm-lv").value = st.lv; }
    if (st.from && st.to && mapsById[st.from] && mapsById[st.to]) {
      S.from = st.from;
      S.to = st.to;
      S.routeOn = true;
      $("wm-route").hidden = false;
      $("wm-route-btn").setAttribute("aria-pressed", "true");
      $("wm-from").value = st.from;
      $("wm-to").value = st.to;
      computeRoute();
    }
    if (st.map && mapsById[st.map] && st.open) goMap(st.map);
    else if (st.map && mapsById[st.map]) {
      goArea(mapsById[st.map].area, { map: st.map, focus: !first });
    }
    else if (st.area && areasById[st.area]) goArea(st.area, { from: "world", focus: !first });
    else { S.level = "world"; render(); fitAll(0); }
    return !!(st.map || st.area);
  }
  window.addEventListener("hashchange", function () {
    if (location.hash === lastHash) return;
    lastHash = location.hash;
    readHash(false);
    wm.scrollIntoView({ block: "start" });
  });

  if (window.ResizeObserver) {
    var size = null;
    new ResizeObserver(function () {
      var v = viewSize();
      if (size && (Math.abs(size.w - v.w) > 1 || Math.abs(size.h - v.h) > 1)) {
        var cx = (size.w / 2 - T.x) / T.k, cy = (size.h / 2 - T.y) / T.k;
        T = clampView({ x: v.w / 2 - cx * T.k, y: v.h / 2 - cy * T.k, k: T.k }, B, v, 0.3);
        schedule();
      }
      size = v;
    }).observe(view);
  }

  // --- start
  wm.hidden = false;
  fillRouteSelects();
  openSheet(!sheetMode());
  var deep = readHash(true);
  if (S.routeOn && S.from && S.to) setRoute(S.from, S.to);
  if (deep) {
    // the browser scrolled to the text list's entry of the same id: show the viewer instead
    var top = function () { wm.scrollIntoView({ block: "start" }); };
    requestAnimationFrame(top);
    window.addEventListener("load", top);
  }
})();
