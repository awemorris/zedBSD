// for await: async iterators, synchronous iterators and their awaited values, and closing the iterator.

// An object with Symbol.asyncIterator whose next returns promises.
function asyncRange(n, log) {
	var i = 0;
	return {
		[Symbol.asyncIterator]() {
			log.push("open");
			return {
				next() {
					log.push("next " + i);
					if (i < n)
						return Promise.resolve({ value: i++, done: false });
					return Promise.resolve({ value: "ignored", done: true });
				},
				return(v) {
					log.push("return");
					return Promise.resolve({ value: v, done: true });
				}
			};
		}
	};
}

async function plain() {
	var log = [];
	for await (const x of asyncRange(3, log))
		log.push("body " + x);
	print("plain:", log.join(", "));
}

// A synchronous iterable: each value (a promise or not) is awaited.
async function sync() {
	var out = [];
	for await (var v of [1, Promise.resolve(2), 3])
		out.push(v);
	print("sync:", out.join(" "));
}

// break, return and throw close the iterator; an exception from next does not.
async function early() {
	var log = [];
	for await (let x of asyncRange(5, log)) {
		if (x === 1)
			break;
	}
	print("break:", log.join(", "));

	log = [];
	async function returns() {
		for await (let x of asyncRange(5, log)) {
			if (x === 2)
				return "returned " + x;
		}
	}
	print("return:", await returns(), log.join(", "));

	log = [];
	try {
		for await (let x of asyncRange(5, log)) {
			if (x === 0)
				throw new Error("thrown");
		}
	} catch (e) {
		print("throw:", e.message, log.join(", "));
	}

	log = [];
	var failing = {
		[Symbol.asyncIterator]() {
			return {
				next() { log.push("next"); return Promise.reject(new Error("next failed")); },
				return() { log.push("return"); return Promise.resolve({ done: true }); }
			};
		}
	};
	try {
		for await (let x of failing)
			log.push("body " + x);
	} catch (e) {
		print("next fails:", e.message, log.join(", "));
	}
}

// An async generator-like iterator with destructuring, continue and a labelled outer loop.
async function patterns() {
	var out = [];
	var pairs = { [Symbol.asyncIterator]() { var k = 0; return { next() { k++; return Promise.resolve(k <= 4 ? { value: [k, k * k], done: false } : { done: true }); } }; } };
	outer: for (var round = 0; round < 2; round++) {
		for await (const [a, b] of pairs) {
			if (a === 2)
				continue;
			if (a === 4 && round === 0)
				continue outer;
			out.push(round + ":" + a + "=" + b);
		}
	}
	print("patterns:", out.join(" "));
}

// Errors: a value that is not iterable, a result that is not an object.
async function errors() {
	try {
		for await (var x of 5)
			print("never", x);
	} catch (e) {
		print("not iterable:", e instanceof TypeError);
	}
	try {
		var bad = { [Symbol.asyncIterator]() { return { next() { return Promise.resolve(42); } }; } };
		for await (var y of bad)
			print("never", y);
	} catch (e) {
		print("not an object:", e instanceof TypeError);
	}
}

plain().then(sync).then(early).then(patterns).then(errors).then(function () { print("done"); });
