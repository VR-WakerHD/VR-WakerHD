# VR-WakerHD discoverability audit (2026-11-22)

How findable **VR-WakerHD**, the Wind Waker HD VR mod, was on 2026-11-22, before this round's changes. **Verified**
means checked on that date with the tool named; everything else is marked as a recommendation or as not checked.
No ranking, traffic or backlink figure here is estimated.

The previous audit and the monthly search log are in [SEARCH-DISCOVERABILITY.md](../SEARCH-DISCOVERABILITY.md).

## Summary

- **GitHub search finds the project well** for Wind Waker VR searches: 1st for most target phrases (below).
- **The web doesn't know it yet.** In the AI assistant's web search, VR-WakerHD appeared for none of the target
  phrases, not even its own name. People and AI assistants who search the web can't find it.
- **The website had one page**, and `llms.txt` returned 404 on it (it was only in the repository).
- **Google Search Console and Bing Webmaster Tools** submissions were still to do from 2026-11-20 (they need the
  maintainer's accounts). This is the most important open step: nothing on the site matters until it is crawled.

## Web search (verified: the AI assistant's web search tool, 2026-11-22)

| Query | VR-WakerHD found | What came up instead |
|---|---|---|
| Wind Waker VR mod | No | 2014 Oculus Rift proof of concept on Dolphin (Kotaku, Nintendo Life, Zelda Dungeon), BotW BetterVR, OoT VR |
| Wind Waker HD VR | No | game wikis and reviews, the 2014 Rift video |
| Wind Waker VR Quest 3 | No | general Quest 3 game lists |
| Wind Waker VR GitHub OpenXR | No | DolphinXR (iChris4/dolphinXR, Wind Waker VR patches), the upstream port's mods README, BotW BetterVR |
| Zelda Wind Waker VR PC Linux | No | the flat-screen recomp ports, BetterVR guides, Arch wiki VR page |
| VR-WakerHD | No | unrelated "wake" VR pages |

Google and Bing were **not checked** directly (the tools here don't query them, and scraping them breaks their terms).
The maintainer should run the queries by hand: see the monthly test in [SEARCH-DISCOVERABILITY.md](../SEARCH-DISCOVERABILITY.md).

## GitHub search (verified: GitHub's search API, 2026-11-22)

| Query | Results | VR-WakerHD |
|---|---|---|
| wind waker vr | 3 | **1st** |
| wind waker vr mod | 1 | **1st** |
| wind waker hd vr | 1 | **1st** |
| zelda wind waker vr | 1 | **1st** |
| zelda vr mod | 1 | **1st** |
| wind waker openxr | 1 | **1st** |
| wind waker vr linux | 1 | **1st** |
| wind waker vr pc | 1 | **1st** |
| wind waker pcvr | 1 | **1st** |
| vr mod zelda | 1 | **1st** |
| wind waker quest 3 | 3 | 2nd |
| wind waker quest | 6 | 3rd |
| zelda vr | 15 | 7th |
| wind waker hd | 45 | 8th |
| wind waker | 522 | not in the top 50 (GitHub weighs stars; 5 stars) |

`topic:wind-waker`: 8th of 16. (The `gh search repos` command with quoted phrases returns no results for these; the
API with plain words is what people's searches match.)

## GitHub repository (verified: GitHub API, 2026-11-22)

| Item | State | Note |
|---|---|---|
| Description | names Zelda, The Wind Waker HD, VR port and mod, OpenXR, Quest 3/3S | lacks the exact phrase "Wind Waker VR mod" |
| Topics | 19, including wind-waker, wind-waker-hd, zelda, vr-mod, openxr, quest-3, pcvr, steamvr | missing: wind-waker-vr, zelda-vr, quest-3s, linux, motion-controls |
| Website link | the Pages site | good |
| Fork | no | forks are left out of search; this isn't one |
| Community profile | 50% | license, README, issue templates (form) present; no CONTRIBUTING, SECURITY, code of conduct |
| Social preview image | not checked (the API doesn't show it) | none is set as far as the repository page shows; it can only be uploaded in the web settings |
| Discussions | off | optional |
| Old name | `ZeldaWWHD-VR` was the private repository's name; it is private, so there is no public redirect | the old name lives on in the README, llms.txt, the site and the structured data |

## Website (verified: HTTP requests, 2026-11-22)

| Address | Status |
|---|---|
| `/` | 200: one page, good metadata (title, description, canonical, Open Graph, SoftwareApplication data) |
| `/robots.txt` | 200, allows all, points to the sitemap |
| `/sitemap.xml` | 200, one URL |
| `/llms.txt` | **404**: in the repository but not on the site |
| `/404.html` | 200 |

Weaknesses: one page can only rank for one intent; nothing answered "Quest 3", "Linux", "PC VR" or "how to install"
on the site itself. The home page's title led with "Wind Waker HD VR" but not "mod".

## Media (verified: the repository, 2026-11-22)

- **Video:** legaiaflame's tutorial and gameplay video (YouTube, 52 minutes, published 2026-11-20) is the only gameplay
  media, linked from the README and the site.
- **Screenshots:** none. The repository has only three SVG controller diagrams. Search engines and people looking for
  a VR mod look for gameplay images; this is a real gap.

## Comparable projects (verified: GitHub API and web search, 2026-11-22)

| Project | What it is | What helps it be found |
|---|---|---|
| BotW BetterVR (Crementif/BotW-BetterVR) | VR mod for Breath of the Wild on Cemu | 1,702 stars; many news articles (Road to VR, Wccftech, Kotaku, Game Informer) and install guides. No topics, no website |
| DolphinXR (iChris4/dolphinXR) | Dolphin with VR, including Wind Waker patches | 196 stars; release pages indexed on newreleases.io |
| 2014 Wind Waker Rift video (Chadtronic) | a video, not a downloadable mod | news coverage from 2014 still ranks first for "Wind Waker VR mod" |

What they have that VR-WakerHD doesn't: **outside references** (news, guides, release trackers) and **stars**. Their
own pages are not better optimized. Outside references come from other people writing about a project; by the
owner's decision (2026-11-20) there is no outreach, so the project's own pages, search console submissions and
players' own videos are the levers.

## Recommendations, by priority

1. **Submit the site to Google Search Console and Bing Webmaster Tools** and request indexing of each page (maintainer;
   instructions in the [implementation report](DISCOVERABILITY_IMPLEMENTATION_REPORT.md)).
2. Topic pages on the site for each search intent (Wind Waker VR mod, Quest 3, PC VR, Linux, features, install, FAQ). *(done)*
3. `llms.txt` and the project's facts on the site itself. *(done)*
4. "Wind Waker VR mod" in the README's title and first sentence, and in the home page's title and heading. *(done)*
5. GitHub: the phrase "Wind Waker VR mod" in the description, the missing topics, a social preview image (maintainer).
6. Real gameplay screenshots in the README and on the site (from your own recordings, or with legaiaflame's permission).
7. CONTRIBUTING and SECURITY files for the community profile. *(done)*
