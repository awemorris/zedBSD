// The web platform pieces that are written in the page's own language (ws074-p230).
// bind/prelude.c holds this text as a C string; this file is the readable source it is made from.
// It is a function of the natives (parseURL, setURL, scrollToFragment) and the global object.
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
    function domError(name, message) { return new DOMException(message, name); }
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
    function makeEntry(url, navState, classic, from) {
      return { url: url, key: uuid(), id: uuid(), navState: navState, classic: classic, scroll: null,
        scrollMode: from ? from.scrollMode : 'auto', nhe: null };
    }
    // The first entry follows the document's URL until it is left (the URL is set after this runs).
    function urlOf(entry) { return entry.url === null ? docURL() : entry.url; }
    function leave(entry) { if (entry.url === null) entry.url = docURL(); saveScroll(entry); }
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
      get url() { return urlOf(this._e); }
      get key() { return this._e.key; }
      get id() { return this._e.id; }
      get index() { return entries.indexOf(this._e); }
      get sameDocument() { return true; }
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
      get key() { return this._d.entry ? this._d.entry.key : ''; }
      get id() { return this._d.entry ? this._d.entry.id : ''; }
      get index() { return this._d.index; }
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
    var loaded = false;
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
    function fireError(e) {
      navigation.dispatchEvent(new ErrorEvent('navigateerror', { error: e, message: errorMessage(e), filename: '', lineno: 0, colno: 0 }));
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
      navigation.dispatchEvent(new NavigationCurrentEntryChangeEvent('currententrychange', { navigationType: type, from: from }));
      for (i = 0; i < disposed.length; i++) disposed[i].dispatchEvent(new Event('dispose'));
    }
    function queueHashChange(oldURL, newURL) {
      setTimeout(function () { g.dispatchEvent(new HashChangeEvent('hashchange', { oldURL: oldURL, newURL: newURL })); }, 0);
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
      g.dispatchEvent(new PopStateEvent('popstate', { state: classicState, hasUAVisualTransition: false }));
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
    // Fires the navigate event of a navigation; o has the navigation's type, url, destination entry (a traversal),
    // navigation API state, classic history state (pushState), sameDocument, hashChange, the source element and
    // commit(), which carries out a navigation of this document that no listener intercepted.
    function fireNavigate(o) {
      var t, controller, event, n, dispatched, intercepted, from, promises, i;
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
        cancelable: true,
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
      try { dispatched = navigation.dispatchEvent(event); } finally { n.dispatching = false; }
      if (!dispatched || n.canceled) {
        if (!controller.signal.aborted) abortOngoing();
        return;
      }
      intercepted = n.state !== 'none';
      if (!intercepted && !o.sameDocument) {
        // Another document: loading it is not carried out here.
        if (t) cleanUp(t);
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
        navigation.dispatchEvent(new Event('navigatesuccess'));
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
      if (handling === 'auto' && !loaded) handling = 'replace';
      navigateTo(url, { history: handling, navState: null });
    }
    function locationReload() {
      navigateTo(docURL(), { history: 'reload', navState: entries[current].navState });
    }
    g.addEventListener('load', function () { setTimeout(function () { loaded = true; }, 0); });

    // A traversal runs as a task, like the browser's own back and forward.
    function traverse(find) {
      setTimeout(function () {
        var entry = find(), t;
        if (!entry) return;
        t = upcomingTraverse.get(entry.key) || null;
        if (entry === entries[current]) { if (t) resolveFinished(t); return; }
        fireNavigate({ type: 'traverse', url: entry.url, entry: entry, navState: entry.navState, sameDocument: true,
          hashChange: noFrag(entry.url) === noFrag(docURL()) && frag(entry.url) !== frag(docURL()),
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
      entries() { return entries.map(nhe); }
      get currentEntry() { return nhe(entries[current]); }
      updateCurrentEntry(options) {
        var state;
        if (arguments.length < 1) throw new TypeError("Failed to execute 'updateCurrentEntry' on 'Navigation': 1 argument required.");
        options = dictionary(options, 'NavigationUpdateCurrentEntryOptions');
        if (options.state === undefined) throw new TypeError("Failed to execute 'updateCurrentEntry' on 'Navigation': required member state is undefined.");
        state = serialize(options.state);
        entries[current].navState = state;
        this.dispatchEvent(new NavigationCurrentEntryChangeEvent('currententrychange', { navigationType: null, from: nhe(entries[current]) }));
      }
      get transition() { return transition; }
      get activation() { return activation; }
      get canGoBack() { return current > 0; }
      get canGoForward() { return current < entries.length - 1; }
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
        t = makeTracker(options.info, state, null);
        upcomingTracker = t;
        if (handling === 'auto' && !loaded) handling = 'replace';
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
        if (!findEntry(key)) return earlyError(domError('InvalidStateError', "Failed to execute 'traverseTo' on 'Navigation': Invalid key."));
        return traverseToKey(key, options);
      }
      back(options) {
        options = dictionary(options, 'NavigationOptions');
        if (current <= 0) return earlyError(domError('InvalidStateError', "Failed to execute 'back' on 'Navigation': Cannot go back."));
        return traverseToKey(entries[current - 1].key, options);
      }
      forward(options) {
        options = dictionary(options, 'NavigationOptions');
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

    // window.stop() aborts the navigation under way (loading the document is not stopped).
    def(g, 'stop', function stop() { if (ongoingEvent) abortOngoing(); });

    entries.push(makeEntry(null, null, null, null));
    var navigation = new Navigation(KEY);
    var activation = new NavigationActivation(KEY, null, nhe(entries[0]), 'push');
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
})
