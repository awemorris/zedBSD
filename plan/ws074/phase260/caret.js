  // ---- document.caretPositionFromPoint() and CaretPosition (the character a point is nearest, by the text node's width shared out evenly)
  if (typeof g.Document === 'function' && typeof g.DOMRect === 'function' && !g.Document.prototype.caretPositionFromPoint) (function () {
    var TOKEN = {};
    function CaretPosition(token, node, offset) {
      if (token !== TOKEN) throw new TypeError('Illegal constructor');
      hidden(this, '_node', node);
      hidden(this, '_offset', offset);
    }
    Object.defineProperty(CaretPosition.prototype, 'offsetNode', { get: function () { return this._node; }, configurable: true, enumerable: true });
    Object.defineProperty(CaretPosition.prototype, 'offset', { get: function () { return this._offset; }, configurable: true, enumerable: true });
    Object.defineProperty(CaretPosition.prototype, Symbol.toStringTag, { value: 'CaretPosition', configurable: true });
    def(CaretPosition.prototype, 'getClientRect', function getClientRect() {
      var range = document.createRange(), r;
      range.setStart(this._node, this._offset);
      range.collapse(true);
      r = range.getBoundingClientRect();
      return new g.DOMRect(r.x, r.y, 0, r.height);
    });
    def(g, 'CaretPosition', CaretPosition);
    function number(value) {
      var n = Number(value);
      if (!isFinite(n)) throw new TypeError("Failed to execute 'caretPositionFromPoint' on 'Document': The provided double value is non-finite.");
      return n;
    }
    def(g.Document.prototype, 'caretPositionFromPoint', function caretPositionFromPoint(x, y, options) {
      var el, node, range, rect, offset, doc = this;
      if (arguments.length < 2) throw new TypeError("Failed to execute 'caretPositionFromPoint' on 'Document': 2 arguments required, but only " + arguments.length + ' present.');
      x = number(x);
      y = number(y);
      if (!doc.defaultView || !doc.documentElement) return null;
      if (x < 0 || y < 0 || x >= doc.documentElement.clientWidth || y >= doc.documentElement.clientHeight) return null;
      el = doc.elementFromPoint(x, y);
      if (!el) return null;
      for (node = el.firstChild; node; node = node.nextSibling) {
        if (node.nodeType !== 3 || node.data.length === 0) continue;
        range = doc.createRange();
        range.selectNodeContents(node);
        rect = range.getBoundingClientRect();
        if (y < rect.top || y >= rect.bottom || rect.width <= 0) continue;
        offset = Math.round((x - rect.left) / rect.width * node.data.length);
        offset = Math.max(0, Math.min(node.data.length, offset));
        return new CaretPosition(TOKEN, node, offset);
      }
      return new CaretPosition(TOKEN, el, 0);
    });
  })();
