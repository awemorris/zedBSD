// Enough garbage to run the collector many times, while live objects (a list, a map, closures, strings
// and typed arrays) are kept and read back after every round.
var keep = null;
var kept = 0;
var map = new Map();
var closures = [];
function make(i) {
	return { index: i, name: "item" + i, list: [i, i + 1, i + 2], next: null };
}
for (var round = 0; round < 40; round++) {
	for (var i = 0; i < 20000; i++) {
		var o = make(round * 20000 + i);
		var s = o.name + ":" + o.list.join(",");
		if (i % 1000 === 0) {
			o.next = keep;
			keep = o;
			kept++;
			map.set(s, o);
			closures.push((function (value) { return function () { return value; }; })(o.index));
		}
	}
	var bytes = new Uint8Array(4096);
	bytes[round] = round;
}

// The list, the map and the closures still hold what was put in them.
var count = 0;
var sum = 0;
var bad = 0;
for (var node = keep; node !== null; node = node.next) {
	count++;
	sum += node.index;
	if (node.name !== "item" + node.index || node.list[2] !== node.index + 2)
		bad++;
}
var mapped = 0;
map.forEach(function (value, key) {
	if (key === value.name + ":" + value.list.join(","))
		mapped++;
});
var called = 0;
for (var c = 0; c < closures.length; c++)
	called += closures[c]();
print("gc", kept, count, sum, bad, map.size, mapped, called === sum);

// Strings built and dropped, and one kept.
var text = "";
for (var r = 0; r < 2000; r++) {
	var dropped = new Array(50).join("x" + r);
	if (r % 100 === 0)
		text += dropped.length + ";";
}
print("strings", text);
