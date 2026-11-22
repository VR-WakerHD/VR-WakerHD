#!/usr/bin/env python3
"""Builds the project's website (site/: GitHub Pages, served from the public repository's gh-pages branch) from one
template: the home page and the topic pages people search for (the Wind Waker VR mod, features, installation, Quest 3,
PC VR, Linux, FAQ), each with its own title, description, canonical address, Open Graph and structured data, plus the
sitemap and copies of llms.txt and the project's facts (docs/project-info.json). Standard library only.

Facts come from the documentation (docs/INSTALLATION.md, QUEST.md, COMPATIBILITY.md, FEATURES.md, FAQ.md) and describe
the latest release only. Edit the pages here, then run it (python3 tools/docs/build_site.py) and tools/docs/check_docs.py.
See docs/discoverability/DISCOVERABILITY_IMPLEMENTATION_REPORT.md."""
import html
import json
import os
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SITE = "https://vr-wakerhd.github.io/VR-WakerHD/"
REPO = "https://github.com/VR-WakerHD/VR-WakerHD"
BLOB = REPO + "/blob/main/"
RELEASES = REPO + "/releases/latest"
VIDEO = "a98maOC3x24"
THUMB = f"https://img.youtube.com/vi/{VIDEO}/hqdefault.jpg"
LASTMOD = "2026-11-22"
# the search engines' site verification codes (the maintainer's, from Google Search Console's "HTML tag" method and
# Bing Webmaster Tools' "Meta tag" method): put the content="..." value here, build, and publish; empty: no tag
GOOGLE_SITE_VERIFICATION = "JqucIGneuSG-VYe8-rhFanRy0RQ246OTexwxUY-nE3g"
BING_SITE_VERIFICATION = ""
# IndexNow (https://www.indexnow.org: Bing, Yandex, Seznam, Naver): the site's key, served as <key>.txt at the site's
# root, which proves the submissions of its pages are the site's own (tools/docs/indexnow.py sends them)
INDEXNOW_KEY = "263b55a00e1d67ba7f14e61c4603b108"
INFO = json.load(open(os.path.join(ROOT, "docs", "project-info.json"), encoding="utf-8"))
VERSION = INFO["latestRelease"]["version"]

# the navigation: (path under the site, label)
NAV = [("", "Home"), ("wind-waker-vr-mod/", "Wind Waker VR mod"), ("features/", "Features"),
       ("installation/", "Install"), ("quest-3/", "Quest 3"), ("pcvr/", "PC VR"), ("linux/", "Linux"), ("faq/", "FAQ")]

CSS = """:root{--bg:#f7f9fb;--fg:#14202b;--muted:#4b5b69;--accent:#0b6aa2;--card:#ffffff;--line:#dbe3ea}
@media (prefers-color-scheme:dark){:root{--bg:#0f1720;--fg:#e6edf3;--muted:#9fb0bf;--accent:#5cb8ee;--card:#16212c;--line:#263442}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:17px/1.6 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:860px;margin:0 auto;padding:24px 16px 48px}
nav.top{border-bottom:1px solid var(--line);background:var(--card)}
nav.top ul{max-width:860px;margin:0 auto;padding:10px 16px;list-style:none;display:flex;flex-wrap:wrap;gap:6px 16px}
nav.top a{text-decoration:none;font-weight:600;font-size:.95rem}
nav.top a[aria-current]{color:var(--fg)}
nav.crumbs{font-size:.9rem;color:var(--muted);margin-bottom:8px}
h1{font-size:2rem;line-height:1.2;margin:0 0 12px}
h2{font-size:1.35rem;margin:36px 0 10px}
h3{font-size:1.1rem;margin:24px 0 8px}
.lead{font-size:1.12rem}
.note{background:var(--card);border:1px solid var(--line);border-left:4px solid var(--accent);padding:12px 16px;border-radius:6px}
.btns{display:flex;flex-wrap:wrap;gap:10px;margin:18px 0}
.btn{display:inline-block;padding:10px 16px;border-radius:6px;border:1px solid var(--accent);color:var(--accent);text-decoration:none;font-weight:600}
.btn.primary{background:var(--accent);color:#fff}
a{color:var(--accent)}
.video{display:block;border-radius:8px;overflow:hidden;border:1px solid var(--line)}
.video img{display:block;width:100%;height:auto}
table{border-collapse:collapse;width:100%;font-size:.95rem}
th,td{border:1px solid var(--line);padding:8px 10px;text-align:left;vertical-align:top}
th{background:var(--card)}
.wrap{overflow-x:auto}
code{background:var(--card);border:1px solid var(--line);border-radius:4px;padding:0 4px;font-size:.9em;overflow-wrap:anywhere}
dt{font-weight:700;margin-top:14px}
dd{margin:4px 0 0}
footer{color:var(--muted);font-size:.9rem;margin-top:40px;border-top:1px solid var(--line);padding-top:16px}"""

NOTICE = ('<p class="note"><strong>Unofficial, and no game included.</strong> VR-WakerHD is a fan-made project, not '
          'made by or affiliated with Nintendo. You need your own, legally obtained copy of the Wii U game: setup '
          'builds the game on your own device from your own game files. The downloads contain no game code, game '
          'data or keys.</p>')
BUTTONS = (f'<div class="btns"><a class="btn primary" href="{RELEASES}">Download VR-WakerHD {VERSION}</a>'
           '<a class="btn" href="/VR-WakerHD/installation/">How to install</a>'
           f'<a class="btn" href="{REPO}">Source code on GitHub</a></div>')
FOOTER = (f'<footer><p>VR mode by VR-WakerHD (<a href="https://github.com/VR-WakerHD/">https://github.com/VR-WakerHD/</a>). '
          'Built on <a href="https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp">ZeldaWWHDRecomp</a>. Earlier name: '
          f'ZeldaWWHD-VR. <a href="{REPO}">GitHub</a> · <a href="{REPO}/releases">Releases</a> · '
          f'<a href="{REPO}/issues">Issues</a> · <a href="/VR-WakerHD/llms.txt">llms.txt</a></p>'
          '<p>An unofficial fan project, not affiliated with, endorsed or sponsored by Nintendo. "The Legend of Zelda", '
          '"The Wind Waker", "Wii U" and related names are trademarks of their respective owners and are used only to '
          'describe compatibility.</p></footer>')


def doc(name, anchor=""):
    return BLOB + "docs/" + name + (("#" + anchor) if anchor else "")


def video_block(caption):
    return (f'<a class="video" href="https://youtu.be/{VIDEO}"><img src="{THUMB}" width="480" height="360" '
            'alt="Wind Waker HD VR mod gameplay tutorial video on YouTube (VR-WakerHD)"></a>'
            f'<p>{caption}</p>')


SOFTWARE = {
    "@context": "https://schema.org",
    "@type": "SoftwareApplication",
    "name": "VR-WakerHD",
    "alternateName": ["Wind Waker HD VR Mod", "Wind Waker VR mod", "Wind Waker HD VR", "ZeldaWWHD-VR"],
    "description": INFO["description"],
    "applicationCategory": "GameApplication",
    "applicationSubCategory": "VR mod",
    "operatingSystem": ["Windows", "Linux", "SteamOS", "Android (Meta Quest 3 / 3S)"],
    "url": SITE,
    "downloadUrl": RELEASES,
    "softwareVersion": VERSION,
    "license": "https://www.gnu.org/licenses/gpl-3.0.html",
    "isAccessibleForFree": True,
    "softwareRequirements": ("Your own copy of The Legend of Zelda: The Wind Waker HD (Wii U); for PC VR, an OpenXR "
                             "runtime and a Vulkan GPU"),
    "isBasedOn": {"@type": "SoftwareSourceCode", "name": "ZeldaWWHDRecomp",
                  "codeRepository": "https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp"},
    "author": {"@type": "Organization", "name": "VR-WakerHD", "url": "https://github.com/VR-WakerHD/"},
    "sameAs": [REPO],
}
WEBSITE = {"@context": "https://schema.org", "@type": "WebSite", "name": "VR-WakerHD",
           "alternateName": "Wind Waker HD VR Mod", "url": SITE}
VIDEO_LD = {
    "@context": "https://schema.org", "@type": "VideoObject",
    "name": "The Legend of Zelda Wind Waker HD VR: (VR-WakerHD) Tutorial + Gameplay! #1",
    "description": ("A tutorial of VR-WakerHD's VR controls in the headset, then gameplay from the Great Sea through "
                    "Dragon Roost Cavern, the Forbidden Woods and the Tower of the Gods, by legaiaflame."),
    "thumbnailUrl": THUMB, "uploadDate": "2026-11-20", "duration": "PT51M53S",
    "contentUrl": f"https://youtu.be/{VIDEO}", "embedUrl": f"https://www.youtube.com/embed/{VIDEO}",
}


def page(path, title, description, h1, body, extra_ld=(), heading=None):
    url = SITE + path
    nav = "".join(f'<li><a href="/VR-WakerHD/{p}"{" aria-current=\"page\"" if p == path else ""}>{html.escape(label)}</a></li>'
                  for p, label in NAV)
    ld = list(extra_ld)
    crumbs = ""
    if path:
        name = heading or h1
        ld.append({"@context": "https://schema.org", "@type": "BreadcrumbList", "itemListElement": [
            {"@type": "ListItem", "position": 1, "name": "VR-WakerHD", "item": SITE},
            {"@type": "ListItem", "position": 2, "name": name, "item": url}]})
        crumbs = f'<nav class="crumbs" aria-label="Breadcrumb"><a href="/VR-WakerHD/">VR-WakerHD</a> › {html.escape(name)}</nav>'
    scripts = "".join(f'<script type="application/ld+json">\n{json.dumps(x, indent=2, ensure_ascii=False)}\n</script>\n'
                      for x in ld)
    e = html.escape
    verification = ""
    if not path:  # (the home page carries the verification tags)
        if GOOGLE_SITE_VERIFICATION:
            verification += f'<meta name="google-site-verification" content="{e(GOOGLE_SITE_VERIFICATION)}">\n'
        if BING_SITE_VERIFICATION:
            verification += f'<meta name="msvalidate.01" content="{e(BING_SITE_VERIFICATION)}">\n'
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{e(title)}</title>
<meta name="description" content="{e(description)}">
<link rel="canonical" href="{url}">
<meta name="robots" content="index, follow">
{verification}<meta property="og:type" content="website">
<meta property="og:site_name" content="VR-WakerHD">
<meta property="og:title" content="{e(title)}">
<meta property="og:description" content="{e(description)}">
<meta property="og:url" content="{url}">
<meta property="og:image" content="{THUMB}">
<meta property="og:image:alt" content="Wind Waker HD VR mod gameplay (VR-WakerHD)">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="{e(title)}">
<meta name="twitter:description" content="{e(description)}">
<meta name="twitter:image" content="{THUMB}">
{scripts}<style>
{CSS}
</style>
</head>
<body>
<nav class="top" aria-label="Site"><ul>{nav}</ul></nav>
<main>
{crumbs}<h1>{h1}</h1>
{body}
{FOOTER}
</main>
</body>
</html>
"""


PAGES = []


def add(path, title, description, h1, body, extra_ld=(), heading=None):
    PAGES.append((path, title, description, page(path, title, description, h1, body, extra_ld, heading)))


# ---- home
add("", "Wind Waker HD VR Mod – VR-WakerHD",
    ("Play Zelda: The Wind Waker HD in VR. VR-WakerHD is a free, open-source Wind Waker VR mod with motion controls "
     "and a physical sword, for PC VR and Quest 3."),
    "Wind Waker HD VR Mod: The Legend of Zelda: The Wind Waker HD in virtual reality",
    f"""<p class="lead"><strong>VR-WakerHD</strong> is a free, open-source <strong>Wind Waker VR mod</strong>: a VR version of <em>The Legend of Zelda: The Wind Waker HD</em>. You play in first person through Link's eyes, with head tracking, motion controls, your arms as Link's arms and a sword you swing yourself. It runs on PC VR headsets through OpenXR (Windows and Linux) and standalone on Meta Quest 3 and 3S. It is built on <a href="https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp">ZeldaWWHDRecomp</a>, the native port (recompilation) of the Wii U game.</p>
{NOTICE}
{BUTTONS}
<h2>Watch it played</h2>
{video_block('<a href="https://youtu.be/' + VIDEO + '">The Legend of Zelda Wind Waker HD VR: (VR-WakerHD) Tutorial + Gameplay! #1</a>, by legaiaflame (52 minutes): a four-minute tutorial of every VR control in the headset, then the game played from the Great Sea to the Master Sword.')}
<h2>What the Wind Waker VR mod does</h2>
<ul>
<li><strong>First person at Link's eyes</strong>, in stereo 3D with 6DoF head tracking; third person is a stick click away. <a href="/VR-WakerHD/features/">All features</a></li>
<li><strong>Your arms are Link's arms</strong>: his hands follow your controllers, with natural elbows and shoulders.</li>
<li><strong>Physical sword</strong>: draw it over your shoulder and swing it. The bow, the Skull Hammer and the Wind Waker baton are motion-controlled too.</li>
<li><strong>Item wheel at your shoulder</strong> and the <strong>HUD on Link's wrists</strong>; menus and the GamePad screen on panels in the headset.</li>
<li><strong>True size</strong> (new in 0.6.0): play at your own height instead of Link's.</li>
<li><strong>Comfort and performance</strong>: smooth or snap turning, multiview, foveated rendering on Quest, dynamic resolution and upscaling.</li>
</ul>
<h2>Where it runs</h2>
<div class="wrap"><table>
<tr><th>Platform</th><th>Download</th><th>Status</th><th>Guide</th></tr>
<tr><td>Meta Quest 3 / 3S, standalone</td><td>APK</td><td>Tested</td><td><a href="/VR-WakerHD/quest-3/">Quest 3</a></td></tr>
<tr><td>Windows PC VR</td><td>zip</td><td>Tested with SteamVR; Quest Link and Virtual Desktop supported</td><td><a href="/VR-WakerHD/pcvr/">PC VR</a></td></tr>
<tr><td>Linux PC VR</td><td>AppImage</td><td>Tested: SteamVR and Monado</td><td><a href="/VR-WakerHD/linux/">Linux</a></td></tr>
<tr><td>Steam Frame (SteamOS, arm64)</td><td>AppImage</td><td>Built; not yet confirmed on the device</td><td><a href="/VR-WakerHD/linux/">Linux</a></td></tr>
</table></div>
<p>Headsets tested most: PlayStation VR2 and Meta Quest 3 / 3S. Any headset with an OpenXR runtime that supports Vulkan should work.</p>
<h2>Get started</h2>
<ol>
<li>Download the file for your platform from the <a href="{RELEASES}">latest release</a>.</li>
<li>Start it and choose your own game files; setup builds the game on your device (once).</li>
<li>Press <strong>Play VR</strong> in the VR launcher.</li>
</ol>
<p>Step by step: <a href="/VR-WakerHD/installation/">How to install</a> · Questions: <a href="/VR-WakerHD/faq/">FAQ</a></p>
<h2>Project status</h2>
<p>VR-WakerHD is experimental and in active development; the latest release is <a href="{INFO['latestRelease']['url']}">{html.escape(INFO['latestRelease']['title'])}</a>. Bug reports with a log are welcome in <a href="{REPO}/issues">GitHub Issues</a>. The VR mode is GPL-3.0-or-later with a credit term; the original port's code stays MPL-2.0. The project was called ZeldaWWHD-VR before.</p>""",
    extra_ld=(WEBSITE, SOFTWARE, VIDEO_LD))

# ---- the Wind Waker VR mod
add("wind-waker-vr-mod/", "Wind Waker VR Mod: Play Wind Waker HD in VR – VR-WakerHD",
    ("Is there a Wind Waker VR mod? Yes: VR-WakerHD plays The Wind Waker HD in first-person VR with motion controls. "
     "What it is and how to get it."),
    "Wind Waker VR mod: playing The Wind Waker HD in VR",
    f"""<p class="lead">If you are looking for a <strong>Wind Waker VR mod</strong>, VR-WakerHD is one you can play today: <em>The Legend of Zelda: The Wind Waker HD</em> in VR, from start to finish, in first person with motion controls. It is free, open source and fan-made.</p>
{NOTICE}
<h2>What makes it a VR version, not a screen in a headset</h2>
<p>VR-WakerHD isn't an emulator, and it isn't a VR injector like VorpX. It builds on <a href="https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp">ZeldaWWHDRecomp</a>, which recompiles the Wii U game's code so it runs natively. The VR is built into the renderer and the game's own logic, which is how Link's arms can follow your controllers, his HUD can sit on his wrists, and your sword swings can be his attacks.</p>
<ul>
<li>Stereo 3D with 6DoF head tracking, at Link's eyes or at your own height (True size).</li>
<li>Motion controls through OpenXR: the sword, the bow (with fire, ice and light arrows), the Skull Hammer, the Wind Waker baton, the boomerang and the grappling hook.</li>
<li>The GamePad screen, menus and dialogue on panels in the headset; hearts and rupees on Link's wrists.</li>
<li>Four views while sailing on the Great Sea.</li>
</ul>
<p>Full list: <a href="/VR-WakerHD/features/">Wind Waker VR features</a>.</p>
<h2>Other Wind Waker VR projects</h2>
<p>Earlier Wind Waker VR efforts ran the GameCube game in an emulator: a 2014 Oculus Rift video made with Dolphin, and DolphinXR, a Dolphin build with VR patches for several games. VR-WakerHD is a different approach: the Wii U HD version, natively recompiled, with the VR mode written for this one game.</p>
<h2>What you need</h2>
<ul>
<li>Your own copy of the Wii U game (a disc image with its keys, a Cemu <code>.wua</code>, or an extracted game folder).</li>
<li>Either a PC VR headset with an OpenXR runtime and a Vulkan GPU, or a Meta Quest 3 / 3S in developer mode.</li>
</ul>
{BUTTONS}
<p>Next: <a href="/VR-WakerHD/installation/">install it</a>, or see the guides for <a href="/VR-WakerHD/quest-3/">Quest 3</a>, <a href="/VR-WakerHD/pcvr/">PC VR</a> and <a href="/VR-WakerHD/linux/">Linux</a>.</p>""",
    heading="Wind Waker VR mod")

# ---- features
add("features/", "Wind Waker VR Features: Motion Controls and Sword – VR-WakerHD",
    ("What the Wind Waker HD VR mod adds: first person with head tracking, Link's arms on your controllers, a "
     "physical sword and motion-controlled items."),
    "Wind Waker VR features",
    f"""<p class="lead">Everything VR-WakerHD adds to <em>The Legend of Zelda: The Wind Waker HD</em>, as of version {VERSION}. Features marked <em>experimental</em> are new and less tested.</p>
<h2>First person and head tracking</h2>
<ul>
<li><strong>The view at Link's eyes</strong> in stereo 3D, with full 6DoF head tracking. Leaning is followed, and the world is scaled to Link's arms.</li>
<li><strong>Link turns to face where you look</strong> when you stand still; walking, he faces where he walks.</li>
<li><strong>True size</strong> <em>(experimental)</em>: your eyes at your real height in his world, with only his hands drawn, moving freely with your controllers.</li>
<li><strong>Third person</strong> (right stick click) uses the game's own camera, turned by your head.</li>
<li><strong>Sailing views</strong> on the Great Sea and in the Tower of the Gods.</li>
</ul>
<h2>Motion controls</h2>
<ul>
<li><strong>Your arms are Link's arms</strong>: inverse kinematics with natural elbows and shoulders.</li>
<li><strong>Physical sword</strong>: draw it over your shoulder; full swings and thrusts play the game's attacks, the blade hits along its tracked path, with hit vibration.</li>
<li><strong>Combat</strong>: lock-on with the trigger, automatic parry, and a shield that guards while locked on.</li>
<li><strong>Items in your hands</strong>: the bow (pull the string with your drawing hand; swap fire, ice and light arrows), the Skull Hammer, the Wind Waker baton (conduct with your hand), the boomerang and grappling hook, the telescope and the Picto Box.</li>
<li><strong>Item wheel at your shoulder</strong>: reach over your shield shoulder, choose with the stick, pull the item out.</li>
<li><strong>Left-handed play</strong> swaps the sword and shield hands.</li>
</ul>
<h2>Interface in VR</h2>
<ul>
<li><strong>Wrist HUD</strong>: hearts, items, rupees and magic on Link's wrists.</li>
<li><strong>Menus on panels</strong>: pause, map, bag and the GamePad screen, in front of you.</li>
<li><strong>In-headset settings menu</strong> (left stick click) and a <strong>VR launcher</strong> on Windows, Linux, SteamOS and Quest.</li>
</ul>
<h2>Comfort</h2>
<ul>
<li>Smooth or snap turning, and Move where you look.</li>
<li>Steady hands while turning; depth of field, bloom and ambient occlusion can be turned off.</li>
</ul>
<h2>Graphics and performance</h2>
<ul>
<li><strong>Multiview</strong> (both eyes in one pass) and <strong>foveated rendering</strong> on Quest.</li>
<li><strong>Headset resolution, dynamic resolution and upscalers</strong>: Quest Super Resolution, SGSR 1 and 2, FSR 1, and sharpening with edge smoothing.</li>
<li><strong>Frame rate choices</strong>: refresh-rate selection, capped modes and frame synthesis; Streaming optimal for Virtual Desktop and Air Link <em>(experimental)</em>.</li>
</ul>
<p>Everything the base port offers still works flat-screen through Play Flat. Every setting: <a href="{doc('VR_SETTINGS.md')}">VR_SETTINGS.md</a> · Controls: <a href="{doc('PLAYING.md')}">PLAYING.md</a></p>""",
    heading="Features")

# ---- installation
add("installation/", "How to Install the Wind Waker HD VR Mod – VR-WakerHD",
    ("Install the Wind Waker HD VR mod on Windows, Linux or Meta Quest 3: what you need, which file to download and "
     "the one-time setup with your game."),
    "How to install the Wind Waker HD VR mod",
    f"""<p class="lead">From a release download to playing in your headset, on Windows, Linux, SteamOS and Meta Quest 3 / 3S. You need your own copy of the Wii U game; nothing from the game is in the download.</p>
<h2>What you need</h2>
<ul>
<li><strong>Your own copy of the Wii U game</strong>, dumped from your own disc and console: a <code>.wud</code> / <code>.wux</code> disc image with its disc key and the Wii U common key, a Cemu <code>.wua</code> archive, or an extracted game folder (holding <code>code</code>, <code>content</code> and <code>meta</code>).</li>
<li><strong>For PC VR:</strong> a Vulkan-capable GPU and an active OpenXR runtime (SteamVR, Meta Quest Link, Virtual Desktop, Windows Mixed Reality, or Monado / WiVRn on Linux).</li>
<li><strong>For Quest standalone:</strong> a Meta Quest 3 or 3S in developer mode.</li>
</ul>
<p>The USA version of the game is tested most in VR; the European version is supported by the base port.</p>
<h2>1. Download</h2>
<div class="wrap"><table>
<tr><th>Platform</th><th>File in the <a href="{RELEASES}">latest release</a></th></tr>
<tr><td>Windows PC (x86-64)</td><td><code>VR-WakerHD-Windows-{VERSION}-x86_64.zip</code></td></tr>
<tr><td>Linux PC (x86-64, glibc 2.35+)</td><td><code>VR-WakerHD-Linux-{VERSION}-x86_64.AppImage</code></td></tr>
<tr><td>Steam Frame (SteamOS, arm64)</td><td><code>VR-WakerHD-SteamOS-aarch64-{VERSION}.AppImage</code></td></tr>
<tr><td>Meta Quest 3 / 3S (standalone)</td><td><code>VR-WakerHD-Quest3-{VERSION}.apk</code></td></tr>
<tr><td>What to install on Linux, per distribution</td><td><code>required_packages.txt</code></td></tr>
</table></div>
<h2>2. Start it</h2>
<ul>
<li><strong>Windows:</strong> unzip into a folder of its own (for example <code>C:\\Games\\VR-WakerHD</code>) and start <code>wind-waker-hd-vr.exe</code>.</li>
<li><strong>Linux / SteamOS:</strong> <code>chmod +x VR-WakerHD-*.AppImage</code> and run it; if it doesn't start, install the packages in <code>required_packages.txt</code>. More: <a href="/VR-WakerHD/linux/">Linux guide</a>.</li>
<li><strong>Quest 3 / 3S:</strong> install the APK with SideQuest or <code>adb install</code>, then open it from <em>Unknown sources</em>. More: <a href="/VR-WakerHD/quest-3/">Quest 3 guide</a>.</li>
</ul>
<h2>3. Set up the game (once)</h2>
<p>The installer opens on the first start. Choose your game files; setup extracts the game if needed and <strong>compiles it for your device</strong>: a few minutes on a PC, longer on a headset. What setup makes stays on your device and must not be shared.</p>
<h2>4. Play</h2>
<ol>
<li>On PC, start your OpenXR runtime first and make it the active one (SteamVR: Settings &gt; OpenXR &gt; "Set SteamVR as OpenXR runtime").</li>
<li>In the VR launcher, check the headset, refresh rate, controls and graphics.</li>
<li>Press <strong>Play VR</strong>, or <strong>Play Flat</strong> to play on the monitor.</li>
</ol>
<p>In the headset, click the <strong>left stick</strong> for the settings menu. How to play: <a href="{doc('PLAYING.md')}">controls</a>. Problems: <a href="/VR-WakerHD/faq/">FAQ and troubleshooting</a>.</p>
{BUTTONS}""",
    heading="Install")

# ---- Quest 3
add("quest-3/", "Wind Waker VR on Quest 3 and 3S (Standalone) – VR-WakerHD",
    ("Play the Wind Waker HD VR mod standalone on Meta Quest 3 and 3S, no PC needed: installing the APK, recommended "
     "settings and frame rate fixes."),
    "Wind Waker VR on Meta Quest 3 and 3S",
    f"""<p class="lead">VR-WakerHD runs <em>The Legend of Zelda: The Wind Waker HD</em> in VR <strong>standalone on Meta Quest 3 and Quest 3S</strong>, with no PC. A Quest can also play the PC version over Quest Link or Virtual Desktop: see <a href="/VR-WakerHD/pcvr/">PC VR</a>. Quest 2 and Quest Pro are not tested.</p>
{NOTICE}
<h2>Install on the Quest</h2>
<ol>
<li>Put the headset in <strong>developer mode</strong> (a Meta developer account, then the setting in the Meta Horizon app).</li>
<li>Install <code>VR-WakerHD-Quest3-{VERSION}.apk</code> from the <a href="{RELEASES}">latest release</a> with SideQuest or <code>adb install</code>.</li>
<li>Open it from <em>Unknown sources</em> and choose your game files. The app offers to extract a disc image; setup compiles the game on the headset, which takes a while the first time.</li>
</ol>
<h2>Recommended settings</h2>
<p>The Quest's processor is the limit, so the defaults are tuned for it:</p>
<div class="wrap"><table>
<tr><th>Setting</th><th>Default on Quest</th><th>If it is not smooth</th></tr>
<tr><td>Refresh rate and frame rate</td><td>90 Hz, Capped 30 fps</td><td>72 Hz with 36 fps and frame synthesis on</td></tr>
<tr><td>Headset resolution</td><td>0.70</td><td>Lower it (for example 0.65)</td></tr>
<tr><td>Field of view</td><td>Full</td><td>Lower it: this helps the frame rate most</td></tr>
<tr><td>Upscaling</td><td>Sharpening (CAS) with edge smoothing (FXAA)</td><td>—</td></tr>
<tr><td>Foveated rendering</td><td>On</td><td>—</td></tr>
<tr><td>Frame synthesis</td><td>Auto</td><td>Off if the picture looks smeary or jittery</td></tr>
<tr><td>Shader compile</td><td>Off</td><td>Fast can reduce stutter on first visits</td></tr>
</table></div>
<p>Change them in the VR launcher or in the headset: click the left stick, then use the grips to switch tabs.</p>
<h2>Low frame rate or stutter</h2>
<ul>
<li><strong>The first visit to an area stutters</strong> while its shaders are made; it eases as you play.</li>
<li><strong>Low frame rate throughout:</strong> lower the field of view and the headset resolution, and try 72 Hz with 36 fps.</li>
<li><strong>Hands look out of place</strong> at low frame rates: raising the frame rate is the fix.</li>
</ul>
<h2>Bug reports</h2>
<p>Attach your log, <code>/sdcard/Android/data/org.wwhdrecomp.wwhd/files/captures/wwhd.log</code>, to a <a href="{REPO}/issues/new/choose">bug report</a>. More: <a href="{doc('QUEST.md')}">QUEST.md</a>.</p>
{BUTTONS}""",
    heading="Quest 3")

# ---- PC VR
add("pcvr/", "Wind Waker PC VR: SteamVR, OpenXR and Quest Link – VR-WakerHD",
    ("Play the Wind Waker HD VR mod on PC through OpenXR: SteamVR, Quest Link, Virtual Desktop and Monado. Tested "
     "headsets and PC requirements."),
    "Wind Waker PC VR: SteamVR, OpenXR and Quest Link",
    f"""<p class="lead">On PC, VR-WakerHD uses <strong>OpenXR</strong> with the <strong>Vulkan</strong> renderer, so any headset with an OpenXR runtime that supports Vulkan (<code>XR_KHR_vulkan_enable2</code>) should work. "Tested" means played in a headset by the developer or testers; "supported" means it should work but has had little testing.</p>
<h2>OpenXR runtimes</h2>
<div class="wrap"><table>
<tr><th>Runtime</th><th>Status</th><th>Notes</th></tr>
<tr><td>SteamVR (Windows)</td><td>Tested</td><td>Set it as the active OpenXR runtime</td></tr>
<tr><td>SteamVR (Linux)</td><td>Tested</td><td>Smooth most of the time; see <a href="/VR-WakerHD/linux/">Linux</a></td></tr>
<tr><td>Monado (Linux)</td><td>Tested</td><td>With a PS VR2 headset</td></tr>
<tr><td>Meta Quest Link (wired)</td><td>Tested</td><td>Quest 3 over a Link cable</td></tr>
<tr><td>Virtual Desktop (VDXR)</td><td>Supported, experimental</td><td>Turn on Streaming optimal; try 90 Hz with Capped 45 fps; a cable is steadier than Wi-Fi</td></tr>
<tr><td>Air Link</td><td>Supported, experimental</td><td>As Virtual Desktop</td></tr>
<tr><td>WiVRn (Linux), Windows Mixed Reality</td><td>Supported</td><td>Little testing</td></tr>
</table></div>
<h2>Headsets</h2>
<p>Tested most: <strong>PlayStation VR2</strong> (SteamVR, Monado) and <strong>Meta Quest 3 / 3S</strong> (Link). Supported: Valve Index, HTC Vive, Windows Mixed Reality headsets, and other OpenXR headsets through the standard controller profiles.</p>
<h2>PC requirements</h2>
<ul>
<li>A GPU with a Vulkan driver: Vulkan 1.3, or 1.1 / 1.2 with <code>VK_KHR_dynamic_rendering</code>.</li>
<li>Windows x86-64 or Linux x86-64. macOS has no VR build (VR needs the Vulkan renderer).</li>
<li>Performance depends on the scene and the headset resolution; the render thread (CPU) usually sets the limit more than the GPU.</li>
</ul>
<h2>Set up</h2>
<ol>
<li>Install your runtime and make it the active OpenXR runtime (SteamVR: Settings &gt; OpenXR).</li>
<li>Download the Windows zip or the Linux AppImage and run setup once with your own game files (<a href="/VR-WakerHD/installation/">install guide</a>).</li>
<li>Start the runtime first, then press Play VR in the launcher.</li>
</ol>
<p>Details: <a href="{doc('COMPATIBILITY.md')}">COMPATIBILITY.md</a>.</p>
{BUTTONS}""",
    heading="PC VR")

# ---- Linux
add("linux/", "Wind Waker VR on Linux: SteamVR and Monado – VR-WakerHD",
    ("Play the Wind Waker HD VR mod on Linux: the AppImage with SteamVR, Monado or WiVRn, supported distributions and "
     "troubleshooting."),
    "Wind Waker VR on Linux",
    f"""<p class="lead">Linux is VR-WakerHD's main development platform (Arch Linux / CachyOS). The Linux release is an AppImage for x86-64 PCs, and there is a separate arm64 AppImage for the Steam Frame.</p>
<h2>Requirements</h2>
<ul>
<li>x86-64 Linux with glibc 2.35 or newer: Arch / CachyOS, Fedora 36+, Ubuntu 22.04+, Debian 12+.</li>
<li>A Vulkan GPU driver, and an OpenXR runtime: <strong>SteamVR</strong> or <strong>Monado</strong> (both tested), or WiVRn (supported, little testing).</li>
<li>The packages for your distribution listed in <code>required_packages.txt</code> in the release.</li>
</ul>
<h2>Install and run</h2>
<ol>
<li>Download <code>VR-WakerHD-Linux-{VERSION}-x86_64.AppImage</code> from the <a href="{RELEASES}">latest release</a>.</li>
<li><code>chmod +x VR-WakerHD-Linux-{VERSION}-x86_64.AppImage</code> and run it.</li>
<li>Choose your own game files; setup compiles the game for your PC (once).</li>
<li>Start SteamVR or Monado, make it the active OpenXR runtime, and press Play VR.</li>
</ol>
<h2>Runtimes on Linux</h2>
<ul>
<li><strong>SteamVR</strong>: tested, and plays smoothly most of the time; occasional pacing dips from SteamVR on Linux are being worked on.</li>
<li><strong>Monado</strong>: tested with a PS VR2 headset.</li>
<li><strong>WiVRn</strong>: supported, little testing.</li>
</ul>
<h2>Steam Frame (SteamOS, arm64)</h2>
<p><code>VR-WakerHD-SteamOS-aarch64-{VERSION}.AppImage</code> is built, with Quest-like tuning, but not yet confirmed on the device.</p>
<h2>Troubleshooting</h2>
<ul>
<li><strong>It doesn't start:</strong> install the packages in <code>required_packages.txt</code>.</li>
<li><strong>VR doesn't start:</strong> an OpenXR runtime must be active; the log's <code>[vr]</code> lines say why.</li>
<li><strong>Log file:</strong> <code>~/.local/share/wwhd/captures/wwhd.log</code> (the previous start's is <code>wwhd-previous.log</code>); attach it to a <a href="{REPO}/issues/new/choose">bug report</a>.</li>
</ul>
<p>More: <a href="/VR-WakerHD/faq/">FAQ</a> · <a href="{doc('COMPATIBILITY.md')}">COMPATIBILITY.md</a></p>
{BUTTONS}""",
    heading="Linux")

# ---- FAQ (with FAQPage data: the same questions and answers as shown)
FAQ = [
    ("Is there a Wind Waker VR mod?",
     "Yes. VR-WakerHD is a free, open-source VR mod of The Legend of Zelda: The Wind Waker HD: first person through "
     "Link's eyes with head tracking, motion controls and a sword you swing yourself, on PC VR and Meta Quest 3 / 3S."),
    ("Is VR-WakerHD official?",
     "No. It is a fan project, not made, endorsed or sponsored by Nintendo, or by the ZeldaWWHDRecomp authors."),
    ("Does it include the game?",
     "No. You need your own, legally obtained copy of the Wii U game. Setup builds it on your device from your own "
     "files, and what it makes must not be shared."),
    ("Is it an emulator or a VR injector like VorpX?",
     "Neither. It builds on ZeldaWWHDRecomp, which recompiles the game's code to native code, and the VR is built into "
     "the renderer and the game's own logic."),
    ("Can I play Wind Waker VR on Quest 3 without a PC?",
     "Yes, standalone on Quest 3 and 3S with the APK. Quest 2 and Quest Pro are not tested."),
    ("Does it work on Linux?", "Yes: the Linux AppImage, with SteamVR, Monado or WiVRn."),
    ("Which headsets work?",
     "Any headset with an OpenXR runtime that supports Vulkan. PlayStation VR2 and Meta Quest 3 / 3S are tested most."),
    ("Is it free and open source?",
     "Yes. The VR mode is GPL-3.0-or-later with a credit term, and the original port's code is MPL-2.0."),
    ("Can I play without a headset?", "Yes. Play Flat in the launcher plays the base port on the monitor."),
    ("What was VR-WakerHD called before?", "ZeldaWWHD-VR."),
]
faq_html = "<dl>" + "".join(f"<dt>{html.escape(q)}</dt><dd>{html.escape(a)}</dd>" for q, a in FAQ) + "</dl>"
add("faq/", "Wind Waker VR Mod FAQ and Troubleshooting – VR-WakerHD",
    ("Wind Waker HD VR mod questions: is it official, does it include the game, does it run on Quest 3 or Linux, "
     "and fixes for common problems."),
    "Wind Waker VR mod: FAQ and troubleshooting",
    f"""<p class="lead">Common questions about VR-WakerHD, the Wind Waker HD VR mod, and fixes for common problems.</p>
<h2>Questions</h2>
{faq_html}
<h2>Playing</h2>
<ul>
<li><strong>Settings in the headset:</strong> click the left stick; the grips switch tabs.</li>
<li><strong>The sword:</strong> reach over your right shoulder, press the grip to draw it, and swing it fully to attack.</li>
<li><strong>First and third person:</strong> click the right stick.</li>
<li><strong>The view is off-centre:</strong> recentre with both stick clicks.</li>
</ul>
<h2>Troubleshooting</h2>
<div class="wrap"><table>
<tr><th>Problem</th><th>What to check</th></tr>
<tr><td>VR does not start</td><td>An OpenXR runtime must be installed and active; the log's <code>[vr]</code> lines say why</td></tr>
<tr><td>Low frame rate</td><td>Lower the headset resolution and field of view; on Quest see the <a href="/VR-WakerHD/quest-3/">Quest guide</a></td></tr>
<tr><td>Stutter on the first visit to an area</td><td>Shaders are being made; it eases as you play</td></tr>
<tr><td>Wireless lags or stutters</td><td>Turn on Streaming optimal, try 90 Hz with Capped 45 fps, or use a cable</td></tr>
<tr><td>Hands drift</td><td>Recentre (both stick clicks)</td></tr>
<tr><td><code>cannot load game/code/cking.rpx</code></td><td>The game folder must hold <code>code</code>, <code>content</code> and <code>meta</code></td></tr>
</table></div>
<h2>Log files</h2>
<div class="wrap"><table>
<tr><th>Where you play</th><th>Log file</th></tr>
<tr><td>Quest 3 / 3S</td><td><code>/sdcard/Android/data/org.wwhdrecomp.wwhd/files/captures/wwhd.log</code></td></tr>
<tr><td>Linux / SteamOS</td><td><code>~/.local/share/wwhd/captures/wwhd.log</code></td></tr>
<tr><td>Windows</td><td><code>data\\captures\\wwhd.log</code> in the folder you unzipped</td></tr>
</table></div>
<p>Still stuck? Open a <a href="{REPO}/issues/new/choose">bug report</a> and attach your log.</p>""",
    extra_ld=({"@context": "https://schema.org", "@type": "FAQPage", "mainEntity": [
        {"@type": "Question", "name": q, "acceptedAnswer": {"@type": "Answer", "text": a}} for q, a in FAQ]},),
    heading="FAQ")


def main():
    site = os.path.join(ROOT, "site")
    for path, _title, _desc, text in PAGES:
        out = os.path.join(site, path, "index.html")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            f.write(text)
    with open(os.path.join(site, "sitemap.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n')
        for path, *_ in PAGES:
            f.write(f"  <url>\n    <loc>{SITE}{path}</loc>\n    <lastmod>{LASTMOD}</lastmod>\n  </url>\n")
        f.write("</urlset>\n")
    with open(os.path.join(site, INDEXNOW_KEY + ".txt"), "w", encoding="utf-8") as f:
        f.write(INDEXNOW_KEY)
    shutil.copyfile(os.path.join(ROOT, "llms.txt"), os.path.join(site, "llms.txt"))
    shutil.copyfile(os.path.join(ROOT, "docs", "project-info.json"), os.path.join(site, "project-info.json"))
    print(f"site: {len(PAGES)} pages, sitemap, llms.txt, project-info.json")


if __name__ == "__main__":
    main()
