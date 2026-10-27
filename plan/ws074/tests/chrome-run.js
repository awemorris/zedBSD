#!/usr/bin/env node
// zedBSD
// Copyright (C) 2026 Awe Morris
// SPDX-License-Identifier: Zlib
//
// ws074: answers browser's headless command lines with Chromium (Playwright's Chrome for
// Testing headless shell), so the WPT runners can measure Chromium on the same inputs:
//
//   chrome-run.js --run [--settle-ms=N] [--width=W] [--height=H] [font options] URL
//       prints every console message of the page to stdout, one per line, as browser --run
//       does; returns when testharness's completion line has been printed (and a short
//       grace for late callbacks), or after CHROME_RUN_WAIT_MS (default 15000) milliseconds.
//   chrome-run.js --render --output=OUT.ppm [--width=W] [--height=H] [font options] FILE|URL
//       writes a binary PPM of the viewport after the page's load, fonts and two frames.
//   chrome-run.js --server FILE
//       starts a shared Chromium and writes its endpoint to FILE (see main).
//   chrome-run.js URL OUT.ppm MESSAGE FONT MONO FALLBACK WIDTH HEIGHT DEADLINE
//       wpt-ready-render's command line: prints the console to stdout and captures the viewport
//       when the console prints MESSAGE exactly, then reports READY_CAPTURE on stderr.
//
// The font options of browser are accepted and ignored: Chromium draws with the fonts of
// build/ws074-chrome/fonts.conf (chrome-fonts.sh), which are the same faces.

"use strict";

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");
const { chromium } = require("playwright");

const ROOT = path.resolve(__dirname, "../../..");
const COMPLETE = "WS074_WPT_COMPLETE ";

function parse(argv) {
	if (argv.length === 9 && !argv[0].startsWith("--")) {
		return { mode: "ready", target: argv[0], output: argv[1], message: argv[2],
			width: parseInt(argv[6], 10), height: parseInt(argv[7], 10), deadline: parseInt(argv[8], 10) };
	}
	const options = { mode: null, width: 800, height: 600, output: null, target: null, settle: 5000 };
	for (const argument of argv) {
		if (argument === "--run" || argument === "--render") {
			options.mode = argument.slice(2);
		} else if (argument.startsWith("--output=")) {
			options.output = argument.slice(9);
		} else if (argument.startsWith("--width=")) {
			options.width = parseInt(argument.slice(8), 10);
		} else if (argument.startsWith("--height=")) {
			options.height = parseInt(argument.slice(9), 10);
		} else if (argument.startsWith("--settle-ms=")) {
			options.settle = parseInt(argument.slice(12), 10);
		} else if (argument.startsWith("--")) {
			// --font, --mono-font, --fallback-font, --async: not needed by Chromium.
		} else {
			options.target = argument;
		}
	}
	if (!options.mode || !options.target || (options.mode === "render" && !options.output)) {
		process.stderr.write("usage: chrome-run.js --run|--render [--output=OUT.ppm] URL\n");
		process.exit(2);
	}
	if (!/^[a-z]+:/.test(options.target)) {
		options.target = "file://" + path.resolve(options.target);
	}
	return options;
}

function writePpm(file, png) {
	// The runners read PPM; PIL (python3-pil, which they need too) converts the PNG.
	const result = spawnSync("python3", ["-I", "-c",
		"import sys\nfrom PIL import Image\nImage.open(sys.argv[1]).convert('RGB').save(sys.argv[2], 'PPM')",
		png, file], { stdio: ["ignore", "inherit", "inherit"] });
	if (result.status !== 0) {
		throw new Error("PPM conversion failed");
	}
}

// The options every Chromium of these runs starts with, and its fonts.
function launchOptions() {
	const fonts = path.join(ROOT, "build/ws074-chrome/fonts.conf");
	const env = Object.assign({}, process.env);
	if (fs.existsSync(fonts)) {
		env.FONTCONFIG_FILE = fonts;
	}
	return {
		env,
		args: ["--hide-scrollbars", "--force-device-scale-factor=1", "--allow-file-access-from-files",
			"--disable-gpu", "--font-render-hinting=none"],
	};
}

async function main() {
	if (process.argv[2] === "--server") {
		// chrome-run.js --server FILE: keeps one Chromium running and writes its endpoint to FILE;
		// runs started with CHROME_RUN_WS set to that endpoint open a fresh context in it
		// instead of starting a browser each (a launch costs about a second).
		const server = await chromium.launchServer(launchOptions());
		fs.writeFileSync(process.argv[3], server.wsEndpoint() + "\n");
		process.stdout.write(server.wsEndpoint() + "\n");
		return;
	}
	const options = parse(process.argv.slice(2));
	const browser = process.env.CHROME_RUN_WS ? await chromium.connect(process.env.CHROME_RUN_WS) :
		await chromium.launch(launchOptions());
	let status = 0;
	try {
		const context = await browser.newContext({ viewport: { width: options.width, height: options.height },
			deviceScaleFactor: 1 });
		const page = await context.newPage();
		if (options.mode === "run") {
			const wait = parseInt(process.env.CHROME_RUN_WAIT_MS || "15000", 10);
			let finish;
			const done = new Promise((resolve) => { finish = resolve; });
			page.on("console", (message) => {
				const text = message.text();
				process.stdout.write(text + "\n");
				if (text.startsWith(COMPLETE)) {
					setTimeout(finish, 200);
				}
			});
			page.on("pageerror", (error) => process.stderr.write("pageerror: " + error.message + "\n"));
			const timer = new Promise((resolve) => setTimeout(resolve, wait));
			await page.goto(options.target, { waitUntil: "commit", timeout: wait }).catch((error) => {
				process.stderr.write("goto: " + error.message + "\n");
			});
			await Promise.race([done, timer]);
		} else if (options.mode === "ready") {
			let finish;
			const done = new Promise((resolve) => { finish = resolve; });
			let acknowledged = false;
			const started = Date.now();
			page.on("console", (message) => {
				const text = message.text();
				process.stdout.write(text + "\n");
				if (text === options.message && !acknowledged) {
					acknowledged = true;
					finish();
				}
			});
			await page.goto(options.target, { waitUntil: "commit", timeout: 20000 });
			await Promise.race([done, new Promise((resolve) => setTimeout(resolve, options.deadline))]);
			if (acknowledged) {
				const png = options.output + ".png";
				await page.screenshot({ path: png });
				writePpm(options.output, png);
				process.stderr.write("READY_CAPTURE budget=" + Math.min(Date.now() - started, options.deadline) +
					" acknowledged=1\n");
			} else {
				status = 1;
			}
		} else {
			// The crash tests' probe reports on the console when the page's own wait is over.
			let finish;
			const ready = new Promise((resolve) => { finish = resolve; });
			page.on("console", (message) => {
				const text = message.text();
				process.stdout.write("console: " + text + "\n");
				if (text.startsWith("WPT_CRASH_READY ") || text.startsWith("WPT_CRASH_WAIT_ERROR ")) {
					finish();
				}
			});
			await page.goto(options.target, { waitUntil: "load", timeout: 20000 });
			const source = await page.content();
			if (source.includes("WPT_CRASH_READY")) {
				await Promise.race([ready, new Promise((resolve) => setTimeout(resolve, options.settle))]);
			}
			await page.evaluate(async () => {
				await document.fonts.ready;
				await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
			});
			const png = options.output + ".png";
			await page.screenshot({ path: png });
			writePpm(options.output, png);
		}
	} catch (error) {
		process.stderr.write("chrome-run: " + error.message + "\n");
		status = 1;
	} finally {
		await browser.close();
	}
	process.exit(status);
}

main();
