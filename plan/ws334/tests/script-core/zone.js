// Local times and the zone's name in the string forms (the runner sets the time zone).
var forms = [new Date(0), new Date(Date.UTC(2021, 6, 1, 12, 0, 0)), new Date(Date.UTC(2021, 0, 15, 3, 4, 5))];
for (var i = 0; i < forms.length; i++) {
	var d = forms[i];
	print(d.toString());
	print(d.toTimeString(), d.getTimezoneOffset(), d.getHours());
}
print(new Date(2021, 2, 14, 12).toString());
print(Math.round(Math.PI * 1e6), Math.hypot(3, 4), Math.sign(-2), Math.trunc(-4.7), Math.min(3, 7), Math.fround(5.5));
print(JSON.stringify({ a: [1, "two", null, true], b: { c: 1.5e300 } }), JSON.parse("[1,{\"x\":2}]")[1].x);
print("2021-07-01".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"), /\bfoo\b/i.test("a FOO b"), "a1b22c333".split(/\d+/).join("|"));
Promise.resolve(1).then(function (v) { print("promise", v); });
