// The parser's way out of a syntax error (setjmp and longjmp) between collections of the heap: the
// values made before, during and after keep their contents.

// A list that lives through the whole run.
var head = null;
for (var i = 0; i < 2000; i++)
	head = { value: i, text: "item " + i, next: head };

// Rounds of garbage, each with code that does not parse (eval and Function), and values kept from each.
var kept = [];
var failures = 0;
var sources = ["var x = ;", "return )", "function (", "{ a: 1, b: }", "((((((((((1)", "let let = 1;", "`${`"];
for (var round = 0; round < 200; round++) {
	var garbage = [];
	for (var j = 0; j < 300; j++)
		garbage.push({ round: round, j: j, s: "g" + round + "-" + j, a: [j, j + 1, j + 2] });
	var source = sources[round % sources.length];
	try {
		if (round % 2 === 0)
			eval(source);
		else
			new Function(source);
	} catch (e) {
		if (e instanceof SyntaxError)
			failures++;
	}
	kept.push(garbage[round % 300].s + ":" + garbage.length);
}

// The deep nesting the parser gives up on, then more garbage.
var deep = "";
for (var k = 0; k < 400; k++)
	deep += "[";
try {
	eval(deep);
} catch (e) {
	failures++;
}
var strings = [];
for (var m = 0; m < 5000; m++)
	strings.push(String(m * 7));

// The list and the kept values.
var sum = 0;
var count = 0;
for (var node = head; node !== null; node = node.next) {
	sum += node.value;
	if (node.text !== "item " + node.value)
		sum = -1e9;
	count++;
}
print("list", count, sum);
print("syntax errors", failures);
print("kept", kept.length, kept[0], kept[199]);
print("strings", strings.length, strings[4999]);
