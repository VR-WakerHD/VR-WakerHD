# VR-WakerHD discoverability: what was done (2026-11-22)

The changes of this round, what still needs the maintainer, and how to check them. The findings they answer are in the
[audit](DISCOVERABILITY_AUDIT.md); the phrases are tracked in the [keyword tracker](SEO_KEYWORD_TRACKER.md).

## 1. The problems

- GitHub search found VR-WakerHD well, but **web search didn't find it at all**, for any phrase or even its name.
- **The website was one page**; `llms.txt` was not on it (404).
- The exact phrase **"Wind Waker VR mod"**, the main search, wasn't in the README's title, the home page's title, or
  the GitHub description.
- No gameplay screenshots; no CONTRIBUTING or SECURITY files.
- The site hadn't been submitted to Google Search Console or Bing Webmaster Tools.

## 2. What changed

- **The website** is now built by `tools/docs/build_site.py` from one template: the home page and seven topic pages,
  each with its own title (up to 65 characters), description (up to 160), canonical address, Open Graph and Twitter
  card, breadcrumb, and structured data. Every page has the no-game notice, the download button and the navigation.
- **Home page:** title "Wind Waker HD VR Mod – VR-WakerHD"; heading "Wind Waker HD VR Mod: The Legend of Zelda: The
  Wind Waker HD in virtual reality"; WebSite, SoftwareApplication and VideoObject data (the tutorial video, with its
  real upload date and length).
- **Topic pages**, written from the documentation and describing release 0.6.0 only:

  | Page | Answers |
  |---|---|
  | `/wind-waker-vr-mod/` | Is there a Wind Waker VR mod, how it works, other Wind Waker VR projects (factually) |
  | `/features/` | What it can do |
  | `/installation/` | Step-by-step setup on each platform |
  | `/quest-3/` | Standalone Quest 3 / 3S: install, settings, frame rate |
  | `/pcvr/` | SteamVR, OpenXR, Quest Link, Virtual Desktop, PC requirements |
  | `/linux/` | AppImage, distributions, SteamVR / Monado / WiVRn, Steam Frame, troubleshooting |
  | `/faq/` | Questions and troubleshooting, with FAQPage data |

- **Sitemap** lists all eight pages; **`llms.txt`** and **`project-info.json`** are now on the site too.
- **README:** title "VR-WakerHD: Wind Waker HD VR Mod (The Legend of Zelda: The Wind Waker HD in VR)", the phrase
  "Wind Waker VR mod" in the first sentence, the earlier name ZeldaWWHD-VR, and links to the Quest 3, PC VR and Linux
  guides.
- **llms.txt and docs/project-info.json:** "Wind Waker HD VR Mod" and "Wind Waker VR mod" as names, and the site's
  guides.
- **CONTRIBUTING.md** and **SECURITY.md** for GitHub's community profile.
- **Checks:** `tools/docs/check_docs.py` now checks every page of the site: balanced tags, image alt text, one h1, a
  unique title and description within the limits, the canonical and og:url matching the page's address, the page in
  the sitemap, structured data that parses with @context and @type, and links within the site that lead to a page.

Unchanged: the game, the build, the releases, the licenses and credits.

Left out on purpose: **COMMUNITY_OUTREACH.md** (posts for Reddit, Discord, news sites). The owner decided on
2026-11-20 that there is no outreach; the plan relies on the project's own pages and search indexing.

## 3. Files

Created: `tools/docs/build_site.py`, `site/{wind-waker-vr-mod,features,installation,quest-3,pcvr,linux,faq}/index.html`,
`site/llms.txt`, `site/project-info.json`, `CONTRIBUTING.md`, `SECURITY.md`, `docs/discoverability/` (this report,
the audit, the keyword tracker).

Changed: `site/index.html`, `site/sitemap.xml`, `README.md`, `llms.txt`, `docs/project-info.json`,
`tools/docs/check_docs.py`, `docs/SEARCH-DISCOVERABILITY.md`.

## 4. GitHub changes for the maintainer

These change the public repository, so they wait for the owner's go-ahead:

- **Description**, with the main phrase, and only verified claims:
  > Wind Waker HD VR mod: open-source first-person VR port of The Legend of Zelda: The Wind Waker HD with motion
  > controls and a physical sword. OpenXR PC VR (Windows/Linux) and standalone Quest 3/3S. Bring your own copy.
- **Topics to add:** `wind-waker-vr`, `zelda-vr`, `quest-3s`, `linux`, `motion-controls` (all true). GitHub allows
  20 topics and there are 19, so four would replace less searched ones (suggested: `static-recompilation`, `monado`,
  `vulkan`, `wii-u`).
- **Social preview image** (Settings > General > Social preview, 1280 x 640): only uploadable in the web settings. Use
  a real gameplay picture.
- **Publish the site:** copy `site/` to the public repository's `gh-pages` branch (see "How the site is served" in
  [SEARCH-DISCOVERABILITY.md](../SEARCH-DISCOVERABILITY.md)), and the README, llms.txt, CONTRIBUTING and SECURITY
  files to `main`, at the next release or when you say so.

## 5. Registering with search engines (the maintainer's accounts)

**Google Search Console** (https://search.google.com/search-console):

1. Add a property: **URL prefix** `https://vr-wakerhd.github.io/VR-WakerHD/`. (A domain property isn't possible:
   github.io belongs to GitHub.)
2. Verify with the **HTML tag** method: copy the `content` value into `GOOGLE_SITE_VERIFICATION` in
   `tools/docs/build_site.py`, run it, publish the site, then press Verify.
3. **Sitemaps:** submit `sitemap.xml`.
4. **URL inspection:** inspect each of the eight pages and press **Request indexing**.
5. After a few days: **Pages** report (indexed or not, and why), and the canonical Google chose for each page.

**Bing Webmaster Tools** (https://www.bing.com/webmasters):

1. **Import from Google Search Console** (quickest), or add the site and use the **Meta tag** method:
   `BING_SITE_VERIFICATION` in `tools/docs/build_site.py`.
2. Submit `sitemap.xml`; use **URL Submission** for the eight pages.
3. Bing's index also feeds DuckDuckGo and several AI assistants' web search, so this one matters for AI answers.

No search engine registration was done in this round: it needs the maintainer's sign-in.

## 6. Media (for the maintainer)

No gameplay images were made or added: only real footage should show the game. Suggested:

- **Screenshots** from your own headset recordings: first person on the Great Sea, a sword fight, the item wheel, the
  wrist HUD, the Quest launcher. Put them in `docs/images/` with descriptive names (for example
  `wind-waker-vr-sword-combat.jpg`) and alt text, then on the README and the site.
- **A short trailer** (1-2 minutes) titled like "Wind Waker HD VR Mod – First-Person Gameplay and Motion Controls |
  VR-WakerHD", with the repository and website in its description.
- legaiaflame's tutorial is already linked. Frames from it need legaiaflame's permission.

## 7. Validation (2026-11-22)

| Check | Result |
|---|---|
| `tools/docs/check_docs.py` (links, every site page, JSON, sitemap, identity) | 0 problems |
| Sitemap | valid XML (xmllint), 8 URLs |
| Structured data | every block parses as JSON with schema.org @context and @type (the checker). Not run through Google's Rich Results Test: that needs the site published |
| Titles / descriptions | unique; 33-65 / 128-160 characters |
| Layout | rendered in headless Firefox at 1280 px and 390 px (phone): no sideways scrolling, tables fit |
| Live site | not changed yet: the new pages go live when published |

## 8. Open issues

- Web search doesn't list the project yet; only indexing (step 5) and time change that.
- No gameplay screenshots.
- The GitHub description, topics and social image (section 4).
- FAQPage data no longer gives rich results in Google for most sites (Google limited them in 2023); it stays as
  plain, accurate data.

## 9. Next steps

1. Search Console and Bing (section 5), right after the site is published.
2. GitHub description, topics and social image (section 4).
3. Screenshots and a short trailer (section 6).
4. Monthly: rerun the searches and fill in the [keyword tracker](SEO_KEYWORD_TRACKER.md).
