// The Reflect namespace: its 13 functions over ordinary objects and Proxy.

function errorName(e) {
	var name = "";
	if (typeof e === "object")
		return e.name;
	for (var i = 0; i < e.length && e[i] !== ":"; i++)
		name += e[i];
	return name;
}

function attempt(f) {
	try {
		return String(f());
	} catch (e) {
		return errorName(e);
	}
}

// The namespace itself.
print("namespace", typeof Reflect, Object.prototype.toString.call(Reflect), Object.getPrototypeOf(Reflect) === Object.prototype);
var globalDescriptor = Object.getOwnPropertyDescriptor(this, "Reflect");
print("global", globalDescriptor.writable, globalDescriptor.enumerable, globalDescriptor.configurable);
var names = Object.getOwnPropertyNames(Reflect).sort();
print("names", names.join(","));
var lengths = [];
for (var i = 0; i < names.length; i++)
	lengths.push(names[i] + "=" + Reflect[names[i]].length);
print("lengths", lengths.join(","));
var tagDescriptor = Object.getOwnPropertyDescriptor(Reflect, Symbol.toStringTag);
print("tag", tagDescriptor.value, tagDescriptor.writable, tagDescriptor.enumerable, tagDescriptor.configurable);
print("not-constructors", attempt(function () { return new Reflect.get({}, "a"); }), attempt(function () { return new Reflect(); }));

// apply: this and the array-like arguments.
function sum() {
	var total = this.base;
	for (var i = 0; i < arguments.length; i++)
		total += arguments[i];
	return total;
}
print("apply", Reflect.apply(sum, { base: 10 }, [1, 2, 3]), Reflect.apply(sum, { base: 1 }, { length: 2, 0: 5, 1: 6 }));
print("apply-errors", attempt(function () { return Reflect.apply(1, null, []); }), attempt(function () { return Reflect.apply(sum, null); }), attempt(function () { return Reflect.apply(sum, null, 3); }));

// construct: newTarget's prototype, and the errors.
function Point(x, y) {
	this.x = x;
	this.y = y;
}
function Other() {
}
Other.prototype.kind = "other";
var point = Reflect.construct(Point, [3, 4]);
print("construct", point instanceof Point, point.x, point.y);
var made = Reflect.construct(Point, [1, 2], Other);
print("construct-new-target", made instanceof Other, made.kind, made.x);
class Base {
	constructor() {
		this.target = new.target === Other;
	}
}
print("construct-class", Reflect.construct(Base, [], Other).target);
var array = Reflect.construct(Array, [3], Other);
print("construct-builtin", Array.isArray(array), array.length, Object.getPrototypeOf(array) === Other.prototype);
print("construct-errors", attempt(function () { return Reflect.construct(function () {}.bind(), [], Math.max); }), attempt(function () { return Reflect.construct(Point, [], undefined); }), attempt(function () { return Reflect.construct(() => 1, []); }));

// defineProperty: true or false instead of an exception.
var sealed = Object.freeze({ fixed: 1 });
var open = {};
print("defineProperty", Reflect.defineProperty(open, "a", { value: 1, enumerable: true }), open.a, Reflect.defineProperty(sealed, "fixed", { value: 2 }), Reflect.defineProperty(sealed, "other", { value: 3 }));
print("defineProperty-errors", attempt(function () { return Reflect.defineProperty(1, "a", {}); }), attempt(function () { return Reflect.defineProperty({}, "a", 1); }));

// deleteProperty.
var deletable = { a: 1 };
Object.defineProperty(deletable, "fixed", { value: 1 });
print("deleteProperty", Reflect.deleteProperty(deletable, "a"), "a" in deletable, Reflect.deleteProperty(deletable, "fixed"), Reflect.deleteProperty(deletable, "missing"));

// get and set with a receiver.
var accessors = {
	get value() {
		return this.stored;
	},
	set value(v) {
		this.stored = v * 2;
	}
};
var receiver = { stored: "receiver" };
print("get", Reflect.get({ a: 1 }, "a"), Reflect.get(accessors, "value", receiver), Reflect.get([5, 6], 1), Reflect.get({}, "missing"));
print("set", Reflect.set(accessors, "value", 4, receiver), receiver.stored, Reflect.set(sealed, "fixed", 9), sealed.fixed);
var target = {};
print("set-receiver", Reflect.set({}, "plain", 1, target), target.plain, Object.keys(target).join(","));
print("get-errors", attempt(function () { return Reflect.get(null, "a"); }), attempt(function () { return Reflect.set("text", "a", 1); }));

// getOwnPropertyDescriptor without converting the target.
var described = Reflect.getOwnPropertyDescriptor({ a: 1 }, "a");
print("getOwnPropertyDescriptor", described.value, described.writable, described.enumerable, described.configurable, Reflect.getOwnPropertyDescriptor({}, "a"));
print("getOwnPropertyDescriptor-errors", attempt(function () { return Reflect.getOwnPropertyDescriptor("text", "length"); }));

// The prototype, extensibility and has.
var proto = { inherited: 1 };
var child = Object.create(proto);
print("getPrototypeOf", Reflect.getPrototypeOf(child) === proto, Reflect.getPrototypeOf(Object.create(null)));
print("setPrototypeOf", Reflect.setPrototypeOf(child, null), Reflect.getPrototypeOf(child), Reflect.setPrototypeOf(sealed, {}), Reflect.setPrototypeOf(proto, proto));
print("setPrototypeOf-errors", attempt(function () { return Reflect.setPrototypeOf({}, 1); }), attempt(function () { return Reflect.setPrototypeOf(1, null); }));
print("has", Reflect.has(Object.create(proto), "inherited"), Reflect.has({}, "inherited"), Reflect.has([1], 0));
var fixed = {};
print("extensible", Reflect.isExtensible(fixed), Reflect.preventExtensions(fixed), Reflect.isExtensible(fixed), Reflect.isExtensible(sealed));
print("extensible-errors", attempt(function () { return Reflect.isExtensible(1); }), attempt(function () { return Reflect.preventExtensions(1); }));

// ownKeys: indices, then strings, then symbols.
var symbol = Symbol("s");
var keyed = { b: 1, 2: 1, a: 1, 1: 1 };
keyed[symbol] = 1;
Object.defineProperty(keyed, "hidden", { value: 1 });
var ownKeys = Reflect.ownKeys(keyed);
print("ownKeys", ownKeys.length, ownKeys.slice(0, 5).join(","), typeof ownKeys[0], ownKeys[5] === symbol);
print("ownKeys-array", Reflect.ownKeys([7, 8]).join(","));

// The key is converted once, after the target is checked.
var conversions = 0;
var key = {
	toString: function () {
		conversions++;
		return "a";
	}
};
Reflect.get({ a: 1 }, key);
Reflect.has({ a: 1 }, key);
print("key-conversion", conversions, attempt(function () { return Reflect.get(1, key); }), conversions);

// Proxy traps run for each function.
var log = [];
var handler = {};
["apply", "construct", "defineProperty", "deleteProperty", "get", "getOwnPropertyDescriptor", "getPrototypeOf", "has", "isExtensible", "ownKeys", "preventExtensions", "set", "setPrototypeOf"].forEach(function (name) {
	handler[name] = function () {
		log.push(name);
		return Reflect[name].apply(null, arguments);
	};
});
var proxied = new Proxy(function () {
	return "called";
}, handler);
Reflect.apply(proxied, null, []);
Reflect.construct(proxied, []);
Reflect.defineProperty(proxied, "x", { value: 1, configurable: true, writable: true });
Reflect.get(proxied, "x");
Reflect.set(proxied, "x", 2);
Reflect.has(proxied, "x");
Reflect.getOwnPropertyDescriptor(proxied, "x");
Reflect.deleteProperty(proxied, "x");
Reflect.ownKeys(proxied);
Reflect.getPrototypeOf(proxied);
Reflect.setPrototypeOf(proxied, Function.prototype);
Reflect.isExtensible(proxied);
Reflect.preventExtensions(proxied);
print("proxy", log.join(","));
