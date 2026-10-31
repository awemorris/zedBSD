// The web platform pieces that are written in the page's own language (ws074-p230).
// bind/prelude.c holds this text as a C string; this file is the readable source it is made from.
// It is a function of the natives (parseURL, setURL, scrollToFragment, fire, setHyperlinkFollower, nodeRect) and the global object.
(function (native, g) {
  'use strict';
  var parseURL = native.parseURL;
  function def(o, name, f) { Object.defineProperty(o, name, { value: f, writable: true, configurable: true, enumerable: false }); }
  function hidden(o, name, v) { Object.defineProperty(o, name, { value: v, writable: true, configurable: true, enumerable: false }); }
  function tag(C, name) { Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true }); }
  var KEY = {};

  // ---- AbortSignal and AbortController
  function abort(signal, reason) {
    if (signal.aborted) return;
    signal.aborted = true;
    signal.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
    var ev = new Event('abort');
    if (typeof signal.onabort === 'function') {
      try { signal.onabort.call(signal, ev); } catch (e) { setTimeout(function () { throw e; }, 0); }
    }
    signal.dispatchEvent(ev);
  }
  class AbortSignal extends EventTarget {
    constructor(key) {
      if (key !== KEY) throw new TypeError('Illegal constructor');
      super();
      this.aborted = false;
      this.reason = undefined;
      this.onabort = null;
    }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(reason) { var s = new AbortSignal(KEY); abort(s, reason); return s; }
    static timeout(ms) {
      var s = new AbortSignal(KEY);
      setTimeout(function () { abort(s, new DOMException('signal timed out', 'TimeoutError')); }, ms);
      return s;
    }
    static any(signals) {
      var s = new AbortSignal(KEY), i, list = Array.from(signals);
      for (i = 0; i < list.length; i++) {
        if (list[i].aborted) { abort(s, list[i].reason); return s; }
      }
      list.forEach(function (x) { x.addEventListener('abort', function () { abort(s, x.reason); }); });
      return s;
    }
  }
  tag(AbortSignal, 'AbortSignal');
  class AbortController {
    constructor() { hidden(this, '_s', new AbortSignal(KEY)); }
    get signal() { return this._s; }
    abort(reason) { abort(this._s, reason); }
  }
  tag(AbortController, 'AbortController');
  def(g, 'AbortSignal', AbortSignal);
  def(g, 'AbortController', AbortController);

  // ---- URLSearchParams and URL
  function formEncode(s) {
    return encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, function (c) {
      return '%' + c.charCodeAt(0).toString(16).toUpperCase();
    });
  }
  function formDecode(s) {
    s = s.replace(/\+/g, ' ');
    try { return decodeURIComponent(s); } catch (e) {
      return s.replace(/(%[0-9a-fA-F]{2})+/g, function (m) { try { return decodeURIComponent(m); } catch (e2) { return m; } });
    }
  }
  class URLSearchParams {
    constructor(init) {
      hidden(this, '_l', []);
      hidden(this, '_u', null);
      if (init === undefined || init === null) return;
      if (typeof init === 'object' || typeof init === 'function') {
        if (typeof init[Symbol.iterator] === 'function') {
          var pair, p;
          for (pair of init) {
            p = Array.from(pair);
            if (p.length !== 2) throw new TypeError("Failed to construct 'URLSearchParams': Each pair must be an iterable [name, value] tuple");
            this._l.push([String(p[0]), String(p[1])]);
          }
        } else {
          Object.keys(init).forEach(function (k) { this._l.push([k, String(init[k])]); }, this);
        }
      } else {
        this._parse(String(init));
      }
    }
    _parse(s) {
      if (s.charAt(0) === '?') s = s.slice(1);
      this._l.length = 0;
      if (s === '') return;
      s.split('&').forEach(function (part) {
        if (part === '') return;
        var i = part.indexOf('=');
        this._l.push(i < 0 ? [formDecode(part), ''] : [formDecode(part.slice(0, i)), formDecode(part.slice(i + 1))]);
      }, this);
    }
    _changed() { if (this._u) this._u._setSearch(this.toString()); }
    get size() { return this._l.length; }
    append(k, v) { this._l.push([String(k), String(v)]); this._changed(); }
    delete(k, v) {
      k = String(k);
      this._l = this._l.filter(function (e) { return !(e[0] === k && (v === undefined || e[1] === String(v))); });
      this._changed();
    }
    get(k) { k = String(k); for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === k) return this._l[i][1]; return null; }
    getAll(k) { k = String(k); return this._l.filter(function (e) { return e[0] === k; }).map(function (e) { return e[1]; }); }
    has(k, v) {
      k = String(k);
      return this._l.some(function (e) { return e[0] === k && (v === undefined || e[1] === String(v)); });
    }
    set(k, v) {
      k = String(k); v = String(v);
      var found = false;
      this._l = this._l.filter(function (e) {
        if (e[0] !== k) return true;
        if (found) return false;
        found = true; e[1] = v; return true;
      });
      if (!found) this._l.push([k, v]);
      this._changed();
    }
    sort() {
      var l = this._l.map(function (e, i) { return [e, i]; });
      l.sort(function (a, b) { return a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]; });
      this._l = l.map(function (x) { return x[0]; });
      this._changed();
    }
    forEach(f, thisArg) { this._l.slice().forEach(function (e) { f.call(thisArg, e[1], e[0], this); }, this); }
    keys() { return this._l.map(function (e) { return e[0]; })[Symbol.iterator](); }
    values() { return this._l.map(function (e) { return e[1]; })[Symbol.iterator](); }
    entries() { return this._l.map(function (e) { return [e[0], e[1]]; })[Symbol.iterator](); }
    toString() { return this._l.map(function (e) { return formEncode(e[0]) + '=' + formEncode(e[1]); }).join('&'); }
  }
  def(URLSearchParams.prototype, Symbol.iterator, URLSearchParams.prototype.entries);
  tag(URLSearchParams, 'URLSearchParams');

  var PARTS = ['href', 'origin', 'protocol', 'username', 'password', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'];
  function reparse(url, text) {
    var r = parseURL(text);
    if (!r) return false;
    url._r = r;
    if (url._sp) url._sp._parse(r[9]);
    return true;
  }
  class URL {
    constructor(url, base) {
      var r = parseURL(String(url), base === undefined ? undefined : String(base));
      if (!r) throw new TypeError("Failed to construct 'URL': Invalid URL");
      hidden(this, '_r', r);
      hidden(this, '_sp', null);
    }
    static canParse(url, base) { return !!parseURL(String(url), base === undefined ? undefined : String(base)); }
    static parse(url, base) { try { return new URL(url, base); } catch (e) { return null; } }
    static createObjectURL() { return 'blob:' + location.origin + '/' + Math.random().toString(16).slice(2); }
    static revokeObjectURL() {}
    _setSearch(s) {
      var h = this._r[0], i = h.indexOf('#'), hash = i < 0 ? '' : h.slice(i), rest = i < 0 ? h : h.slice(0, i), q = rest.indexOf('?');
      if (q >= 0) rest = rest.slice(0, q);
      reparse(this, rest + (s ? '?' + s : '') + hash);
    }
    get href() { return this._r[0]; }
    set href(v) { if (!reparse(this, String(v))) throw new TypeError("Failed to set the 'href' property on 'URL': Invalid URL"); }
    get origin() { return this._r[1]; }
    get protocol() { return this._r[2]; }
    set protocol(v) { v = String(v).replace(/:.*$/, ''); reparse(this, v + ':' + this._r[0].slice(this._r[2].length)); }
    get username() { return this._r[3]; }
    set username(v) { this._auth(encodeURIComponent(String(v)), this._r[4]); }
    get password() { return this._r[4]; }
    set password(v) { this._auth(this._r[3], encodeURIComponent(String(v))); }
    _auth(u, p) {
      var h = this._r[0], m = /^([a-zA-Z][a-zA-Z0-9+.-]*:\/\/)([^\/?#]*@)?(.*)$/.exec(h);
      if (!m) return;
      reparse(this, m[1] + (u ? u + (p ? ':' + p : '') + '@' : '') + m[3]);
    }
    _host(host) {
      var h = this._r[0], m = /^([a-zA-Z][a-zA-Z0-9+.-]*:\/\/)([^\/?#@]*@)?([^\/?#]*)(.*)$/.exec(h);
      if (!m) return;
      reparse(this, m[1] + (m[2] || '') + host + m[4]);
    }
    get host() { return this._r[5]; }
    set host(v) { v = String(v); if (v) this._host(v); }
    get hostname() { return this._r[6]; }
    set hostname(v) { v = String(v); if (v) this._host(v + (this._r[7] ? ':' + this._r[7] : '')); }
    get port() { return this._r[7]; }
    set port(v) { v = String(v).replace(/[^0-9].*$/, ''); this._host(this._r[6] + (v ? ':' + v : '')); }
    get pathname() { return this._r[8]; }
    set pathname(v) {
      v = String(v);
      var h = this._r[0], m = /^([a-zA-Z][a-zA-Z0-9+.-]*:\/\/[^\/?#]*)([^?#]*)(.*)$/.exec(h);
      if (!m) return;
      reparse(this, m[1] + (v.charAt(0) === '/' ? '' : '/') + v.replace(/\?/g, '%3F').replace(/#/g, '%23') + m[3]);
    }
    get search() { return this._r[9]; }
    set search(v) { v = String(v); if (v.charAt(0) === '?') v = v.slice(1); this._setSearch(v); }
    get searchParams() {
      if (!this._sp) {
        var sp = new URLSearchParams(this._r[9]);
        sp._u = this;
        this._sp = sp;
      }
      return this._sp;
    }
    get hash() { return this._r[10]; }
    set hash(v) {
      v = String(v); if (v.charAt(0) === '#') v = v.slice(1);
      var h = this._r[0], i = h.indexOf('#');
      reparse(this, (i < 0 ? h : h.slice(0, i)) + (v ? '#' + v : ''));
    }
    toString() { return this._r[0]; }
    toJSON() { return this._r[0]; }
  }
  tag(URL, 'URL');
  def(g, 'URLSearchParams', URLSearchParams);
  def(g, 'URL', URL);

  // ---- Language additions
  if (!Object.fromEntries) {
    def(Object, 'fromEntries', function fromEntries(entries) {
      var o = {}, e;
      for (e of entries) o[e[0]] = e[1];
      return o;
    });
  }
  if (!String.prototype.matchAll) {
    def(String.prototype, 'matchAll', function matchAll(re) {
      var s = String(this), r, out = [], m;
      if (typeof re === 'string') r = new RegExp(re, 'g');
      else {
        if (re instanceof RegExp && !re.global) throw new TypeError('String.prototype.matchAll called with a non-global RegExp argument');
        r = new RegExp(re.source, re.flags); r.lastIndex = re.lastIndex;
      }
      while ((m = r.exec(s)) !== null) { out.push(m); if (m[0] === '') r.lastIndex++; }
      return out[Symbol.iterator]();
    });
  }
  if (!g.AggregateError) {
    class AggregateError extends Error {
      constructor(errors, message) {
        super(message);
        hidden(this, 'errors', Array.from(errors));
        hidden(this, 'name', 'AggregateError');
      }
    }
    def(g, 'AggregateError', AggregateError);
  }
  if (!Error.captureStackTrace) {
    def(Error, 'captureStackTrace', function captureStackTrace(o) {
      hidden(o, 'stack', String(o && o.name ? o.name + ': ' + o.message : o));
    });
  }
  if (!g.Reflect) {
    var R = {
      apply: function (f, t, a) { return Function.prototype.apply.call(f, t, a); },
      construct: function (C, a) { return new (Function.prototype.bind.apply(C, [null].concat(Array.from(a))))(); },
      defineProperty: function (o, k, d) { try { Object.defineProperty(o, k, d); return true; } catch (e) { return false; } },
      deleteProperty: function (o, k) { try { return delete o[k]; } catch (e) { return false; } },
      get: function (o, k) { return o[k]; },
      set: function (o, k, v) { try { o[k] = v; return true; } catch (e) { return false; } },
      has: function (o, k) { return k in o; },
      ownKeys: function (o) { return Object.getOwnPropertyNames(o).concat(Object.getOwnPropertySymbols(o)); },
      getPrototypeOf: function (o) { return Object.getPrototypeOf(o); },
      setPrototypeOf: function (o, p) { try { Object.setPrototypeOf(o, p); return true; } catch (e) { return false; } },
      isExtensible: function (o) { return Object.isExtensible(o); },
      preventExtensions: function (o) { Object.preventExtensions(o); return true; },
      getOwnPropertyDescriptor: function (o, k) { return Object.getOwnPropertyDescriptor(o, k); }
    };
    var Refl = {};
    Object.keys(R).forEach(function (k) { def(Refl, k, R[k]); });
    Object.defineProperty(Refl, Symbol.toStringTag, { value: 'Reflect', configurable: true });
    def(g, 'Reflect', Refl);
  }

  // ---- Intl (the formats of the page's locale, without locale data)
  if (!g.Intl) {
    var Intl_ = {};
    var locales = function (l) { return Array.isArray(l) ? (l[0] || 'ja-JP') : (l || 'ja-JP'); };
    function intlType(name, init, methods) {
      var C = function (l, o) {
        if (!(this instanceof C)) return new C(l, o);
        hidden(this, '_l', locales(l));
        hidden(this, '_o', o || {});
        if (init) init.call(this);
      };
      Object.defineProperty(C, 'name', { value: name });
      Object.keys(methods).forEach(function (k) { def(C.prototype, k, methods[k]); });
      def(C, 'supportedLocalesOf', function (l) { return Array.isArray(l) ? l : [l]; });
      return C;
    }
    var DateTimeFormat = intlType('DateTimeFormat', null, {
      format: function (d) { d = d === undefined ? new Date() : new Date(d); return this._o.timeStyle || this._o.hour ? d.toLocaleString() : d.toLocaleDateString(); },
      formatToParts: function (d) { return [{ type: 'literal', value: this.format(d) }]; },
      resolvedOptions: function () { return { locale: this._l, calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }; }
    });
    var NumberFormat = intlType('NumberFormat', null, {
      format: function (n) { return Number(n).toLocaleString(this._l, this._o); },
      formatToParts: function (n) { return [{ type: 'integer', value: this.format(n) }]; },
      resolvedOptions: function () { return { locale: this._l, numberingSystem: 'latn', style: this._o.style || 'decimal' }; }
    });
    var Collator = intlType('Collator', null, {
      compare: function (a, b) { return String(a).localeCompare(String(b)); },
      resolvedOptions: function () { return { locale: this._l, usage: 'sort', sensitivity: 'variant' }; }
    });
    var PluralRules = intlType('PluralRules', null, {
      select: function (n) { return n === 1 ? 'one' : 'other'; },
      resolvedOptions: function () { return { locale: this._l }; }
    });
    var RelativeTimeFormat = intlType('RelativeTimeFormat', null, { format: function (v, unit) { return v + ' ' + unit; } });
    var ListFormat = intlType('ListFormat', null, { format: function (list) { return Array.from(list).join(', '); } });
    var Segmenter = intlType('Segmenter', null, {
      segment: function (s) { s = String(s); return Array.from(s).map(function (c, i) { return { segment: c, index: i, input: s }; }); }
    });
    [['DateTimeFormat', DateTimeFormat], ['NumberFormat', NumberFormat], ['Collator', Collator], ['PluralRules', PluralRules],
      ['RelativeTimeFormat', RelativeTimeFormat], ['ListFormat', ListFormat], ['Segmenter', Segmenter]].forEach(function (p) {
      def(Intl_, p[0], p[1]);
    });
    def(Intl_, 'getCanonicalLocales', function (l) { return l === undefined ? [] : Array.isArray(l) ? l : [l]; });
    def(Intl_, 'supportedValuesOf', function () { return []; });
    Object.defineProperty(Intl_, Symbol.toStringTag, { value: 'Intl', configurable: true });
    def(g, 'Intl', Intl_);
  }

  // ---- matchMedia, MessageChannel
  if (!g.matchMedia) {
    def(g, 'matchMedia', function matchMedia(q) {
      q = String(q);
      var m = {
        media: q, matches: false, onchange: null,
        addListener: function () {}, removeListener: function () {},
        addEventListener: function () {}, removeEventListener: function () {}, dispatchEvent: function () { return false; }
      };
      if (/^\(\s*prefers-color-scheme\s*:\s*light\s*\)$/.test(q)) m.matches = true;
      if (/^\(\s*(any-)?hover\s*:\s*hover\s*\)$/.test(q)) m.matches = true;
      if (/^\(\s*(any-)?pointer\s*:\s*fine\s*\)$/.test(q)) m.matches = true;
      return m;
    });
  }
  class MessagePort extends EventTarget {
    constructor(key) { if (key !== KEY) throw new TypeError('Illegal constructor'); super(); this.onmessage = null; hidden(this, '_peer', null); hidden(this, '_open', true); }
    postMessage(data) {
      var peer = this._peer;
      if (!peer || !this._open) return;
      setTimeout(function () {
        var ev = new MessageEvent('message', { data: data });
        if (typeof peer.onmessage === 'function') peer.onmessage.call(peer, ev);
        peer.dispatchEvent(ev);
      }, 0);
    }
    start() {}
    close() { this._open = false; }
  }
  class MessageChannel {
    constructor() {
      var a = new MessagePort(KEY), b = new MessagePort(KEY);
      a._peer = b; b._peer = a;
      this.port1 = a; this.port2 = b;
    }
  }
  if (!g.MessageChannel) { def(g, 'MessagePort', MessagePort); def(g, 'MessageChannel', MessageChannel); }
  if (!g.requestIdleCallback) {
    def(g, 'requestIdleCallback', function requestIdleCallback(f) {
      return setTimeout(function () { f({ didTimeout: false, timeRemaining: function () { return 10; } }); }, 1);
    });
    def(g, 'cancelIdleCallback', function cancelIdleCallback(id) { clearTimeout(id); });
  }
  // ---- Session history: History, Location's navigation and the Navigation API
  // (https://html.spec.whatwg.org/multipage/nav-history-apis.html).  Every entry is of this document: a navigation to
  // another document fires its navigate event and, unless a listener intercepts it, is not carried out here.
  if (typeof document !== 'undefined' && g.location && !g.navigation) (function () {
    var LocationProto = Object.getPrototypeOf(g.location);
    var hrefGetter = Object.getOwnPropertyDescriptor(LocationProto, 'href').get;
    var then = Promise.prototype.then;
    function docURL() { return hrefGetter.call(g.location); }
    function noFrag(u) { var i = u.indexOf('#'); return i < 0 ? u : u.slice(0, i); }
    function frag(u) { var i = u.indexOf('#'); return i < 0 ? null : u.slice(i + 1); }
    // The errors the browser makes (not a script's) are told apart for navigateerror's filename.
    var browserErrors = new WeakSet();
    function domError(name, message) { var e = new DOMException(message, name); browserErrors.add(e); return e; }
    function abortError() { return domError('AbortError', 'The navigation was aborted.'); }
    // A serialized state is { v: copy } (null is no state); each read is a fresh copy.
    function serialize(v) { return { v: structuredClone(v) }; }
    function deserialize(s) { return s ? structuredClone(s.v) : undefined; }
    function uuid() {
      if (g.crypto && typeof g.crypto.randomUUID === 'function') return g.crypto.randomUUID();
      return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function (c) {
        var r = Math.random() * 16 | 0;
        return (c === 'x' ? r : (r & 3 | 8)).toString(16);
      });
    }
    function deferred() {
      var d = {};
      d.promise = new Promise(function (resolve, reject) { d.resolve = resolve; d.reject = reject; });
      return d;
    }
    function markHandled(p) { then.call(p, undefined, function () {}); return p; }
    function baseURL() {
      var b = document.querySelector('base[href]');
      if (b) { try { return new URL(b.getAttribute('href'), docURL()).href; } catch (e) { /* the document's URL */ } }
      return docURL();
    }
    function resolveURL(u) { try { return new URL(String(u), baseURL()).href; } catch (e) { return null; } }
    // Whether the document's URL can become u without loading another document.
    function canRewrite(u) {
      var a, b;
      try { a = new URL(u); b = new URL(docURL()); } catch (e) { return false; }
      if (a.protocol !== b.protocol || a.username !== b.username || a.password !== b.password || a.host !== b.host) return false;
      if (a.protocol === 'http:' || a.protocol === 'https:') return true;
      if (a.protocol === 'file:' && a.pathname !== b.pathname) return false;
      return true;
    }
    function enumValue(v, values, what) {
      v = String(v);
      if (values.indexOf(v) < 0) throw new TypeError("The provided value '" + v + "' is not a valid enum value of type " + what + '.');
      return v;
    }
    function dictionary(o, what) {
      if (o === undefined || o === null) return {};
      if (typeof o !== 'object' && typeof o !== 'function') throw new TypeError("The provided value is not of type '" + what + "'.");
      return o;
    }
    // An event handler IDL attribute (onnavigate and the like) over one listener.
    function handlerAttribute(proto, type) {
      Object.defineProperty(proto, 'on' + type, {
        get: function () { var h = this._handlers && this._handlers[type]; return h ? h.f : null; },
        set: function (v) {
          var self = this, h;
          if (!this._handlers) hidden(this, '_handlers', {});
          h = this._handlers[type];
          if (typeof v !== 'function' && (typeof v !== 'object' || v === null)) v = null;
          if (v === null) {
            if (h) { this.removeEventListener(type, h.l); delete this._handlers[type]; }
            return;
          }
          if (h) { h.f = v; return; }
          h = { f: v, l: null };
          h.l = function (e) {
            if (typeof h.f !== 'function') return;
            if (h.f.call(self, e) === false) e.preventDefault();
          };
          this._handlers[type] = h;
          this.addEventListener(type, h.l);
        },
        configurable: true, enumerable: true
      });
    }

    // The session history entries of this document, oldest first, and the current one.
    var entries = [];
    var current = 0;
    var classicState = null;
    // The document an entry belongs to: the entries of the documents that came before this one in its frame are carried over to it.
    var docId = uuid();
    var handover = null;
    function makeEntry(url, navState, classic, from) {
      return { url: url, key: uuid(), id: uuid(), navState: navState, classic: classic, scroll: null,
        scrollMode: from ? from.scrollMode : 'auto', nhe: null, docId: docId };
    }
    // The first entry follows the document's URL until it is left (the URL is set after this runs).
    var loadedURL = null;
    function urlOf(entry) { return entry.url === null ? docURL() : entry.url; }
    function leave(entry) {
      if (entry.url === null) { entry.url = docURL(); loadedURL = entry.url; }
      saveScroll(entry);
    }
    function findEntry(key) {
      for (var i = 0; i < entries.length; i++) if (entries[i].key === key) return entries[i];
      return null;
    }

    class NavigationHistoryEntry extends EventTarget {
      constructor(key, entry) {
        if (key !== KEY) throw new TypeError('Illegal constructor');
        super();
        hidden(this, '_e', entry);
      }
      get url() { return active() ? urlOf(this._e) : ''; }
      get key() { return active() ? this._e.key : ''; }
      get id() { return active() ? this._e.id : ''; }
      get index() { return active() ? entries.indexOf(this._e) : -1; }
      get sameDocument() { return active() && this._e.docId === docId; }
      getState() { return deserialize(this._e.navState); }
    }
    handlerAttribute(NavigationHistoryEntry.prototype, 'dispose');
    tag(NavigationHistoryEntry, 'NavigationHistoryEntry');
    function nhe(entry) {
      if (!entry.nhe) entry.nhe = new NavigationHistoryEntry(KEY, entry);
      return entry.nhe;
    }

    class NavigationDestination {
      constructor(key, d) {
        if (key !== KEY) throw new TypeError('Illegal constructor');
        hidden(this, '_d', d);
      }
      get url() { return this._d.url; }
      get key() { return this._d.entry && active() ? this._d.entry.key : ''; }
      get id() { return this._d.entry && active() ? this._d.entry.id : ''; }
      get index() { return active() ? this._d.index : -1; }
      get sameDocument() { return this._d.sameDocument; }
      getState() { return deserialize(this._d.navState); }
    }
    tag(NavigationDestination, 'NavigationDestination');

    class NavigationTransition {
      constructor(key, type, from) {
        if (key !== KEY) throw new TypeError('Illegal constructor');
        hidden(this, '_type', type);
        hidden(this, '_from', from);
        hidden(this, '_c', deferred());
        hidden(this, '_f', deferred());
        markHandled(this._f.promise);
      }
      get navigationType() { return this._type; }
      get from() { return this._from; }
      get committed() { return this._c.promise; }
      get finished() { return this._f.promise; }
    }
    tag(NavigationTransition, 'NavigationTransition');

    class NavigationActivation {
      constructor(key, from, entry, type) {
        if (key !== KEY) throw new TypeError('Illegal constructor');
        hidden(this, '_from', from);
        hidden(this, '_entry', entry);
        hidden(this, '_type', type);
      }
      get from() { return this._from; }
      get entry() { return this._entry; }
      get navigationType() { return this._type; }
    }
    tag(NavigationActivation, 'NavigationActivation');

    var TYPES = ['push', 'replace', 'reload', 'traverse'];
    class NavigateEvent extends Event {
      constructor(type, init) {
        if (arguments.length < 2) throw new TypeError("Failed to construct 'NavigateEvent': 2 arguments required.");
        init = dictionary(init, 'NavigateEventInit');
        if (!(init.destination instanceof NavigationDestination)) {
          throw new TypeError("Failed to construct 'NavigateEvent': required member destination is not a NavigationDestination.");
        }
        if (!(init.signal instanceof AbortSignal)) throw new TypeError("Failed to construct 'NavigateEvent': required member signal is not an AbortSignal.");
        super(type, init);
        hidden(this, '_i', {
          navigationType: init.navigationType === undefined ? 'push' : enumValue(init.navigationType, TYPES, 'NavigationType'),
          destination: init.destination,
          canIntercept: !!init.canIntercept,
          userInitiated: !!init.userInitiated,
          hashChange: !!init.hashChange,
          signal: init.signal,
          formData: init.formData === undefined ? null : init.formData,
          downloadRequest: init.downloadRequest === undefined || init.downloadRequest === null ? null : String(init.downloadRequest),
          info: init.info,
          hasUAVisualTransition: !!init.hasUAVisualTransition,
          sourceElement: init.sourceElement === undefined ? null : init.sourceElement
        });
        hidden(this, '_n', null);
      }
      get navigationType() { return this._i.navigationType; }
      get destination() { return this._i.destination; }
      get canIntercept() { return this._i.canIntercept; }
      get userInitiated() { return this._i.userInitiated; }
      get hashChange() { return this._i.hashChange; }
      get signal() { return this._i.signal; }
      get formData() { return this._i.formData; }
      get downloadRequest() { return this._i.downloadRequest; }
      get info() { return this._i.info; }
      get hasUAVisualTransition() { return this._i.hasUAVisualTransition; }
      get sourceElement() { return this._i.sourceElement; }
      intercept(options) {
        var n = this._n, handler, focusReset, scroll;
        options = dictionary(options, 'NavigationInterceptOptions');
        handler = options.handler;
        if (handler !== undefined && typeof handler !== 'function') throw new TypeError("Failed to execute 'intercept' on 'NavigateEvent': The handler is not a function.");
        focusReset = options.focusReset === undefined ? undefined : enumValue(options.focusReset, ['after-transition', 'manual'], 'NavigationFocusReset');
        scroll = options.scroll === undefined ? undefined : enumValue(options.scroll, ['after-transition', 'manual'], 'NavigationScrollBehavior');
        sharedChecks(this, 'intercept');
        if (!this.canIntercept) throw domError('SecurityError', "Failed to execute 'intercept' on 'NavigateEvent': The navigation cannot be intercepted.");
        if (!n.dispatching) throw domError('InvalidStateError', "Failed to execute 'intercept' on 'NavigateEvent': intercept() may only be called while the navigate event is being dispatched.");
        n.state = 'intercepted';
        if (handler !== undefined) n.handlers.push(handler);
        if (focusReset !== undefined) n.focusReset = focusReset;
        if (scroll !== undefined) n.scroll = scroll;
      }
      scroll() {
        sharedChecks(this, 'scroll');
        if (this._n.state !== 'committed') throw domError('InvalidStateError', "Failed to execute 'scroll' on 'NavigateEvent': scroll() may only be called after the navigation commits and before it finishes or scrolls.");
        processScroll(this);
      }
    }
    tag(NavigateEvent, 'NavigateEvent');
    function sharedChecks(event, method) {
      if (!event._n) throw domError('SecurityError', "Failed to execute '" + method + "' on 'NavigateEvent': The event was not fired by the browser.");
      if (!active()) throw domError('InvalidStateError', "Failed to execute '" + method + "' on 'NavigateEvent': The document is not fully active.");
      if (event.defaultPrevented || event._n.canceled) throw domError('InvalidStateError', "Failed to execute '" + method + "' on 'NavigateEvent': The event was canceled.");
    }

    class NavigationCurrentEntryChangeEvent extends Event {
      constructor(type, init) {
        if (arguments.length < 2) throw new TypeError("Failed to construct 'NavigationCurrentEntryChangeEvent': 2 arguments required.");
        init = dictionary(init, 'NavigationCurrentEntryChangeEventInit');
        if (!(init.from instanceof NavigationHistoryEntry)) {
          throw new TypeError("Failed to construct 'NavigationCurrentEntryChangeEvent': required member from is not a NavigationHistoryEntry.");
        }
        super(type, init);
        hidden(this, '_type', init.navigationType === undefined || init.navigationType === null ? null : enumValue(init.navigationType, TYPES, 'NavigationType'));
        hidden(this, '_from', init.from);
      }
      get navigationType() { return this._type; }
      get from() { return this._from; }
    }
    tag(NavigationCurrentEntryChangeEvent, 'NavigationCurrentEntryChangeEvent');

    // The navigation API's state: the navigate event under way, the transition, and the method trackers whose
    // promises navigate(), reload(), traverseTo(), back() and forward() return.
    var ongoingEvent = null;
    var ongoingTracker = null;
    var upcomingTracker = null;
    var upcomingTraverse = new Map();
    var transition = null;
    var focusChanged = false;
    var suppressScroll = false;
    function makeTracker(info, navState, key) {
      var t = { committed: deferred(), finished: deferred(), info: info, navState: navState, key: key, entry: null };
      markHandled(t.finished.promise);
      return t;
    }
    function trackerResult(t) { return { committed: t.committed.promise, finished: t.finished.promise }; }
    function earlyError(e) { return { committed: Promise.reject(e), finished: Promise.reject(e) }; }
    function cleanUp(t) {
      if (ongoingTracker === t) ongoingTracker = null;
      else if (t.key !== null && upcomingTraverse.get(t.key) === t) upcomingTraverse.delete(t.key);
    }
    function notifyCommitted(t, entry) {
      t.entry = entry;
      if (t.key === null && t.navState !== null) entry._e.navState = t.navState;
      t.committed.resolve(entry);
    }
    function resolveFinished(t) { t.committed.resolve(t.entry); t.finished.resolve(t.entry); cleanUp(t); }
    function rejectFinished(t, e) { t.committed.reject(e); t.finished.reject(e); cleanUp(t); }
    function errorMessage(e) {
      try { if (e instanceof Error) return 'Uncaught ' + String(e); } catch (x) { /* none */ }
      return '';
    }
    // The events the browser fires are trusted.  A browser error is the document's; a script error's script is not
    // known, so the document as it was loaded stands for it, at line 1 column 1 (the engine keeps no position for an Error).
    function fire(target, event) { return native.fire(target, event); }
    function fireError(e) {
      var filename = browserErrors.has(e) || loadedURL === null ? docURL() : loadedURL;
      fire(navigation, new ErrorEvent('navigateerror', { error: e, message: errorMessage(e), filename: filename, lineno: 1, colno: 1 }));
    }

    // Aborts the navigate event under way (a new navigation, window.stop(), a canceled event).
    function abortOngoing(e) {
      var event = ongoingEvent, n = event._n;
      focusChanged = false;
      suppressScroll = false;
      if (e === undefined) e = abortError();
      if (n.dispatching) { n.canceled = true; if (event.cancelable) event.preventDefault(); }
      n.controller.abort(e);
      ongoingEvent = null;
      fireError(e);
      if (ongoingTracker) rejectFinished(ongoingTracker, e);
      if (transition) {
        transition._c.reject(e);
        transition._f.reject(e);
        transition = null;
      }
    }

    // Scrolling: an entry keeps where the page was scrolled when it was left, and a fragment navigation scrolls to its target.
    function saveScroll(entry) { entry.scroll = [g.scrollX || 0, g.scrollY || 0]; }
    function restoreScroll(entry) { if (entry.scroll) g.scrollTo(entry.scroll[0], entry.scroll[1]); }
    function scrollToFragment(url, toTop) {
      var f = frag(url), name;
      if (f === null) { if (toTop) g.scrollTo(0, 0); return; }
      if (native.scrollToFragment(f)) return;
      try { name = decodeURIComponent(f); } catch (e) { name = f; }
      if (toTop || f === '' || name.toLowerCase() === 'top') g.scrollTo(0, 0);
    }
    function processScroll(event) {
      var n = event._n;
      n.state = 'scrolled';
      if (n.type === 'traverse' || n.type === 'reload') restoreScroll(entries[current]);
      else scrollToFragment(docURL(), true);
    }

    // Focus: an intercepted navigation that finishes moves the focus to the autofocus candidate or the viewport,
    // unless something moved it while the navigation was under way.
    document.addEventListener('focusin', function () { if (ongoingEvent) focusChanged = true; }, true);
    document.addEventListener('focusout', function () { if (ongoingEvent) focusChanged = true; }, true);
    function resetFocus(n) {
      var changed = focusChanged, list, i, active;
      focusChanged = false;
      if (changed || n.focusReset === 'manual') return;
      list = document.querySelectorAll('[autofocus]');
      for (i = 0; i < list.length; i++) {
        if (list[i].disabled || typeof list[i].focus !== 'function') continue;
        list[i].focus();
        if (document.activeElement === list[i]) return;
      }
      active = document.activeElement;
      if (active && active !== document.body && typeof active.blur === 'function') active.blur();
    }
    function finish(event, fulfilled) {
      var n = event._n;
      if (n.state === 'none') return;
      resetFocus(n);
      if (fulfilled && n.state !== 'scrolled' && n.scroll !== 'manual') processScroll(event);
      n.state = 'finished';
    }

    // Changing the current entry: a pushed entry drops those after the current one, a replacing entry keeps
    // its key; either way the entries that left are disposed of after currententrychange.
    function putEntry(entry, handling) {
      var disposed = [], removed, i;
      if (handling === 'push') {
        removed = entries.splice(current + 1);
        for (i = 0; i < removed.length; i++) if (removed[i].nhe) disposed.push(removed[i].nhe);
        entries.push(entry);
        current = entries.length - 1;
      } else {
        entry.key = entries[current].key;
        if (entries[current].nhe) disposed.push(entries[current].nhe);
        entries[current] = entry;
      }
      return disposed;
    }
    function entriesChanged(from, type, disposed) {
      var i;
      if (ongoingTracker) notifyCommitted(ongoingTracker, nhe(entries[current]));
      if (transition) transition._c.resolve();
      fire(navigation, new NavigationCurrentEntryChangeEvent('currententrychange', { navigationType: type, from: from }));
      for (i = 0; i < disposed.length; i++) fire(disposed[i], new Event('dispose'));
    }
    function queueHashChange(oldURL, newURL) {
      setTimeout(function () { fire(g, new HashChangeEvent('hashchange', { oldURL: oldURL, newURL: newURL })); }, 0);
    }
    // The URL and history update steps: pushState(), replaceState() and intercepted navigations.
    function updateURLAndHistory(url, classic, handling) {
      var old = entries[current], from = nhe(old), entry, disposed;
      leave(old);
      entry = makeEntry(url, null, classic !== null ? classic : old.classic, old);
      disposed = putEntry(entry, handling);
      nhe(entry);
      if (classic !== null) classicState = deserialize(classic);
      native.setURL(url);
      entriesChanged(from, handling, disposed);
    }
    // A navigation to a fragment of this document.
    function navigateToFragment(url, handling, navState) {
      var old = entries[current], from = nhe(old), oldURL = docURL(), entry, disposed;
      leave(old);
      entry = makeEntry(url, navState !== null ? navState : old.navState, null, old);
      disposed = putEntry(entry, handling);
      nhe(entry);
      classicState = null;
      native.setURL(url);
      entriesChanged(from, handling, disposed);
      scrollToFragment(url, false);
      if (frag(oldURL) !== frag(url)) queueHashChange(oldURL, url);
    }
    // A traversal to another entry of this document.
    function commitTraverse(entry) {
      var old = entries[current], from = nhe(old), oldURL = docURL();
      leave(old);
      current = entries.indexOf(entry);
      native.setURL(entry.url);
      classicState = entry.classic ? deserialize(entry.classic) : null;
      entriesChanged(from, 'traverse', []);
      fire(g, new PopStateEvent('popstate', { state: classicState, hasUAVisualTransition: false }));
      if (entry.scrollMode === 'auto' && !suppressScroll) restoreScroll(entry);
      if (frag(oldURL) !== frag(entry.url)) queueHashChange(oldURL, entry.url);
    }

    function waitForAll(list, success, failure) {
      var left = list.length, rejected = false, i;
      for (i = 0; i < list.length; i++) {
        then.call(list[i], function () { if (--left === 0 && !rejected) success(); },
          function (r) { if (rejected) return; rejected = true; failure(r); });
      }
    }
    // A document that is being left (beforeunload fired, then pagehide) refuses further navigations; one that is
    // still on its initial about:blank has no navigate events, activation or entry state of its own.
    var unloading = false;
    function active() { return native.fullyActive() && !unloading; }
    function initialBlank() { return native.initialBlank(); }
    // A navigation to another document in this frame: the entries as they will be, handed to the document that loads.
    function packEntry(e) {
      return { url: urlOf(e), key: e.key, id: e.id, navState: e.navState, classic: e.classic, scrollMode: e.scrollMode, docId: e.docId };
    }
    function leaveForDocument(o) {
      var frame = null, list, target, index, old, h;
      frame = native.frameElement();
      if (!frame || o.formData || (o.downloadRequest !== undefined && o.downloadRequest !== null)) return false;
      old = entries[current];
      leave(old);
      list = entries.slice();
      index = current;
      if (initialBlank()) o = { type: 'replace', url: o.url, navState: null };
      if (o.type === 'push') {
        target = makeEntry(o.url, o.navState, null, old);
        target.docId = uuid();
        list.splice(current + 1);
        list.push(target);
        index = list.length - 1;
      } else if (o.type === 'replace') {
        target = makeEntry(o.url, o.navState !== null && o.navState !== undefined ? o.navState : old.navState, null, old);
        target.docId = uuid();
        target.key = old.key;
        list[current] = target;
      } else if (o.type === 'reload') {
        if (o.navState !== undefined) old.navState = o.navState;
      } else {
        index = entries.indexOf(o.entry);
      }
      h = { url: o.url, type: o.type, current: index, from: initialBlank() ? null : packEntry(old), entries: list.map(packEntry) };
      if (!initialBlank()) {
        unloading = true;
        fire(g, new Event('beforeunload', { cancelable: true }));
      }
      Object.defineProperty(frame, '__zedNav', { value: h, writable: true, configurable: true, enumerable: false });
      if (native.navigateFrame(o.url)) return true;
      unloading = false;
      delete frame.__zedNav;
      return false;
    }
    // The document that was loaded after another of its frame: the entries are the ones that were handed over.
    function unpackEntry(r) {
      return { url: r.url, key: r.key, id: r.id, navState: r.navState ? { v: structuredClone(r.navState.v) } : null,
        classic: r.classic ? { v: structuredClone(r.classic.v) } : null, scroll: null, scrollMode: r.scrollMode, nhe: null, docId: r.docId };
    }
    function adoptHandover() {
      var h = handover, i, fromEntry = null;
      entries = h.entries.map(unpackEntry);
      current = h.current;
      docId = entries[current].docId;
      entries[current].url = null;
      classicState = entries[current].classic ? deserialize(entries[current].classic) : null;
      for (i = 0; i < entries.length; i++) if (entries[i].key === h.from.key && entries[i].id === h.from.id) fromEntry = entries[i];
      if (!fromEntry && h.from) fromEntry = unpackEntry(h.from);
      return new NavigationActivation(KEY, fromEntry ? nhe(fromEntry) : null, nhe(entries[current]), h.type);
    }

    // Fires the navigate event of a navigation; o has the navigation's type, url, destination entry (a traversal),
    // navigation API state, classic history state (pushState), sameDocument, hashChange, the source element and
    // commit(), which carries out a navigation of this document that no listener intercepted.
    function fireNavigate(o) {
      var t, controller, event, n, dispatched, intercepted, from, promises, i;
      if (initialBlank() && !o.sameDocument) {
        // The initial about:blank document is replaced by what it navigates to: no navigate event, and nothing settles.
        upcomingTracker = null;
        if (o.type !== 'reload' && o.type !== 'traverse') leaveForDocument(o);
        return;
      }
      if (ongoingEvent) abortOngoing();
      if (o.type === 'traverse') {
        t = upcomingTraverse.get(o.entry.key) || null;
        if (t) upcomingTraverse.delete(o.entry.key);
      } else {
        t = upcomingTracker;
        upcomingTracker = null;
      }
      ongoingTracker = t;
      controller = new AbortController();
      event = new NavigateEvent('navigate', {
        cancelable: o.type !== 'traverse' || o.sameDocument,
        navigationType: o.type,
        destination: new NavigationDestination(KEY, { url: o.url, entry: o.entry || null, index: o.entry ? entries.indexOf(o.entry) : -1,
          navState: o.navState, sameDocument: o.sameDocument }),
        canIntercept: canRewrite(o.url) && (o.type !== 'traverse' || o.sameDocument),
        userInitiated: false,
        hashChange: !!o.hashChange,
        signal: controller.signal,
        formData: o.formData || null,
        downloadRequest: o.downloadRequest === undefined ? null : o.downloadRequest,
        info: t ? t.info : undefined,
        sourceElement: o.sourceElement || null
      });
      n = { state: 'none', handlers: [], focusReset: null, scroll: null, controller: controller, dispatching: true, canceled: false, type: o.type };
      event._n = n;
      ongoingEvent = event;
      focusChanged = false;
      suppressScroll = false;
      try { dispatched = fire(navigation, event); } finally { n.dispatching = false; }
      if (!active()) {
        if (!controller.signal.aborted) abortOngoing();
        return;
      }
      if (!dispatched || n.canceled) {
        if (!controller.signal.aborted) abortOngoing();
        return;
      }
      intercepted = n.state !== 'none';
      if (!intercepted && !o.sameDocument) {
        // Another document: a frame loads it, and the document that comes carries on the entries; a window
        // that is not a frame does not load it here.
        if (t) cleanUp(t);
        leaveForDocument(o);
        return;
      }
      if (intercepted) {
        from = nhe(entries[current]);
        transition = new NavigationTransition(KEY, o.type, from);
        if (o.type === 'traverse') suppressScroll = true;
        if (o.type === 'push' || o.type === 'replace') updateURLAndHistory(o.url, o.classic || serialize(null), o.type);
        else if (o.type === 'reload') entriesChanged(from, 'reload', []);
        else o.commit();
        n.state = 'committed';
      } else {
        o.commit();
      }
      promises = [];
      for (i = 0; i < n.handlers.length; i++) {
        try { promises.push(Promise.resolve(n.handlers[i]())); } catch (e) { promises.push(Promise.reject(e)); }
      }
      if (!promises.length) promises.push(Promise.resolve());
      waitForAll(promises, function () {
        if (controller.signal.aborted) return;
        if (ongoingEvent === event) ongoingEvent = null;
        finish(event, true);
        fire(navigation, new Event('navigatesuccess'));
        if (t) resolveFinished(t);
        if (transition) { transition._f.resolve(); transition = null; }
      }, function (reason) {
        if (controller.signal.aborted) return;
        if (ongoingEvent === event) ongoingEvent = null;
        controller.abort(reason);
        finish(event, false);
        fireError(reason);
        if (t) rejectFinished(t, reason);
        if (transition) { transition._f.reject(reason); transition = null; }
      });
    }

    // The navigate algorithm for a URL: a fragment of this document is navigated to here; another document only
    // gets its navigate event.
    function navigateTo(url, o) {
      var handling = o.history, now = docURL(), f;
      if (/^javascript:/i.test(url)) return;
      // A page that is not a local file cannot navigate to one.
      if (/^file:/i.test(url) && !/^file:/i.test(now)) return;
      if (handling === 'auto') handling = url === now ? 'replace' : 'push';
      if (handling === 'reload') {
        fireNavigate({ type: 'reload', url: url, navState: o.navState, sameDocument: false, sourceElement: o.sourceElement });
        return;
      }
      f = frag(url);
      if (f !== null && noFrag(url) === noFrag(now)) {
        fireNavigate({ type: handling, url: url, navState: o.navState, sameDocument: true, hashChange: f !== frag(now),
          sourceElement: o.sourceElement, commit: function () { navigateToFragment(url, handling, o.navState); } });
        return;
      }
      fireNavigate({ type: handling, url: url, navState: o.navState, sameDocument: false, sourceElement: o.sourceElement,
        formData: o.formData, downloadRequest: o.downloadRequest });
    }
    // Location's navigation: before the load event finishes, a new entry replaces the current one.
    function locationNavigate(url, handling) {
      if (handling === 'auto' && !native.loadEnded()) handling = 'replace';
      navigateTo(url, { history: handling, navState: null });
    }
    function locationReload() {
      navigateTo(docURL(), { history: 'reload', navState: entries[current].navState });
    }

    // A traversal runs as a task, like the browser's own back and forward.
    function traverse(find) {
      setTimeout(function () {
        var entry = find(), t, same;
        if (!entry) return;
        t = upcomingTraverse.get(entry.key) || null;
        if (entry === entries[current]) { if (t) resolveFinished(t); return; }
        same = entry.docId === docId;
        fireNavigate({ type: 'traverse', url: entry.url, entry: entry, navState: entry.navState, sameDocument: same,
          hashChange: same && noFrag(entry.url) === noFrag(docURL()) && frag(entry.url) !== frag(docURL()),
          commit: function () { commitTraverse(entry); } });
      }, 0);
    }
    function traverseToKey(key, options) {
      var entry = entries[current], t;
      if (entry.key === key) return { committed: Promise.resolve(nhe(entry)), finished: Promise.resolve(nhe(entry)) };
      t = upcomingTraverse.get(key);
      if (t) return trackerResult(t);
      t = makeTracker(options.info, null, key);
      upcomingTraverse.set(key, t);
      traverse(function () {
        var target = findEntry(key);
        if (!target && upcomingTraverse.get(key) === t) rejectFinished(t, domError('InvalidStateError', 'The entry to traverse to is no longer in the session history.'));
        return target;
      });
      return trackerResult(t);
    }

    class Navigation extends EventTarget {
      constructor(key) {
        if (key !== KEY) throw new TypeError('Illegal constructor');
        super();
      }
      entries() { return active() ? entries.map(nhe) : []; }
      get currentEntry() { return active() ? nhe(entries[current]) : null; }
      updateCurrentEntry(options) {
        var state;
        if (arguments.length < 1) throw new TypeError("Failed to execute 'updateCurrentEntry' on 'Navigation': 1 argument required.");
        options = dictionary(options, 'NavigationUpdateCurrentEntryOptions');
        if (!active()) throw domError('InvalidStateError', "Failed to execute 'updateCurrentEntry' on 'Navigation': The document is not fully active.");
        if (initialBlank()) throw domError('InvalidStateError', "Failed to execute 'updateCurrentEntry' on 'Navigation': The document is still on the initial about:blank.");
        if (options.state === undefined) throw new TypeError("Failed to execute 'updateCurrentEntry' on 'Navigation': required member state is undefined.");
        state = serialize(options.state);
        entries[current].navState = state;
        fire(this, new NavigationCurrentEntryChangeEvent('currententrychange', { navigationType: null, from: nhe(entries[current]) }));
      }
      get transition() { return transition; }
      get activation() { return activation; }
      get canGoBack() { return active() && current > 0; }
      get canGoForward() { return active() && current < entries.length - 1; }
      navigate(url, options) {
        var handling, u, state = null, t;
        options = dictionary(options, 'NavigationNavigateOptions');
        handling = options.history === undefined ? 'auto' : enumValue(options.history, ['auto', 'push', 'replace'], 'NavigationHistoryBehavior');
        u = resolveURL(url);
        if (u === null) return earlyError(domError('SyntaxError', "Failed to execute 'navigate' on 'Navigation': Invalid URL '" + String(url) + "'."));
        if (/^javascript:/i.test(u)) return earlyError(domError('NotSupportedError', "Failed to execute 'navigate' on 'Navigation': A javascript: URL cannot be navigated to with navigate()."));
        if (options.state !== undefined) {
          try { state = serialize(options.state); } catch (e) { return earlyError(e); }
        }
        if (!active()) return earlyError(domError('InvalidStateError', "Failed to execute 'navigate' on 'Navigation': The document is not fully active."));
        t = makeTracker(options.info, state, null);
        upcomingTracker = t;
        if (handling === 'auto' && !native.loadEnded()) handling = 'replace';
        navigateTo(u, { history: handling, navState: state });
        if (upcomingTracker === t) { upcomingTracker = null; return earlyError(abortError()); }
        return trackerResult(t);
      }
      reload(options) {
        var state, t;
        options = dictionary(options, 'NavigationReloadOptions');
        state = entries[current].navState;
        if (options.state !== undefined) {
          try { state = serialize(options.state); } catch (e) { return earlyError(e); }
        }
        if (!active()) return earlyError(domError('InvalidStateError', "Failed to execute 'reload' on 'Navigation': The document is not fully active."));
        t = makeTracker(options.info, state, null);
        upcomingTracker = t;
        navigateTo(docURL(), { history: 'reload', navState: state });
        if (upcomingTracker === t) { upcomingTracker = null; return earlyError(abortError()); }
        return trackerResult(t);
      }
      traverseTo(key, options) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'traverseTo' on 'Navigation': 1 argument required.");
        key = String(key);
        options = dictionary(options, 'NavigationOptions');
        if (!active()) return earlyError(domError('InvalidStateError', "Failed to execute 'traverseTo' on 'Navigation': The document is not fully active."));
        if (!findEntry(key)) return earlyError(domError('InvalidStateError', "Failed to execute 'traverseTo' on 'Navigation': Invalid key."));
        return traverseToKey(key, options);
      }
      back(options) {
        options = dictionary(options, 'NavigationOptions');
        if (!active()) return earlyError(domError('InvalidStateError', "Failed to execute 'back' on 'Navigation': The document is not fully active."));
        if (current <= 0) return earlyError(domError('InvalidStateError', "Failed to execute 'back' on 'Navigation': Cannot go back."));
        return traverseToKey(entries[current - 1].key, options);
      }
      forward(options) {
        options = dictionary(options, 'NavigationOptions');
        if (!active()) return earlyError(domError('InvalidStateError', "Failed to execute 'forward' on 'Navigation': The document is not fully active."));
        if (current >= entries.length - 1) return earlyError(domError('InvalidStateError', "Failed to execute 'forward' on 'Navigation': Cannot go forward."));
        return traverseToKey(entries[current + 1].key, options);
      }
    }
    ['navigate', 'navigatesuccess', 'navigateerror', 'currententrychange'].forEach(function (type) { handlerAttribute(Navigation.prototype, type); });
    tag(Navigation, 'Navigation');

    class History {
      constructor(key) { if (key !== KEY) throw new TypeError('Illegal constructor'); }
      get length() { return entries.length; }
      get state() { return classicState; }
      get scrollRestoration() { return entries[current].scrollMode; }
      set scrollRestoration(v) { v = String(v); if (v === 'auto' || v === 'manual') entries[current].scrollMode = v; }
      go(delta) {
        delta = delta === undefined ? 0 : Number(delta) | 0;
        if (delta === 0) { locationReload(); return; }
        traverse(function () { return entries[current + delta] || null; });
      }
      back() { this.go(-1); }
      forward() { this.go(1); }
      pushState(data, unused, url) {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'pushState' on 'History': 2 arguments required.");
        pushOrReplace(data, url, 'push');
      }
      replaceState(data, unused, url) {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'replaceState' on 'History': 2 arguments required.");
        pushOrReplace(data, url, 'replace');
      }
    }
    tag(History, 'History');
    function pushOrReplace(data, url, handling) {
      var classic = serialize(data), target = docURL();
      if (url !== undefined && url !== null) {
        target = resolveURL(url);
        if (target === null) throw domError('SyntaxError', "Failed to execute '" + handling + "State' on 'History': Invalid URL '" + String(url) + "'.");
        if (!canRewrite(target)) {
          throw domError('SecurityError', "Failed to execute '" + handling + "State' on 'History': A history state object with URL '" + target +
            "' cannot be created in a document with URL '" + docURL() + "'.");
        }
      }
      fireNavigate({ type: handling, url: target, navState: null, classic: classic, sameDocument: true, hashChange: false,
        commit: function () { updateURLAndHistory(target, classic, handling); } });
    }

    // Location's setters and methods navigate (the getters read the document's URL).
    function locationSetter(name, apply) {
      Object.defineProperty(LocationProto, name, {
        get: Object.getOwnPropertyDescriptor(LocationProto, name).get,
        set: function (v) { apply(String(v)); },
        configurable: true, enumerable: true
      });
    }
    function invalid(u) { return domError('SyntaxError', "'" + u + "' is not a valid URL."); }
    locationSetter('href', function (v) {
      var u = resolveURL(v);
      if (u === null) throw invalid(v);
      locationNavigate(u, 'auto');
    });
    locationSetter('hash', function (v) {
      var u, now = docURL();
      if (v.charAt(0) === '#') v = v.slice(1);
      u = resolveURL(noFrag(now) + '#' + v);
      if (u === null || frag(u) === frag(now)) return;
      locationNavigate(u, 'auto');
    });
    ['search', 'pathname', 'host', 'hostname', 'port', 'protocol'].forEach(function (name) {
      locationSetter(name, function (v) {
        var u = new URL(docURL());
        u[name] = v;
        if (name === 'protocol' && u.protocol !== 'http:' && u.protocol !== 'https:') return;
        locationNavigate(u.href, 'auto');
      });
    });
    def(LocationProto, 'assign', function assign(url) {
      var u = resolveURL(url);
      if (u === null) throw invalid(url);
      locationNavigate(u, 'auto');
    });
    def(LocationProto, 'replace', function replace(url) {
      var u = resolveURL(url);
      if (u === null) throw invalid(url);
      locationNavigate(u, 'replace');
    });
    def(LocationProto, 'reload', function reload() { locationReload(); });
    (function () {
      var loc = g.location, d = Object.getOwnPropertyDescriptor(g, 'location'), proto, dd;
      if (d && d.configurable) {
        Object.defineProperty(g, 'location', { get: function () { return loc; }, set: function (v) { loc.href = v; }, configurable: true, enumerable: true });
      }
      for (proto = document; proto && !Object.prototype.hasOwnProperty.call(proto, 'location'); proto = Object.getPrototypeOf(proto));
      dd = proto ? Object.getOwnPropertyDescriptor(proto, 'location') : null;
      if (dd && dd.get && dd.configurable) {
        Object.defineProperty(proto, 'location', { get: dd.get, set: function (v) { var l = dd.get.call(this); if (l) l.href = v; },
          configurable: true, enumerable: dd.enumerable });
      }
    })();

    // A link a script clicks (an a or area element, or SVG's a) navigates; a download link only fires its navigate event.
    native.setHyperlinkFollower(function (link) {
      var svg = link.namespaceURI === 'http://www.w3.org/2000/svg', href, target, u, download;
      href = link.getAttribute('href');
      if (href === null && svg) href = link.getAttributeNS('http://www.w3.org/1999/xlink', 'href');
      if (href === null || !link.isConnected) return;
      target = link.getAttribute('target');
      if (target !== null && !/^\s*(_self)?\s*$/i.test(target) && !(/^\s*_(top|parent)\s*$/i.test(target) && g.top === g)) return;
      u = resolveURL(href);
      if (u === null) return;
      download = !svg && link.hasAttribute('download') ? link.getAttribute('download') : null;
      if (download !== null) {
        fireNavigate({ type: 'push', url: u, navState: null, sameDocument: false, sourceElement: link, downloadRequest: download });
        return;
      }
      navigateTo(u, { history: 'auto', navState: null, sourceElement: link });
    });

    // A form's submission navigates to its action: GET with the entries as the query, POST with them as the
    // navigate event's formData (before the load event finishes, in place of the current entry), as a task.  requestSubmit() fires the submit event first, which a listener may cancel.
    function submitForm(form, submitter) {
      function attr(name, own) {
        return submitter && submitter.hasAttribute(name) ? submitter.getAttribute(name) : form.getAttribute(own);
      }
      var action = attr('formaction', 'action'), method = String(attr('formmethod', 'method') || 'get').toLowerCase();
      var target = attr('formtarget', 'target'), u, data, query, source, handling;
      if (!form.isConnected || method === 'dialog') return;
      if (target !== null && !/^\s*(_self)?\s*$/i.test(target) && !(/^\s*_(top|parent)\s*$/i.test(target) && g.top === g)) return;
      u = resolveURL(action === null || action === '' ? docURL() : action);
      if (u === null) return;
      data = typeof g.FormData === 'function' ? new g.FormData(form) : null;
      if (data && submitter && submitter.name) data.append(submitter.name, submitter.value);
      source = submitter || form;
      handling = native.loadEnded() ? 'auto' : 'replace';
      if (method !== 'post') {
        query = data ? new URLSearchParams(data).toString() : '';
        u = noFrag(u).replace(/\?.*$/, '') + '?' + query;
        data = null;
      }
      setTimeout(function () { navigateTo(u, { history: handling, navState: null, sourceElement: source, formData: data }); }, 0);
    }
    if (g.HTMLFormElement && !g.HTMLFormElement.prototype.submit) {
      def(g.HTMLFormElement.prototype, 'submit', function submit() { submitForm(this, null); });
      def(g.HTMLFormElement.prototype, 'requestSubmit', function requestSubmit(submitter) {
        var event;
        if (submitter !== undefined && submitter !== null) {
          if (!/^(submit|image)$/.test(submitter.type) || !/^(button|input)$/.test(submitter.localName))
            throw new TypeError("Failed to execute 'requestSubmit' on 'HTMLFormElement': The specified element is not a submit button.");
          if (submitter.form !== this)
            throw new DOMException("Failed to execute 'requestSubmit' on 'HTMLFormElement': The specified element is not owned by this form element.", 'NotFoundError');
        } else {
          submitter = null;
        }
        event = new (g.SubmitEvent || Event)('submit', { bubbles: true, cancelable: true, submitter: submitter });
        if (fire(this, event)) submitForm(this, submitter);
      });
    }

    // window.stop() aborts the navigation under way (loading the document is not stopped).
    def(g, 'stop', function stop() { if (ongoingEvent) abortOngoing(); });

    // A frame's earlier document leaves its entries on the frame element for the one that replaces it.
    (function () {
      var frame = null, h;
      frame = native.frameElement();
      if (!frame) return;
      h = frame.__zedNav;
      if (h) { delete frame.__zedNav; handover = h; }
    })();
    var navigation = new Navigation(KEY);
    var activation;
    if (handover) {
      activation = adoptHandover();
    } else {
      entries.push(makeEntry(null, null, null, null));
      activation = initialBlank() ? null : new NavigationActivation(KEY, null, nhe(entries[0]), 'push');
    }
    def(g, 'navigation', navigation);
    def(g, 'history', new History(KEY));
    [['Navigation', Navigation], ['NavigationHistoryEntry', NavigationHistoryEntry], ['NavigationDestination', NavigationDestination],
      ['NavigationTransition', NavigationTransition], ['NavigationActivation', NavigationActivation], ['NavigateEvent', NavigateEvent],
      ['NavigationCurrentEntryChangeEvent', NavigationCurrentEntryChangeEvent], ['History', History]].forEach(function (p) {
      def(g, p[0], p[1]);
    });
  })();
  // ---- Small objects scripts probe for
  if (!('isSecureContext' in g)) def(g, 'isSecureContext', location.protocol === 'https:');
  if (!g.open) def(g, 'open', function open() { return null; });
  if (!g.close) def(g, 'close', function close() {});
  if (!g.focus) { def(g, 'focus', function focus() {}); def(g, 'blur', function blur() {}); }
  // Node.baseURI: the document's first <base href> resolved against its URL, or the URL.
  if (g.Node && !('baseURI' in g.Node.prototype)) {
    Object.defineProperty(g.Node.prototype, 'baseURI', {
      get: function () {
        var d = this.ownerDocument || this, url = d.URL || location.href;
        var base = d.querySelector ? d.querySelector('base[href]') : null;
        try { return base ? new URL(base.getAttribute('href'), url).href : url; } catch (e) { return url; }
      },
      configurable: true, enumerable: true
    });
  }
  if (!g.getSelection) {
    def(g, 'getSelection', function getSelection() {
      return { rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null,
        toString: function () { return ''; }, removeAllRanges: function () {}, addRange: function () {}, getRangeAt: function () { return null; } };
    });
  }
  if (!g.PerformanceObserver) {
    class PerformanceObserver {
      constructor(f) { hidden(this, '_f', f); }
      observe() {}
      disconnect() {}
      takeRecords() { return []; }
    }
    PerformanceObserver.supportedEntryTypes = [];
    def(g, 'PerformanceObserver', PerformanceObserver);
  }
  // ---- Largest Contentful Paint: after each frame that follows a change, the largest text or image painted so far
  // (its visible area, an image's scaled down to its natural one) becomes an entry for the observers of the type.
  (function () {
    var Native = g.PerformanceObserver, Entry = g.PerformanceEntry;
    if (typeof Native !== 'function' || g.LargestContentfulPaint) return;
    var TYPE = 'largest-contentful-paint';
    var buffer = [], observers = new Set(), largest = 0, active = false, scheduled = false;
    var reported = new WeakMap(), loads = new WeakMap(), pictures = new Map();
    class LargestContentfulPaint {
      constructor(key) { if (key !== KEY) throw new TypeError('Illegal constructor'); }
      get name() { return ''; }
      get entryType() { return TYPE; }
      get startTime() { return this._t; }
      get duration() { return 0; }
      get renderTime() { return this._t; }
      get paintTime() { return this._t; }
      get loadTime() { return this._l; }
      get size() { return this._s; }
      get id() { return this._i; }
      get url() { return this._u; }
      get element() { var e = this._e; return e && e.isConnected && e.ownerDocument === document ? e : null; }
      toJSON() {
        return { name: this.name, entryType: this.entryType, startTime: this.startTime, duration: this.duration,
          renderTime: this.renderTime, loadTime: this.loadTime, size: this.size, id: this.id, url: this.url };
      }
    }
    if (Entry) Object.setPrototypeOf(LargestContentfulPaint.prototype, Entry.prototype);
    tag(LargestContentfulPaint, 'LargestContentfulPaint');
    function list(entries) {
      var l = Object.create(g.PerformanceObserverEntryList ? g.PerformanceObserverEntryList.prototype : Object.prototype);
      hidden(l, 'getEntries', function getEntries() { return entries.slice(); });
      hidden(l, 'getEntriesByType', function getEntriesByType(t) { return entries.filter(function (e) { return e.entryType === String(t); }); });
      hidden(l, 'getEntriesByName', function getEntriesByName(n, t) {
        return entries.filter(function (e) { return e.name === String(n) && (t === undefined || e.entryType === String(t)); });
      });
      return l;
    }
    function deliver(o) {
      if (o._lcpQueued) return;
      o._lcpQueued = true;
      setTimeout(function () {
        o._lcpQueued = false;
        var entries = o._lcpPending;
        o._lcpPending = [];
        if (!entries.length || !observers.has(o)) return;
        o._lcpCallback.call(o, list(entries), o);
      }, 0);
    }
    // The rectangle a node paints in the viewport, clipped to it, or null.
    function visible(node) { return clip(native.nodeRect(node)); }
    function shown(element) {
      for (var e = element; e && e.nodeType === 1; e = e.parentElement) {
        var s = getComputedStyle(e);
        if (s.display === 'none' || Number(s.opacity) === 0) return false;
        if (e === element && s.visibility !== 'visible') return false;
      }
      return true;
    }
    function transparent(s) {
      var c = s.color.replace(/\s/g, '');
      if (c !== 'transparent' && !/^rgba\(\d+,\d+,\d+,0\)$/.test(c)) return false;
      if (s.textShadow && s.textShadow !== 'none') return false;
      if (s.webkitTextStrokeWidth && parseFloat(s.webkitTextStrokeWidth) > 0) return false;
      return true;
    }
    // The element a text node's paint counts for: its nearest ancestor that is not inline (a MathML element is its own).
    function holder(element) {
      if (element.namespaceURI === 'http://www.w3.org/1998/Math/MathML') return element;
      for (var e = element; e; e = e.parentElement) {
        var d = getComputedStyle(e).display;
        if (d !== 'inline' && d !== 'contents') return e;
      }
      return element;
    }
    function picture(url) {
      var p = pictures.get(url);
      if (!p) {
        p = new Image();
        p.onload = p.onerror = schedule;
        p.src = url;
        pictures.set(url, p);
      }
      return p.complete && p.naturalWidth > 0 ? p : null;
    }
    function consider(candidates, element, url, area) {
      if (area <= 0) return;
      var key = url || '#text', done = reported.get(element);
      if (done && done.indexOf(key) >= 0) return;
      candidates.push({ element: element, url: url, size: Math.floor(area), key: key });
    }
    function area(rect) { return rect ? (rect[2] - rect[0]) * (rect[3] - rect[1]) : 0; }
    function scan() {
      scheduled = false;
      if (!document.body) return schedule();
      var candidates = [], texts = new Map(), walker = document.createTreeWalker(document.body, 5), node;
      while ((node = walker.nextNode())) {
        if (node.nodeType === 3) {
          if (!/\S/.test(node.data) || !node.parentElement) continue;
          var parent = node.parentElement, h = holder(parent);
          if (!shown(parent) || transparent(getComputedStyle(parent))) continue;
          var r = visible(node);
          if (!r) continue;
          var u = texts.get(h);
          texts.set(h, u ? [Math.min(u[0], r[0]), Math.min(u[1], r[1]), Math.max(u[2], r[2]), Math.max(u[3], r[3])] : r);
          continue;
        }
        var name = node.localName, element = node;
        if (name === 'script' || name === 'style' || name === 'iframe') continue;
        var s = getComputedStyle(element);
        if (s.display === 'none') continue;
        if (name === 'img' && element.complete && element.naturalWidth > 0 && element.namespaceURI === 'http://www.w3.org/1999/xhtml') {
          if (shown(element)) image(candidates, element, element.currentSrc || element.src, element);
        } else if (name === 'video' && element.getAttribute('poster')) {
          var poster = picture(new URL(element.getAttribute('poster'), (document.baseURI || location.href)).href);
          if (poster && shown(element)) image(candidates, element, poster.src, poster);
        } else if (name === 'image' && element.namespaceURI === 'http://www.w3.org/2000/svg') {
          var href = element.getAttribute('href') || element.getAttributeNS('http://www.w3.org/1999/xlink', 'href');
          var p = href && picture(new URL(href, (document.baseURI || location.href)).href);
          if (p && shown(element)) image(candidates, element, p.src, p);
        }
        var bgs = s.backgroundImage || '', bg = bgs.indexOf('url(', bgs.indexOf('url(') + 1) < 0 ? /^\s*url\(\s*(["']?)([\s\S]*)\1\s*\)\s*$/.exec(bgs) : /url\(\s*["']?([^"')]+)["']?\s*\)/.exec(bgs);
        if (bg && bg.length === 3) bg = [bg[0], bg[2]];
        if (bg && element !== document.body && element !== document.documentElement) {
          var b = picture(new URL(bg[1], (document.baseURI || location.href)).href);
          if (b && shown(element)) image(candidates, element, b.src, b);
        }
        var before = getComputedStyle(element, '::before').content;
        if (before && before !== 'none' && before !== 'normal' && /^["']|attr\(/.test(before) && shown(element)) {
          var rb = visible(element) || visible(holder(element));
          if (rb) {
            // The generated text's own width (about 0.55 em a character), not its block's.
            var made = /^["']/.test(before) ? before.slice(1, -1) : (element.getAttribute(before.slice(5, -1)) || ''),
              em = parseFloat(getComputedStyle(element, '::before').fontSize) || 16;
            rb = [rb[0], rb[1], Math.min(rb[2], rb[0] + made.length * em * 0.55), rb[3]];
          }
          if (rb && !texts.has(holder(element))) texts.set(holder(element), rb);
        }
      }
      texts.forEach(function (rect, element) { consider(candidates, element, '', area(rect)); });
      var now = performance.now(), best = null;
      candidates.forEach(function (c) {
        var done = reported.get(c.element) || [];
        done.push(c.key);
        reported.set(c.element, done);
        if (c.size > largest && (!best || c.size > best.size)) best = c;
      });
      if (best) {
        largest = best.size;
        var e = new LargestContentfulPaint(KEY);
        hidden(e, '_t', now); hidden(e, '_s', best.size); hidden(e, '_i', best.element.id || '');
        hidden(e, '_u', best.url); hidden(e, '_e', best.element);
        hidden(e, '_l', best.url ? Math.min(loads.get(best.element) || now, now) : 0);
        if (buffer.length < 150) buffer.push(e);
        observers.forEach(function (o) { o._lcpPending.push(e); deliver(o); });
      }
    }
    // An SVG <image> has no box of its own: its place is its x, y, width and height (its natural size by default)
    // from the top left of its <svg>, clipped to the <svg>.
    function svgRect(element, source) {
      var svg = element.ownerSVGElement || element.closest('svg'), box = svg && native.nodeRect(svg);
      if (!box) return null;
      function length(name, fallback) { var v = parseFloat(element.getAttribute(name)); return isFinite(v) ? v : fallback; }
      var x = box[0] + length('x', 0), y = box[1] + length('y', 0);
      var w = length('width', source.naturalWidth), h = length('height', source.naturalHeight);
      var x1 = Math.min(x + w, box[0] + box[2]), y1 = Math.min(y + h, box[1] + box[3]);
      x = Math.max(x, box[0]); y = Math.max(y, box[1]);
      return x1 > x && y1 > y ? [x, y, x1 - x, y1 - y] : null;
    }
    function clip(r) {
      if (!r) return null;
      var x0 = Math.max(r[0], 0), y0 = Math.max(r[1], 0);
      var x1 = Math.min(r[0] + r[2], innerWidth), y1 = Math.min(r[1] + r[3], innerHeight);
      return x1 > x0 && y1 > y0 ? [x0, y0, x1, y1] : null;
    }
    function image(candidates, element, url, source) {
      var r = element.namespaceURI === 'http://www.w3.org/2000/svg' ? svgRect(element, source) : native.nodeRect(element);
      var v = clip(r);
      if (!r || !v) return;
      // An image that covers the whole viewport is a background, not content.
      if (r[0] <= 0 && r[1] <= 0 && r[0] + r[2] >= innerWidth && r[1] + r[3] >= innerHeight) return;
      var shownArea = r[2] * r[3], natural = source.naturalWidth * source.naturalHeight, a = area(v);
      if (shownArea > natural && shownArea > 0) a = a * natural / shownArea;
      if (!loads.has(element)) loads.set(element, performance.now());
      var key = url, done = reported.get(element);
      if (done && done.indexOf(key) >= 0) return;
      candidates.push({ element: element, url: url, size: Math.floor(a + 1e-6), key: key });
    }
    function schedule() {
      if (scheduled || !active) return;
      scheduled = true;
      requestAnimationFrame(function () { setTimeout(scan, 0); });
    }
    function start() {
      if (active) return;
      active = true;
      new MutationObserver(schedule).observe(document, { childList: true, subtree: true, attributes: true, characterData: true });
      document.addEventListener('load', function (event) {
        if (event.target && event.target.nodeType === 1) loads.set(event.target, performance.now());
        schedule();
      }, true);
      schedule();
    }
    function wanted(options) {
      if (!options) return false;
      if (options.type !== undefined) return String(options.type) === TYPE;
      return Array.isArray(options.entryTypes) && options.entryTypes.indexOf(TYPE) >= 0;
    }
    class PerformanceObserver extends Native {
      constructor(callback) {
        super(callback);
        hidden(this, '_lcpCallback', callback);
        hidden(this, '_lcpPending', []);
        hidden(this, '_lcpQueued', false);
      }
      observe(options) {
        if (!wanted(options)) return super.observe(options);
        if (options.type === undefined) {
          var rest = options.entryTypes.filter(function (t) { return t !== TYPE; });
          if (rest.length) super.observe(Object.assign({}, options, { entryTypes: rest }));
        }
        var fresh = !observers.has(this);
        observers.add(this);
        if (fresh && options.buffered && buffer.length) {
          this._lcpPending.push.apply(this._lcpPending, buffer);
          deliver(this);
        }
        start();
      }
      disconnect() { observers.delete(this); this._lcpPending = []; return super.disconnect(); }
      takeRecords() { var mine = this._lcpPending; this._lcpPending = []; return super.takeRecords().concat(mine); }
      static get supportedEntryTypes() { return Object.freeze(Native.supportedEntryTypes.concat([TYPE]).sort()); }
    }
    def(g, 'PerformanceObserver', PerformanceObserver);
    def(g, 'LargestContentfulPaint', LargestContentfulPaint);
  })();
  if (!g.customElements) {
    var registry = new Map();
    def(g, 'customElements', {
      define: function (name, ctor) { registry.set(name, ctor); },
      get: function (name) { return registry.get(name); },
      whenDefined: function (name) { return Promise.resolve(registry.get(name)); },
      upgrade: function () {}
    });
  }
  [['ErrorEvent', 'error'], ['ProgressEvent', 'progress'], ['HashChangeEvent', 'hashchange'], ['PopStateEvent', 'popstate'],
    ['PointerEvent', 'pointer'], ['InputEvent', 'input'], ['TouchEvent', 'touch'], ['StorageEvent', 'storage']].forEach(function (p) {
    if (g[p[0]]) return;
    var Base = p[0] === 'PointerEvent' && g.MouseEvent ? g.MouseEvent : Event;
    var C = class extends Base {
      constructor(type, init) {
        super(type, init);
        if (init) Object.keys(init).forEach(function (k) { if (!(k in this)) hidden(this, k, init[k]); }, this);
      }
    };
    Object.defineProperty(C, 'name', { value: p[0] });
    def(g, p[0], C);
  });
  if (!g.DOMPointReadOnly) {
    class DOMPointReadOnly {
      constructor(x, y, z, w) {
        hidden(this, '_x', Number(x) || 0); hidden(this, '_y', Number(y) || 0);
        hidden(this, '_z', Number(z) || 0); hidden(this, '_w', w === undefined ? 1 : Number(w) || 0);
      }
      get x() { return this._x; }
      get y() { return this._y; }
      get z() { return this._z; }
      get w() { return this._w; }
      static fromPoint(p) { p = p || {}; return new this(p.x, p.y, p.z, p.w); }
      toJSON() { return { x: this.x, y: this.y, z: this.z, w: this.w }; }
    }
    class DOMPoint extends DOMPointReadOnly {
      get x() { return this._x; } set x(v) { this._x = Number(v) || 0; }
      get y() { return this._y; } set y(v) { this._y = Number(v) || 0; }
      get z() { return this._z; } set z(v) { this._z = Number(v) || 0; }
      get w() { return this._w; } set w(v) { this._w = Number(v) || 0; }
    }
    def(g, 'DOMPointReadOnly', DOMPointReadOnly);
    def(g, 'DOMPoint', DOMPoint);
  }
  // queueMicrotask is the global object's own property (a library that decides how to queue a microtask reads it there),
  // and PromiseRejectionEvent exists (core-js keeps the engine's Promise when it does; its own is the fallback).
  if (typeof g.queueMicrotask === 'function' && !Object.prototype.hasOwnProperty.call(g, 'queueMicrotask')) def(g, 'queueMicrotask', g.queueMicrotask);
  if (!g.PromiseRejectionEvent) {
    class PromiseRejectionEvent extends Event {
      constructor(type, init) {
        super(type, init);
        init = init || {};
        hidden(this, '_promise', init.promise); hidden(this, '_reason', init.reason);
      }
      get promise() { return this._promise; }
      get reason() { return this._reason; }
    }
    def(g, 'PromiseRejectionEvent', PromiseRejectionEvent);
  }
// @include plan/ws074/phase260/cssom-rules.js
  // The CSSOM rule interfaces scripts test with instanceof (a rule is told by what it has, as the engine's rules have no class of their own).
  [['CSSRule', function (x) { return !!x && typeof x === 'object' && typeof x.cssText === 'string'; }],
   ['CSSStyleRule', function (x) { return !!x && typeof x.selectorText === 'string'; }],
   ['CSSMediaRule', function (x) { return !!x && typeof x.media === 'object' && !!x.cssRules; }],
   ['CSSImportRule', function (x) { return !!x && typeof x.href === 'string' && !!x.styleSheet; }],
   ['CSSFontFaceRule', function (x) { return !!x && x.type === 5; }],
   ['CSSKeyframesRule', function (x) { return !!x && x.type === 7; }]].forEach(function (p) {
    if (g[p[0]]) return;
    var C = function () { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(C, 'name', { value: p[0] });
    Object.defineProperty(C, Symbol.hasInstance, { value: p[1] });
    def(g, p[0], C);
  });
  var HTMLElementPrototype = null;
  if (typeof document !== 'undefined') {
    // The interfaces are made when the first element is wrapped, so one is made here to find HTMLElement's prototype.
    HTMLElementPrototype = Object.getPrototypeOf(document.createElement('div'));
    while (HTMLElementPrototype && HTMLElementPrototype.constructor && HTMLElementPrototype.constructor.name !== 'HTMLElement')
      HTMLElementPrototype = Object.getPrototypeOf(HTMLElementPrototype);
  }
  if (HTMLElementPrototype && HTMLElementPrototype.constructor && !HTMLElementPrototype.focus) {
    // Focus (ws074-p232): the element a script focuses is document.activeElement and gets focus and blur events.
    var focused = null;
    var focusable = /^(a|area|button|input|select|textarea|iframe|summary)$/i;
    var fire = function (el, type, related) {
      var ev;
      try { ev = new FocusEvent(type, { relatedTarget: related, bubbles: false }); } catch (e) { ev = new Event(type); }
      el.dispatchEvent(ev);
      try { el.dispatchEvent(new FocusEvent(type === 'focus' ? 'focusin' : 'focusout', { relatedTarget: related, bubbles: true })); } catch (e) { /* no FocusEvent */ }
    };
    var canFocus = function (el) {
      return focusable.test(el.tagName) || el.hasAttribute('tabindex') || el.isContentEditable === true;
    };
    def(HTMLElementPrototype, 'focus', function focus() {
      var old = focused;
      if (!this.isConnected || !canFocus(this) || old === this) return;
      focused = this;
      if (old) fire(old, 'blur', this);
      fire(this, 'focus', old);
    });
    def(HTMLElementPrototype, 'blur', function blur() {
      var old = focused;
      if (old !== this) return;
      focused = null;
      fire(old, 'blur', null);
    });
    if (!('activeElement' in document)) {
      Object.defineProperty(document, 'activeElement', { get: function () { return focused && focused.isConnected ? focused : document.body; }, configurable: true });
    }
    def(document, 'hasFocus', function hasFocus() { return true; });
  } else if (typeof document !== 'undefined' && !('activeElement' in document)) {
    Object.defineProperty(document, 'activeElement', { get: function () { return document.body; }, configurable: true });
    def(document, 'hasFocus', function hasFocus() { return true; });
  }
  // Interfaces with an indexed getter and a length iterate as arrays do; NodeList and DOMTokenList are also iterable<>,
  // with the array's entries, keys, values and forEach.
  (function () {
    var AP = Array.prototype;
    ['NodeList', 'HTMLCollection', 'DOMTokenList', 'HTMLAllCollection', 'HTMLFormControlsCollection', 'HTMLOptionsCollection',
      'DOMStringList', 'FileList', 'StyleSheetList', 'CSSRuleList', 'MediaList', 'DOMRectList', 'CSSStyleDeclaration',
      'NamedNodeMap', 'TouchList', 'SVGNumberList', 'SVGLengthList', 'SVGStringList', 'SVGTransformList'].forEach(function (name) {
      var C = g[name];
      if (typeof C !== 'function' || !C.prototype || Object.prototype.hasOwnProperty.call(C.prototype, Symbol.iterator)) return;
      def(C.prototype, Symbol.iterator, AP.values);
    });
    ['NodeList', 'DOMTokenList'].forEach(function (name) {
      var C = g[name];
      if (typeof C !== 'function' || !C.prototype) return;
      ['entries', 'keys', 'values', 'forEach'].forEach(function (m) {
        if (!Object.prototype.hasOwnProperty.call(C.prototype, m)) Object.defineProperty(C.prototype, m, { value: AP[m], writable: true, configurable: true, enumerable: true });
      });
    });
  })();
  // ---- View Transitions (https://drafts.csswg.org/css-view-transitions-1/)
  // The script side: the states of a transition, its three promises and its types.  What the transition shows
  // (the captured images and the pseudo-element tree) is the engine's, told through native.viewTransition.
  if (typeof document !== 'undefined' && typeof g.Document === 'function' && !g.Document.prototype.startViewTransition) (function () {
    var then = Promise.prototype.then;
    var VT = {};
    var current = null;
    function deferred() {
      var d = {};
      d.promise = new Promise(function (resolve, reject) { d.resolve = resolve; d.reject = reject; });
      return d;
    }
    function markHandled(p) { then.call(p, undefined, function () {}); return p; }
    class ViewTransitionTypeSet extends Set {
      constructor(key) {
        if (key !== VT) throw new TypeError('Illegal constructor');
        super();
      }
      add(value) { var r = super.add(String(value)); typesChanged(this); return r; }
      delete(value) { var r = super.delete(String(value)); typesChanged(this); return r; }
      clear() { super.clear(); typesChanged(this); }
    }
    // The style engine matches :active-view-transition-type() against the active transition's types.
    function typesChanged(set) { if (current && current._types === set) native.viewTransitionActive(1, Array.from(set)); }
    tag(ViewTransitionTypeSet, 'ViewTransitionTypeSet');
    class ViewTransition {
      constructor(key) {
        if (key !== VT) throw new TypeError('Illegal constructor');
        hidden(this, '_ready', deferred());
        hidden(this, '_done', deferred());
        hidden(this, '_finished', deferred());
        hidden(this, '_phase', 'pending-capture');
        hidden(this, '_callback', null);
        hidden(this, '_types', new ViewTransitionTypeSet(VT));
        markHandled(this._ready.promise);
        markHandled(this._done.promise);
        markHandled(this._finished.promise);
      }
      get ready() { return this._ready.promise; }
      get finished() { return this._finished.promise; }
      get updateCallbackDone() { return this._done.promise; }
      get types() { return this._types; }
      skipTransition() { if (this._phase !== 'done') skip(this, new DOMException('Skipped view transition due to skipTransition() call.', 'AbortError')); }
    }
    tag(ViewTransition, 'ViewTransition');
    // Calls the update callback once; its outcome settles updateCallbackDone.
    function callUpdate(t) {
      var result, d = t._done;
      if (t._phase === 'pending-capture') t._phase = 'update-callback-called';
      if (t._updated) return;
      t._updated = true;
      try {
        result = t._callback ? t._callback.call(undefined) : undefined;
      } catch (e) {
        d.reject(e);
        return;
      }
      then.call(Promise.resolve(result), function () { d.resolve(undefined); t._updateDone = 1; }, function (e) { d.reject(e); t._updateDone = 2; });
    }
    function skip(t, reason) {
      var wasPending = t._phase === 'pending-capture';
      t._phase = 'done';
      if (current === t) {
        current = null;
        native.viewTransition(0);
        native.viewTransitionActive(0, []);
      }
      if (wasPending) setTimeout(function () { callUpdate(t); }, 0);
      t._ready.reject(reason);
      then.call(t._done.promise, function () { t._finished.resolve(undefined); }, function (e) { t._finished.reject(e); });
      if (!wasPending && !t._updated) callUpdate(t);
    }
    function finish(t) {
      if (t._phase === 'done') return;
      t._phase = 'done';
      if (current === t) {
        current = null;
        native.viewTransition(0);
        native.viewTransitionActive(0, []);
      }
      t._finished.resolve(undefined);
    }
    // After the update callback: the new state, the pseudo-element tree and the animations.
    function activate(t) {
      if (t._phase === 'done') return;
      t._phase = 'animating';
      if (!native.viewTransition(2)) {
        skip(t, new DOMException('Skipped view transition due to an invalid state.', 'InvalidStateError'));
        return;
      }
      t._ready.resolve(undefined);
      requestAnimationFrame(function () { setTimeout(function () { finish(t); }, Math.max(native.viewTransitionDuration(), typeof g.__vtAnimations === 'function' ? g.__vtAnimations() : 0)); });
    }
    g.Document.prototype.startViewTransition = function startViewTransition(callbackOptions) {
      var t = new ViewTransition(VT), cb = callbackOptions, i, list;
      if (typeof cb === 'function') {
        t._callback = cb;
      } else if (cb !== undefined && cb !== null) {
        if (typeof cb !== 'object') throw new TypeError("Failed to execute 'startViewTransition' on 'Document': The provided value is not of type 'StartViewTransitionOptions'.");
        if (cb.update !== undefined && cb.update !== null) {
          if (typeof cb.update !== 'function') throw new TypeError("Failed to execute 'startViewTransition' on 'Document': The provided callback is not a function.");
          t._callback = cb.update;
        }
        if (cb.types !== undefined && cb.types !== null) {
          list = Array.from(cb.types);
          for (i = 0; i < list.length; i++) t._types.add(String(list[i]));
        }
      }
      if (document.visibilityState === 'hidden') {
        skip(t, new DOMException('Skipped view transition due to document being hidden.', 'InvalidStateError'));
        return t;
      }
      if (current) skip(current, new DOMException('Old view transition aborted by new view transition.', 'AbortError'));
      current = t;
      native.viewTransitionActive(1, Array.from(t._types));
      // The next rendering opportunity: the old state is captured, then the callback runs and the new state is made.
      requestAnimationFrame(function () {
        if (t._phase === 'done') return;
        if (!native.viewTransition(1)) {
          skip(t, new DOMException('Skipped view transition due to an invalid state.', 'InvalidStateError'));
          return;
        }
        setTimeout(function () {
          if (t._phase === 'done') return;
          // The new state is made before the page's own frame callbacks (the update callback's) run: this one is asked for first.
          var fired = false, waiting = false;
          requestAnimationFrame(function () {
            fired = true;
            if (t._phase === 'done') return;
            if (t._updateDone === 1) activate(t); else waiting = true;
          });
          callUpdate(t);
          then.call(t._done.promise, function () { if (fired && waiting) requestAnimationFrame(function () { activate(t); }); }, function (e) { skip(t, e); });
        }, 0);
      });
      return t;
    };
    Object.defineProperty(g.Document.prototype, 'activeViewTransition', { get: function () { return current; }, configurable: true, enumerable: true });
    def(g, 'ViewTransition', ViewTransition);
    def(g, 'ViewTransitionTypeSet', ViewTransitionTypeSet);
  })();
  // ---- Element.animate (https://drafts.csswg.org/web-animations-1/)
  // The timeline of an animation: its promises, play state and time, without any effect on the page's style.
  if (typeof document !== 'undefined' && typeof g.Element === 'function' && !g.Element.prototype.animate) (function () {
    var ANIM = {};
    function deferred() {
      var d = {};
      d.promise = new Promise(function (resolve, reject) { d.resolve = resolve; d.reject = reject; });
      return d;
    }
    var all = new Set();
    class Animation extends EventTarget {
      constructor(key, target, keyframes, options) {
        if (key !== ANIM) throw new TypeError('Illegal constructor');
        super();
        var o = typeof options === 'number' ? { duration: options } : (options || {});
        hidden(this, '_target', target);
        hidden(this, '_keyframes', keyframes);
        hidden(this, '_timing', {
          delay: +o.delay || 0, endDelay: +o.endDelay || 0, fill: o.fill || 'auto', iterationStart: +o.iterationStart || 0,
          iterations: o.iterations === undefined ? 1 : +o.iterations, duration: o.duration === undefined ? 'auto' : o.duration,
          direction: o.direction || 'normal', easing: o.easing || 'linear'
        });
        hidden(this, '_state', 'running');
        hidden(this, '_ready', deferred());
        hidden(this, '_finished', deferred());
        hidden(this, '_timer', 0);
        hidden(this, '_start', performance.now());
        hidden(this, '_hold', null);
        this.id = o.id || '';
        this.onfinish = null;
        this.oncancel = null;
        this._ready.resolve(this);
        this._finished.promise.then(undefined, function () {});
        all.add(this);
        this._arm();
      }
      _total() {
        var t = this._timing, d = t.duration === 'auto' ? 0 : +t.duration;
        return t.delay + d * t.iterations + t.endDelay;
      }
      _arm() {
        var self = this, left;
        if (this._timer) clearTimeout(this._timer);
        this._timer = 0;
        if (this._state !== 'running') return;
        left = this._total() - this.currentTime;
        if (!isFinite(left)) return;
        this._timer = setTimeout(function () { self._finish(); }, Math.max(0, left));
      }
      _finish() {
        if (this._state === 'finished' || this._state === 'idle') return;
        this._state = 'finished';
        this._hold = this._total();
        if (this._timer) clearTimeout(this._timer);
        this._timer = 0;
        this._finished.resolve(this);
        var ev = new Event('finish');
        if (typeof this.onfinish === 'function') { try { this.onfinish.call(this, ev); } catch (e) { setTimeout(function () { throw e; }, 0); } }
        this.dispatchEvent(ev);
      }
      get effect() { var a = this; return { target: a._target, getTiming: function () { return Object.assign({}, a._timing); }, getComputedTiming: function () { return Object.assign({ endTime: a._total(), activeDuration: a._total() - a._timing.delay - a._timing.endDelay, currentIteration: 0, progress: 0 }, a._timing); }, getKeyframes: function () { return a._keyframes; } }; }
      get playState() { return this._state; }
      get pending() { return false; }
      get replaceState() { return 'active'; }
      get ready() { return this._ready.promise; }
      get finished() { return this._finished.promise; }
      get startTime() { return this._state === 'idle' ? null : this._start; }
      get currentTime() {
        if (this._state === 'idle') return null;
        if (this._hold !== null) return this._hold;
        return performance.now() - this._start;
      }
      set currentTime(v) { this._start = performance.now() - v; if (this._hold !== null) this._hold = v; this._arm(); }
      get timeline() { return document.timeline || null; }
      play() { if (this._state === 'finished' || this._state === 'idle') { this._start = performance.now(); this._hold = null; this._finished = deferred(); this._finished.promise.then(undefined, function () {}); } else if (this._hold !== null) { this._start = performance.now() - this._hold; this._hold = null; } this._state = 'running'; this._arm(); }
      pause() { if (this._state === 'running') { this._hold = this.currentTime; this._state = 'paused'; if (this._timer) clearTimeout(this._timer); this._timer = 0; } }
      finish() { this._finish(); }
      cancel() {
        if (this._state === 'idle') return;
        this._state = 'idle';
        if (this._timer) clearTimeout(this._timer);
        this._timer = 0;
        this._finished.reject(new DOMException('The animation was aborted', 'AbortError'));
        this._finished = deferred();
        this._finished.promise.then(undefined, function () {});
        all.delete(this);
        var ev = new Event('cancel');
        if (typeof this.oncancel === 'function') { try { this.oncancel.call(this, ev); } catch (e) { setTimeout(function () { throw e; }, 0); } }
        this.dispatchEvent(ev);
      }
      reverse() {}
      updatePlaybackRate() {}
      persist() {}
      commitStyles() {}
    }
    tag(Animation, 'Animation');
    def(g.Element.prototype, 'animate', function animate(keyframes, options) { return new Animation(ANIM, this, keyframes, options); });
    def(g.Element.prototype, 'getAnimations', function getAnimations() {
      var self = this;
      return Array.from(all).filter(function (a) { return a._target === self && (a._state === 'running' || a._state === 'paused'); });
    });
    def(g.Document.prototype, 'getAnimations', function getAnimations() { return Array.from(all).filter(function (a) { return a._state === 'running' || a._state === 'paused'; }); });
    def(g, 'Animation', Animation);
// @include plan/ws074/phase260/animation-style.js
  })();
  // ---- document.fonts: load() and check() (the faces the page names are loaded or not by its stylesheets alone)
  (function () {
    var desc = typeof g.Document === 'function' ? Object.getOwnPropertyDescriptor(g.Document.prototype, 'fonts') : null;
    if (!desc || !desc.get) return;
    Object.defineProperty(g.Document.prototype, 'fonts', {
      get: function () {
        var set = desc.get.call(this);
        if (set && typeof set.load !== 'function') {
          def(set, 'load', function load() { return Promise.resolve([]); });
          def(set, 'check', function check() { return true; });
          hidden(set, 'status', 'loaded');
        }
        return set;
      },
      configurable: true, enumerable: true
    });
  })();
  // ---- Range.getClientRects() and getBoundingClientRect(): the rectangles of the text nodes the range covers (whole nodes)
  if (typeof g.Range === 'function' && typeof g.DOMRect === 'function' && !g.Range.prototype.getBoundingClientRect) (function () {
    function rects(range) {
      var out = [], start = range.startContainer, end = range.endContainer;
      function add(node) {
        var r = native.nodeRect(node);
        if (r) out.push(new g.DOMRect(r[0], r[1], r[2], r[3]));
      }
      if (start === end && start.nodeType === 3) { add(start); return out; }
      var walker = document.createTreeWalker(range.commonAncestorContainer, 4), node;
      while ((node = walker.nextNode())) {
        if (typeof range.intersectsNode !== 'function' || range.intersectsNode(node)) add(node);
      }
      return out;
    }
    def(g.Range.prototype, 'getClientRects', function getClientRects() { return rects(this); });
    def(g.Range.prototype, 'getBoundingClientRect', function getBoundingClientRect() {
      var list = rects(this), x0 = 0, y0 = 0, x1 = 0, y1 = 0, i;
      for (i = 0; i < list.length; i++) {
        if (i === 0) { x0 = list[i].left; y0 = list[i].top; x1 = list[i].right; y1 = list[i].bottom; continue; }
        x0 = Math.min(x0, list[i].left); y0 = Math.min(y0, list[i].top);
        x1 = Math.max(x1, list[i].right); y1 = Math.max(y1, list[i].bottom);
      }
      return new g.DOMRect(x0, y0, x1 - x0, y1 - y0);
    });
  })();
  // ---- Popover: the popover attribute and showPopover(), hidePopover(), togglePopover() (the top layer is a high z-index)
  if (typeof g.Element === 'function' && !g.Element.prototype.showPopover) (function () {
    function kind(element) {
      var v = element.getAttribute('popover');
      if (v === null) return null;
      v = v.toLowerCase();
      if (v === '' || v === 'auto') return 'auto';
      return v === 'hint' ? 'hint' : 'manual';
    }
    function open(element) { return element.matches(':popover-open'); }
    function need(element) {
      if (kind(element) === null) throw new DOMException('The element has no popover attribute.', 'NotSupportedError');
    }
    function change(element, to) {
      var from = to === 'open' ? 'closed' : 'open', ev;
      if (typeof g.ToggleEvent === 'function') ev = new g.ToggleEvent('beforetoggle', { oldState: from, newState: to, cancelable: to === 'open' });
      else { ev = new Event('beforetoggle', { cancelable: to === 'open' }); ev.oldState = from; ev.newState = to; }
      if (!element.dispatchEvent(ev) && to === 'open') return false;
      native.setPopover(element, to === 'open');
      Promise.resolve().then(function () {
        var t;
        if (typeof g.ToggleEvent === 'function') t = new g.ToggleEvent('toggle', { oldState: from, newState: to });
        else { t = new Event('toggle'); t.oldState = from; t.newState = to; }
        element.dispatchEvent(t);
      });
      return true;
    }
    Object.defineProperty(g.Element.prototype, 'popover', {
      get: function () { return kind(this); },
      set: function (v) { if (v === null) this.removeAttribute('popover'); else this.setAttribute('popover', String(v)); },
      configurable: true, enumerable: true
    });
    def(g.Element.prototype, 'showPopover', function showPopover() {
      need(this);
      if (open(this)) return;
      if (!this.isConnected) throw new DOMException('The element is not connected.', 'InvalidStateError');
      if (kind(this) === 'auto') {
        Array.from(document.querySelectorAll('[popover]:popover-open')).forEach(function (other) {
          if (other !== this && kind(other) === 'auto' && !other.contains(this) && !this.contains(other)) change(other, 'closed');
        }, this);
      }
      change(this, 'open');
    });
    def(g.Element.prototype, 'hidePopover', function hidePopover() {
      need(this);
      if (!open(this)) return;
      change(this, 'closed');
    });
    def(g.Element.prototype, 'togglePopover', function togglePopover(force) {
      need(this);
      var want = force === undefined ? !open(this) : !!force;
      if (want) this.showPopover(); else this.hidePopover();
      return open(this);
    });
  })();
  // ---- document.scrollingElement: the root element (this engine has no quirks-mode body scrolling)
  if (typeof g.Document === 'function' && !('scrollingElement' in g.Document.prototype)) {
    Object.defineProperty(g.Document.prototype, 'scrollingElement', {
      get: function () { return this.documentElement; }, configurable: true, enumerable: true
    });
  }
  // ---- Canvas: width and height reflect the attributes (300 by 150 when absent or not a number)
  if (typeof g.HTMLCanvasElement === 'function') (function () {
    var P = g.HTMLCanvasElement.prototype;
    [['width', 300], ['height', 150]].forEach(function (d) {
      var name = d[0], fallback = d[1];
      Object.defineProperty(P, name, {
        get: function () {
          var v = parseInt(this.getAttribute(name), 10);
          return isFinite(v) && v >= 0 && v <= 2147483647 ? v : fallback;
        },
        set: function (v) {
          v = Math.trunc(Number(v));
          this.setAttribute(name, String(isFinite(v) && v >= 0 && v <= 2147483647 ? v : fallback));
        },
        configurable: true, enumerable: true
      });
    });
  })();
  // ---- Dialog: open, returnValue, show(), showModal() and close() (a modal dialog is in the top layer: a high z-index, fixed)
  if (typeof g.HTMLDialogElement === 'function') (function () {
    var P = g.HTMLDialogElement.prototype;
    function isOpen(d) { return d.hasAttribute('open'); }
    function modal(d) { return d.__zedModal === true; }
    Object.defineProperty(P, 'open', {
      get: function () { return isOpen(this); },
      set: function (v) { if (v) { if (!isOpen(this)) this.setAttribute('open', ''); } else if (isOpen(this)) this.close(); },
      configurable: true, enumerable: true
    });
    Object.defineProperty(P, 'returnValue', {
      get: function () { return this.__zedReturn === undefined ? '' : this.__zedReturn; },
      set: function (v) { hidden(this, '__zedReturn', String(v)); },
      configurable: true, enumerable: true
    });
    def(P, 'show', function show() {
      if (isOpen(this)) {
        if (modal(this)) throw new DOMException("Failed to execute 'show' on 'HTMLDialogElement': The dialog is already open as a modal dialog, and therefore cannot be opened as a non-modal dialog.", 'InvalidStateError');
        return;
      }
      this.setAttribute('open', '');
    });
    def(P, 'showModal', function showModal() {
      if (isOpen(this)) {
        if (modal(this)) return;
        throw new DOMException("Failed to execute 'showModal' on 'HTMLDialogElement': The dialog is already open as a non-modal dialog, and therefore cannot be opened as a modal dialog.", 'InvalidStateError');
      }
      if (!this.isConnected) throw new DOMException("Failed to execute 'showModal' on 'HTMLDialogElement': The element is not in a document.", 'InvalidStateError');
      this.setAttribute('open', '');
      hidden(this, '__zedModal', true);
      native.setPopover(this, true);
    });
    def(P, 'close', function close(returnValue) {
      var d = this;
      if (!isOpen(d)) return;
      if (returnValue !== undefined) hidden(d, '__zedReturn', String(returnValue));
      d.removeAttribute('open');
      if (modal(d)) { hidden(d, '__zedModal', false); native.setPopover(d, false); }
      Promise.resolve().then(function () { d.dispatchEvent(new Event('close')); });
    });
  })();
  // ---- Streams and WebAssembly.Memory as far as a page can tell them apart from plain data: objects that cannot be cloned
  (function () {
    function stream(name) {
      var C = class { constructor() { } get locked() { return false; } };
      Object.defineProperty(C, 'name', { value: name });
      tag(C, name);
      if (typeof g[name] === 'undefined') def(g, name, C);
    }
    stream('ReadableStream');
    stream('WritableStream');
    stream('TransformStream');
    if (typeof g.WebAssembly === 'undefined') {
      class SharedBuffer { constructor(length) { hidden(this, '_length', length); } get byteLength() { return this._length; } }
      tag(SharedBuffer, 'SharedArrayBuffer');
      class Memory {
        constructor(descriptor) {
          if (descriptor === null || typeof descriptor !== 'object') throw new TypeError("WebAssembly.Memory(): Argument 0 must be a memory descriptor");
          if (descriptor.initial === undefined) throw new TypeError("WebAssembly.Memory(): Property 'initial' is required");
          if (descriptor.shared && descriptor.maximum === undefined) throw new TypeError("WebAssembly.Memory(): If shared is true, maximum property should be defined.");
          hidden(this, '_shared', !!descriptor.shared);
          hidden(this, '_pages', Number(descriptor.initial));
          hidden(this, '_buffer', null);
        }
        get buffer() {
          if (!this._buffer) this._buffer = this._shared ? new SharedBuffer(this._pages * 65536) : new ArrayBuffer(this._pages * 65536);
          return this._buffer;
        }
      }
      tag(Memory, 'WebAssembly.Memory');
      var wasm = {};
      def(wasm, 'Memory', Memory);
      tag({ prototype: wasm }, 'WebAssembly');
      def(g, 'WebAssembly', wasm);
    }
  })();
})
