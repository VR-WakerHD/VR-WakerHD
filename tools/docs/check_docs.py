#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# VR mode by VR-WakerHD (https://github.com/VR-WakerHD/)
"""Checks the public documentation (docs/SEARCH-DISCOVERABILITY.md): links and anchors between the documents,
images, the JSON and XML files, the site's HTML, and that the project's name, repository and description agree
across the README, llms.txt, docs/project-info.json and the site. Offline: no web requests. Exit 1 on a problem."""
import html.parser
import json
import os
import re
import sys
import xml.dom.minidom

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# the documents players and search engines read (the older technical docs are not held to this check)
DOCS = ["README.md", "llms.txt", "LICENSE-VR.md", ".github/RELEASE_TEMPLATE.md"] + [
    "docs/" + f for f in ("INSTALLATION.md", "COMPATIBILITY.md", "QUEST.md", "FAQ.md", "FEATURES.md", "PLAYING.md",
                          "BUILDING.md", "DEVELOPMENT.md", "PROMOTION.md", "SEARCH-DISCOVERABILITY.md",
                          "diagnosis/README.md")]
REPO = "https://github.com/VR-WakerHD/VR-WakerHD"
SITE = "https://vr-wakerhd.github.io/VR-WakerHD/"
problems = []


def slug(heading):
    h = re.sub(r"[^\w\- ]", "", heading.strip().lower())
    return h.replace(" ", "-")


def anchors(path):
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    return {slug(h) for h in re.findall(r"^#+ (.*)$", text, re.M)}


def check_links(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        problems.append(f"{rel}: missing")
        return
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    for target in re.findall(r"\]\(([^)\s]+)\)", text):
        if re.match(r"[a-z]+:", target):
            continue
        file, _, anchor = target.partition("#")
        dest = os.path.normpath(os.path.join(os.path.dirname(path), file)) if file else path
        if not os.path.exists(dest):
            problems.append(f"{rel}: broken link {target}")
        elif anchor and dest.endswith(".md") and anchor not in anchors(dest):
            problems.append(f"{rel}: no heading for {target}")


class Html(html.parser.HTMLParser):
    VOID = {"meta", "link", "img", "br", "hr", "input", "source"}

    def __init__(self):
        super().__init__()
        self.stack, self.bad, self.meta, self.imgs_without_alt = [], [], {}, 0
        self.title = ""
        self._in_title = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "meta":
            self.meta[a.get("name") or a.get("property") or ""] = a.get("content", "")
        if tag == "link" and a.get("rel") == "canonical":
            self.meta["canonical"] = a.get("href", "")
        if tag == "img" and not a.get("alt"):
            self.imgs_without_alt += 1
        if tag == "title":
            self._in_title = True
        if tag not in self.VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag == "title":
            self._in_title = False
        if self.stack and self.stack[-1] == tag:
            self.stack.pop()
        else:
            self.bad.append(tag)

    def handle_data(self, data):
        if self._in_title:
            self.title += data


def check_site():
    index = os.path.join(ROOT, "site", "index.html")
    for name in ("index.html", "404.html"):
        p = Html()
        text = open(os.path.join(ROOT, "site", name), encoding="utf-8").read()
        p.feed(text)
        if p.stack or p.bad:
            problems.append(f"site/{name}: unbalanced tags {p.stack or p.bad}")
        if p.imgs_without_alt:
            problems.append(f"site/{name}: {p.imgs_without_alt} image(s) without alt text")
        if not p.title.strip():
            problems.append(f"site/{name}: no title")
    p = Html()
    text = open(index, encoding="utf-8").read()
    p.feed(text)
    for key in ("description", "og:title", "og:description", "og:url", "twitter:card", "canonical"):
        if not p.meta.get(key):
            problems.append(f"site/index.html: no {key}")
    if p.meta.get("canonical") != SITE or p.meta.get("og:url") != SITE:
        problems.append("site/index.html: canonical / og:url is not " + SITE)
    for block in re.findall(r'<script type="application/ld\+json">(.*?)</script>', text, re.S):
        try:
            json.loads(block)
        except ValueError as e:
            problems.append(f"site/index.html: structured data is not JSON ({e})")
    try:
        sitemap = xml.dom.minidom.parse(os.path.join(ROOT, "site", "sitemap.xml"))
        locs = [n.firstChild.data.strip() for n in sitemap.getElementsByTagName("loc")]
        if SITE not in locs:
            problems.append("site/sitemap.xml: the home page is not listed")
    except Exception as e:  # noqa: BLE001 (any parse error is a problem to report)
        problems.append(f"site/sitemap.xml: {e}")
    robots = open(os.path.join(ROOT, "site", "robots.txt"), encoding="utf-8").read()
    if SITE + "sitemap.xml" not in robots:
        problems.append("site/robots.txt: no Sitemap line for the sitemap")


def check_identity():
    try:
        info = json.load(open(os.path.join(ROOT, "docs", "project-info.json"), encoding="utf-8"))
    except ValueError as e:
        problems.append(f"docs/project-info.json: not JSON ({e})")
        return
    if info.get("name") != "VR-WakerHD" or info.get("repository") != REPO:
        problems.append("docs/project-info.json: name or repository changed")
    version = info.get("latestRelease", {}).get("version", "")
    files = {"README.md": None, "llms.txt": None, "site/index.html": None}
    for rel in files:
        text = open(os.path.join(ROOT, rel), encoding="utf-8").read()
        for needed in ("VR-WakerHD", "The Wind Waker HD", "ZeldaWWHDRecomp", "Quest 3", "OpenXR"):
            if needed not in text:
                problems.append(f"{rel}: does not mention {needed!r} (the project's identity)")
        if "unofficial" not in text.lower() and "not affiliated" not in text.lower():
            problems.append(f"{rel}: does not say the project is unofficial")
    for rel in ("llms.txt", "site/index.html"):
        text = open(os.path.join(ROOT, rel), encoding="utf-8").read()
        if version and version not in text:
            problems.append(f"{rel}: the latest release {version} (docs/project-info.json) is not mentioned")


def main():
    for rel in DOCS:
        check_links(rel)
    check_site()
    check_identity()
    for p in problems:
        print("docs check:", p)
    print(f"docs check: {len(problems)} problem(s) in {len(DOCS)} documents and the site")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
