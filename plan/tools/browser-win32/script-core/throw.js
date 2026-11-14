// Many exceptions: thrown and caught across nested calls, finally blocks, native callbacks, generators
// and promises, and syntax errors of eval and Function (the parser and the compiler leave by longjmp),
// with garbage made between them, so that the engine's non-local exits run thousands of times.
function deep(n) {
	if (n === 0)
		throw new RangeError("bottom");
	try {
		return deep(n - 1);
	} finally {
		finals++;
	}
}
var finals = 0;
var caught = 0;
var names = {};
for (var i = 0; i < 2000; i++) {
	try {
		deep(i % 50);
	} catch (e) {
		caught++;
		names[e.name] = (names[e.name] || 0) + 1;
	}
}
print("deep", caught, finals, names.RangeError);

// Rethrown, wrapped and replaced exceptions.
var chain = 0;
for (var j = 0; j < 1000; j++) {
	try {
		try {
			try {
				null.x;
			} catch (e) {
				throw new Error("wrapped " + e.name, { cause: e });
			}
		} catch (e) {
			if (e.cause instanceof TypeError)
				chain++;
			throw e;
		}
	} catch (e) {
		if (e.message === "wrapped TypeError")
			chain++;
	}
}
print("chain", chain);

// A finally that replaces the exception, and one that returns over it.
function replaced() {
	try {
		throw 1;
	} finally {
		throw 2;
	}
}
function swallowed() {
	try {
		throw 1;
	} finally {
		return "returned";
	}
}
var got = [];
try { replaced(); } catch (e) { got.push(e); }
got.push(swallowed());
print("finally", got.join(" "));

// Exceptions out of native callbacks and back into script.
var native = 0;
for (var k = 0; k < 500; k++) {
	try {
		[1, 2, 3].map(function (x) { if (x === 2) throw new SyntaxError("in map"); return x; });
	} catch (e) {
		if (e instanceof SyntaxError)
			native++;
	}
	try {
		JSON.parse("{bad");
	} catch (e) {
		if (e instanceof SyntaxError)
			native++;
	}
	try {
		[3, 1, 2].sort(function () { throw new EvalError("in sort"); });
	} catch (e) {
		if (e instanceof EvalError)
			native++;
	}
	try {
		JSON.stringify({ toJSON: function () { throw new URIError("in toJSON"); } });
	} catch (e) {
		if (e instanceof URIError)
			native++;
	}
}
print("native", native);

// A generator thrown into, and one that throws out.
function* guarded() {
	var seen = 0;
	for (;;) {
		try {
			yield seen;
		} catch (e) {
			seen += e;
		}
	}
}
var g = guarded();
g.next();
var last;
for (var m = 1; m <= 300; m++)
	last = g.throw(m).value;
function* thrower() {
	yield 1;
	throw new TypeError("from the generator");
}
var t = thrower();
t.next();
var out = "";
try { t.next(); } catch (e) { out = e.message; }
print("generator", last, out, JSON.stringify(t.next()));

// Syntax errors found by the parser and by the compiler, from nested calls, with garbage between them.
var syntax = ["var = ;", "{", "let a; let a;", "class C { m() { return this.#nope; } }", "for (let x of) {}", "a => { return; } }"];
function compileAt(depth, source) {
	if (depth > 0) {
		try {
			return compileAt(depth - 1, source);
		} finally {
			finals++;
		}
	}
	return eval(source);
}
var syntaxErrors = 0;
var garbage = 0;
finals = 0;
for (var n = 0; n < 400; n++) {
	for (var q = 0; q < syntax.length; q++) {
		try {
			compileAt(n % 20, syntax[q]);
		} catch (e) {
			if (e instanceof SyntaxError)
				syntaxErrors++;
		}
	}
	try {
		new Function("a", "return a +");
	} catch (e) {
		if (e instanceof SyntaxError)
			syntaxErrors++;
	}
	var junk = [];
	for (var w = 0; w < 200; w++)
		junk.push({ w: w, s: "junk" + w });
	garbage += junk.length;
}
print("syntax", syntaxErrors, finals, garbage, eval("1 + 2"), new Function("a", "b", "return a * b")(6, 7));

// Rejections handled by promises and async functions.
var rejected = 0;
async function failing(n) {
	await null;
	throw new Error("async " + n);
}
async function runAll() {
	for (var n = 0; n < 300; n++) {
		try {
			await failing(n);
		} catch (e) {
			rejected++;
		}
	}
	var settled = await Promise.allSettled([failing(1), Promise.resolve(2), Promise.reject(3)]);
	print("async", rejected, settled.map(function (s) { return s.status; }).join(","));
}
runAll();
