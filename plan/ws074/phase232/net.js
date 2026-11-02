// The network objects of the web platform written in the page's own language (ws074-p232): fetch, Headers,
// Request, Response, Blob, File, FileReader, FormData, TextEncoder, TextDecoder, XMLHttpRequest, sendBeacon.
// bind/prelude.c runs this over three natives: send(method, url, headerLines, body, callback) starts a request
// (the page's loader; callback(error, status, url, headerLines, bodyText) is called on the page's event loop),
// cancel(id) drops it, syncGet(url) is a blocking GET and syncSend(method, url, headers, body) a blocking request of any
// method for a synchronous XMLHttpRequest.
// bind/prelude-net.inc is made from this file by plan/ws074/phase230/make-prelude.py.
(function (native, g) {
  'use strict';
  function def(o, name, f) { Object.defineProperty(o, name, { value: f, writable: true, configurable: true, enumerable: false }); }
  function hidden(o, name, v) { Object.defineProperty(o, name, { value: v, writable: true, configurable: true, enumerable: false }); }
  function tag(C, name) { Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true }); }
  function lower(s) { return String(s).toLowerCase(); }
  var KEY = {};
  var STATUS_TEXT = { 200: 'OK', 201: 'Created', 202: 'Accepted', 204: 'No Content', 206: 'Partial Content', 301: 'Moved Permanently',
    302: 'Found', 303: 'See Other', 304: 'Not Modified', 307: 'Temporary Redirect', 308: 'Permanent Redirect', 400: 'Bad Request',
    401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed', 408: 'Request Timeout', 409: 'Conflict',
    410: 'Gone', 413: 'Payload Too Large', 429: 'Too Many Requests', 500: 'Internal Server Error', 501: 'Not Implemented',
    502: 'Bad Gateway', 503: 'Service Unavailable', 504: 'Gateway Timeout' };

  // ---- TextEncoder and TextDecoder (UTF-8)
  function utf8Encode(str) {
    var out = [], i, c, d;
    str = String(str);
    for (i = 0; i < str.length; i++) {
      c = str.charCodeAt(i);
      if (c >= 0xd800 && c < 0xdc00 && i + 1 < str.length) {
        d = str.charCodeAt(i + 1);
        if (d >= 0xdc00 && d < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
      }
      if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
  }
  function utf8Decode(bytes) {
    var out = '', i = 0, n = bytes.length, c, e, parts = [];
    while (i < n) {
      c = bytes[i++];
      if (c < 0x80) { out += String.fromCharCode(c); }
      else if (c >= 0xc2 && c < 0xe0 && i < n && (bytes[i] & 0xc0) === 0x80) { out += String.fromCharCode(((c & 31) << 6) | (bytes[i++] & 63)); }
      else if (c >= 0xe0 && c < 0xf0 && i + 1 < n && (bytes[i] & 0xc0) === 0x80 && (bytes[i + 1] & 0xc0) === 0x80) {
        e = ((c & 15) << 12) | ((bytes[i] & 63) << 6) | (bytes[i + 1] & 63); i += 2;
        out += (e >= 0x800 && (e < 0xd800 || e > 0xdfff)) ? String.fromCharCode(e) : '�';
      } else if (c >= 0xf0 && c < 0xf5 && i + 2 < n && (bytes[i] & 0xc0) === 0x80 && (bytes[i + 1] & 0xc0) === 0x80 && (bytes[i + 2] & 0xc0) === 0x80) {
        e = ((c & 7) << 18) | ((bytes[i] & 63) << 12) | ((bytes[i + 1] & 63) << 6) | (bytes[i + 2] & 63); i += 3;
        if (e >= 0x10000 && e < 0x110000) { e -= 0x10000; out += String.fromCharCode(0xd800 + (e >> 10), 0xdc00 + (e & 1023)); }
        else out += '�';
      } else { out += '�'; }
      if (out.length > 8192) { parts.push(out); out = ''; }
    }
    parts.push(out);
    return parts.join('');
  }
  if (!g.TextEncoder) {
    class TextEncoder {
      get encoding() { return 'utf-8'; }
      encode(s) { return utf8Encode(s === undefined ? '' : s); }
      encodeInto(s, dest) { var b = utf8Encode(s), n = Math.min(b.length, dest.length), i; for (i = 0; i < n; i++) dest[i] = b[i]; return { read: s.length, written: n }; }
    }
    def(g, 'TextEncoder', TextEncoder);
  }
  if (!g.TextDecoder) {
    class TextDecoder {
      constructor(label) { hidden(this, '_label', label === undefined ? 'utf-8' : lower(label)); }
      get encoding() { return this._label === 'utf8' ? 'utf-8' : this._label; }
      get fatal() { return false; }
      get ignoreBOM() { return false; }
      decode(input) {
        var b;
        if (input === undefined) return '';
        b = input instanceof ArrayBuffer ? new Uint8Array(input) : new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
        if (b.length >= 3 && b[0] === 0xef && b[1] === 0xbb && b[2] === 0xbf) b = b.subarray(3);
        return utf8Decode(b);
      }
    }
    def(g, 'TextDecoder', TextDecoder);
  }

  // ---- Headers
  function normName(name) {
    name = String(name);
    if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(name)) throw new TypeError("Failed to execute on 'Headers': Invalid name");
    return lower(name);
  }
  function normValue(value) {
    value = String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
    if (/[\0\r\n]/.test(value)) throw new TypeError("Failed to execute on 'Headers': Invalid value");
    return value;
  }
  class Headers {
    constructor(init) {
      var self = this;
      hidden(this, '_list', []);
      if (init === undefined || init === null) return;
      if (init instanceof Headers) { init._list.forEach(function (p) { self._list.push([p[0], p[1]]); }); return; }
      if (typeof init[Symbol.iterator] === 'function') {
        Array.from(init).forEach(function (p) {
          p = Array.from(p);
          if (p.length !== 2) throw new TypeError("Failed to construct 'Headers': Invalid value");
          self.append(p[0], p[1]);
        });
        return;
      }
      Object.keys(init).forEach(function (k) { self.append(k, init[k]); });
    }
    append(name, value) {
      var n = normName(name), v = normValue(value), i;
      for (i = 0; i < this._list.length; i++) {
        if (this._list[i][0] === n) {
          if (n === 'set-cookie') break;
          this._list[i][1] += ', ' + v; return;
        }
      }
      this._list.push([n, v]);
    }
    delete(name) { var n = normName(name); this._list = this._list.filter(function (p) { return p[0] !== n; }); }
    get(name) {
      var n = normName(name), i;
      for (i = 0; i < this._list.length; i++) if (this._list[i][0] === n) return this._list[i][1];
      return null;
    }
    getSetCookie() { return this._list.filter(function (p) { return p[0] === 'set-cookie'; }).map(function (p) { return p[1]; }); }
    has(name) { return this.get(name) !== null; }
    set(name, value) {
      var n = normName(name), v = normValue(value), i, found = false, out = [];
      for (i = 0; i < this._list.length; i++) {
        if (this._list[i][0] === n) { if (!found) { out.push([n, v]); found = true; } }
        else out.push(this._list[i]);
      }
      if (!found) out.push([n, v]);
      this._list = out;
    }
    _sorted() { return this._list.map(function (p) { return [p[0], p[1]]; }).sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); }
    forEach(fn, thisArg) { var self = this; this._sorted().forEach(function (p) { fn.call(thisArg, p[1], p[0], self); }); }
    entries() { return this._sorted()[Symbol.iterator](); }
    keys() { return this._sorted().map(function (p) { return p[0]; })[Symbol.iterator](); }
    values() { return this._sorted().map(function (p) { return p[1]; })[Symbol.iterator](); }
  }
  def(Headers.prototype, Symbol.iterator, Headers.prototype.entries);
  tag(Headers, 'Headers');
  function headerLines(headers) {
    var out = '';
    headers._list.forEach(function (p) { out += p[0] + ': ' + p[1] + '\r\n'; });
    return out;
  }
  function parseHeaderLines(text) {
    var h = new Headers();
    String(text || '').split('\n').forEach(function (line) {
      var i = line.indexOf(':');
      if (i <= 0) return;
      try { h.append(line.slice(0, i), line.slice(i + 1)); } catch (e) { /* a header the Headers object refuses is dropped */ }
    });
    return h;
  }

  // ---- Blob and File
  // A Blob holds bytes; _text is their UTF-8 reading for the consumers that send text (fetch, FormData).
  function copyBytes(view) { var out = new Uint8Array(view.length); out.set(view); return out; }
  function partBytes(p) {
    if (p instanceof Blob) return p._bytes;
    if (p instanceof ArrayBuffer) return new Uint8Array(p);
    if (ArrayBuffer.isView(p)) return new Uint8Array(p.buffer, p.byteOffset, p.byteLength);
    return utf8Encode(p);
  }
  function blobType(t) {
    // A type with a character outside U+0020 to U+007E is dropped (File API, the Blob constructor).
    t = t === undefined ? '' : String(t);
    return /^[\x20-\x7e]*$/.test(t) ? lower(t) : '';
  }
  class Blob {
    constructor(parts, options) {
      var list = [], total = 0, bytes, at = 0;
      if (parts !== undefined && parts !== null) {
        if (typeof parts !== 'object' || typeof parts[Symbol.iterator] !== 'function')
          throw new TypeError("Failed to construct 'Blob': The provided value cannot be converted to a sequence.");
        Array.from(parts).forEach(function (p) { var b = partBytes(p); list.push(b); total += b.length; });
      }
      bytes = new Uint8Array(total);
      list.forEach(function (b) { bytes.set(b, at); at += b.length; });
      hidden(this, '_bytes', bytes);
      hidden(this, '_type', blobType(options && options.type));
    }
    get _text() { return utf8Decode(this._bytes); }
    get size() { return this._bytes.length; }
    get type() { return this._type; }
    text() { return Promise.resolve(utf8Decode(this._bytes)); }
    arrayBuffer() { return Promise.resolve(copyBytes(this._bytes).buffer); }
    bytes() { return Promise.resolve(copyBytes(this._bytes)); }
    slice(start, end, type) {
      var n = this._bytes.length, s = start === undefined ? 0 : Math.trunc(Number(start)) || 0, e = end === undefined ? n : Math.trunc(Number(end)) || 0;
      if (s < 0) s = Math.max(n + s, 0);
      if (e < 0) e = Math.max(n + e, 0);
      s = Math.min(s, n); e = Math.min(e, n);
      return new Blob([this._bytes.subarray(s, Math.max(s, e))], { type: type });
    }
  }
  class File extends Blob {
    constructor(parts, name, options) {
      super(parts, options);
      hidden(this, '_name', String(name));
      hidden(this, '_modified', options && options.lastModified !== undefined ? options.lastModified : Date.now());
    }
    get name() { return this._name; }
    get lastModified() { return this._modified; }
  }
  tag(File, 'File');
  if (!g.Blob) def(g, 'Blob', Blob);
  if (!g.File) def(g, 'File', File);
  if (g.URL && !g.URL.createObjectURL) {
    var objectUrls = {}, objectCount = 0;
    def(g.URL, 'createObjectURL', function createObjectURL(blob) {
      var u = 'blob:' + g.location.origin + '/zedbsd-' + (++objectCount);
      objectUrls[u] = blob; return u;
    });
    def(g.URL, 'revokeObjectURL', function revokeObjectURL(u) { delete objectUrls[u]; });
  }

  // ---- FormData
  class FormData {
    constructor(form) {
      var self = this;
      hidden(this, '_entries', []);
      if (form && form.elements) {
        Array.from(form.elements).forEach(function (el) {
          var type = lower(el.type || '');
          if (!el.name || el.disabled) return;
          if ((type === 'checkbox' || type === 'radio') && !el.checked) return;
          if (type === 'submit' || type === 'button' || type === 'reset' || type === 'image' || type === 'file') return;
          if (lower(el.tagName) === 'select' && el.multiple) {
            Array.from(el.options).forEach(function (o) { if (o.selected) self.append(el.name, o.value); });
            return;
          }
          self.append(el.name, el.value);
        });
      }
    }
    _make(name, value, filename) {
      if (value instanceof Blob) {
        if (!(value instanceof File) || filename !== undefined) value = new File([value], filename === undefined ? 'blob' : String(filename), { type: value.type });
        return [String(name), value];
      }
      return [String(name), String(value)];
    }
    append(name, value, filename) { this._entries.push(this._make(name, value, filename)); }
    delete(name) { name = String(name); this._entries = this._entries.filter(function (e) { return e[0] !== name; }); }
    get(name) { var i; name = String(name); for (i = 0; i < this._entries.length; i++) if (this._entries[i][0] === name) return this._entries[i][1]; return null; }
    getAll(name) { name = String(name); return this._entries.filter(function (e) { return e[0] === name; }).map(function (e) { return e[1]; }); }
    has(name) { return this.get(name) !== null; }
    set(name, value, filename) {
      var made = this._make(name, value, filename), i, out = [], done = false;
      for (i = 0; i < this._entries.length; i++) {
        if (this._entries[i][0] === made[0]) { if (!done) { out.push(made); done = true; } }
        else out.push(this._entries[i]);
      }
      if (!done) out.push(made);
      this._entries = out;
    }
    forEach(fn, thisArg) { var self = this; this._entries.slice().forEach(function (e) { fn.call(thisArg, e[1], e[0], self); }); }
    entries() { return this._entries.map(function (e) { return [e[0], e[1]]; })[Symbol.iterator](); }
    keys() { return this._entries.map(function (e) { return e[0]; })[Symbol.iterator](); }
    values() { return this._entries.map(function (e) { return e[1]; })[Symbol.iterator](); }
  }
  def(FormData.prototype, Symbol.iterator, FormData.prototype.entries);
  tag(FormData, 'FormData');
  if (!g.FormData) def(g, 'FormData', FormData);
  function multipart(fd, boundary) {
    var out = '';
    fd._entries.forEach(function (e) {
      out += '--' + boundary + '\r\n';
      if (typeof e[1] === 'string') {
        out += 'Content-Disposition: form-data; name="' + e[0].replace(/"/g, '%22') + '"\r\n\r\n' + e[1].replace(/\r?\n/g, '\r\n') + '\r\n';
      } else {
        out += 'Content-Disposition: form-data; name="' + e[0].replace(/"/g, '%22') + '"; filename="' + e[1].name.replace(/"/g, '%22') + '"\r\n' +
          'Content-Type: ' + (e[1].type || 'application/octet-stream') + '\r\n\r\n' + e[1]._text + '\r\n';
      }
    });
    return out + '--' + boundary + '--\r\n';
  }
  function urlencoded(fd) {
    var p = new g.URLSearchParams();
    fd.forEach(function (v, k) { p.append(k, typeof v === 'string' ? v : v.name); });
    return p.toString();
  }

  // ---- The body of a Request or a Response: a string, and its Content-Type when none was given
  function extractBody(body) {
    var boundary;
    if (body === undefined || body === null) return { text: null, type: null };
    if (typeof body === 'string') return { text: body, type: 'text/plain;charset=UTF-8' };
    if (g.URLSearchParams && body instanceof g.URLSearchParams) return { text: body.toString(), type: 'application/x-www-form-urlencoded;charset=UTF-8' };
    if (body instanceof FormData) {
      boundary = '----zedBSDFormBoundary' + Math.random().toString(36).slice(2, 12);
      return { text: multipart(body, boundary), type: 'multipart/form-data; boundary=' + boundary };
    }
    if (body instanceof Blob) return { text: body._text, type: body.type || null };
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return { text: partText(body), type: null };
    return { text: String(body), type: 'text/plain;charset=UTF-8' };
  }
  function bodyMixin(C) {
    var P = C.prototype;
    function consume(self) {
      if (self._used) return Promise.reject(new TypeError('Body has already been consumed.'));
      if (self._body !== null) self._used = true;
      return Promise.resolve(self._body === null ? '' : self._body);
    }
    Object.defineProperty(P, 'bodyUsed', { get: function () { return !!this._used; }, configurable: true });
    Object.defineProperty(P, 'body', { get: function () { return null; }, configurable: true });
    def(P, 'text', function text() { return consume(this); });
    def(P, 'json', function json() { return consume(this).then(function (t) { return JSON.parse(t); }); });
    def(P, 'arrayBuffer', function arrayBuffer() { return consume(this).then(function (t) { return utf8Encode(t).buffer; }); });
    def(P, 'bytes', function bytes() { return consume(this).then(function (t) { return utf8Encode(t); }); });
    def(P, 'blob', function blob() {
      var type = this.headers.get('content-type') || '';
      return consume(this).then(function (t) { return new Blob([t], { type: type }); });
    });
    def(P, 'formData', function formData() {
      var type = lower(this.headers.get('content-type') || '');
      return consume(this).then(function (t) {
        var fd = new FormData(), p;
        if (type.indexOf('application/x-www-form-urlencoded') !== 0) throw new TypeError('Failed to fetch');
        p = new g.URLSearchParams(t);
        p.forEach(function (v, k) { fd.append(k, v); });
        return fd;
      });
    });
  }

  // ---- Response
  class Response {
    constructor(body, init) {
      var b = extractBody(body), status, headers;
      init = init || {};
      status = init.status === undefined ? 200 : Number(init.status);
      if (status < 200 || status > 599) throw new RangeError("Failed to construct 'Response': The status provided (" + status + ') is outside the range [200, 599].');
      headers = new Headers(init.headers);
      if (b.text !== null && !headers.has('content-type') && b.type) headers.set('content-type', b.type);
      hidden(this, '_body', b.text);
      hidden(this, '_used', false);
      hidden(this, '_status', status);
      hidden(this, '_statusText', init.statusText === undefined ? (STATUS_TEXT[status] || '') : String(init.statusText));
      hidden(this, '_headers', headers);
      hidden(this, '_url', '');
      hidden(this, '_redirected', false);
      hidden(this, '_type', 'default');
    }
    get status() { return this._status; }
    get ok() { return this._status >= 200 && this._status < 300; }
    get statusText() { return this._statusText; }
    get headers() { return this._headers; }
    get url() { return this._url; }
    get redirected() { return this._redirected; }
    get type() { return this._type; }
    clone() {
      var r;
      if (this._used) throw new TypeError("Failed to execute 'clone' on 'Response': Response body is already used");
      r = new Response(null, { status: this._status === 0 ? 200 : this._status, statusText: this._statusText, headers: this._headers });
      r._body = this._body; r._url = this._url; r._redirected = this._redirected; r._type = this._type; r._status = this._status;
      return r;
    }
    static error() { var r = new Response(null, { status: 200 }); r._status = 0; r._type = 'error'; return r; }
    static json(data, init) {
      var r = new Response(JSON.stringify(data), init);
      if (!(init && init.headers && new Headers(init.headers).has('content-type'))) r._headers.set('content-type', 'application/json');
      return r;
    }
    static redirect(url, status) {
      var r = new Response(null, { status: status === undefined ? 302 : status });
      r._headers.set('location', new g.URL(url, g.location.href).href);
      return r;
    }
  }
  bodyMixin(Response);
  tag(Response, 'Response');
  def(g, 'Response', Response);
  def(g, 'Headers', Headers);

  // ---- Request
  var METHODS = { DELETE: 1, GET: 1, HEAD: 1, OPTIONS: 1, POST: 1, PUT: 1 };
  class Request {
    constructor(input, init) {
      var src = input instanceof Request ? input : null, b, method, headers;
      init = init || {};
      hidden(this, '_url', new g.URL(src ? src._url : String(input), g.location.href).href);
      method = init.method !== undefined ? String(init.method) : (src ? src._method : 'GET');
      if (METHODS[method.toUpperCase()]) method = method.toUpperCase();
      if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(method)) throw new TypeError("Failed to construct 'Request': '" + method + "' is not a valid HTTP method.");
      headers = new Headers(init.headers !== undefined ? init.headers : (src ? src._headers : undefined));
      hidden(this, '_method', method);
      hidden(this, '_headers', headers);
      b = init.body !== undefined ? extractBody(init.body) : (src ? { text: src._body, type: null } : { text: null, type: null });
      if (b.text !== null && (method === 'GET' || method === 'HEAD')) throw new TypeError("Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
      if (b.text !== null && !headers.has('content-type') && b.type) headers.set('content-type', b.type);
      hidden(this, '_body', b.text);
      hidden(this, '_used', false);
      hidden(this, '_credentials', init.credentials !== undefined ? init.credentials : (src ? src._credentials : 'same-origin'));
      hidden(this, '_mode', init.mode !== undefined ? init.mode : (src ? src._mode : 'cors'));
      hidden(this, '_cache', init.cache !== undefined ? init.cache : (src ? src._cache : 'default'));
      hidden(this, '_redirect', init.redirect !== undefined ? init.redirect : (src ? src._redirect : 'follow'));
      hidden(this, '_referrer', init.referrer !== undefined ? init.referrer : (src ? src._referrer : 'about:client'));
      hidden(this, '_signal', init.signal !== undefined && init.signal !== null ? init.signal : (src ? src._signal : new g.AbortController().signal));
      hidden(this, '_keepalive', !!init.keepalive);
    }
    get url() { return this._url; }
    get method() { return this._method; }
    get headers() { return this._headers; }
    get credentials() { return this._credentials; }
    get mode() { return this._mode; }
    get cache() { return this._cache; }
    get redirect() { return this._redirect; }
    get referrer() { return this._referrer; }
    get signal() { return this._signal; }
    get keepalive() { return this._keepalive; }
    get destination() { return ''; }
    get integrity() { return ''; }
    clone() { return new Request(this); }
  }
  bodyMixin(Request);
  tag(Request, 'Request');
  def(g, 'Request', Request);

  // ---- fetch
  function sameOrigin(url) {
    try { return new g.URL(url).origin === g.location.origin; } catch (e) { return true; }
  }
  function startRequest(method, url, headers, body, callback) {
    // A completion inside the call that started the request (a data: URL, a refused request) comes later.
    var sync = true, queued = null, id;
    id = native.send(method, url, headers, body, function (error, status, finalUrl, lines, text) {
      if (sync) { queued = [error, status, finalUrl, lines, text]; return; }
      callback(error, status, finalUrl, lines, text);
    });
    sync = false;
    if (queued) Promise.resolve().then(function () { callback(queued[0], queued[1], queued[2], queued[3], queued[4]); });
    return id;
  }
  function networkError() { return new TypeError('Failed to fetch'); }
  def(g, 'fetch', function fetch(input, init) {
    return new Promise(function (resolve, reject) {
      var request, signal, headers, body, id, finished = false, onabort, abortReason;
      try { request = new Request(input, init); } catch (e) { reject(e); return; }
      signal = request._signal;
      abortReason = function () { return signal.reason !== undefined ? signal.reason : new DOMException('signal is aborted without reason', 'AbortError'); };
      if (signal && signal.aborted) { reject(abortReason()); return; }
      headers = new Headers(request._headers);
      if (!headers.has('referer') && g.location && /^https?:/.test(g.location.href)) headers.set('referer', g.location.href.split('#')[0]);
      if (!headers.has('origin') && g.location && /^https?:/.test(g.location.href) && (request._method !== 'GET' && request._method !== 'HEAD' || !sameOrigin(request._url)))
        headers.set('origin', g.location.origin);
      body = request._body;
      if (/^(data|blob|file):/i.test(request._url)) headers = new Headers();
      id = startRequest(request._method, request._url, headerLines(headers), body, function (error, status, finalUrl, lines, text) {
        var response;
        if (finished) return;
        finished = true;
        if (signal && onabort) signal.removeEventListener('abort', onabort);
        if (error !== 0) { reject(networkError()); return; }
        response = new Response(status === 204 || status === 205 || status === 304 || request._method === 'HEAD' ? null : text, { status: status < 200 ? 200 : status, headers: new Headers() });
        response._status = status;
        response._statusText = STATUS_TEXT[status] || '';
        response._headers = parseHeaderLines(lines);
        response._url = finalUrl;
        response._redirected = finalUrl !== request._url.split('#')[0] && finalUrl !== request._url;
        response._type = sameOrigin(request._url) ? 'basic' : 'cors';
        if (response._headers.get('content-type') === null && /^data:/i.test(request._url)) {
          response._headers.set('content-type', (/^data:([^,;]*)/i.exec(request._url) || [0, 'text/plain'])[1] || 'text/plain');
        }
        resolve(response);
      });
      if (signal) {
        onabort = function () {
          if (finished) return;
          finished = true;
          native.cancel(id);
          reject(abortReason());
        };
        signal.addEventListener('abort', onabort);
      }
    });
  });
  if (g.navigator && !g.navigator.sendBeacon) {
    def(g.navigator, 'sendBeacon', function sendBeacon(url, data) {
      var b = extractBody(data), h = new Headers();
      if (b.type && !(data instanceof Blob && !data.type)) h.set('content-type', b.type);
      try { startRequest('POST', new g.URL(String(url), g.location.href).href, headerLines(h), b.text === null ? '' : b.text, function () {}); } catch (e) { return false; }
      return true;
    });
  }

  // ---- ProgressEvent and XMLHttpRequest
  if (!g.ProgressEvent) {
    class ProgressEvent extends Event {
      constructor(type, init) {
        super(type, init);
        init = init || {};
        hidden(this, '_loaded', init.loaded || 0); hidden(this, '_total', init.total || 0); hidden(this, '_computable', !!init.lengthComputable);
      }
      get loaded() { return this._loaded; }
      get total() { return this._total; }
      get lengthComputable() { return this._computable; }
    }
    def(g, 'ProgressEvent', ProgressEvent);
  }
  var XHR_EVENTS = ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend'];
  function fire(target, type, loaded) {
    var ev = type === 'readystatechange' ? new Event(type) : new g.ProgressEvent(type, { loaded: loaded || 0, total: loaded || 0, lengthComputable: !!loaded });
    var handler = target['on' + type];
    if (typeof handler === 'function') {
      try { handler.call(target, ev); } catch (e) { setTimeout(function () { throw e; }, 0); }
    }
    target.dispatchEvent(ev);
  }
  function setState(xhr, state) {
    xhr._state = state;
    fire(xhr, 'readystatechange');
  }
  // ---- FileReader (File API): reads a Blob in a later task and reports it through progress events
  function latin1(bytes) {
    var parts = [], i;
    for (i = 0; i < bytes.length; i += 8192) parts.push(String.fromCharCode.apply(null, bytes.subarray(i, i + 8192)));
    return parts.join('');
  }
  var READER_EVENTS = ['loadstart', 'progress', 'load', 'abort', 'error', 'loadend'];
  class FileReader extends EventTarget {
    constructor() {
      super();
      var self = this;
      READER_EVENTS.forEach(function (t) { hidden(self, 'on' + t, null); });
      hidden(this, '_state', 0); hidden(this, '_result', null); hidden(this, '_error', null); hidden(this, '_run', 0);
    }
    get readyState() { return this._state; }
    get result() { return this._result; }
    get error() { return this._error; }
    _read(blob, kind, label) {
      var self = this, run;
      if (!(blob instanceof Blob)) throw new TypeError("Failed to execute 'read' on 'FileReader': parameter 1 is not of type 'Blob'.");
      if (this._state === 1) throw new DOMException("Failed to execute 'read' on 'FileReader': The object is already busy reading Blobs.", 'InvalidStateError');
      this._state = 1; this._result = null; this._error = null;
      run = ++this._run;
      setTimeout(function () {
        var bytes = blob._bytes, n = bytes.length, result;
        if (self._run !== run) return;
        fire(self, 'loadstart', 0);
        if (self._run !== run) return;
        switch (kind) {
          case 'buffer': result = copyBytes(bytes).buffer; break;
          case 'binary': result = latin1(bytes); break;
          case 'url': result = 'data:' + (blob.type || 'application/octet-stream') + ';base64,' + g.btoa(latin1(bytes)); break;
          default: result = new g.TextDecoder(label === undefined ? 'utf-8' : label).decode(bytes); break;
        }
        fire(self, 'progress', n);
        if (self._run !== run) return;
        self._state = 2; self._result = result;
        fire(self, 'load', n);
        if (self._state !== 1) fire(self, 'loadend', n);
      }, 0);
    }
    readAsArrayBuffer(blob) { this._read(blob, 'buffer'); }
    readAsBinaryString(blob) { this._read(blob, 'binary'); }
    readAsText(blob, label) { this._read(blob, 'text', label); }
    readAsDataURL(blob) { this._read(blob, 'url'); }
    abort() {
      if (this._state === 0 || this._state === 2) { this._result = null; return; }
      this._run++; this._state = 2; this._result = null;
      fire(this, 'abort', 0);
      if (this._state !== 1) fire(this, 'loadend', 0);
    }
  }
  [['EMPTY', 0], ['LOADING', 1], ['DONE', 2]].forEach(function (c) {
    Object.defineProperty(FileReader, c[0], { value: c[1], enumerable: true });
    Object.defineProperty(FileReader.prototype, c[0], { value: c[1], enumerable: true });
  });
  tag(FileReader, 'FileReader');
  if (!g.FileReader) def(g, 'FileReader', FileReader);

  class XMLHttpRequestEventTarget extends EventTarget {
    constructor() {
      super();
      var self = this;
      XHR_EVENTS.forEach(function (t) { hidden(self, 'on' + t, null); });
    }
  }
  class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}
  class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
      super();
      hidden(this, '_state', 0);
      hidden(this, '_method', 'GET');
      hidden(this, '_url', '');
      hidden(this, '_async', true);
      hidden(this, '_headers', new Headers());
      hidden(this, '_status', 0);
      hidden(this, '_statusText', '');
      hidden(this, '_text', '');
      hidden(this, '_responseHeaders', new Headers());
      hidden(this, '_responseUrl', '');
      hidden(this, '_responseType', '');
      hidden(this, '_timeout', 0);
      hidden(this, '_withCredentials', false);
      hidden(this, '_id', 0);
      hidden(this, '_sent', false);
      hidden(this, '_timer', 0);
      hidden(this, '_mime', '');
      hidden(this, '_upload', new XMLHttpRequestUpload());
      hidden(this, 'onreadystatechange', null);
    }
    get readyState() { return this._state; }
    get status() { return this._status; }
    get statusText() { return this._statusText; }
    get responseURL() { return this._responseUrl; }
    get upload() { return this._upload; }
    get withCredentials() { return this._withCredentials; }
    set withCredentials(v) { this._withCredentials = !!v; }
    get timeout() { return this._timeout; }
    set timeout(v) { this._timeout = Number(v) || 0; }
    get responseType() { return this._responseType; }
    set responseType(v) {
      if (this._state >= 3) throw new DOMException("Failed to set the 'responseType' property on 'XMLHttpRequest': The response type cannot be set if the object's state is LOADING or DONE.", 'InvalidStateError');
      this._responseType = String(v);
    }
    get responseText() {
      if (this._responseType !== '' && this._responseType !== 'text')
        throw new DOMException("Failed to read the 'responseText' property from 'XMLHttpRequest': The value is only accessible if the object's 'responseType' is '' or 'text' (was '" + this._responseType + "').", 'InvalidStateError');
      return this._state === 4 || this._state === 3 ? this._text : '';
    }
    get response() {
      var t = this._text;
      if (this._state !== 4 && !(this._state === 3 && (this._responseType === '' || this._responseType === 'text'))) return this._responseType === '' || this._responseType === 'text' ? '' : null;
      switch (this._responseType) {
        case 'json': try { return JSON.parse(t); } catch (e) { return null; }
        case 'arraybuffer': return utf8Encode(t).buffer;
        case 'blob': return new Blob([t], { type: this._responseHeaders.get('content-type') || '' });
        case 'document':
          try { return new g.DOMParser().parseFromString(t, (this._responseHeaders.get('content-type') || 'text/html').split(';')[0]); } catch (e) { return null; }
        default: return t;
      }
    }
    get responseXML() {
      if (this._state !== 4) return null;
      try { return new g.DOMParser().parseFromString(this._text, 'text/xml'); } catch (e) { return null; }
    }
    open(method, url, async, user, password) {
      var m = String(method);
      if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(m)) throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': '" + m + "' is not a valid HTTP method.", 'SyntaxError');
      if (/^(connect|trace|track)$/i.test(m)) throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': '" + m + "' HTTP method is unsupported.", 'SecurityError');
      if (METHODS[m.toUpperCase()]) m = m.toUpperCase();
      this._abortQuiet();
      this._method = m;
      this._url = new g.URL(String(url), g.location.href).href;
      this._async = async === undefined ? true : !!async;
      this._headers = new Headers();
      this._status = 0; this._statusText = ''; this._text = ''; this._responseHeaders = new Headers(); this._sent = false;
      if (this._state !== 1) setState(this, 1); else fire(this, 'readystatechange');
    }
    setRequestHeader(name, value) {
      if (this._state !== 1 || this._sent) throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
      this._headers.append(name, value);
    }
    overrideMimeType(mime) { this._mime = String(mime); }
    getResponseHeader(name) { return this._state >= 2 ? this._responseHeaders.get(name) : null; }
    getAllResponseHeaders() {
      var out = '';
      if (this._state < 2) return '';
      this._responseHeaders._list.forEach(function (p) { out += p[0] + ': ' + p[1] + '\r\n'; });
      return out;
    }
    _abortQuiet() {
      if (this._id) { native.cancel(this._id); this._id = 0; }
      if (this._timer) { clearTimeout(this._timer); this._timer = 0; }
    }
    _finish(error, status, finalUrl, lines, text) {
      var self = this, kind = error !== 0 ? 'error' : 'load';
      this._id = 0;
      if (this._timer) { clearTimeout(this._timer); this._timer = 0; }
      if (error === 0) {
        this._status = status;
        this._statusText = STATUS_TEXT[status] || '';
        this._responseHeaders = parseHeaderLines(lines);
        this._responseUrl = finalUrl;
        if (/^data:/i.test(this._url) && !this._responseHeaders.has('content-type'))
          this._responseHeaders.set('content-type', (/^data:([^,;]*)/i.exec(this._url) || [0, 'text/plain'])[1] || 'text/plain');
        setState(this, 2);
        this._text = text;
        setState(this, 3);
        fire(this, 'progress', utf8Encode(text).length);
        setState(this, 4);
      } else {
        this._status = 0; this._text = '';
        setState(this, 4);
      }
      fire(this, kind, kind === 'load' ? utf8Encode(this._text).length : 0);
      fire(this, 'loadend');
    }
    send(body) {
      var self = this, b, headers, got;
      if (this._state !== 1 || this._sent) throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
      if (this._method === 'GET' || this._method === 'HEAD') body = null;
      b = extractBody(body);
      headers = new Headers(this._headers);
      if (b.text !== null && !headers.has('content-type') && b.type) headers.set('content-type', b.type);
      if (!headers.has('x-requested-with') && false) headers.set('x-requested-with', 'XMLHttpRequest');
      if (!headers.has('referer') && /^https?:/.test(g.location.href)) headers.set('referer', g.location.href.split('#')[0]);
      if (!headers.has('origin') && /^https?:/.test(g.location.href) && (this._method !== 'GET' && this._method !== 'HEAD' || !sameOrigin(this._url)))
        headers.set('origin', g.location.origin);
      if (/^(data|blob|file):/i.test(this._url)) headers = new Headers();
      this._sent = true;
      fire(this, 'loadstart');
      if (!this._async) {
        // A web resource is requested with its method, headers and body, and its status and headers are reported.
        if (native.syncSend && /^https?:/i.test(new URL(this._url, g.location.href).href)) {
          got = native.syncSend(this._method, this._url, headerLines(headers), b.text);
          if (got === null) { this._finish(1); throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': Failed to load '" + this._url + "'.", 'NetworkError'); }
          this._finish(0, got[0], got[1], got[2], got[3]);
          return;
        }
        if (this._method !== 'GET') throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': Failed to load '" + this._url + "'.", 'NetworkError');
        got = native.syncGet(this._url);
        if (got === null) { this._finish(1); throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': Failed to load '" + this._url + "'.", 'NetworkError'); }
        this._finish(0, 200, got[0], '', got[1]);
        return;
      }
      if (this._timeout > 0) {
        this._timer = setTimeout(function () {
          self._timer = 0;
          if (self._id) { native.cancel(self._id); self._id = 0; }
          self._status = 0; self._text = '';
          setState(self, 4);
          fire(self, 'timeout');
          fire(self, 'loadend');
        }, this._timeout);
      }
      this._id = startRequest(this._method, this._url, headerLines(headers), b.text, function (error, status, finalUrl, lines, text) {
        self._finish(error, status, finalUrl, lines, text);
      });
    }
    abort() {
      var active = this._id !== 0 || this._timer !== 0;
      this._abortQuiet();
      if (this._state === 4 || (this._state === 1 && !this._sent) || this._state === 0) { this._state = 0; return; }
      if (active || this._state === 2 || this._state === 3 || this._sent) {
        this._status = 0; this._text = ''; this._responseHeaders = new Headers();
        setState(this, 4);
        fire(this, 'abort');
        fire(this, 'loadend');
      }
      this._state = 0;
    }
  }
  ['UNSENT', 'OPENED', 'HEADERS_RECEIVED', 'LOADING', 'DONE'].forEach(function (n, i) {
    Object.defineProperty(XMLHttpRequest, n, { value: i, enumerable: true });
    Object.defineProperty(XMLHttpRequest.prototype, n, { value: i, enumerable: true });
  });
  tag(XMLHttpRequest, 'XMLHttpRequest');
  def(g, 'XMLHttpRequestEventTarget', XMLHttpRequestEventTarget);
  def(g, 'XMLHttpRequestUpload', XMLHttpRequestUpload);
  def(g, 'XMLHttpRequest', XMLHttpRequest);
})
