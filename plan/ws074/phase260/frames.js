  // ---- window.length and window.frames[n]: the child windows of the frames in the document, in tree order
  (function () {
    function children() {
      var list = [], found = g.document.querySelectorAll('iframe, frame'), i, w;
      for (i = 0; i < found.length; i++) {
        w = found[i].contentWindow;
        if (w) list.push(w);
      }
      return list;
    }
    if (!Object.getOwnPropertyDescriptor(g, 'length')) {
      Object.defineProperty(g, 'length', { get: function () { return children().length; }, configurable: true, enumerable: true });
    }
    if (typeof g.Proxy === 'function') {
      var view = new Proxy(g, {
        get: function (target, key) {
          var n;
          if (typeof key === 'string' && /^(0|[1-9][0-9]*)$/.test(key)) {
            n = Number(key);
            return children()[n];
          }
          return Reflect.get(target, key, target);
        },
        has: function (target, key) {
          if (typeof key === 'string' && /^(0|[1-9][0-9]*)$/.test(key)) return Number(key) < children().length;
          return Reflect.has(target, key);
        }
      });
      Object.defineProperty(g, 'frames', { get: function () { return view; }, configurable: true, enumerable: true });
    }
  })();
