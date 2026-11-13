#!/usr/bin/env python3
"""Draws the cockpit's work map (plan/cockpit-map.svg) from map.json.

The map shows Objective -> Milestone -> WS / Bug -> Phase -> owner on a
white background with frosted-glass cards.  Edit map.json and run:

    python3 -I plan/tools/cockpit-map/make-map.py
"""

import json
import os
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DATA = os.path.join(HERE, "map.json")
OUT = os.path.join(ROOT, "plan", "cockpit-map.svg")

FONT = "'Hiragino Sans','Noto Sans CJK JP','Noto Sans JP','Yu Gothic',sans-serif"
MONO = "'SF Mono','JetBrains Mono','Noto Sans Mono',monospace"

# State colours (system-like accents).
COLOURS = {
	"done": "#30b45a",
	"run": "#f29a0c",
	"plan": "#8e8e93",
	"bug": "#ff453a",
	"agy": "#6a5cff",
}

W = 1840
COL_OBJ = 40
COL_MS = 300
COL_WS = 660
COL_PH = 1060
COL_OWN = 1500
OBJ_W = 220
MS_W = 320
WS_W = 340
PH_W = 390
OWN_W = 290
ROW = 40
CARD_H = 32


def text(x, y, s, size=14, weight=400, fill="#1d1d1f", family=FONT, anchor="start"):
	return (f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" '
		f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{escape(s)}</text>')


def glass(x, y, w, h, accent=None, r=14):
	"""A frosted card: translucent white, a light rim and a top sheen."""
	parts = [
		f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="url(#glassFill)" '
		f'stroke="url(#glassRim)" stroke-width="1.2" filter="url(#shadow)"/>',
		f'<rect x="{x + 1}" y="{y + 1}" width="{w - 2}" height="{h / 2}" rx="{r - 1}" fill="url(#sheen)"/>',
	]
	if accent:
		parts.append(f'<rect x="{x + 6}" y="{y + 7}" width="4" height="{h - 14}" rx="2" fill="{accent}"/>')
	return "".join(parts)


def dot(x, y, colour):
	return f'<circle cx="{x}" cy="{y}" r="5" fill="{colour}"/>'


def curve(x1, y1, x2, y2, colour, dashed=False):
	mx = (x1 + x2) / 2
	dash = ' stroke-dasharray="4 4"' if dashed else ""
	return (f'<path d="M{x1},{y1} C{mx},{y1} {mx},{y2} {x2},{y2}" fill="none" '
		f'stroke="{colour}" stroke-opacity="0.55" stroke-width="1.6"{dash}/>')


def main():
	with open(DATA, encoding="utf-8") as f:
		data = json.load(f)

	out = []
	top = 250

	# Rows of the WS column and the phases beside them.
	y = top + 50
	ws_boxes = []
	for g in data["groups"]:
		n = max(1, len(g["phases"]))
		h = n * ROW - (ROW - CARD_H)
		ws_boxes.append((g, y, h))
		y += h + 16
	body_bottom = y

	# Milestones spread over the same height.
	ms_y = {}
	ms_list = data["milestones"]
	step = max(56, (body_bottom - top - 50) / len(ms_list))
	for i, (mid, _name, _n) in enumerate(ms_list):
		ms_y[mid] = top + 50 + i * step
	obj_step = 66

	height = int(max(body_bottom, top + 50 + len(ms_list) * step) + 260)

	out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{height}" viewBox="0 0 {W} {height}">')
	out.append("""<defs>
<linearGradient id="glassFill" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#ffffff" stop-opacity="0.78"/>
  <stop offset="1" stop-color="#ffffff" stop-opacity="0.52"/>
</linearGradient>
<linearGradient id="glassRim" x1="0" y1="0" x2="1" y2="1">
  <stop offset="0" stop-color="#ffffff" stop-opacity="1"/>
  <stop offset="1" stop-color="#c7cbd6" stop-opacity="0.7"/>
</linearGradient>
<linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#ffffff" stop-opacity="0.6"/>
  <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
</linearGradient>
<filter id="shadow" x="-10%" y="-30%" width="120%" height="170%">
  <feDropShadow dx="0" dy="3" stdDeviation="5" flood-color="#5b6478" flood-opacity="0.14"/>
</filter>
<filter id="blur"><feGaussianBlur stdDeviation="70"/></filter>
</defs>""")
	# White page with soft colour washes behind the glass.
	out.append(f'<rect width="{W}" height="{height}" fill="#ffffff"/>')
	out.append('<g filter="url(#blur)" opacity="0.55">'
		f'<circle cx="300" cy="420" r="260" fill="#bfe3ff"/>'
		f'<circle cx="1000" cy="300" r="300" fill="#e3d9ff"/>'
		f'<circle cx="1550" cy="700" r="280" fill="#ffe2c4"/>'
		f'<circle cx="700" cy="{height - 300}" r="300" fill="#c9f2dc"/>'
		'</g>')

	out.append(text(40, 62, data["title"], 32, 700))
	out.append(text(40, 100, data["subtitle"], 16, 400, "#3a3a3c"))
	out.append(text(40, 134, data["asof"], 13, 400, "#6e6e73", MONO))
	lx = 40
	for key, label in (("done", "cleared・fixed・completed"), ("run", "in-progress"),
			("plan", "planned・次"), ("bug", "open の Bug"), ("agy", "Gemini（agy）")):
		out.append(dot(lx + 6, 170, COLOURS[key]))
		out.append(text(lx + 18, 175, label, 14, 400, "#3a3a3c"))
		lx += 40 + len(label) * 12

	out.append(glass(24, top - 30, W - 48, body_bottom - top + 50, r=22))
	for x, label in ((COL_OBJ + 20, "OBJECTIVE"), (COL_MS, "MILESTONE（未完の WS の数）"),
			(COL_WS, "WS・BUG"), (COL_PH, "PHASE"), (COL_OWN, "担当・QUEUE")):
		out.append(text(x, top + 10, label, 13, 700, "#6e6e73", MONO))

	# Objectives.
	for i, (oid, name) in enumerate(data["objectives"]):
		oy = top + 50 + i * obj_step
		out.append(glass(COL_OBJ + 20, oy, OBJ_W, 50))
		out.append(text(COL_OBJ + 34, oy + 21, oid, 13, 700, "#1d1d1f", MONO))
		out.append(text(COL_OBJ + 34, oy + 40, name, 13, 400, "#3a3a3c"))
		for mid in ms_y:
			out.append(curve(COL_OBJ + 20 + OBJ_W, oy + 25, COL_MS, ms_y[mid] + 25, "#b8bcc8"))

	# Milestones.
	for mid, name, n in ms_list:
		my = ms_y[mid]
		out.append(glass(COL_MS, my, MS_W, 50))
		out.append(text(COL_MS + 14, my + 21, mid, 13, 700, "#1d1d1f", MONO))
		out.append(text(COL_MS + 14, my + 40, name, 13, 400, "#3a3a3c"))
		out.append(text(COL_MS + MS_W - 14, my + 22, str(n), 14, 600, "#1d1d1f", anchor="end"))

	# WS and phases.
	for g, gy, gh in ws_boxes:
		accent = COLOURS.get(g["state"], COLOURS["plan"])
		out.append(curve(COL_MS + MS_W, ms_y[g["ms"]] + 25, COL_WS, gy + gh / 2, accent))
		out.append(glass(COL_WS, gy, WS_W, gh, accent))
		out.append(text(COL_WS + 20, gy + 21, g["id"], 14, 700, "#1d1d1f", MONO))
		out.append(text(COL_WS + 20 + 92, gy + 21, g["name"], 13, 400, "#3a3a3c"))
		for i, (pid, pname, pstate, owner) in enumerate(g["phases"]):
			py = gy + i * ROW
			colour = COLOURS.get(pstate, COLOURS["plan"])
			out.append(curve(COL_WS + WS_W, gy + 16, COL_PH, py + 16, colour, pstate == "plan"))
			out.append(glass(COL_PH, py, PH_W, CARD_H, r=12))
			out.append(dot(COL_PH + 16, py + 16, colour))
			out.append(text(COL_PH + 30, py + 21, pid, 13, 700, "#1d1d1f", MONO))
			out.append(text(COL_PH + 30 + 9 * max(6, len(pid)) + 10, py + 21, pname, 13, 400, "#3a3a3c"))
			if owner:
				who, what = owner
				oc = COLOURS["agy"] if who == "agy" else colour
				out.append(curve(COL_PH + PH_W, py + 16, COL_OWN, py + 16, oc))
				out.append(glass(COL_OWN, py, OWN_W, CARD_H, r=16))
				out.append(dot(COL_OWN + 16, py + 16, oc))
				out.append(text(COL_OWN + 30, py + 21, who, 13, 700, "#1d1d1f", MONO))
				out.append(text(COL_OWN + 80, py + 21, what, 13, 400, "#3a3a3c"))

	# Idle owners below the owners' column.
	iy = body_bottom - 40 - len(data["idle"]) * ROW
	for who, what in data["idle"]:
		out.append(glass(COL_OWN, iy, OWN_W, CARD_H, r=16))
		out.append(text(COL_OWN + 16, iy + 21, who, 13, 700, "#6e6e73", MONO))
		out.append(text(COL_OWN + 60, iy + 21, what, 13, 400, "#6e6e73"))
		iy += ROW

	# Bottom panels.
	py = body_bottom + 40
	pw = (W - 48 - 40) / 3
	for i, (title, lines) in enumerate(data["panels"]):
		px = 24 + i * (pw + 20)
		out.append(glass(px, py, pw, 170, r=20))
		out.append(text(px + 22, py + 34, title, 15, 700, "#1d1d1f"))
		for j, line in enumerate(lines):
			out.append(text(px + 30, py + 68 + j * 30, "• " + line, 15, 400, "#3a3a3c"))

	out.append("</svg>")
	with open(OUT, "w", encoding="utf-8") as f:
		f.write("\n".join(out) + "\n")
	print(f"make-map: wrote {os.path.relpath(OUT, ROOT)} ({W}x{height})")


if __name__ == "__main__":
	sys.exit(main())
