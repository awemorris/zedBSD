// Property lookup and order through object shapes: small and large objects, deletion, re-adding,
// attribute changes, shared and diverging layouts, prototype chains, accessors, symbols and Proxy.

function keysOf(o) {
	return Object.keys(o).join(",");
}

// Order after a delete and a re-add: the re-added key goes last.
var small = { a: 1, b: 2, c: 3 };
delete small.b;
small.b = 4;
print("small order", keysOf(small), small.a, small.b, small.c, "b" in small);

// Integer keys come first in ascending order, then strings in the order added, then symbols.
var mixed = {};
mixed.z = 1;
mixed[10] = 2;
mixed.y = 3;
mixed[2] = 4;
var sym = Symbol("s");
mixed[sym] = 5;
mixed.x = 6;
print("mixed order", Object.getOwnPropertyNames(mixed).concat(Object.getOwnPropertySymbols(mixed).map(String)).join(","));

// A large object built one property at a time, read back between additions.
var big = {};
var missing = 0;
for (var i = 0; i < 1500; i++) {
	if (("p" + i) in big)
		missing++;
	big["p" + i] = i;
	if (big["p" + i] !== i || big["p" + (i >> 1)] !== (i >> 1))
		missing++;
}
print("big built", Object.keys(big).length, missing, big.p0, big.p777, big.p1499, big.p1500);

// Deleting every third property keeps the others and their order.
for (i = 0; i < 1500; i += 3)
	delete big["p" + i];
var order = Object.keys(big);
print("big deleted", order.length, order[0], order[1], order[2], order[order.length - 1], big.p3, big.p4, "p6" in big);

// Re-adding a deleted key puts it last; the rest keep their places.
big.p3 = "again";
order = Object.keys(big);
print("big re-added", order.length, order[order.length - 1], order[0], big.p3, big.p5);

// for-in sees the own properties in order, then the prototype's that are not shadowed.
var proto = { shared: "proto", only: "proto-only" };
var child = Object.create(proto);
for (i = 0; i < 40; i++)
	child["c" + i] = i;
child.shared = "own";
var seen = [];
for (var key in child)
	seen.push(key);
print("for-in", seen.length, seen[0], seen[39], seen[40], seen[41]);

// A large prototype: lookups through the chain, shadowing and deleting the shadow.
var base = {};
for (i = 0; i < 300; i++)
	base["m" + i] = "base" + i;
var derived = Object.create(base);
derived.m10 = "own10";
print("chain", derived.m10, derived.m11, derived.m299, derived.m300, "m200" in derived, derived.hasOwnProperty("m200"));
delete derived.m10;
print("chain unshadowed", derived.m10, derived.hasOwnProperty("m10"));

// Accessors among many data properties.
var log = [];
var withAccessors = {};
for (i = 0; i < 50; i++)
	withAccessors["d" + i] = i;
Object.defineProperty(withAccessors, "computed", {
	get: function () { log.push("get"); return this.d49 * 2; },
	set: function (v) { log.push("set " + v); this.d0 = v; },
	enumerable: true,
	configurable: true
});
for (i = 50; i < 60; i++)
	withAccessors["d" + i] = i;
withAccessors.computed = 7;
print("accessors", withAccessors.computed, withAccessors.d0, log.join(";"), keysOf(withAccessors).split(",").indexOf("computed"));

// Changing a property's attributes keeps its place; a non-configurable one cannot be deleted.
var attributes = {};
for (i = 0; i < 20; i++)
	attributes["k" + i] = i;
Object.defineProperty(attributes, "k5", { writable: false });
attributes.k5 = 99;
Object.defineProperty(attributes, "k7", { configurable: false });
var deleted = delete attributes.k7;
Object.defineProperty(attributes, "k9", { enumerable: false });
print("attributes", attributes.k5, deleted, attributes.k7, Object.keys(attributes).length, keysOf(attributes).slice(0, 23));
print("descriptor", JSON.stringify(Object.getOwnPropertyDescriptor(attributes, "k5")), JSON.stringify(Object.getOwnPropertyDescriptor(attributes, "k9")));

// Two objects made alike share a layout until one of them diverges.
function make(n, extra) {
	var o = {};
	for (var j = 0; j < n; j++)
		o["s" + j] = j;
	if (extra)
		o.extra = extra;
	return o;
}
var one = make(30, null);
var two = make(30, "two");
var three = make(15, "three");
three.s15 = "late";
print("diverge", one.extra, two.extra, three.extra, one.s29, two.s29, three.s29, three.s15, keysOf(three).split(",").length);
print("diverge order", keysOf(three).split(",").slice(13, 18).join(","));

// A frozen large object refuses writes and additions silently in sloppy code.
var frozen = make(25, "x");
Object.freeze(frozen);
frozen.s3 = "changed";
frozen.added = 1;
delete frozen.s4;
print("frozen", frozen.s3, frozen.added, frozen.s4, Object.isFrozen(frozen), Object.keys(frozen).length);

// Symbols and strings among many properties.
var symbols = [];
var bySymbol = {};
for (i = 0; i < 20; i++) {
	symbols.push(Symbol("y" + i));
	bySymbol[symbols[i]] = i;
	bySymbol["n" + i] = -i;
}
delete bySymbol[symbols[3]];
print("symbols", bySymbol[symbols[19]], bySymbol[symbols[3]], symbols[3] in bySymbol, Object.getOwnPropertySymbols(bySymbol).length, Object.keys(bySymbol).length);

// A Proxy over a large object reports its target's keys in order and traps the reads.
var traps = 0;
var target = make(40, "end");
delete target.s20;
var proxy = new Proxy(target, {
	get: function (t, k) { traps++; return t[k]; }
});
print("proxy", Object.keys(proxy).length, Object.keys(proxy)[20], proxy.s21, proxy.end, proxy.s20, traps);

// JSON keeps the order after deletions and re-additions.
var json = make(12, null);
delete json.s1;
delete json.s5;
json.s1 = "b";
print("json", JSON.stringify(json));

// Many objects with many layouts, read back.
var layouts = [];
for (i = 0; i < 200; i++) {
	var o = {};
	o["first" + (i % 7)] = i;
	for (var j = 0; j < 12; j++)
		o["f" + ((i + j) % 17)] = j;
	layouts.push(o);
}
var total = 0;
for (i = 0; i < layouts.length; i++)
	total += layouts[i]["first" + (i % 7)] + (layouts[i]["f" + (i % 17)] || 0);
print("layouts", total, Object.keys(layouts[5]).join(","));
