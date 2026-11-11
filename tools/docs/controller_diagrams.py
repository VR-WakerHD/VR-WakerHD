#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
# credit required, see LICENSE-VR.md.
"""The README's controller diagrams (docs/images/controls-*.svg): a Touch-style controller pair, each input labelled
with what it does in that mode. The labels follow docs/VR_CONTROLS.md and runtime/src/vr/vr_game.cpp (right-handed
play; left-handed swaps the hands). Run it again after a control changes:

    python3 tools/docs/controller_diagrams.py
"""
import os
from xml.sax.saxutils import escape

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "images")

W, H = 1200, 700
BG, INK, MUTED, BODY, RING, KEY, ACCENT = "#ffffff", "#1f2933", "#52606d", "#e4e7eb", "#9aa5b1", "#3e4c59", "#0b6e99"

def controller(cx, cy, side):
    """A Touch-style controller seen from above: the body, the tracking ring, stick, two face buttons, the menu
    button (left only), and marks for the trigger (front) and grip (inner side)."""
    m = 1 if side == "L" else -1  # (mirrored: the left controller's stick sits left of its buttons)
    p = []
    p.append(f'<g transform="translate({cx},{cy})">')
    p.append(f'<path d="M {-70*m} -70 Q {-80*m} 40 {-30*m} 120 L {30*m} 120 Q {78*m} 40 {70*m} -70 Q 0 -112 {-70*m} -70 Z" '
             f'fill="{BODY}" stroke="{RING}" stroke-width="3"/>')
    p.append(f'<path d="M {-92*m} -96 A 110 70 0 0 1 {92*m} -96" fill="none" stroke="{RING}" stroke-width="10" '
             f'stroke-linecap="round"/>')
    sx = -28 * m
    p.append(f'<circle cx="{sx}" cy="-40" r="24" fill="{KEY}"/><circle cx="{sx}" cy="-40" r="15" fill="#616e7c"/>')
    for i, (bx, by, name) in enumerate(((34 * m, -64, "Y" if side == "L" else "B"), (40 * m, -24, "X" if side == "L" else "A"))):
        p.append(f'<circle cx="{bx}" cy="{by}" r="15" fill="{KEY}"/>'
                 f'<text x="{bx}" y="{by+6}" font-size="16" font-weight="700" fill="#fff" text-anchor="middle">{name}</text>')
    if side == "L":
        p.append(f'<rect x="-12" y="16" width="16" height="10" rx="5" fill="{KEY}"/>')
    p.append(f'<rect x="-10" y="-116" width="20" height="14" rx="5" fill="{ACCENT}"/>')  # trigger (front)
    p.append(f'<rect x="{(-66 if side == "L" else 54)}" y="-34" width="12" height="40" rx="5" fill="{ACCENT}"/>')  # grip
    p.append('</g>')
    return "".join(p)


def anchor(cx, cy, side, key):
    """Where an input's leader line ends: on the input's edge facing its labels (the left controller's labels are on
    the left, the right's on the right), so the dot never covers a button's letter."""
    m = 1 if side == "L" else -1
    out = -1 if side == "L" else 1  # (towards the labels)
    pts = {"trigger": (0, -110), "grip": (-60 * m, -14), "stick": (-28 * m + 18 * out, -54),
           "click": (-28 * m + 14 * out, -40), "down": (-28 * m + 10 * out, -20), "upper": (34 * m + 15 * out, -64),
           "lower": (40 * m + 15 * out, -24), "menu": (-4 + 9 * out, 21)}
    x, y = pts[key]
    return cx + x, cy + y


def wrap(text, width=36):
    words, lines, cur = text.split(), [], ""
    for w in words:
        if len(cur) + len(w) + 1 > width and cur:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    if cur:
        lines.append(cur)
    return lines


def diagram(title, subtitle, left, right, notes, path):
    cxL, cxR, cy = 410, 790, 360
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
           f'font-family="Segoe UI, Helvetica, Arial, sans-serif">',
           f'<rect width="{W}" height="{H}" rx="18" fill="{BG}"/>',
           f'<text x="{W/2}" y="44" font-size="28" font-weight="700" fill="{INK}" text-anchor="middle">{escape(title)}</text>',
           f'<text x="{W/2}" y="74" font-size="16" fill="{MUTED}" text-anchor="middle">{escape(subtitle)}</text>',
           f'<text x="{cxL}" y="{cy+160}" font-size="17" font-weight="700" fill="{INK}" text-anchor="middle">Left: shield hand</text>',
           f'<text x="{cxR}" y="{cy+160}" font-size="17" font-weight="700" fill="{INK}" text-anchor="middle">Right: sword hand</text>',
           controller(cxL, cy, "L"), controller(cxR, cy, "R")]
    for side, items, cx in (("L", left, cxL), ("R", right, cxR)):
        items = sorted(items, key=lambda it: anchor(cx, cy, side, it[0])[1])  # (top to bottom: no crossing lines)
        n = len(items)
        top, gap = 116, (H - 170 - 116) / max(n - 1, 1)
        for i, (key, label, text) in enumerate(items):
            ax, ay = anchor(cx, cy, side, key)
            ty = top + i * gap
            tx = 28 if side == "L" else W - 28
            lx = 262 if side == "L" else W - 262
            anc = "start" if side == "L" else "end"
            out.append(f'<polyline points="{ax},{ay} {lx},{ty-6}" fill="none" stroke="{RING}" stroke-width="1.5"/>')
            out.append(f'<circle cx="{ax}" cy="{ay}" r="4" fill="{ACCENT}"/>')
            out.append(f'<text x="{tx}" y="{ty-12}" font-size="15" font-weight="700" fill="{ACCENT}" text-anchor="{anc}">{escape(label)}</text>')
            for j, line in enumerate(wrap(text)):
                out.append(f'<text x="{tx}" y="{ty+8+j*18}" font-size="15" fill="{INK}" text-anchor="{anc}">{escape(line)}</text>')
    for i, note in enumerate(notes):
        out.append(f'<text x="{W/2}" y="{H-62+i*22}" font-size="15" fill="{MUTED}" text-anchor="middle">{escape(note)}</text>')
    out.append('</svg>')
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


COMMON_NOTES = ["Hold both stick clicks 0.6 s to recentre. Left-handed (vr.leftHanded=1) swaps the hands.",
                "Defaults; rebind in the VR launcher's Controllers tab."]

FIRST = (
    "First person", "The view at Link's eyes; your hands are Link's hands",
    [("trigger", "Trigger", "Lock on (ZL)"),
     ("grip", "Grip", "Shield raised: block (ZR). Over the shoulder: item wheel"),
     ("stick", "Stick", "Move (forward is where you look)"),
     ("click", "Stick click", "VR settings menu"),
     ("upper", "Y", "Item on Y"),
     ("lower", "X", "Item on X"),
     ("menu", "Menu", "Pause menu (+)")],
    [("trigger", "Trigger", "Empty hands: item wheel. Aimed items: fire"),
     ("grip", "Grip", "Item on R. Over the shoulder: draw the sword"),
     ("stick", "Stick", "Turn (smooth or snap)"),
     ("down", "Stick down (hold)", "Crouch / shield (ZR)"),
     ("click", "Stick click", "First / third person"),
     ("upper", "B", "Sword (swinging works too)"),
     ("lower", "A", "Talk, open, climb, sidle; parry when prompted")],
    COMMON_NOTES)

THIRD = (
    "Third person", "The game's own camera, turned by your head; no motion controls",
    [("trigger", "Trigger", "Lock on (ZL). Bow out: hold to draw, let go to shoot"),
     ("grip", "Grip (hold)", "Block (ZR)"),
     ("stick", "Stick", "Move. With the right grip held: the D-pad"),
     ("click", "Stick click", "VR settings menu"),
     ("upper", "Y", "Item on Y"),
     ("lower", "X", "Item on X"),
     ("menu", "Menu", "Pause menu (+)")],
    [("trigger", "Trigger", "Item on R. Bow out: hold to draw, let go to shoot"),
     ("grip", "Grip (hold)", "Turns the left stick into the D-pad"),
     ("stick", "Stick", "Turn the camera"),
     ("down", "Stick down (hold)", "Crouch / block (ZR)"),
     ("click", "Stick click", "First / third person"),
     ("upper", "B", "Sword"),
     ("lower", "A", "Talk, open, climb, roll")],
    COMMON_NOTES)

BOAT = (
    "Sailing the King of Red Lions", "In every view on the boat; the stick click cycles the views",
    [("trigger", "Trigger", "Lock on (ZL). Free Roam camera: zoom in"),
     ("grip", "Grip (hold)", "Turns the right stick into the D-pad"),
     ("stick", "Stick", "Steer. With the right grip held: the D-pad"),
     ("click", "Stick click", "VR settings menu"),
     ("upper", "Y", "Item on Y (cannon, grappling hook...)"),
     ("lower", "X", "Item on X"),
     ("menu", "Menu", "Pause menu (+)")],
    [("trigger", "Trigger", "Third person: item on R. Free Roam: zoom out"),
     ("grip", "Grip (hold)", "Turns the left stick into the D-pad"),
     ("stick", "Stick", "Look around. Free Roam: slide the camera"),
     ("down", "Stick down", "Camera only (no crouch on the boat)"),
     ("click", "Stick click", "View: first, third, figurehead, Free Roam"),
     ("upper", "B", "Sword"),
     ("lower", "A", "Hop while sailing; talk, get off")],
    COMMON_NOTES)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, d in (("first-person", FIRST), ("third-person", THIRD), ("boat", BOAT)):
        path = os.path.join(OUT, "controls-%s.svg" % name)
        diagram(*d, path)
        print("wrote", os.path.relpath(path))


if __name__ == "__main__":
    main()
