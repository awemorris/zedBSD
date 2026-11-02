// The names functions take from the keys they are defined under (SetFunctionName).

var key = "computed";
var described = Symbol("described");
var bare = Symbol();
function existing() {
}

// Object literals: plain, numeric and computed keys; functions, arrows, classes and methods.
var o = {
	plain: function () {},
	"quoted": () => 1,
	1: function () {},
	0x10: class {},
	[key]: function () {},
	[key + "Arrow"]: () => 1,
	[key + "Class"]: class {},
	[key + "Paren"]: (function () {}),
	[described]: function () {},
	[bare]: function () {},
	[2 + 3]: function () {},
	[key + "Method"]() {},
	method() {},
	7() {},
	*[key + "Generator"]() {},
	async [key + "Async"]() {},
	own: function named() {},
	[key + "Named"]: function inner() {},
	[key + "Existing"]: existing,
	get plainGetter() { return 1; },
	set plainSetter(v) {},
	get [key + "Getter"]() { return 1; },
	set [key + "Setter"](v) {},
	get [Symbol("getter")]() { return 1; },
	get 9() { return 1; }
};
function getter(object, name) {
	return Object.getOwnPropertyDescriptor(object, name).get.name;
}
function setter(object, name) {
	return Object.getOwnPropertyDescriptor(object, name).set.name;
}
print("object", o.plain.name, o.quoted.name, o[1].name, o[16].name);
print("computed", o.computed.name, o.computedArrow.name, o.computedClass.name, o.computedParen.name, o[5].name);
print("symbols", JSON.stringify(o[described].name), JSON.stringify(o[bare].name));
print("methods", o.computedMethod.name, o.method.name, o[7].name, o.computedGenerator.name, o.computedAsync.name);
print("kept", o.own.name, o.computedNamed.name, o.computedExisting.name, existing.name);
print("accessors", getter(o, "plainGetter"), setter(o, "plainSetter"), getter(o, "computedGetter"), setter(o, "computedSetter"),
    getter(o, Object.getOwnPropertySymbols(o)[2]), getter(o, "9"));
var descriptor = Object.getOwnPropertyDescriptor(o.computed, "name");
print("descriptor", descriptor.writable, descriptor.enumerable, descriptor.configurable);

// Classes: methods, statics, accessors, private ones and fields.
var C = class {
	[key]() {}
	static [key + "Static"]() {}
	3() {}
	get [key + "Getter"]() { return 1; }
	static set [key + "Setter"](v) {}
	get plain() { return 1; }
	[described]() {}
	#hidden() {}
	get #secret() { return 1; }
	static names(instance) {
		return [instance.#hidden.name, Object.getOwnPropertyDescriptor ? "private" : ""];
	}
	[key + "Field"] = function () {};
	4 = () => 1;
	plainField = function () {};
	#privateField = function () {};
	privateFieldName() {
		return this.#privateField.name;
	}
};
var instance = new C();
print("class", C.prototype.computed.name, C.computedStatic.name, C.prototype[3].name, getter(C.prototype, "computedGetter"),
    setter(C, "computedSetter"), getter(C.prototype, "plain"), C.prototype[described].name);
print("private", C.names(instance)[0]);
print("fields", instance.computedField.name, instance[4].name, instance.plainField.name, instance.privateFieldName());

