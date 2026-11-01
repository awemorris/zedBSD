  // ---- Element.setHTMLUnsafe(), ShadowRoot.setHTMLUnsafe() and Document.parseHTMLUnsafe() (the fragment parser without a sanitizer)
  (function () {
    function setUnsafe(html) {
      if (arguments.length < 1) throw new TypeError("Failed to execute 'setHTMLUnsafe': 1 argument required, but only 0 present.");
      this.innerHTML = html === null ? '' : String(html);
    }
    if (typeof g.Element === 'function' && !g.Element.prototype.setHTMLUnsafe) def(g.Element.prototype, 'setHTMLUnsafe', function setHTMLUnsafe(html) { return setUnsafe.apply(this, arguments); });
    if (typeof g.ShadowRoot === 'function' && !g.ShadowRoot.prototype.setHTMLUnsafe) def(g.ShadowRoot.prototype, 'setHTMLUnsafe', function setHTMLUnsafe(html) { return setUnsafe.apply(this, arguments); });
    if (typeof g.Document === 'function' && typeof g.DOMParser === 'function' && !g.Document.parseHTMLUnsafe) {
      def(g.Document, 'parseHTMLUnsafe', function parseHTMLUnsafe(html) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'parseHTMLUnsafe' on 'Document': 1 argument required, but only 0 present.");
        return new g.DOMParser().parseFromString(String(html), 'text/html');
      });
    }
    })();
  // ---- HTMLIFrameElement.srcdoc (the attribute the frame loader reads)
  if (typeof g.HTMLIFrameElement === 'function' && !('srcdoc' in g.HTMLIFrameElement.prototype)) {
    Object.defineProperty(g.HTMLIFrameElement.prototype, 'srcdoc', {
      get: function () { var v = this.getAttribute('srcdoc'); return v === null ? '' : v; },
      set: function (v) { this.setAttribute('srcdoc', String(v)); },
      configurable: true, enumerable: true
    });
  }
