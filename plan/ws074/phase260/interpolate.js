    // ---- The computed values of animated properties: CSS transitions, CSS animations and script animations (Element.animate) give a
    // property its value at the time getComputedStyle reads it.  A value is read from its computed text into a typed value, blended
    // (mixed, or added for the composite operations), and written back as the text the engine serializes for the same value.
    var R4 = 10000;
    function r4(n) { n = Math.round(n * R4) / R4; return n === 0 ? 0 : n; }
    var NUMBER_SRC = '[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:e[+-]?\\d+)?';
    var NUM_RE = new RegExp('^(' + NUMBER_SRC + ')$', 'i');
    var LP_RE = new RegExp('^(' + NUMBER_SRC + ')(px|%)$', 'i');
    var FR_RE = new RegExp('^(' + NUMBER_SRC + ')fr$', 'i');
    var CALC_RE = new RegExp('^calc\\(\\s*(' + NUMBER_SRC + ')(px|%)\\s*([+-])\\s*(' + NUMBER_SRC.replace(/\[\+-\]\?/, '') + ')(px|%)\\s*\\)$', 'i');
    // The type of each animatable property: how its value is blended.  A property that is only named in DISCRETE changes at the halfway.
    var TYPE_OF = {
      'flex-grow': 'number+', 'flex-shrink': 'number+', 'order': 'integer', 'flex-basis': 'lp',
      'grid-template-columns': 'tracks', 'grid-template-rows': 'tracks', 'backdrop-filter': 'filter'
    };
    var DISCRETE = {
      'anchor-name': 1, 'position-anchor': 1, 'position-area': 1, 'position-try-fallbacks': 1, 'position-try-order': 1
    };
    var INITIAL_OF = {
      'flex-grow': '0', 'flex-shrink': '1', 'order': '0', 'flex-basis': 'auto', 'grid-template-columns': 'none', 'grid-template-rows': 'none',
      'backdrop-filter': 'none', 'anchor-name': 'none', 'position-anchor': 'auto', 'position-area': 'none', 'position-try-fallbacks': 'none',
      'position-try-order': 'normal'
    };
    function animatable(prop) { return TYPE_OF[prop] !== undefined || DISCRETE[prop] === 1; }
    function lpOf(text) {
      var m = LP_RE.exec(text), px, pct;
      if (m) return m[2] === '%' ? { t: 'lp', px: 0, pct: +m[1], p: true } : { t: 'lp', px: +m[1], pct: 0 };
      m = CALC_RE.exec(text);
      if (!m) return null;
      px = 0;
      pct = 0;
      if (m[2] === '%') pct += +m[1]; else px += +m[1];
      if (m[5] === '%') pct += (m[3] === '-' ? -1 : 1) * +m[4]; else px += (m[3] === '-' ? -1 : 1) * +m[4];
      return { t: 'lp', px: px, pct: pct, p: m[2] === '%' || m[5] === '%' };
    }
    function lpText(v, nonneg) {
      var px = r4(v.px), pct = r4(v.pct);
      if (pct === 0 && px === 0 && v.p) return '0%';
      if (pct === 0 && !v.p) return (nonneg && px < 0 ? 0 : px) + 'px';
      if (px === 0) return (nonneg && pct < 0 ? 0 : pct) + '%';
      return 'calc(' + pct + '% ' + (px < 0 ? '- ' + (-px) : '+ ' + px) + 'px)';
    }
    // The pieces of a track list item: a length-percentage, a flexible length, a keyword, or fit-content() and minmax() of those.
    function breadthOf(text) {
      var m;
      text = text.trim();
      if ((m = FR_RE.exec(text))) return { t: 'fr', v: +m[1] };
      m = lpOf(text);
      if (m) return m;
      if (/^(auto|min-content|max-content)$/.test(text)) return { t: 'kw', s: text };
      return null;
    }
    function splitArgs(text) {
      var out = [], depth = 0, cur = '', i, c;
      for (i = 0; i < text.length; i++) {
        c = text[i];
        if (c === '(') depth++;
        if (c === ')') depth--;
        if (c === ',' && depth === 0) { out.push(cur); cur = ''; } else cur += c;
      }
      out.push(cur);
      return out;
    }
    function trackOf(text) {
      var m = /^(minmax|fit-content)\((.*)\)$/.exec(text), args, parts = [], i, b, list, inner = [], t;
      m = /^repeat\(\s*(\d+)\s*,(.*)\)$/.exec(text);
      if (m) {
        list = parseTracks(m[2].trim());
        if (!list) return null;
        for (i = 0; i < list.tracks.length; i++) {
          t = trackOf(list.tracks[i]);
          if (!t) return null;
          inner.push(t);
        }
        return { t: 'repeat', n: +m[1], items: inner, lines: list.lines };
      }
      m = /^(minmax|fit-content)\((.*)\)$/.exec(text);
      if (!m) return breadthOf(text);
      args = splitArgs(m[2]);
      if ((m[1] === 'minmax' && args.length !== 2) || (m[1] === 'fit-content' && args.length !== 1)) return null;
      for (i = 0; i < args.length; i++) {
        b = breadthOf(args[i]);
        if (!b) return null;
        parts.push(b);
      }
      return { t: 'fn', name: m[1], args: parts };
    }
    function breadthText(v) {
      if (v.t === 'repeat') return 'repeat(' + v.n + ', ' + trackText({ tracks: v.items.map(breadthText), lines: v.lines }) + ')';
      if (v.t === 'fr') return r4(v.v) + 'fr';
      if (v.t === 'lp') return lpText(v, true);
      if (v.t === 'fn') return v.name + '(' + v.args.map(breadthText).join(', ') + ')';
      return v.s;
    }
    function parseValue(type, text) {
      var list, tracks, i, t, filters;
      text = String(text).trim();
      if (type === 'number' || type === 'number+' || type === 'integer') return NUM_RE.test(text) ? { t: 'num', v: +text } : null;
      if (type === 'lp') {
        list = lpOf(text);
        return list || (/^(auto|content|min-content|max-content|fit-content)$/.test(text) ? { t: 'kw', s: text } : { t: 'kw', s: text });
      }
      if (type === 'tracks') {
        list = parseTracks(text);
        if (!list) return { t: 'kw', s: 'none' };
        tracks = [];
        for (i = 0; i < list.tracks.length; i++) {
          t = trackOf(list.tracks[i]);
          if (!t) return { t: 'kw', s: text };
          tracks.push(t);
        }
        return { t: 'list', items: tracks, lines: list.lines, text: text };
      }
      if (type === 'filter') {
        filters = parseFilters(text);
        return filters || { t: 'kw', s: text };
      }
      return { t: 'kw', s: text };
    }
    function valueText(type, v) {
      var i, tracks;
      if (v.t === 'num') return fmt(type === 'integer' ? Math.round(v.v) : type === 'number+' ? Math.max(0, v.v) : v.v);
      if (v.t === 'lp') return lpText(v, true);
      if (v.t === 'list') {
        tracks = [];
        for (i = 0; i < v.items.length; i++) tracks.push(breadthText(v.items[i]));
        return trackText({ tracks: tracks, lines: v.lines });
      }
      if (v.t === 'filters') return filtersText(v);
      return v.s;
    }
    function mixNumber(a, b, p) { return a + (b - a) * p; }
    // The blend of two typed values at p, or null when they do not blend.
    function mixValues(a, b, p) {
      var i, items, r;
      if (a.t !== b.t) return null;
      if (a.t === 'num') return { t: 'num', v: mixNumber(a.v, b.v, p) };
      if (a.t === 'lp') return { t: 'lp', px: mixNumber(a.px, b.px, p), pct: mixNumber(a.pct, b.pct, p), p: a.p || b.p };
      if (a.t === 'fr') return { t: 'fr', v: mixNumber(a.v, b.v, p) };
      if (a.t === 'fn') {
        if (a.name !== b.name || a.args.length !== b.args.length) return null;
        items = [];
        for (i = 0; i < a.args.length; i++) {
          r = mixValues(a.args[i], b.args[i], p);
          items.push(r === null ? (p < 0.5 ? a.args[i] : b.args[i]) : r);
        }
        return { t: 'fn', name: a.name, args: items };
      }
      if (a.t === 'repeat') {
        if (a.n !== b.n || a.items.length !== b.items.length || a.lines.length !== b.lines.length) return null;
        r = mixValues({ t: 'list', items: a.items, lines: a.lines }, { t: 'list', items: b.items, lines: b.lines }, p);
        return r === null ? null : { t: 'repeat', n: a.n, items: r.items, lines: r.lines };
      }
      if (a.t === 'list') {
        if (a.items.length !== b.items.length || a.lines.length !== b.lines.length) return null;
        for (i = 0; i < a.items.length; i++) if ((a.items[i].t === 'repeat') !== (b.items[i].t === 'repeat')) return null;
        items = [];
        for (i = 0; i < a.items.length; i++) {
          if (trackKind(a.items[i]) !== trackKind(b.items[i]) && trackKind(a.items[i]) !== 'kw' && trackKind(b.items[i]) !== 'kw') return p < 0.5 ? a : b;
          r = mixValues(a.items[i], b.items[i], p);
          if (r === null && a.items[i].t === 'repeat') return null;
          items.push(r === null ? (p < 0.5 ? a.items[i] : b.items[i]) : r);
        }
        return { t: 'list', items: items, lines: p < 0.5 ? a.lines : b.lines };
      }
      if (a.t === 'filters') return mixFilters(a, b, p);
      return null;
    }
    function trackKind(v) { return v.t === 'fn' ? v.name : v.t; }
    // The sum of the underlying value and an animation's value (composite add and accumulate), or null when they do not add.
    function addValues(a, b) {
      var i, items, r;
      if (a.t !== b.t) return null;
      if (a.t === 'num') return { t: 'num', v: a.v + b.v };
      if (a.t === 'lp') return { t: 'lp', px: a.px + b.px, pct: a.pct + b.pct, p: a.p || b.p };
      if (a.t === 'fr') return { t: 'fr', v: a.v + b.v };
      if (a.t === 'fn') {
        if (a.name !== b.name || a.args.length !== b.args.length) return null;
        items = [];
        for (i = 0; i < a.args.length; i++) {
          r = addValues(a.args[i], b.args[i]);
          items.push(r === null ? b.args[i] : r);
        }
        return { t: 'fn', name: a.name, args: items };
      }
      if (a.t === 'repeat') {
        if (a.n !== b.n || a.items.length !== b.items.length) return null;
        r = addValues({ t: 'list', items: a.items, lines: a.lines }, { t: 'list', items: b.items, lines: b.lines });
        return r === null ? null : { t: 'repeat', n: a.n, items: r.items, lines: b.lines };
      }
      if (a.t === 'list') {
        if (a.items.length !== b.items.length) return null;
        items = [];
        for (i = 0; i < a.items.length; i++) {
          r = addValues(a.items[i], b.items[i]);
          items.push(r === null ? b.items[i] : r);
        }
        return { t: 'list', items: items, lines: b.lines };
      }
      if (a.t === 'filters') return addFilters(a, b);
      return null;
    }

    // ---- filter lists (backdrop-filter): the functions with their arguments as numbers.
    var FILTER_ARGS = {
      blur: ['len'], brightness: ['num'], contrast: ['num'], grayscale: ['num'], invert: ['num'], opacity: ['num'], saturate: ['num'], sepia: ['num'],
      'hue-rotate': ['deg']
    };
    var FILTER_IDENTITY = {
      blur: 0, brightness: 1, contrast: 1, grayscale: 0, invert: 0, opacity: 1, saturate: 1, sepia: 0, 'hue-rotate': 0
    };
    function parseFilters(text) {
      var list, items = [], i, m, w, arg;
      if (text === 'none') return { t: 'filters', items: [] };
      list = words(text);
      for (i = 0; i < list.length; i++) {
        w = list[i];
        m = /^([a-z-]+)\((.*)\)$/.exec(w);
        if (!m || FILTER_ARGS[m[1]] === undefined) return null;
        arg = m[2].trim();
        if (FILTER_ARGS[m[1]][0] === 'len') {
          if (!/^[+-]?[\d.]+(?:e[+-]?\d+)?px$/i.test(arg)) return null;
          arg = parseFloat(arg);
        } else if (FILTER_ARGS[m[1]][0] === 'deg') {
          if (!/^[+-]?[\d.]+(?:e[+-]?\d+)?deg$/i.test(arg)) return null;
          arg = parseFloat(arg);
        } else if (/%$/.test(arg)) {
          arg = parseFloat(arg) / 100;
        } else {
          arg = parseFloat(arg);
        }
        if (!isFinite(arg)) return null;
        items.push({ name: m[1], v: arg });
      }
      return { t: 'filters', items: items };
    }
    function filtersText(v) {
      var out = [], i, f, unit;
      if (v.items.length === 0) return 'none';
      for (i = 0; i < v.items.length; i++) {
        f = v.items[i];
        unit = f.name === 'blur' ? 'px' : f.name === 'hue-rotate' ? 'deg' : '';
        out.push(f.name + '(' + r4(f.v) + unit + ')');
      }
      return out.join(' ');
    }
    function filterClamp(f) {
      var v = f.v;
      if (f.name === 'blur') v = Math.max(0, v);
      else if (f.name === 'hue-rotate') v = v;
      else if (f.name === 'grayscale' || f.name === 'invert' || f.name === 'opacity' || f.name === 'sepia') v = Math.min(1, Math.max(0, v));
      else v = Math.max(0, v);
      return { name: f.name, v: v };
    }
    function mixFilters(a, b, p) {
      var n = Math.max(a.items.length, b.items.length), items = [], i, x, y;
      var common = Math.min(a.items.length, b.items.length);
      for (i = 0; i < common; i++) if (a.items[i].name !== b.items[i].name) return null;
      for (i = 0; i < n; i++) {
        x = a.items[i] || { name: b.items[i].name, v: FILTER_IDENTITY[b.items[i].name] };
        y = b.items[i] || { name: a.items[i].name, v: FILTER_IDENTITY[a.items[i].name] };
        items.push(filterClamp({ name: x.name, v: mixNumber(x.v, y.v, p) }));
      }
      return { t: 'filters', items: items };
    }
    function addFilters(a, b) {
      return { t: 'filters', items: a.items.concat(b.items) };
    }

    // ---- the effects on one element: the values a transition, a CSS animation and the script animations give
    var states = new WeakMap();
    function stateOf(el) {
      var st = states.get(el);
      if (!st) { st = { last: {}, trans: {}, anim: null }; states.set(el, st); }
      return st;
    }
    function nowSeconds() { return performance.now() / 1000; }
    // The progress an animation's timing gives: the iteration's progress (before the easing) and whether the effect is in force.
    function timingProgress(delay, duration, iterations, direction, fill, elapsed) {
      var local = elapsed - delay, iteration, p, current, forwards;
      if (!(duration > 0)) return null;
      if (local < 0) {
        if (fill !== 'backwards' && fill !== 'both') return null;
        local = 0;
      }
      iteration = local / duration;
      if (iterations >= 0 && iteration >= iterations) {
        if (fill !== 'forwards' && fill !== 'both') return null;
        iteration = iterations;
      }
      current = Math.floor(iteration);
      p = iteration - current;
      if (iteration > 0 && p === 0 && current === iteration && iterations >= 0 && iteration >= iterations) { p = 1; current--; }
      forwards = direction === 'normal' || (direction === 'alternate' && current % 2 === 0) || (direction === 'alternate-reverse' && current % 2 === 1);
      return forwards ? p : 1 - p;
    }
    // The easing function text of the engine's animation-timing-function and the transition's: "ease", "steps N position", "bezier a b c d".
    function easeText(timing, x) {
      var m = /^bezier ([-\d.e]+) ([-\d.e]+) ([-\d.e]+) ([-\d.e]+)$/.exec(timing || '');
      if (m) return ease('cubic-bezier(' + m[1] + ',' + m[2] + ',' + m[3] + ',' + m[4] + ')', x);
      m = /^steps (\d+) (\w+)$/.exec(timing || '');
      if (m) return ease('steps(' + m[1] + ',' + (m[2] === 'end' ? 'end' : m[2] === 'start' ? 'start' : 'jump-' + m[2]) + ')', x);
      if (timing === 'step-start') return ease('steps(1,start)', x);
      if (timing === 'step-end') return ease('steps(1,end)', x);
      return ease(timing, x);
    }
    function firstOf(list) { return String(list).split(',')[0].trim(); }
    function nth(list, i) { var items = String(list).split(','); return items[i % items.length].trim(); }
    function seconds(text) { var m = /^(-?[\d.]+(?:e-?\d+)?)(ms|s)$/.exec(text.trim()); return m ? (m[2] === 'ms' ? +m[1] / 1000 : +m[1]) : 0; }
    // The value a blend between two keyframe values gives at an eased progress, with a discrete flip at the halfway when they do not blend.
    var canonCache = new Map();
    // The text the engine serializes for a specified value of a property (a scratch element reads it).
    function canonical(prop, text) {
      var key = prop + '\n' + text, held = canonCache.get(key), holder, e, v;
      if (held !== undefined) return held;
      holder = document.body || document.documentElement;
      v = text;
      if (holder) {
        e = document.createElement('div');
        e.style.setProperty(prop, text);
        holder.appendChild(e);
        v = nativeGetComputedStyle.call(g, e).getPropertyValue(prop) || text;
        holder.removeChild(e);
      }
      canonCache.set(key, v);
      return v;
    }
    function blendText(type, aText, bText, p, prop) {
      var a = parseValue(type, aText), b = parseValue(type, bText), r, chosen;
      if (a && b) {
        r = mixValues(a, b, p);
        if (r !== null) return valueText(type, r);
        chosen = p < 0.5 ? a : b;
        return chosen.t === 'kw' ? canonical(prop, chosen.s) : valueText(type, chosen);
      }
      return canonical(prop, p < 0.5 ? aText : bText);
    }
    function composeText(type, composite, underlying, text) {
      var a, b, r;
      if (composite !== 'add' && composite !== 'accumulate') return text;
      a = parseValue(type, underlying);
      b = parseValue(type, text);
      if (!a || !b) return text;
      r = addValues(a, b);
      return r === null ? text : valueText(type, r);
    }
    // The value of a keyframe: the specified text turned into the computed text (inherit and initial; the rest is as written).
    function keyframeText(el, prop, text) {
      var t = text.trim(), parent;
      if (t === 'initial' || t === 'unset') return INITIAL_OF[prop] !== undefined ? INITIAL_OF[prop] : t;
      if (t === 'inherit') {
        parent = el.parentElement || el.parentNode;
        if (parent && parent.nodeType === 1) return nativeGetComputedStyle.call(g, parent).getPropertyValue(prop);
        return INITIAL_OF[prop] !== undefined ? INITIAL_OF[prop] : t;
      }
      return t;
    }
    // Blends keyframes [{offset, value (text, or null for the neutral keyframe), easing, composite}] at a progress over the underlying value.
    function framesValue(el, prop, frames, p, underlying, defaultEasing, defaultComposite) {
      var type = TYPE_OF[prop] || 'discrete', i, a, b, list = [], f, text;
      for (i = 0; i < frames.length; i++) {
        f = frames[i];
        text = f.value === null ? underlying : composeText(type, f.composite || defaultComposite, underlying, keyframeText(el, prop, f.value));
        list.push({ offset: f.offset, value: text, easing: f.easing });
      }
      if (list.length === 0) return null;
      if (list[0].offset > 0) list.unshift({ offset: 0, value: underlying, easing: null });
      if (list[list.length - 1].offset < 1) list.push({ offset: 1, value: underlying, easing: null });
      for (i = 0; i + 2 < list.length && list[i + 1].offset <= p; i++);
      a = list[i];
      b = list[i + 1];
      if (b.offset <= a.offset) return b.value;
      return blendText(type, a.value, b.value, (a.easing ? easeText(a.easing, (p - a.offset) / (b.offset - a.offset)) : defaultEasing((p - a.offset) / (b.offset - a.offset))), prop);
    }
    // A script animation's keyframes for one property (propertyFrames' entries, with easing and composite).
    function scriptFrames(anim, camel) {
      var frames = anim._keyframes, list = propertyFrames(anim, camel), i, out = [], src;
      if (list.length === 0) return out;
      if (Array.isArray(frames)) {
        // propertyFrames keeps every keyframe; the ones without the property are neutral and kept only when an end is needed.
        for (i = 0; i < list.length; i++) {
          src = frames[i] || {};
          if (src[camel] === undefined) continue;
          out.push({ offset: list[i].offset, value: String(src[camel]), easing: src.easing && src.easing !== 'linear' ? src.easing : null, composite: src.composite && src.composite !== 'auto' ? src.composite : null });
        }
        return out;
      }
      return list.map(function (f) { return { offset: f.offset, value: f.value, easing: null, composite: null }; });
    }
    function scriptValue(anim, el, prop, underlying) {
      var t = anim._timing, d = t.duration === 'auto' ? 0 : +t.duration, time = anim.currentTime, p, frames, camel = camelOf(prop), elapsed;
      if (time === null || anim._state === 'idle') return null;
      frames = scriptFrames(anim, camel);
      if (frames.length === 0) return null;
      p = timingProgress(t.delay, d, t.iterations, t.direction, t.fill, time);
      if (p === null) return null;
      p = ease(t.easing, p);
      return framesValue(el, prop, frames, p, underlying, function (x) { return x; }, anim._composite || 'replace');
    }
    // The keyframes of an @keyframes rule for one property: [{offset, value, easing, composite}], and the rule's other properties' names.
    var keyframesCache = new Map();
    function findKeyframes(name) {
      var sheets = document.styleSheets, i, rules, j, rule;
      for (i = sheets.length - 1; i >= 0; i--) {
        try { rules = sheets[i].cssRules; } catch (e) { continue; }
        for (j = rules.length - 1; j >= 0; j--) {
          rule = rules[j];
          if (rule && rule.type === 7 && rule.name === name) return rule;
        }
      }
      return null;
    }
    function keyframeOffsets(text) {
      return text.split(',').map(function (s) {
        s = s.trim();
        return s === 'from' ? 0 : s === 'to' ? 1 : parseFloat(s) / 100;
      });
    }
    function keyframesFor(name, prop) {
      var rule = findKeyframes(name), frames = [], i, k, rr, offs, style, v, easing, comp;
      if (!rule) return frames;
      for (i = 0; i < rule.cssRules.length; i++) {
        rr = rule.cssRules[i];
        style = rr.style;
        v = style.getPropertyValue(prop);
        if (v === '') continue;
        easing = style.getPropertyValue('animation-timing-function');
        comp = style.getPropertyValue('animation-composition');
        offs = keyframeOffsets(rr.keyText);
        for (k = 0; k < offs.length; k++) frames.push({ offset: offs[k], value: v, easing: easing || null, composite: comp || null });
      }
      frames.sort(function (a, b) { return a.offset - b.offset; });
      return frames;
    }
    function cssAnimationValue(el, prop, underlying, st, cs) {
      var name = cs.getPropertyValue('animation-name'), key, frames, a, state, elapsed, p, duration, delay, iterations, direction, fill, timing;
      if (!name || name === 'none') { st.anim = null; return null; }
      a = st.anim;
      if (!a || a.name !== name) { a = st.anim = { name: name, start: nowSeconds() }; }
      frames = keyframesFor(name, prop);
      if (frames.length === 0) return null;
      duration = seconds(cs.getPropertyValue('animation-duration'));
      delay = seconds(cs.getPropertyValue('animation-delay'));
      iterations = cs.getPropertyValue('animation-iteration-count');
      iterations = iterations === 'infinite' ? -1 : +iterations;
      direction = cs.getPropertyValue('animation-direction');
      fill = cs.getPropertyValue('animation-fill-mode');
      timing = cs.getPropertyValue('animation-timing-function');
      elapsed = nowSeconds() - a.start;
      if (cs.getPropertyValue('animation-play-state') === 'paused') {
        if (a.pausedAt === undefined) a.pausedAt = elapsed;
        elapsed = a.pausedAt;
      } else if (a.pausedAt !== undefined) {
        a.start += elapsed - a.pausedAt;
        elapsed = a.pausedAt;
        a.pausedAt = undefined;
      }
      p = timingProgress(delay, duration, iterations, direction, fill, elapsed);
      if (p === null) return null;
      return framesValue(el, prop, frames, p, underlying, function (x) { return easeText(timing, x); }, cs.getPropertyValue('animation-composition') || 'replace');
    }
    // CSS transitions: a change of the computed value of a property that has a transition (read at the style recalculation, here the
    // next read after the change) starts one from the value before the change.
    function transitionValue(el, prop, base, st, cs) {
      var last = st.last[prop], tr = st.trans[prop], current = base, type = TYPE_OF[prop] || 'discrete', props, i, n, index, dur, delay, behavior, timing, from;
      if (tr) {
        if (tr.to !== base) { delete st.trans[prop]; tr = null; }
        else {
          current = transitionAt(tr, el, prop);
          if (current === null) { delete st.trans[prop]; tr = null; current = base; }
        }
      }
      if (last !== undefined && last !== base && !tr) {
        props = cs.getPropertyValue('transition-property').split(',').map(function (s) { return s.trim(); });
        index = -1;
        for (i = 0, n = props.length; i < n; i++) if (props[i] === prop || props[i] === 'all') index = i;
        if (index >= 0 && props[0] !== 'none') {
          dur = seconds(nth(cs.getPropertyValue('transition-duration'), index));
          delay = seconds(nth(cs.getPropertyValue('transition-delay'), index));
          behavior = nth(cs.getPropertyValue('transition-behavior'), index);
          timing = nth(cs.getPropertyValue('transition-timing-function'), index);
          if (dur + delay > 0 && (behavior === 'allow-discrete' || interpolable(type, last, base))) {
            from = last;
            tr = st.trans[prop] = { from: from, to: base, dur: dur, delay: delay, timing: timing, start: nowSeconds(), type: type, discrete: type === 'discrete' };
            current = transitionAt(tr, el, prop);
            if (current === null) { delete st.trans[prop]; current = base; }
          }
        }
      }
      st.last[prop] = base;
      return current;
    }
    function interpolable(type, aText, bText) {
      var a, b;
      if (type === 'discrete') return false;
      a = parseValue(type, aText);
      b = parseValue(type, bText);
      return !!(a && b && mixValues(a, b, 0.5) !== null);
    }
    function transitionAt(tr, el, prop) {
      var elapsed = nowSeconds() - tr.start - tr.delay, x;
      if (elapsed >= tr.dur) return null;
      x = elapsed < 0 ? 0 : elapsed / tr.dur;
      if (elapsed < 0) return tr.from;
      return blendText(tr.type, tr.from, tr.to, easeText(tr.timing, x), prop);
    }
    function animatedValue(el, prop, unused, cs) {
      var st = stateOf(el), value, list = [], v, a, base;
      // The value without the animation that the style engine gives the filters and the grid tracks at the start of the page's time.
      if (cs.getPropertyValue('animation-name') !== 'none') {
        native.animationOff(1);
        base = cs.getPropertyValue(prop);
        native.animationOff(0);
      } else {
        base = cs.getPropertyValue(prop);
      }
      value = transitionValue(el, prop, base, st, cs);
      v = cssAnimationValue(el, prop, value, st, cs);
      if (v !== null) value = v;
      if (all.size !== 0) {
        all.forEach(function (x) { if (x._target === el && (x._state === 'running' || x._state === 'paused' || x._state === 'finished')) list.push(x); });
        for (a = 0; a < list.length; a++) {
          v = scriptValue(list[a], el, prop, value);
          if (v !== null) value = v;
        }
      }
      return value;
    }
    var nativeGetComputedStyle = g.getComputedStyle;
    if (typeof nativeGetComputedStyle === 'function') {
      def(g, 'getComputedStyle', function getComputedStyle(element, pseudo) {
        var cs = nativeGetComputedStyle.apply(this, arguments);
        if (pseudo || !element || element.nodeType !== 1) return cs;
        return new Proxy(cs, {
          get: function (target, key) {
            var dashed;
            if (typeof key === 'string') {
              dashed = dashOf(key);
              if (animatable(dashed)) return animatedValue(element, dashed, target[key], target);
              if (key === 'getPropertyValue') {
                return function (name) {
                  var n = String(name).toLowerCase(), v = target.getPropertyValue(n);
                  return animatable(n) ? animatedValue(element, n, v, target) : v;
                };
              }
              if (key === 'cssText') return target.cssText;
            }
            var v2 = Reflect.get(target, key, target);
            return typeof v2 === 'function' ? v2.bind(target) : v2;
          }
        });
      });
    }
