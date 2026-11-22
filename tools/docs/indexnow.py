#!/usr/bin/env python3
"""Tells the IndexNow search engines (Bing, Yandex, Seznam, Naver) about the website's pages: every page in
site/sitemap.xml, with the site's key (tools/docs/build_site.py INDEXNOW_KEY, served at the site's root). Run it after
the site is published to gh-pages, when pages are added or changed. Google doesn't take IndexNow: use Search Console.
Standard library only."""
import json
import os
import sys
import urllib.request
import xml.dom.minidom

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_site  # noqa: E402  (its SITE and INDEXNOW_KEY)

ROOT = build_site.ROOT


def main():
    sitemap = xml.dom.minidom.parse(os.path.join(ROOT, "site", "sitemap.xml"))
    urls = [n.firstChild.data.strip() for n in sitemap.getElementsByTagName("loc")]
    key_url = build_site.SITE + build_site.INDEXNOW_KEY + ".txt"
    live = urllib.request.urlopen(key_url, timeout=30).read().decode().strip()
    if live != build_site.INDEXNOW_KEY:
        sys.exit(f"the key file at {key_url} doesn't hold the key: publish the site first")
    body = json.dumps({"host": "vr-wakerhd.github.io", "key": build_site.INDEXNOW_KEY, "keyLocation": key_url,
                       "urlList": urls}).encode()
    req = urllib.request.Request("https://api.indexnow.org/indexnow", data=body,
                                 headers={"Content-Type": "application/json; charset=utf-8"})
    with urllib.request.urlopen(req, timeout=30) as r:
        print(f"IndexNow: {r.status} for {len(urls)} pages")


if __name__ == "__main__":
    main()
