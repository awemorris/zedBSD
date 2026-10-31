    // The effect of a script animation on the computed style: a property the animation's keyframes name reads as the keyframes' blend at the
    // animation's current time (getComputedStyle), over the value the element would have without it (the neutral keyframe's).  Properties
    // handled so far: grid-template-columns and grid-template-rows (track lists).
    var EFFECT_PROPS = { 'grid-template-columns': 1, 'grid-template-rows': 1 };
    function camelOf(name) { return name.replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }
    function dashOf(name) { return name.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); }); }
    // The keyframes of an animation for a property: [{ offset, value (null for the neutral keyframe) }] in order.
    function propertyFrames(anim, camel) {
      var frames = anim._keyframes, list = [], i, n, k, offsets;
      if (frames && !Array.isArray(frames) && typeof frames === 'object') {
        var values = frames[camel];
        if (values === undefined) return [];
        if (!Array.isArray(values)) values = [values];
        n = values.length;
        for (i = 0; i < n; i++) list.push({ offset: n === 1 ? 1 : i / (n - 1), value: values[i] === null ? null : String(values[i]) });
        return list;
      }
      if (!Array.isArray(frames)) return [];
      n = frames.length;
      offsets = [];
      for (i = 0; i < n; i++) offsets.push(frames[i] && frames[i].offset !== undefined && frames[i].offset !== null ? +frames[i].offset : null);
      if (n > 0 && offsets[0] === null) offsets[0] = 0;
      if (n > 1 && offsets[n - 1] === null) offsets[n - 1] = 1;
      for (i = 0; i < n; i++) {
        if (offsets[i] !== null) continue;
        k = i;
        while (offsets[k] === null) k++;
        var from = offsets[i - 1], step = (offsets[k] - from) / (k - i + 1), j;
        for (j = i; j < k; j++) offsets[j] = from + step * (j - i + 1);
      }
      for (i = 0; i < n; i++) {
        var f = frames[i] || {};
        list.push({ offset: offsets[i], value: f[camel] === undefined ? null : String(f[camel]) });
      }
      return list;
    }
    // Splits a value into top-level words (parentheses and brackets kept whole).
    function words(text) {
      var out = [], depth = 0, cur = '', i, c;
      for (i = 0; i < text.length; i++) {
        c = text[i];
        if (c === '(' || c === '[') depth++;
        if (c === ')' || c === ']') depth--;
        if (/\s/.test(c) && depth === 0) { if (cur) out.push(cur); cur = ''; } else cur += c;
      }
      if (cur) out.push(cur);
      return out;
    }
    function fmt(n) { return (Math.round(n * 10000) / 10000) + ''; }
    // A track list as { tracks: [word], lines: [[name]] } (lines has one more entry than tracks), or null for none and other values.
    function parseTracks(text) {
      var list = words(text.trim()), tracks = [], lines = [[]], i, w;
      if (list.length === 1 && list[0] === 'none') return null;
      for (i = 0; i < list.length; i++) {
        w = list[i];
        if (w[0] === '[') {
          lines[lines.length - 1] = lines[lines.length - 1].concat(w.slice(1, -1).trim().split(/\s+/).filter(Boolean));
        } else {
          tracks.push(w);
          lines.push([]);
        }
      }
      return { tracks: tracks, lines: lines };
    }
    function trackText(list) {
      var out = [], i;
      for (i = 0; i < list.tracks.length; i++) {
        if (list.lines[i].length) out.push('[' + list.lines[i].join(' ') + ']');
        out.push(list.tracks[i]);
      }
      if (list.lines[list.tracks.length].length) out.push('[' + list.lines[list.tracks.length].join(' ') + ']');
      return out.join(' ');
    }
    // Blends two values of one track (a length, a flexible length, minmax() or fit-content()); the first when they do not match at less than half.
    function blendTrack(a, b, p) {
      var ma = /^(-?[\d.]+(?:e-?\d+)?)(px|%|fr)$/.exec(a), mb = /^(-?[\d.]+(?:e-?\d+)?)(px|%|fr)$/.exec(b);
      if (ma && mb && ma[2] === mb[2]) return fmt(+ma[1] + (+mb[1] - +ma[1]) * p) + ma[2];
      var fa = /^(minmax|fit-content)\((.*)\)$/.exec(a), fb = /^(minmax|fit-content)\((.*)\)$/.exec(b);
      if (fa && fb && fa[1] === fb[1]) {
        var pa = fa[2].split(/\s*,\s*/), pb = fb[2].split(/\s*,\s*/), parts = [], i, r;
        if (pa.length === pb.length) {
          for (i = 0; i < pa.length; i++) {
            r = blendTrack(pa[i], pb[i], p);
            if (r === null) { parts = null; break; }
            parts.push(r);
          }
          if (parts) return fa[1] + '(' + parts.join(', ') + ')';
        }
      }
      return a === b ? a : null;
    }
    function blendTrackList(from, to, p) {
      var a = parseTracks(from), b = parseTracks(to), tracks = [], lines = [], i, r;
      if (!a || !b || a.tracks.length !== b.tracks.length) return p < 0.5 ? from : to;
      for (i = 0; i < a.tracks.length; i++) {
        r = blendTrack(a.tracks[i], b.tracks[i], p);
        tracks.push(r === null ? (p < 0.5 ? a.tracks[i] : b.tracks[i]) : r);
      }
      lines = (p < 0.5 ? a : b).lines;
      return trackText({ tracks: tracks, lines: lines });
    }
    // The easing of a keyframe pair: linear and the keywords, steps(), and cubic-bezier().
    function ease(timing, x) {
      var m;
      if (timing === 'linear' || !timing) return x;
      m = /^cubic-bezier\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)$/.exec(timing);
      if (!m && timing === 'ease') m = [0, 0.25, 0.1, 0.25, 1];
      if (!m && timing === 'ease-in') m = [0, 0.42, 0, 1, 1];
      if (!m && timing === 'ease-out') m = [0, 0, 0, 0.58, 1];
      if (!m && timing === 'ease-in-out') m = [0, 0.42, 0, 0.58, 1];
      if (m) {
        var x1 = +m[1], y1 = +m[2], x2 = +m[3], y2 = +m[4], lo = 0, hi = 1, t = x, i, cx;
        for (i = 0; i < 40; i++) {
          cx = 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t;
          if (cx < x) lo = t; else hi = t;
          t = (lo + hi) / 2;
        }
        return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t;
      }
      m = /^steps\(\s*(\d+)\s*(?:,\s*([a-z-]+))?\s*\)$/.exec(timing);
      if (m) {
        var steps = +m[1], jump = m[2] || 'end', k = Math.floor(x * steps);
        if (jump === 'start' || jump === 'jump-start') k++;
        if (jump === 'jump-both') k++;
        if (x >= 1) k = steps;
        return Math.min(1, Math.max(0, k / (jump === 'jump-both' ? steps + 1 : steps)));
      }
      return x;
    }
    // The value of a property an animation gives now, or null when it gives none (underlying: the value without any animation).
    function effectValue(anim, camel, underlying) {
      var t = anim._timing, d = t.duration === 'auto' ? 0 : +t.duration, time = anim.currentTime, local, iteration, frames, i, a, b, p;
      if (time === null || anim._state === 'idle') return null;
      local = time - t.delay;
      if (local < 0) { if (t.fill !== 'backwards' && t.fill !== 'both') return null; local = 0; }
      if (d <= 0) return null;
      iteration = local / d;
      if (iteration >= t.iterations) {
        if (t.fill !== 'forwards' && t.fill !== 'both') return null;
        iteration = t.iterations;
      }
      p = iteration - Math.floor(iteration);
      if (iteration > 0 && p === 0) p = 1;
      p = ease(t.easing, p);
      frames = propertyFrames(anim, camel);
      if (frames.length === 0) return null;
      for (i = 0; i < frames.length; i++) if (frames[i].value === null) frames[i].value = underlying;
      if (frames.length === 1) return frames[0].value;
      for (i = 0; i + 1 < frames.length && frames[i + 1].offset < p; i++);
      a = frames[i];
      b = frames[Math.min(i + 1, frames.length - 1)];
      if (p <= a.offset) return a.value;
      if (b.offset <= a.offset) return b.value;
      return blendTrackList(a.value, b.value, ease('linear', (p - a.offset) / (b.offset - a.offset)));
    }
    function animatedValue(el, dashed, native) {
      var camel = camelOf(dashed), value = native, list = [], a;
      all.forEach(function (x) { if (x._target === el && (x._state === 'running' || x._state === 'paused' || x._state === 'finished')) list.push(x); });
      for (a = 0; a < list.length; a++) {
        var v = effectValue(list[a], camel, native);
        if (v !== null) value = v;
      }
      return value;
    }
    var nativeGetComputedStyle = g.getComputedStyle;
    if (typeof nativeGetComputedStyle === 'function') {
      def(g, 'getComputedStyle', function getComputedStyle(element, pseudo) {
        var cs = nativeGetComputedStyle.apply(this, arguments), animated = false;
        if (pseudo || all.size === 0 || !element) return cs;
        all.forEach(function (x) { if (x._target === element) animated = true; });
        if (!animated) return cs;
        return new Proxy(cs, {
          get: function (target, key) {
            var dashed;
            if (typeof key === 'string') {
              dashed = dashOf(key);
              if (EFFECT_PROPS[dashed]) return animatedValue(element, dashed, target[key]);
              if (key === 'getPropertyValue') {
                return function (name) {
                  var n = String(name).toLowerCase(), v = target.getPropertyValue(n);
                  return EFFECT_PROPS[n] ? animatedValue(element, n, v) : v;
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
