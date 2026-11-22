# VR-WakerHD search discoverability: baseline, causes and monthly test

The 2026-11-22 round: [audit](discoverability/DISCOVERABILITY_AUDIT.md), [what was done](discoverability/DISCOVERABILITY_IMPLEMENTATION_REPORT.md), [keyword tracker](discoverability/SEO_KEYWORD_TRACKER.md).

**VR-WakerHD**, the open-source VR version of *The Legend of Zelda: The Wind Waker HD*, should be findable by people
who have never heard its name. On 2026-11-20 an AI assistant researching Wind Waker VR projects did not find it. This
page records that failure, what caused it, what was done about it, and a search test to repeat each month.

Record only what was really seen. A search not run is marked "not checked", never guessed.

## Baseline (2026-11-20)

**Sources used.** A web search tool (one US web index, used by AI assistants; not Google or Bing themselves) and
GitHub's repository search (API, `gh search repos`). **Google and Bing were not checked directly**: those rows are
"not checked" until they are searched by hand.

### Web search (AI assistant's index)

| Query | VR-WakerHD found | What came up instead |
|---|---|---|
| Wind Waker VR | No | 2014 Oculus Rift first-person videos (Kotaku, Nintendo Life, Zelda Dungeon), Dolphin VR (PC Gamer), the game's wiki pages |
| Zelda Wind Waker VR mod | No | Breath of the Wild VR mods (BetterVR), Ocarina of Time VR mod |
| Wind Waker HD VR | No | The game's wiki and review pages, the 2014 Rift videos |
| Wind Waker PCVR | No | The 2014 Rift videos, the upstream port's press (DSOGaming, GamingBible) |
| Wind Waker VR Quest 3 | No | General Quest 3 game lists |
| Wind Waker OpenXR first person VR | No | The 2014 Rift videos, BetterVR, TPVR (Twilight Princess) |
| Wind Waker HD recompilation | No | The upstream port, a mods repository, Wind Waker decompilations, SwitchWakerHD |
| ZeldaWWHDRecomp VR fork | No | Breath of the Wild VR mods, Zelda64Recomp |
| VR-WakerHD (the exact name) | No | Unrelated "wake" and VR pages |
| Wind Waker HD native port VR mode Quest 3 OpenXR open source | No | The upstream port, CitraVR, other VR ports |
| VR-WakerHD Wind Waker VR, restricted to github.com | **Yes: only the 0.5.0 release page** | `nelsonj1031/ZeldaWWHDRecomp-VR` (an abandoned VR fork, 0 stars), the upstream port's releases and pull requests |

The repository's main page and README never came up. The only page in the index was one release page.

### GitHub repository search

| Query | Results | VR-WakerHD's place |
|---|---|---|
| `wind waker vr in:name,description` | 4 | **1st** |
| `wind waker vr in:readme` | 100+ | 17th |
| `wind waker HD` | 39 | 16th |
| `wind waker` | many | not in the top 10 (sorted by best match; 4 stars against hundreds) |
| `vr zelda` | 2 | **missing**: "Zelda" was not in the description, and there were no topics |
| `zelda openxr` | 0 | **missing** |
| `openxr wind` | 0 | **missing** |
| `VR-WakerHD` | 1 | 1st |

### Outside references found

- No news article, directory or upstream page links to VR-WakerHD.
- The upstream README credits community forks (Android) but has no VR link.
- [PCVR-Mods-Installer-Hub issue #159](https://github.com/Mr-Nlce/PCVR-Mods-Installer-Hub/issues/159), "Mod
  suggestion: Zelda Windwaker", is open and links only the flat-screen port.

## Causes

1. **The repository's page was not in the web index.** It was public for 9 days, with 4 stars and no outside links,
   so crawlers had no reason to find it. AI assistants that search the web cannot recommend a page the index does not
   have. A better README and `llms.txt` only help once the page is crawled.
2. **The name and metadata missed the words people search for.** "VR-WakerHD" contains neither "Zelda" nor "Wind
   Waker"; the description lacked "Zelda"; there were no topics.
3. **No references.** Search engines and AI assistants weigh what other sites link to. Nothing linked here.
4. **The public README was out of date.** It said Linux VR was untested, that there was no Android VR, and that the
   repository was private.

## What was done

| Change | Cause it addresses | Status |
|---|---|---|
| README rewritten: "Wind Waker HD VR" in the title and first lines, the relationship to ZeldaWWHDRecomp, accurate status | 2, 4 | **Published 2026-11-20** |
| Install, compatibility, Quest, features, FAQ, development pages | 2, 4 | **Published 2026-11-20** |
| `llms.txt`, `docs/project-info.json` | 2 (once crawled) | **Published 2026-11-20** |
| GitHub description with "Zelda", 19 topics, the website as the homepage | 2 | **Done 2026-11-20**; "port", "mod" and "head tracking" added to the description that evening |
| GitHub Pages site with a sitemap, submitted to Google Search Console and Bing Webmaster Tools | 1 | **Live 2026-11-20** at https://vr-wakerhd.github.io/VR-WakerHD/; Search Console and Bing Webmaster Tools submissions still to do (the maintainer's accounts) |

## Monthly search test

Run each query by hand in each engine (signed out, or in a private window), and on GitHub. Write down what you see,
including "not found". Don't use automated scraping of search engines; it breaks their terms of service.

| # | Group | Query |
|---|---|---|
| 1 | Generic Wind Waker | Wind Waker mods |
| 2 | Generic Wind Waker | Wind Waker HD modifications |
| 3 | Generic Wind Waker | Wind Waker HD open source |
| 4 | Wind Waker VR | Wind Waker VR |
| 5 | Wind Waker VR | Wind Waker HD VR |
| 6 | Wind Waker VR | Wind Waker VR mod |
| 7 | Wind Waker VR | Wind Waker VR port |
| 8 | Wind Waker VR | Wind Waker first person VR |
| 9 | Wind Waker VR | Wind Waker VR head tracking |
| 10 | Recompilation | Wind Waker HD recompilation |
| 11 | Recompilation | Wind Waker recomp mods |
| 12 | Recompilation | ZeldaWWHDRecomp VR |
| 13 | Recompilation | Zelda recompilation VR |
| 14 | Zelda VR | Zelda VR games |
| 15 | Zelda VR | Zelda VR mod |
| 16 | Zelda VR | Open-source Zelda VR |
| 17 | OpenXR | Wind Waker OpenXR |
| 18 | OpenXR | OpenXR Zelda |
| 19 | Quest and PCVR | Wind Waker PCVR |
| 20 | Quest and PCVR | Wind Waker VR Quest 3 |
| 21 | Quest and PCVR | Zelda Quest 3 standalone |
| 22 | Name | VR-WakerHD |

### Results log

Add one row per query, engine and date. Position: the result's place on the first page (1-10), "page 2+", or "not
found".

| Date | Engine | Query # | VR-WakerHD | Position | Competing results | Notes |
|---|---|---|---|---|---|---|
| 2026-11-20 | AI web search | 4-7, 10, 12, 17, 19, 20, 22 | No | not found | see the baseline | the repository page was not indexed |
| 2026-11-20 | GitHub | 4, 5, 18, 22 | Yes for 4, 5, 22; no for 18 | 1st (4), 16th (5) | upstream port, randomizers | "Zelda" missing from the description |
| 2026-11-20 | Google | all | not checked | | | |
| 2026-11-20 | Bing | all | not checked | | | |
| 2026-11-20 (after publishing) | GitHub | 4, 18, `topic:wind-waker` | Yes for 4 and the topic; not yet for "vr zelda" / 18 | 1st (4), 9th of 16 (topic) | | the new description was not in GitHub's search index yet |
| 2026-11-20 (evening) | GitHub | 1-22 | Yes in 15 of 22 | **1st** for 3, 4, 5, 8, 12, 16, 17, 18, 19, 20, 22; 2nd for 13, 21; 3rd for 10 | upstream port and its mods, SwitchWakerHD, Android forks | missing for 1, 2, 6, 7, 9, 11, 14, 15: "mod", "port", "modifications", "head tracking", "games" were not in the description; "port", "mod" and "head tracking" added to it the same evening |
| 2026-11-20 (evening) | AI web search | 5 (variant "Wind Waker HD VR open source"), the site's address | No | not found | the upstream port's mods README, decompilations, Dolphin VR, BetterVR | hours after publishing: not crawled yet |

### Monthly checklist

- [ ] Run the 22 queries on Google, Bing, an AI assistant with web search, and GitHub; fill in the log.
- [ ] Google Search Console and Bing Webmaster Tools: are the site's pages indexed? Any crawl errors?
- [ ] Check the GitHub description, topics and website link are still set, and match `docs/project-info.json`.
- [ ] Check the latest release's notes follow `.github/RELEASE_TEMPLATE.md` (introduction, downloads, no-game notice).
- [ ] Keep `llms.txt`, `docs/project-info.json` and the site's facts in step with the latest release.
- [ ] Check new outside references (articles, listings, videos) and add them below.
- [ ] Note any new competing Wind Waker VR project.

**How the site is served.** GitHub Actions is turned off on the public repository, so the site is served by GitHub
Pages from the `gh-pages` branch (the files of `site/`, with its page folders, and `.nojekyll`), not by
`.github/workflows/pages.yml`. The pages are generated: edit `tools/docs/build_site.py`, run it, then copy `site/` to
`gh-pages` and push. If Actions is ever turned on, switch Pages' source to "GitHub
Actions" and the workflow takes over.

The workflow `.github/workflows/docs-check.yml` checks the documentation's links, the JSON, the sitemap and that the
project's description matches across files on every change. It does not search the web.

### Outside references

| Date | Where | Link | Notes |
|---|---|---|---|
| | | | |

See also: [back to the README](../README.md)
