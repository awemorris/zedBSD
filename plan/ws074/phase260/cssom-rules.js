  // ---- CSSOM rule objects.  The engine keeps each top-level rule of a sheet as source text (native.ruleInfo, native.ruleReplace);
  // the objects here are parsed from that text, and any change to one is written back as the rule's canonical text.
  if (typeof document !== 'undefined' && typeof g.CSSStyleSheet === 'function' && !g.CSSPositionTryRule) (function () {
    var RK = {};
    var SheetProto = g.CSSStyleSheet.prototype;
    var nativeRules = Object.getOwnPropertyDescriptor(SheetProto, 'cssRules');
    if (!nativeRules || !nativeRules.get) return;
    var nativeInsert = SheetProto.insertRule;

    // --- Scanning of rule text
    function skipString(s, i) {
      var q = s[i];
      i++;
      while (i < s.length && s[i] !== q) { if (s[i] === '\\') i++; i++; }
      return i + 1;
    }
    function skipComment(s, i) { var e = s.indexOf('*/', i + 2); return e < 0 ? s.length : e + 2; }
    // The first index at or after i of one of the stop characters outside strings, comments, escapes and brackets; -1 when there is none.
    function scanTo(s, i, stops) {
      var depth = 0, c;
      while (i < s.length) {
        c = s[i];
        if (c === '\\') { i += 2; continue; }
        if (c === '"' || c === "'") { i = skipString(s, i); continue; }
        if (c === '/' && s[i + 1] === '*') { i = skipComment(s, i); continue; }
        if (c === '(' || c === '[') depth++;
        else if (c === ')' || c === ']') { if (depth) depth--; }
        else if (!depth && stops.indexOf(c) >= 0) return i;
        i++;
      }
      return -1;
    }
    // The index of the brace that closes the one at i (the end of the text when it is not closed).
    function matchBlock(s, i) {
      var depth = 0, c;
      while (i < s.length) {
        c = s[i];
        if (c === '\\') { i += 2; continue; }
        if (c === '"' || c === "'") { i = skipString(s, i); continue; }
        if (c === '/' && s[i + 1] === '*') { i = skipComment(s, i); continue; }
        if (c === '{') depth++;
        else if (c === '}') { depth--; if (!depth) return i; }
        i++;
      }
      return s.length;
    }
    function trim(s) { return s.replace(/^[\s]+|[\s]+$/g, ''); }
    function collapse(s) { return trim(s.replace(/\s+/g, ' ')); }
    // The items of a block: declaration runs and nested rules, in order.
    function parseItems(body) {
      var items = [], i = 0, n = body.length, j, end, run = null;
      function flush() { if (run !== null) { items.push({ decls: run }); run = null; } }
      while (i < n) {
        while (i < n && (body[i] === ';' || /\s/.test(body[i]))) i++;
        if (i >= n) break;
        j = scanTo(body, i, '{;}');
        if (j < 0) { run = (run === null ? '' : run + ';') + body.slice(i); break; }
        if (body[j] === '{' && !/^--[^:]*:/.test(body.slice(i, j))) {
          flush();
          end = matchBlock(body, j);
          items.push({ rule: body.slice(i, end + 1) });
          i = end + 1;
        } else if (body[j] === '{') {
          end = matchBlock(body, j);
          run = (run === null ? '' : run + ';') + body.slice(i, end + 1);
          i = end + 1;
        } else {
          run = (run === null ? '' : run + ';') + body.slice(i, j);
          i = j + 1;
        }
      }
      flush();
      return items;
    }
    // The pieces of a rule's source: its at-keyword (lower case, without the @), prelude and body.
    function parseRule(src) {
      var at = null, i = 0, j, end, m, body = null;
      src = trim(src);
      if (src[0] === '@') {
        m = /^@([-\w\\]+)/.exec(src);
        at = m ? m[1].toLowerCase() : '';
        i = m ? m[0].length : 1;
      }
      j = scanTo(src, i, '{;');
      if (j < 0) return { at: at, prelude: collapse(src.slice(i)), body: null };
      if (src[j] === '{') {
        end = matchBlock(src, j);
        body = src.slice(j + 1, end);
      }
      return { at: at, prelude: collapse(src.slice(i, j)), body: body };
    }
    // Declarations as written: [name, value, important] (for the kinds whose descriptors the engine's element style does not know).
    function rawDecls(text) {
      var out = [], i = 0, j, c, d, name, value, imp, m;
      while (i < text.length) {
        j = scanTo(text, i, ';');
        if (j < 0) j = text.length;
        c = text.slice(i, j);
        i = j + 1;
        d = scanTo(c, 0, ':');
        if (d < 0) continue;
        name = trim(c.slice(0, d));
        value = trim(c.slice(d + 1));
        imp = false;
        m = /\s*!\s*important$/i.exec(value);
        if (m) { imp = true; value = trim(value.slice(0, m.index)); }
        if (!name || value === '') continue;
        out.push([/^--/.test(name) ? name : name.toLowerCase(), value, imp]);
      }
      return out;
    }
    function rawText(list) {
      return list.map(function (d) { return d[0] + ': ' + d[1] + (d[2] ? ' !important' : '') + ';'; }).join(' ');
    }
    // A selector list in the canonical spacing.
    function selector(s) {
      var out = '', i = 0, c, prev;
      s = collapse(s);
      while (i < s.length) {
        c = s[i];
        if (c === '"' || c === "'") { var e = skipString(s, i); out += s.slice(i, e); i = e; continue; }
        if (c === '(' || c === '[') {
          var close = c === '(' ? ')' : ']', depth = 1, k = i + 1;
          while (k < s.length && depth) {
            if (s[k] === '"' || s[k] === "'") { k = skipString(s, k); continue; }
            if (s[k] === c) depth++;
            else if (s[k] === close) depth--;
            k++;
          }
          var inner = s.slice(i + 1, k - 1);
          out += c + (c === '(' ? selectorInner(inner, out) : trim(inner)) + close;
          i = k;
          continue;
        }
        if (c === ',') { out = out.replace(/\s+$/, '') + ', '; i++; while (s[i] === ' ') i++; continue; }
        if (c === '\\') { out += s.slice(i, i + 2); i += 2; continue; }
        if (c === '>' || c === '+' || c === '~') {
          prev = out.replace(/\s+$/, '');
          out = prev + (prev === '' || /\($/.test(prev) ? '' : ' ') + c + ' ';
          i++;
          while (s[i] === ' ') i++;
          continue;
        }
        out += c;
        i++;
      }
      return trim(out);
    }
    // A nested rule's relative selectors are shown with their implicit nesting selector.
    function relative(text, parent) {
      while (parent && parent._kind !== 'style' && parent._kind !== 'scope') parent = parent._parent;
      if (!parent || parent._kind === 'scope') return text;
      return text.split(', ').map(function (part) { return /^[>+~]/.test(part) ? '& ' + part : part; }).join(', ');
    }
    // Inside parentheses: a selector list for the functional pseudo-classes, the text as written for an argument like 2n+1 or a name.
    function selectorInner(inner, before) {
      var m = /:([-\w]+)$/.exec(before);
      var fn = m ? m[1].toLowerCase() : '';
      if (fn === 'is' || fn === 'where' || fn === 'not' || fn === 'has' || fn === 'matches' || fn === 'host' || fn === 'host-context' || fn === 'global' || fn === 'local') return selector(inner);
      if (fn === 'nth-child' || fn === 'nth-last-child') {
        var o = /^(.*?)\s+of\s+(.*)$/i.exec(inner);
        if (o) return trim(o[1]) + ' of ' + selector(o[2]);
      }
      return trim(inner);
    }

    // --- Rule objects
    var TYPES = { style: 1, import: 3, media: 4, 'font-face': 5, page: 6, keyframes: 7, '-webkit-keyframes': 7, keyframe: 8, namespace: 10, 'counter-style': 11, supports: 12 };
    function throwIllegal(k) { if (k !== RK) throw new TypeError('Illegal constructor'); }
    class CSSRule {
      constructor(k) { throwIllegal(k); }
      get type() { return TYPES[this._kind] || 0; }
      get cssText() { return this._text(); }
      set cssText(v) { }
      get parentRule() { return this._parent || null; }
      get parentStyleSheet() { return this._sheet || null; }
    }
    ['STYLE_RULE:1', 'CHARSET_RULE:2', 'IMPORT_RULE:3', 'MEDIA_RULE:4', 'FONT_FACE_RULE:5', 'PAGE_RULE:6', 'KEYFRAMES_RULE:7', 'KEYFRAME_RULE:8',
      'NAMESPACE_RULE:10', 'COUNTER_STYLE_RULE:11', 'SUPPORTS_RULE:12', 'FONT_FEATURE_VALUES_RULE:14'].forEach(function (p) {
      var kv = p.split(':');
      Object.defineProperty(CSSRule, kv[0], { value: +kv[1], enumerable: true });
      Object.defineProperty(CSSRule.prototype, kv[0], { value: +kv[1], enumerable: true });
    });
    tag(CSSRule, 'CSSRule');

    // The rules inside a rule, as a live list.
    function makeList(owner, items) {
      var target = Object.create(g.CSSRuleList ? g.CSSRuleList.prototype : Object.prototype);
      function isIndex(k) { return typeof k === 'string' && /^(0|[1-9][0-9]*)$/.test(k); }
      return new Proxy(target, {
        get: function (t, k, r) {
          if (k === 'length') return items().length;
          if (k === 'item') return function item(i) { return items()[i >>> 0] || null; };
          if (isIndex(k)) return items()[+k];
          return Reflect.get(t, k, r);
        },
        has: function (t, k) { return isIndex(k) ? +k < items().length : Reflect.has(t, k); },
        getOwnPropertyDescriptor: function (t, k) {
          if (isIndex(k) && +k < items().length) return { value: items()[+k], writable: false, enumerable: true, configurable: true };
          return Reflect.getOwnPropertyDescriptor(t, k);
        },
        ownKeys: function (t) {
          var keys = [], i, n = items().length;
          for (i = 0; i < n; i++) keys.push(String(i));
          return keys.concat(Reflect.ownKeys(t));
        },
        set: function (t, k, v, r) { return isIndex(k) ? false : Reflect.set(t, k, v, r); }
      });
    }

    // The declarations of a rule are kept in an element's style (the engine validates and writes them); a change is written back.
    function scratchStyle(rule, text) {
      var el = document.createElement('div');
      el.style.cssText = text;
      rule._scratch = el;
      return el.style;
    }
    function declProxy(rule) {
      var st = rule._scratch.style;
      return new Proxy(st, {
        get: function (t, k) {
          var v = Reflect.get(t, k, t);
          if (typeof v === 'function' && k !== 'constructor') {
            return function () {
              var r = v.apply(t, arguments);
              if (k === 'setProperty' || k === 'removeProperty') rule._commit();
              return r;
            };
          }
          return v;
        },
        set: function (t, k, v) { var ok = Reflect.set(t, k, v, t); rule._commit(); return ok; }
      });
    }

    class CSSGroupingRule extends CSSRule {
      get cssRules() {
        if (!this._list) hidden(this, '_list', makeList(this, function () { return this._children; }.bind(this)));
        return this._list;
      }
      insertRule(text, index) {
        var n = this._children.length, parsed, rule;
        index = index === undefined ? 0 : index >>> 0;
        if (index > n) throw new DOMException('The index is outside the rule list.', 'IndexSizeError');
        text = String(text);
        parsed = parseItems(text);
        if (parsed.length !== 1 || !parsed[0].rule && !parsed[0].decls) throw new DOMException('The input is not one supported CSS rule.', 'SyntaxError');
        rule = parsed[0].rule ? makeRule(parsed[0].rule, this._sheet, this) : null;
        if (!rule) throw new DOMException('The input is not one supported CSS rule.', 'SyntaxError');
        this._children.splice(index, 0, rule);
        this._commit();
        return index;
      }
      deleteRule(index) {
        index = index >>> 0;
        if (index >= this._children.length) throw new DOMException('The index is outside the rule list.', 'IndexSizeError');
        this._children.splice(index, 1);
        this._commit();
      }
    }
    tag(CSSGroupingRule, 'CSSGroupingRule');
    class CSSConditionRule extends CSSGroupingRule {
      get conditionText() { return this._prelude; }
    }
    tag(CSSConditionRule, 'CSSConditionRule');
    class CSSStyleRule extends CSSGroupingRule {
      get selectorText() { return this._prelude; }
      set selectorText(v) {
        var old = this._prelude;
        this._prelude = relative(selector(String(v)), this._parent);
        if (!this._commit()) this._prelude = old;
      }
      get style() { if (!this._proxy) hidden(this, '_proxy', declProxy(this)); return this._proxy; }
      set style(v) { this.style.cssText = String(v); }
      get styleMap() { return undefined; }
    }
    tag(CSSStyleRule, 'CSSStyleRule');
    class CSSNestedDeclarations extends CSSRule {
      get style() { if (!this._proxy) hidden(this, '_proxy', declProxy(this)); return this._proxy; }
    }
    tag(CSSNestedDeclarations, 'CSSNestedDeclarations');
    class CSSMediaRule extends CSSConditionRule {
      get media() { var t = this._prelude; return { mediaText: t, length: t === '' ? 0 : t.split(',').length, toString: function () { return t; } }; }
    }
    tag(CSSMediaRule, 'CSSMediaRule');
    class CSSSupportsRule extends CSSConditionRule { }
    tag(CSSSupportsRule, 'CSSSupportsRule');
    class CSSContainerRule extends CSSConditionRule {
      get containerName() { var m = /^(?!\(|not\b|style\(|scroll-state\()([-\w\\]+)\s/.exec(this._prelude + ' '); return m ? m[1] : ''; }
      get containerQuery() { var n = this.containerName; return n ? trim(this._prelude.slice(n.length)) : this._prelude; }
    }
    tag(CSSContainerRule, 'CSSContainerRule');
    class CSSScopeRule extends CSSGroupingRule {
      get start() { return this._scope[0]; }
      get end() { return this._scope[1]; }
    }
    tag(CSSScopeRule, 'CSSScopeRule');
    class CSSLayerBlockRule extends CSSGroupingRule {
      get name() { return this._prelude; }
    }
    tag(CSSLayerBlockRule, 'CSSLayerBlockRule');
    class CSSLayerStatementRule extends CSSRule {
      get nameList() { return this._prelude.split(',').map(trim); }
    }
    tag(CSSLayerStatementRule, 'CSSLayerStatementRule');
    class CSSStartingStyleRule extends CSSGroupingRule { }
    tag(CSSStartingStyleRule, 'CSSStartingStyleRule');
    class CSSFontFaceRule extends CSSRule {
      get style() { return this._descs(); }
    }
    tag(CSSFontFaceRule, 'CSSFontFaceRule');
    class CSSPropertyRule extends CSSRule {
      get name() { return this._prelude; }
      get syntax() { return unquote(findDesc(this, 'syntax')); }
      get inherits() { return findDesc(this, 'inherits') === 'true'; }
      get initialValue() { var v = findDesc(this, 'initial-value'); return v === '' ? null : v; }
    }
    tag(CSSPropertyRule, 'CSSPropertyRule');
    class CSSCounterStyleRule extends CSSRule {
      get name() { return this._prelude; }
    }
    tag(CSSCounterStyleRule, 'CSSCounterStyleRule');
    class CSSPageRule extends CSSRule {
      get selectorText() { return this._prelude; }
      get style() { return this._descs(); }
    }
    tag(CSSPageRule, 'CSSPageRule');
    class CSSNamespaceRule extends CSSRule {
      get prefix() { var m = /^([-\w]+)\s+/.exec(this._prelude); return m ? m[1] : ''; }
      get namespaceURI() { var m = /url\(\s*["']?([^"')]*)["']?\s*\)|["']([^"']*)["']/.exec(this._prelude); return m ? (m[1] !== undefined ? m[1] : m[2]) : ''; }
    }
    tag(CSSNamespaceRule, 'CSSNamespaceRule');
    class CSSImportRule extends CSSRule {
      get href() { var m = /^(?:url\(\s*["']?([^"')]*)["']?\s*\)|["']([^"']*)["'])/.exec(this._prelude); return m ? (m[1] !== undefined ? m[1] : m[2]) : ''; }
      get media() { return { mediaText: '', length: 0 }; }
      get styleSheet() { return null; }
      get layerName() { return null; }
      get supportsText() { return null; }
    }
    tag(CSSImportRule, 'CSSImportRule');
    class CSSKeyframesRule extends CSSRule {
      get name() { return this._prelude; }
      get cssRules() {
        if (!this._list) hidden(this, '_list', makeList(this, function () { return this._children; }.bind(this)));
        return this._list;
      }
    }
    tag(CSSKeyframesRule, 'CSSKeyframesRule');
    class CSSKeyframeRule extends CSSRule {
      get keyText() { return this._prelude; }
      get style() { if (!this._proxy) hidden(this, '_proxy', declProxy(this)); return this._proxy; }
    }
    tag(CSSKeyframeRule, 'CSSKeyframeRule');
    class CSSViewTransitionRule extends CSSRule { }
    tag(CSSViewTransitionRule, 'CSSViewTransitionRule');

    // CSSPositionTryRule's declarations: only the properties a position fallback can change.
    var TRY_PROPS = ['margin', 'margin-top', 'margin-right', 'margin-bottom', 'margin-left', 'margin-block', 'margin-block-start', 'margin-block-end',
      'margin-inline', 'margin-inline-start', 'margin-inline-end', 'inset', 'top', 'left', 'right', 'bottom', 'inset-block', 'inset-block-start',
      'inset-block-end', 'inset-inline', 'inset-inline-start', 'inset-inline-end', 'width', 'height', 'min-width', 'max-width', 'min-height', 'max-height',
      'block-size', 'min-block-size', 'max-block-size', 'inline-size', 'min-inline-size', 'max-inline-size', 'place-self', 'align-self', 'justify-self',
      'position-anchor', 'position-area'];
    var scratchProbe = null;
    function validValue(name, value) {
      if (/^(unset|initial|inherit|revert|revert-layer)$/i.test(value)) return value.toLowerCase();
      if (!scratchProbe) scratchProbe = document.createElement('div');
      var st = scratchProbe.style;
      st.cssText = '';
      st.setProperty(name, value);
      var out = st.getPropertyValue(name);
      st.cssText = '';
      return out;
    }
    class CSSPositionTryDescriptors {
      constructor(k, rule) { throwIllegal(k); hidden(this, '_rule', rule); hidden(this, '_list', []); }
      get length() { return this._list.length; }
      item(i) { var d = this._list[i >>> 0]; return d ? d[0] : ''; }
      getPropertyValue(name) {
        name = String(name).toLowerCase();
        for (var i = 0; i < this._list.length; i++) if (this._list[i][0] === name) return this._list[i][1];
        return '';
      }
      getPropertyPriority() { return ''; }
      setProperty(name, value) {
        name = String(name).toLowerCase();
        if (TRY_PROPS.indexOf(name) < 0) return;
        value = trim(String(value));
        if (value === '') { this.removeProperty(name); return; }
        var v = validValue(name, value);
        if (v === '') return;
        for (var i = 0; i < this._list.length; i++) if (this._list[i][0] === name) { this._list[i][1] = v; this._rule._commit(); return; }
        this._list.push([name, v]);
        this._rule._commit();
      }
      removeProperty(name) {
        name = String(name).toLowerCase();
        for (var i = 0; i < this._list.length; i++) {
          if (this._list[i][0] === name) {
            var old = this._list[i][1];
            this._list.splice(i, 1);
            this._rule._commit();
            return old;
          }
        }
        return '';
      }
      get cssText() { return this._list.map(function (d) { return d[0] + ': ' + d[1] + ';'; }).join(' '); }
      set cssText(text) {
        this._list.length = 0;
        this._fill(String(text));
        this._rule._commit();
      }
      _fill(text) {
        var self = this;
        rawDecls(text).forEach(function (d) {
          if (TRY_PROPS.indexOf(d[0]) < 0 || d[2]) return;
          var v = validValue(d[0], d[1]);
          if (v === '') return;
          for (var i = 0; i < self._list.length; i++) if (self._list[i][0] === d[0]) { self._list[i][1] = v; return; }
          self._list.push([d[0], v]);
        });
      }
    }
    tag(CSSPositionTryDescriptors, 'CSSPositionTryDescriptors');
    function camel(p) { return p.replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }
    TRY_PROPS.forEach(function (p) {
      function get() { return this.getPropertyValue(p); }
      function set(v) { this.setProperty(p, v); }
      Object.defineProperty(CSSPositionTryDescriptors.prototype, p, { get: get, set: set, enumerable: true, configurable: true });
      if (camel(p) !== p) Object.defineProperty(CSSPositionTryDescriptors.prototype, camel(p), { get: get, set: set, enumerable: true, configurable: true });
    });
    class CSSPositionTryRule extends CSSRule {
      get name() { return this._prelude; }
      get style() { return this._tryDecls; }
    }
    tag(CSSPositionTryRule, 'CSSPositionTryRule');

    // Descriptor blocks as written (font-face, property, counter-style, page).
    function descsOf(rule) {
      if (!rule._descObj) {
        var list = rule._rawList, obj = Object.create(g.CSSStyleDeclaration ? g.CSSStyleDeclaration.prototype : Object.prototype);
        hidden(rule, '_descObj', new Proxy(obj, {
          get: function (t, k, r) {
            var i;
            if (k === 'length') return list.length;
            if (k === 'cssText') return rawText(list);
            if (k === 'getPropertyValue') return function (n) { n = String(n).toLowerCase(); for (i = 0; i < list.length; i++) if (list[i][0] === n) return list[i][1]; return ''; };
            if (k === 'item') return function (n) { return list[n >>> 0] ? list[n >>> 0][0] : ''; };
            if (typeof k === 'string' && !(k in t)) { var n = k.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); }); for (i = 0; i < list.length; i++) if (list[i][0] === n) return list[i][1]; return ''; }
            return Reflect.get(t, k, r);
          }
        }));
      }
      return rule._descObj;
    }
    function findDesc(rule, name) { for (var i = 0; i < rule._rawList.length; i++) if (rule._rawList[i][0] === name) return rule._rawList[i][1]; return ''; }
    function unquote(s) { return /^(["']).*\1$/.test(s) ? s.slice(1, -1) : s; }
    CSSFontFaceRule.prototype._descs = CSSPageRule.prototype._descs = function () { return descsOf(this); };

    // --- Parsing a rule's text into an object
    function pickClass(r, inStyle) {
      var at = r.at;
      if (at === null) return CSSStyleRule;
      switch (at) {
        case 'media': return CSSMediaRule;
        case 'supports': return CSSSupportsRule;
        case 'container': return CSSContainerRule;
        case 'scope': return CSSScopeRule;
        case 'layer': return r.body === null ? CSSLayerStatementRule : CSSLayerBlockRule;
        case 'starting-style': return CSSStartingStyleRule;
        case 'position-try': return CSSPositionTryRule;
        case 'font-face': return CSSFontFaceRule;
        case 'property': return CSSPropertyRule;
        case 'counter-style': return CSSCounterStyleRule;
        case 'page': return CSSPageRule;
        case 'namespace': return CSSNamespaceRule;
        case 'import': return CSSImportRule;
        case 'keyframes': case '-webkit-keyframes': return CSSKeyframesRule;
        case 'view-transition': return CSSViewTransitionRule;
        default: return CSSRule;
      }
    }
    function newRule(C) { var r = new C(RK); return r; }
    function scopePair(prelude) {
      var start = null, end = null, m, rest = prelude, close;
      if (rest[0] === '(') {
        close = scanTo(rest, 1, ')');
        // scanTo skips brackets, so the closing parenthesis is found by balance.
        close = balance(rest, 0);
        start = selector(rest.slice(1, close));
        rest = trim(rest.slice(close + 1));
      }
      m = /^to\s*\(/i.exec(rest);
      if (m) {
        close = balance(rest, m[0].length - 1);
        end = selector(rest.slice(m[0].length, close));
      }
      return [start, end];
    }
    function balance(s, open) {
      var depth = 0, i = open, c;
      while (i < s.length) {
        c = s[i];
        if (c === '\\') { i += 2; continue; }
        if (c === '"' || c === "'") { i = skipString(s, i); continue; }
        if (c === '(') depth++;
        else if (c === ')') { depth--; if (!depth) return i; }
        i++;
      }
      return s.length;
    }
    function makeRule(src, sheet, parent) {
      var r = parseRule(src), C = pickClass(r), rule = newRule(C), kind = r.at === null ? 'style' : r.at, items, i, own, seenRule;
      hidden(rule, '_kind', kind);
      hidden(rule, '_sheet', sheet);
      hidden(rule, '_parent', parent || null);
      hidden(rule, '_src', trim(src));
      hidden(rule, '_prelude', r.at === null ? relative(selector(r.prelude), parent) : r.prelude);
      hidden(rule, '_commit', commit);
      hidden(rule, '_text', serialize);
      if (C === CSSGroupingRule || C.prototype instanceof CSSGroupingRule) {
        hidden(rule, '_children', []);
        items = r.body === null ? [] : parseItems(r.body);
        seenRule = false;
        var inStyle = false, p = rule;
        while (p) { if (p._kind === 'style') inStyle = true; p = p._parent; }
        if (C === CSSScopeRule) hidden(rule, '_scope', scopePair(r.prelude));
        for (i = 0; i < items.length; i++) {
          if (items[i].rule) {
            seenRule = true;
            rule._children.push(makeRule(items[i].rule, sheet, rule));
          } else if (C === CSSStyleRule && !seenRule) {
            own = items[i].decls;
            scratchStyle(rule, own);
          } else if (C === CSSStyleRule || C === CSSScopeRule || inStyle) {
            rule._children.push(makeDecls(items[i].decls, sheet, rule));
          }
        }
        if (C === CSSStyleRule && !rule._scratch) scratchStyle(rule, '');
      } else if (C === CSSPositionTryRule) {
        var d = new CSSPositionTryDescriptors(RK, rule);
        hidden(rule, '_tryDecls', d);
        if (r.body !== null) d._fill(parseItems(r.body).filter(function (x) { return x.decls !== undefined; }).map(function (x) { return x.decls; }).join(';'));
      } else if (C === CSSKeyframesRule) {
        hidden(rule, '_children', []);
        items = r.body === null ? [] : parseItems(r.body);
        for (i = 0; i < items.length; i++) if (items[i].rule) rule._children.push(makeKeyframe(items[i].rule, sheet, rule));
      } else if (C === CSSNamespaceRule || C === CSSImportRule || C === CSSLayerStatementRule || C === CSSViewTransitionRule || C === CSSRule) {
        // Kept as written.
      } else {
        hidden(rule, '_rawList', r.body === null ? [] : rawDecls(r.body));
      }
      return rule;
    }
    function makeDecls(text, sheet, parent) {
      var rule = newRule(CSSNestedDeclarations);
      hidden(rule, '_kind', 'nested-declarations');
      hidden(rule, '_sheet', sheet);
      hidden(rule, '_parent', parent);
      hidden(rule, '_commit', commit);
      hidden(rule, '_text', serialize);
      hidden(rule, '_prelude', '');
      scratchStyle(rule, text);
      return rule;
    }
    function makeKeyframe(src, sheet, parent) {
      var r = parseRule(src), rule = newRule(CSSKeyframeRule);
      hidden(rule, '_kind', 'keyframe');
      hidden(rule, '_sheet', sheet);
      hidden(rule, '_parent', parent);
      hidden(rule, '_commit', commit);
      hidden(rule, '_text', serialize);
      hidden(rule, '_prelude', r.prelude.split(',').map(trim).join(', '));
      scratchStyle(rule, r.body === null ? '' : r.body);
      return rule;
    }

    // --- Serialization
    function indent(text) { return text.split('\n').join('\n  '); }
    function declsText(rule) { return rule._scratch ? rule._scratch.style.cssText : ''; }
    function serialize() {
      var rule = this, kind = rule._kind, head, parts, i, inner;
      switch (kind) {
        case 'nested-declarations': return declsText(rule);
        case 'style':
          parts = [];
          if (declsText(rule) !== '') parts.push(declsText(rule));
          for (i = 0; i < rule._children.length; i++) parts.push(rule._children[i].cssText);
          return rule._prelude + ' { ' + (parts.length ? parts.join(' ') + ' ' : '') + '}';
        case 'keyframe': return rule._prelude + ' { ' + (declsText(rule) !== '' ? declsText(rule) + ' ' : '') + '}';
        case 'position-try': return '@position-try ' + rule._prelude + ' { ' + (rule._tryDecls.cssText !== '' ? rule._tryDecls.cssText + ' ' : '') + '}';
        case 'font-face': case 'property': case 'counter-style': case 'page':
          return '@' + kind + (rule._prelude ? ' ' + rule._prelude : '') + ' { ' + (rule._rawList.length ? rawText(rule._rawList) + ' ' : '') + '}';
        case 'layer':
          if (rule._children === undefined) return '@layer ' + rule._prelude.split(',').map(trim).join(', ') + ';';
          // fall through
        case 'media': case 'supports': case 'container': case 'scope': case 'starting-style': case 'keyframes': case '-webkit-keyframes':
          head = '@' + kind;
          if (kind === 'scope') {
            head += (rule._scope[0] !== null ? ' (' + rule._scope[0] + ')' : '') + (rule._scope[1] !== null ? ' to (' + rule._scope[1] + ')' : '');
          } else if (rule._prelude !== '') {
            head += ' ' + rule._prelude;
          }
          inner = '';
          for (i = 0; i < rule._children.length; i++) inner += '  ' + indent(rule._children[i].cssText) + '\n';
          return head + ' {\n' + inner + '}';
        default: return rule._src;
      }
    }
    // Writes a changed rule back to the sheet; true when the engine accepted the text.
    function commit() {
      var top = this, map, idx, id;
      while (top._parent) top = top._parent;
      if (!top._sheet || top._id === undefined) return true;
      idx = indexOf(top._sheet, top._id);
      if (idx < 0) return true;
      try {
        id = native.ruleReplace(top._sheet, idx, top.cssText);
      } catch (e) {
        return false;
      }
      map = sheetMap(top._sheet);
      map.delete(top._id);
      top._id = id;
      map.set(id, top);
      return true;
    }

    // --- The sheet's list
    var sheets = new WeakMap();
    function sheetMap(sheet) { var m = sheets.get(sheet); if (!m) { m = new Map(); sheets.set(sheet, m); } return m; }
    function lengthOf(sheet) { return nativeRules.get.call(sheet).length; }
    function parseInfo(info) {
      var a = info.indexOf(' '), b = info.indexOf(' ', a + 1);
      return { id: +info.slice(0, a), type: +info.slice(a + 1, b), src: info.slice(b + 1) };
    }
    function indexOf(sheet, id) {
      var n = lengthOf(sheet), i, info;
      for (i = 0; i < n; i++) {
        info = native.ruleInfo(sheet, i);
        if (info !== undefined && parseInt(info, 10) === id) return i;
      }
      return -1;
    }
    function ruleAt(sheet, i) {
      var info = native.ruleInfo(sheet, i), p, map, rule;
      if (info === undefined) return undefined;
      p = parseInfo(info);
      map = sheetMap(sheet);
      rule = map.get(p.id);
      if (!rule) {
        rule = makeRule(p.src, sheet, null);
        hidden(rule, '_id', p.id);
        map.set(p.id, rule);
      }
      return rule;
    }
    var lists = new WeakMap();
    function sheetList(sheet) {
      var l = lists.get(sheet);
      if (!l) {
        l = makeList(sheet, function () {
          var n = lengthOf(sheet), out = [], i;
          for (i = 0; i < n; i++) out.push(ruleAt(sheet, i));
          return out;
        });
        lists.set(sheet, l);
      }
      return l;
    }
    var listDescriptor = { get: function () { return sheetList(this); }, enumerable: true, configurable: true };
    Object.defineProperty(SheetProto, 'cssRules', listDescriptor);
    Object.defineProperty(SheetProto, 'rules', listDescriptor);
    // A rule that cannot be one is refused here (the engine's list keeps what it can parse).
    SheetProto.insertRule = function insertRule(rule, index) {
      var r = parseRule(String(rule));
      if (r.at === 'position-try' && !/^--[^\s,{}]+$/.test(r.prelude)) throw new DOMException('The input is not one supported CSS rule.', 'SyntaxError');
      return arguments.length > 1 ? nativeInsert.call(this, rule, index) : nativeInsert.call(this, rule);
    };
    def(SheetProto, 'insertRule', SheetProto.insertRule);

    [['CSSRule', CSSRule], ['CSSGroupingRule', CSSGroupingRule], ['CSSConditionRule', CSSConditionRule], ['CSSStyleRule', CSSStyleRule],
      ['CSSNestedDeclarations', CSSNestedDeclarations], ['CSSMediaRule', CSSMediaRule], ['CSSSupportsRule', CSSSupportsRule],
      ['CSSContainerRule', CSSContainerRule], ['CSSScopeRule', CSSScopeRule], ['CSSLayerBlockRule', CSSLayerBlockRule],
      ['CSSLayerStatementRule', CSSLayerStatementRule], ['CSSStartingStyleRule', CSSStartingStyleRule], ['CSSFontFaceRule', CSSFontFaceRule],
      ['CSSPropertyRule', CSSPropertyRule], ['CSSCounterStyleRule', CSSCounterStyleRule], ['CSSPageRule', CSSPageRule],
      ['CSSNamespaceRule', CSSNamespaceRule], ['CSSImportRule', CSSImportRule], ['CSSKeyframesRule', CSSKeyframesRule],
      ['CSSKeyframeRule', CSSKeyframeRule], ['CSSViewTransitionRule', CSSViewTransitionRule],
      ['CSSPositionTryRule', CSSPositionTryRule], ['CSSPositionTryDescriptors', CSSPositionTryDescriptors]].forEach(function (p) { def(g, p[0], p[1]); });
  })();
