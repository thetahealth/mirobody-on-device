# Colors & Fonts

<style>
/* Color swatches — render in VSCode's markdown preview; GitHub strips them
   harmlessly (the hex text still reads fine). */
i.sw { display:inline-block; width:.9em; height:.9em; margin-right:.3em;
       border:1px solid rgba(128,128,128,.55); border-radius:3px;
       vertical-align:-.08em; }
</style>

How every mirobody client colors and typesets its UI — current state per client, plus the
**unified token sheet (§2)** that all clients converge on. This is the map, not the territory:
**source of truth is always the code file listed for each client** — change the code first,
then update this doc. The dark-mode method (§8) is self-contained — distilled from an earlier
ThetaSmart iOS design spec that is no longer in this repo.

There is no shared token file. Every client re-types its hex values, so this doc doubles as
the cross-client consistency checklist.

---

## 1. Where things stand today

**Decision (2026-07-28): converge on one system.** The navy/cream palette is the product's
visual identity; harmony's token *schema* (semantic colors, chat-specific tokens, paired
light/dark) is the structure everyone adopts. The merged result is §2. Harmony migrates first
(it is the only fully tokenized client, so the recolor is cheap); the navy clients then adopt
the semantic tokens incrementally.

Harmony's blue/neutral palette (`brand` `#3F7FC1`, white page) was the second system and is
**gone as of 2026-07-29** — see §2.5 step 1. Every client now brands on
<i class="sw" style="background:#1E3A6B"></i>`#1E3A6B` over
<i class="sw" style="background:#F2EFE9"></i>`#F2EFE9`; what still differs between clients is
how much of the §2.2 *semantic* set each one has adopted, not the brand or the surfaces.

Support matrix (current):

| Client | Token file | Dark mode | Semantic tokens | Charts |
| --- | --- | --- | --- | --- |
| android | `android/app/src/main/java/ai/thetahealth/mirobody/ui/theme/Color.kt` (**canonical for the navy system**) | `values-night/` + `isSystemInDarkTheme()`; Material You off | none (M3 default error) | shared theme ✅ |
| ios | `ios/Mirobody/UI/Theme.swift` (`MBColors`, subset of Color.kt) | runtime `ColorScheme` | `error` only | shared theme ✅ |
| harmony | `harmony/entry/src/main/resources/{base,dark}/element/color.json` | `base/` + `dark/` resource dirs | full set (danger/success/warn) | shared theme ✅ |
| qt | `qt/qml/Theme.qml` (singleton, light only) | **none** | `error` only | no charts |
| miniapp | inline in each `.wxss` (no token layer) | **none** | `error` only | no charts |
| htdoc / electron | `htdoc/src/config.js` `LIGHT_COLOR`/`DARK_COLOR` + `htdoc/src/index.css` | runtime: system/light/dark menu setting; palette swap + `html.mb-dark` + `app.render()` | `error`/`onError` | shared theme ✅ |

---

## 2. Unified token sheet (target)

Harmony's token names, navy/cream values. Every value below was verified with a WCAG contrast
script against **all** surfaces it actually sits on — page, surface, and bubble in both modes,
plus the composited inline-code fill (`bg_bubble` + `bg_inline_code`). Text tokens clear 4.5:1
on every one of them; fill/graphical tokens clear 3:1. The contrast column shows the worst
light / worst dark pairing.

### 2.1 Brand

| Token | Light | Dark | Contrast | Role |
| --- | --- | --- | --- | --- |
| `brand` | <i class="sw" style="background:#1E3A6B"></i>`#1E3A6B` | <i class="sw" style="background:#A0CDE5"></i>`#A0CDE5` | on_brand on it: 11.2:1 / 10.0:1 | filled buttons, user bubble, send button. Dark follows android's M3 dark primary: pale slate fill + dark glyph, not white-on-blue |
| `on_brand` | <i class="sw" style="background:#FFFFFF"></i>`#FFFFFF` | <i class="sw" style="background:#0A1B3D"></i>`#0A1B3D` | (above) | text/icon on `brand` fills |
| `brand_text` | <i class="sw" style="background:#1E3A6B"></i>`#1E3A6B` | <i class="sw" style="background:#A0CDE5"></i>`#A0CDE5` | 9.2:1 / 9.7:1 | links, active labels, selected states — on surfaces, not on fills |

### 2.2 Semantic

Derived fresh for cream/near-black surfaces (keep-hue-raise-lightness for dark; see §8 rules).
`danger` keeps the value the navy clients already use; `warn` dark keeps harmony's tuned value.

| Token | Light | Dark | Contrast | Role |
| --- | --- | --- | --- | --- |
| `danger` | <i class="sw" style="background:#BA1A1A"></i>`#BA1A1A` | <i class="sw" style="background:#FFB4AB"></i>`#FFB4AB` | 5.3:1 / 9.8:1 | destructive fills, error icons; text-capable in both modes |
| `on_danger` | <i class="sw" style="background:#FFFFFF"></i>`#FFFFFF` | <i class="sw" style="background:#5C0A06"></i>`#5C0A06` | 6.5:1 / 8.2:1 | text on `danger` fills |
| `danger_text` | <i class="sw" style="background:#BA1A1A"></i>`#BA1A1A` | <i class="sw" style="background:#FFB4AB"></i>`#FFB4AB` | (same as `danger`) | kept as a separate slot — harmony code already references it |
| `success` | <i class="sw" style="background:#2A7248"></i>`#2A7248` | <i class="sw" style="background:#7CC79A"></i>`#7CC79A` | 4.8:1 / 8.3:1 | in-range readings, checkmarks; text-capable |
| `warn` | <i class="sw" style="background:#B0762A"></i>`#B0762A` | <i class="sw" style="background:#EDA85A"></i>`#EDA85A` | 3.1:1 / 8.2:1 | warning icons, badges — **graphical only in light mode** (under 4.5:1) |
| `warn_text` | <i class="sw" style="background:#8A5E15"></i>`#8A5E15` | <i class="sw" style="background:#EDA85A"></i>`#EDA85A` | 4.7:1 / 8.2:1 | warning body text in light mode |

### 2.3 Surfaces & containers

| Token | Light | Dark | Role |
| --- | --- | --- | --- |
| `bg_page` / `start_window_background` | <i class="sw" style="background:#F2EFE9"></i>`#F2EFE9` | <i class="sw" style="background:#101315"></i>`#101315` | warm cream page / near-black page |
| `bg_surface` | <i class="sw" style="background:#FAF7F1"></i>`#FAF7F1` | <i class="sw" style="background:#181B1D"></i>`#181B1D` | fields, cards (one step lighter than page) |
| `bg_bubble` | <i class="sw" style="background:#EDE8DE"></i>`#EDE8DE` | <i class="sw" style="background:#1C1F22"></i>`#1C1F22` | assistant chat bubble (= android `surfaceContainerHigh` / `surfaceContainer`) |
| `bg_overlay` | <i class="sw" style="background:rgba(0,0,0,0.04)"></i>`#0A000000` | <i class="sw" style="background:rgba(255,255,255,0.08)"></i>`#14FFFFFF` | pressed/hover wash (ARGB, surface-independent) |
| `bg_inline_code` | <i class="sw" style="background:rgba(0,0,0,0.08)"></i>`#14000000` | <i class="sw" style="background:rgba(255,255,255,0.12)"></i>`#1FFFFFFF` | inline-code fill (ARGB; composites warm on cream) |
| `notice_bg` | <i class="sw" style="background:#FBEEDD"></i>`#FBEEDD` | <i class="sw" style="background:#3A2E1B"></i>`#3A2E1B` | amber callout container (warn family) |
| `notice_fg` | <i class="sw" style="background:#8A5E15"></i>`#8A5E15` | <i class="sw" style="background:#E3B573"></i>`#E3B573` | text inside `notice_bg` (5.0:1 / 7.0:1); light = `warn_text` by design. Harmony's current `#9A6B1E` is 4.1:1 — an AA miss this migration fixes |

The page/surface/bubble separation is **deliberately subtle** (≈1.06–1.13:1 luminance steps,
same as the shipping apps) — elevation is carried by borders and shadows too, so don't "fix"
the steps by widening them.

### 2.4 Text, code, lines

| Token | Light | Dark | Role |
| --- | --- | --- | --- |
| `text_primary` | <i class="sw" style="background:#1A1C1E"></i>`#1A1C1E` | <i class="sw" style="background:#E2E2E5"></i>`#E2E2E5` | body (navy system's onSurface) |
| `text_secondary` | <i class="sw" style="background:#52565C"></i>`#52565C` | <i class="sw" style="background:#C4C7CB"></i>`#C4C7CB` | subtitles, hints |
| `text_tertiary` | <i class="sw" style="background:#63666B"></i>`#63666B` | <i class="sw" style="background:#8E9194"></i>`#8E9194` | timestamps, footers (4.7:1 / 5.2:1). Deliberately darker than the navy clients' outline `#74787C`, which is only 3.6–4.2:1 as text |
| `text_disabled` | <i class="sw" style="background:#A9A49A"></i>`#A9A49A` | <i class="sw" style="background:#626973"></i>`#626973` | disabled labels, placeholders (warm in light) |
| `code_fg` | <i class="sw" style="background:#2A2E36"></i>`#2A2E36` | <i class="sw" style="background:#D3D8DE"></i>`#D3D8DE` | code-block ink (unchanged from harmony) |
| `inline_code_fg` | <i class="sw" style="background:#9F3048"></i>`#9F3048` | <i class="sw" style="background:#F0899F"></i>`#F0899F` | inline-code accent, checked on the **composited** fill (`bg_bubble` + `bg_inline_code` = <i class="sw" style="background:#DAD6CD"></i>`#DAD6CD` / <i class="sw" style="background:#383A3D"></i>`#383A3D`): 4.8:1 / 4.8:1. Harmony's current <i class="sw" style="background:#C2415A"></i>`#C2415A` is 4.1:1 there |
| `divider` | <i class="sw" style="background:#E7E1D5"></i>`#E7E1D5` | <i class="sw" style="background:#262A31"></i>`#262A31` | soft separators (warm) |
| `border` | <i class="sw" style="background:#DDD6C9"></i>`#DDD6C9` | <i class="sw" style="background:#333941"></i>`#333941` | field borders, hairlines (warm) |
| `quote_bar` | <i class="sw" style="background:#C9C2B4"></i>`#C9C2B4` | <i class="sw" style="background:#464D57"></i>`#464D57` | blockquote bar; chart axis |
| `scrim` | <i class="sw" style="background:rgba(0,0,0,0.20)"></i>`#33000000` | <i class="sw" style="background:rgba(0,0,0,0.60)"></i>`#99000000` | modal scrim (ARGB) |
| `shadow` | <i class="sw" style="background:rgba(0,0,0,0.15)"></i>`#26000000` | <i class="sw" style="background:rgba(0,0,0,0.70)"></i>`#B3000000` | elevation shadow (ARGB) |

### 2.5 Migration order

1. ~~**harmony**~~ — **DONE 2026-07-29.** All 28 tokens carry §2 values in both files;
   `on_danger` and `warn_text` added; `render.html`'s six mirrored constants updated in the same
   commit; icons regenerated on the new `brand`. ArkTS holds no hardcoded hex at all — every
   color resolves through `$r('app.color.*')` — so the two JSON files were the whole UI.
   Original instructions kept below for the record: swap values in `resources/base/element/color.json` and
   `resources/dark/element/color.json`; add the new tokens (`on_brand` dark value,
   `on_danger`, `success` unchanged name, `warn_text`); update the hand-mirrored constants in
   `rawfile/render/render.html` (new values: `INK` unchanged `#2A2E36`/`#D3D8DE`,
   `CHART_BG` → `#EDE8DE`/`#1C1F22`, `TEXT` → `#1A1C1E`/`#E2E2E5`,
   `TEXT_DIM` → `#52565C`/`#C4C7CB`, `AXIS` → `#C9C2B4`/`#464D57`,
   `GRID` → `#DDD6C9`/`#333941`); regenerate the app icon (`harmony/icon/gen_icon.py` bakes
   `#3F7FC1`).
2. **android / ios** — already on the navy values; add the §2.2 semantic tokens when first
   needed. Fix the iOS `AccentColor` asset to `brand` (`#1E3A6B` + dark variant `#A0CDE5`).
   Align android's `DarkOnPrimary` `#003549` (teal-leaning, 7.7:1) to the sheet's `on_brand`
   dark `#0A1B3D` (navy, 10.0:1) — both pass, but they should be one value.
3. **htdoc / qt / miniapp** — adopt semantic tokens opportunistically; dark mode is the
   larger missing piece (§8).

Open items, deliberately not decided here: the logo's `#005CF5` (a third blue — whether it
joins `brand` is a brand decision); whether `danger_text` stays a separate slot or collapses
into `danger`.

Fonts are untouched by the convergence — all clients stay on system fonts; the shared type
scale is §7.2.

---

## 3. Navy / warm cream palette — current implementation

Canonical definition: [`Color.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/theme/Color.kt).
The other clients carry hand-copied subsets; the "used by" column says who re-types each value.

### 3.1 Light

| Role | Hex | Used by |
| --- | --- | --- |
| primary / brand (buttons, links, user bubble, logo tint) | <i class="sw" style="background:#1E3A6B"></i>`#1E3A6B` | all five |
| onPrimary | <i class="sw" style="background:#FFFFFF"></i>`#FFFFFF` | all five |
| primaryContainer / onPrimaryContainer | <i class="sw" style="background:#DCE4F4"></i>`#DCE4F4` / <i class="sw" style="background:#0A1B3D"></i>`#0A1B3D` | android, ios |
| secondary / secondaryContainer | <i class="sw" style="background:#4A5568"></i>`#4A5568` / <i class="sw" style="background:#DDE2EA"></i>`#DDE2EA` | android only |
| background & surface (warm cream page) | <i class="sw" style="background:#F2EFE9"></i>`#F2EFE9` | all five |
| onSurface (near-black text) | <i class="sw" style="background:#1A1C1E"></i>`#1A1C1E` | all five |
| onSurfaceVariant (secondary text) | <i class="sw" style="background:#52565C"></i>`#52565C` | all five |
| surfaceVariant / surfaceContainerHighest | <i class="sw" style="background:#E7E1D5"></i>`#E7E1D5` | android only |
| surfaceContainerLowest (pure white card) | <i class="sw" style="background:#FFFFFF"></i>`#FFFFFF` | android, miniapp, htdoc |
| surfaceContainerLow (field / card fill) | <i class="sw" style="background:#FAF7F1"></i>`#FAF7F1` | all five |
| surfaceContainer / High | <i class="sw" style="background:#F4F0E8"></i>`#F4F0E8` / <i class="sw" style="background:#EDE8DE"></i>`#EDE8DE` | android only |
| outline | <i class="sw" style="background:#74787C"></i>`#74787C` | all five |
| outlineVariant (warm hairlines) | <i class="sw" style="background:#DDD6C9"></i>`#DDD6C9` | all five |
| error | <i class="sw" style="background:#BA1A1A"></i>`#BA1A1A` | ios, qt, miniapp, htdoc (android uses the M3 default) |
| brand wordmark black | <i class="sw" style="background:#0F1115"></i>`#0F1115` | android (`brand_logo_mark`), miniapp, htdoc |

htdoc-only extras (live in `htdoc/src/index.css`, not in the `config.js` token object):
code-block / table-header fill `#EFE9DD`, blockquote text `#44474A`, placeholder `#6B6E74`.

### 3.2 Dark (android + ios only)

| Role | Hex |
| --- | --- |
| primary (soft slate-blue accent) | <i class="sw" style="background:#A0CDE5"></i>`#A0CDE5` |
| onPrimary / primaryContainer / onPrimaryContainer | <i class="sw" style="background:#003549"></i>`#003549` / <i class="sw" style="background:#184D67"></i>`#184D67` / <i class="sw" style="background:#CFE5F2"></i>`#CFE5F2` |
| background & surface | <i class="sw" style="background:#101315"></i>`#101315` |
| onBackground / onSurface | <i class="sw" style="background:#E2E2E5"></i>`#E2E2E5` |
| onSurfaceVariant | <i class="sw" style="background:#C4C7CB"></i>`#C4C7CB` |
| surfaceContainerLowest → Highest | <i class="sw" style="background:#0A0D0F"></i>`#0A0D0F` <i class="sw" style="background:#181B1D"></i>`#181B1D` <i class="sw" style="background:#1C1F22"></i>`#1C1F22` <i class="sw" style="background:#272A2D"></i>`#272A2D` <i class="sw" style="background:#313437"></i>`#313437` |
| outline / outlineVariant | <i class="sw" style="background:#8E9194"></i>`#8E9194` / <i class="sw" style="background:#44474A"></i>`#44474A` |
| error (ios) | <i class="sw" style="background:#FFB4AB"></i>`#FFB4AB` |

The dark scheme follows the keep-hue-raise-lightness rule: navy `#1E3A6B` becomes slate-blue
`#A0CDE5`, never a system blue.

---

## 4. Harmony palette — MIGRATED to §2 on 2026-07-29

Definition: [`base/element/color.json`](../harmony/entry/src/main/resources/base/element/color.json)
(light) and [`dark/element/color.json`](../harmony/entry/src/main/resources/dark/element/color.json).
Same token names in both files; HarmonyOS resolves by resource qualifier.

**The values below are HISTORICAL — the blue/neutral palette harmony shipped until the
migration.** For what those files contain now, read §2; harmony carries every token in it.
Kept because the schema is still harmony's (the other clients adopted these token *names*), and
because knowing what moved explains the two AA fixes the migration carried: `notice_fg`
`#9A6B1E` → `#8A5E15` (was 4.1:1) and `inline_code_fg` `#C2415A` → `#9F3048` (4.1:1 on the
composited fill).

| Token | Light | Dark | Notes |
| --- | --- | --- | --- |
| `brand` | <i class="sw" style="background:#3F7FC1"></i>`#3F7FC1` | <i class="sw" style="background:#3E7CC0"></i>`#3E7CC0` | fills stay ~same; on-brand text is white in both modes |
| `brand_text` | <i class="sw" style="background:#3F7FC1"></i>`#3F7FC1` | <i class="sw" style="background:#7FB4E8"></i>`#7FB4E8` | text/icon on surface: hue kept, lightness raised |
| `danger` / `danger_text` | <i class="sw" style="background:#D24C57"></i>`#D24C57` / same | <i class="sw" style="background:#CF5561"></i>`#CF5561` / <i class="sw" style="background:#E88A93"></i>`#E88A93` | |
| `success` | <i class="sw" style="background:#46A86A"></i>`#46A86A` | <i class="sw" style="background:#479F68"></i>`#479F68` | |
| `warn` | <i class="sw" style="background:#E8912F"></i>`#E8912F` | <i class="sw" style="background:#EDA85A"></i>`#EDA85A` | |
| `bg_page` / `bg_surface` / `bg_bubble` | <i class="sw" style="background:#FFFFFF"></i>`#FFFFFF` / <i class="sw" style="background:#FAFAFA"></i>`#FAFAFA` / <i class="sw" style="background:#F1F1F3"></i>`#F1F1F3` | <i class="sw" style="background:#101216"></i>`#101216` / <i class="sw" style="background:#191C21"></i>`#191C21` / <i class="sw" style="background:#22262C"></i>`#22262C` | three-layer elevation |
| `text_primary` → `text_disabled` | <i class="sw" style="background:#1A1A1A"></i>`#1A1A1A` <i class="sw" style="background:#5A6068"></i>`#5A6068` <i class="sw" style="background:#8A8A8A"></i>`#8A8A8A` <i class="sw" style="background:#B0B0B0"></i>`#B0B0B0` | <i class="sw" style="background:#E6E8EB"></i>`#E6E8EB` <i class="sw" style="background:#AFB6BF"></i>`#AFB6BF` <i class="sw" style="background:#868D96"></i>`#868D96` <i class="sw" style="background:#626973"></i>`#626973` | four-step hierarchy |
| `notice_bg` / `notice_fg` | <i class="sw" style="background:#FBEEDD"></i>`#FBEEDD` / <i class="sw" style="background:#9A6B1E"></i>`#9A6B1E` | <i class="sw" style="background:#3A2E1B"></i>`#3A2E1B` / <i class="sw" style="background:#E3B573"></i>`#E3B573` | amber notice container |
| `code_fg` / `inline_code_fg` | <i class="sw" style="background:#2A2E36"></i>`#2A2E36` / <i class="sw" style="background:#C2415A"></i>`#C2415A` | <i class="sw" style="background:#D3D8DE"></i>`#D3D8DE` / <i class="sw" style="background:#F0899F"></i>`#F0899F` | code on translucent black/white fills (`bg_inline_code`) |
| `divider` / `border` / `quote_bar` | <i class="sw" style="background:#EEEEEE"></i>`#EEEEEE` / <i class="sw" style="background:#E0E0E0"></i>`#E0E0E0` / <i class="sw" style="background:#C9CDD4"></i>`#C9CDD4` | <i class="sw" style="background:#262A31"></i>`#262A31` / <i class="sw" style="background:#333941"></i>`#333941` / <i class="sw" style="background:#464D57"></i>`#464D57` | |
| `scrim` / `shadow` | <i class="sw" style="background:rgba(0,0,0,0.20)"></i>`#33000000` / <i class="sw" style="background:rgba(0,0,0,0.15)"></i>`#26000000` | <i class="sw" style="background:rgba(0,0,0,0.60)"></i>`#99000000` / <i class="sw" style="background:rgba(0,0,0,0.70)"></i>`#B3000000` | ARGB |

⚠ Five of these values are **mirrored by hand** into
[`rawfile/render/render.html`](../harmony/entry/src/main/resources/rawfile/render/render.html)
(`INK`, `CHART_BG`, `TEXT`, `TEXT_DIM`, `AXIS`, `GRID`) because the render webview cannot read
app resources. Change `color.json` → update `render.html` in the same commit.

Component conventions for harmony (dialog style, picker, checkbox, spacing) are a separate
concern and survive the recolor; the palette plus those rules together define the harmony look.

---

## 5. Fixed colors (identical in both modes, on purpose)

| Color | What | Where |
| --- | --- | --- |
| <i class="sw" style="background:#005CF5"></i>`#005CF5` | logo lower glyph | `android/.../drawable/ic_mirobody_logo.xml`, `htdoc/src/icons.js` (`MIROBODY_SVG`) + `htdoc/src/assets/mirobody.svg` (favicon). Only the **lower** glyph is fixed; the upper one is the theme-aware `brand_logo_mark` — <i class="sw" style="background:#0F1115"></i>`#0F1115` light / <i class="sw" style="background:#FFFFFF"></i>`#FFFFFF` dark — in every client that has dark mode. htdoc draws it in `currentColor` and tints it `wordmark` from `widgets.brandMark()`; the favicon file, a separate document, carries its own `prefers-color-scheme` rule. The fixed blue is 3.5:1 on the dark page, so it needs no dark variant |
| <i class="sw" style="background:#07C160"></i>`#07C160` | WeChat login green (disabled <i class="sw" style="background:#9BE0B8"></i>`#9BE0B8`) | `htdoc/src/login.js`, `miniapp/pages/login/login.wxss`, `android/.../auth/EmailScreen.kt` |
| <i class="sw" style="background:#000000"></i>`#000000` | Apple / GitHub / X OAuth buttons | `htdoc/src/login.js` |
| `#EA4335 #4285F4 #FBBC05 #34A853` | Google logo | `htdoc/src/icons.js` |
| vendor chips: oura <i class="sw" style="background:#4C6EF5"></i>`#4C6EF5`, whoop <i class="sw" style="background:#0CA678"></i>`#0CA678`, polar <i class="sw" style="background:#E8590C"></i>`#E8590C`, fitbit <i class="sw" style="background:#0C8599"></i>`#0C8599`, withings <i class="sw" style="background:#1C7ED6"></i>`#1C7ED6`, dexcom <i class="sw" style="background:#7048E8"></i>`#7048E8`, garmin <i class="sw" style="background:#2F9E44"></i>`#2F9E44`, fallback <i class="sw" style="background:#868E96"></i>`#868E96` | health-vendor branding | duplicated verbatim in `htdoc/src/vendors.js` and `qt/qml/VendorsDialog.qml` — keep in step |

---

## 6. Charts (ECharts)

One shared theme file, vendored per client and **kept byte-identical** (same convention as
`echarts.min.js`):

- source of truth: `ios/Mirobody/Resources/chart-theme.js` (header comment says so)
- copies: `android/app/src/main/assets/chart-theme.js`,
  `harmony/entry/src/main/resources/rawfile/render/chart-theme.js`,
  `htdoc/static/chart-theme.js` (added 2026-07-28; the js header comment predates this copy —
  add it to the list there on the next theme change, syncing all four. Webpack minifies the
  *served* copy; the four source copies are what stay byte-identical)

Two rules it encodes:

1. **Fixed-order categorical palette** — eight hues, one ramp per mode, assigned in series
   order and never cycled (past 8, fold into "Other"):

   | Slot | Light | Dark |
   | --- | --- | --- |
   | 1 blue | <i class="sw" style="background:#2A78D6"></i>`#2A78D6` | <i class="sw" style="background:#3987E5"></i>`#3987E5` |
   | 2 orange | <i class="sw" style="background:#EB6834"></i>`#EB6834` | <i class="sw" style="background:#D95926"></i>`#D95926` |
   | 3 green | <i class="sw" style="background:#1BAF7A"></i>`#1BAF7A` | <i class="sw" style="background:#199E70"></i>`#199E70` |
   | 4 yellow | <i class="sw" style="background:#EDA100"></i>`#EDA100` | <i class="sw" style="background:#C98500"></i>`#C98500` |
   | 5 pink | <i class="sw" style="background:#E87BA4"></i>`#E87BA4` | <i class="sw" style="background:#D55181"></i>`#D55181` |
   | 6 dark green | <i class="sw" style="background:#008300"></i>`#008300` | <i class="sw" style="background:#008300"></i>`#008300` |
   | 7 violet | <i class="sw" style="background:#4A3AA7"></i>`#4A3AA7` | <i class="sw" style="background:#9085E9"></i>`#9085E9` |
   | 8 red | <i class="sw" style="background:#E34948"></i>`#E34948` | <i class="sw" style="background:#E66767"></i>`#E66767` |

   Validated as a set against each mode's surface; light mode has 4 slots under 3:1, which is
   why `mbPrepare()` force-enables the legend.

2. **Chrome is injected, never hardcoded** — the host passes `{ink, inkDim, axis, grid, surface}`
   from its own resolved theme (android: `onSurface`/`onSurfaceVariant`/`outline`/`outlineVariant`;
   ios: same; harmony: `text_primary`/`text_secondary`/`quote_bar`/`border` via `render.html`).
   `surface` is `'transparent'` on live canvases, the opaque bubble color on harmony (it
   rasterizes to PNG). `mbPrepare()` strips any model-supplied `color`/`backgroundColor` so a
   hand-written ```echarts fence can't break one of the modes.

Series colors carry **identity only, never status** — don't reach for danger/success/warn hues
to distinguish lines; state is read from thresholds/bands, not line color.

The series palettes are independent of the §2 convergence and stay as-is.

htdoc (and therefore electron) loads chart-theme.js lazily next to `echarts.min.js`
(best-effort — a load failure degrades to stock ECharts rather than losing the chart) and
passes `{ink: onSurface, inkDim: onSurfaceVar, axis: outline, grid: outlineVar,
surface: 'transparent'}`, with the mode and chrome read at render time so each chart matches
the theme in effect when it arrives.

---

## 7. Fonts

**System fonts only, everywhere.** No bundled UI font files; the only shipped fonts are KaTeX's
woff2 set for math (`ios/Mirobody/Resources/math/fonts/`). htdoc enforces this with a strict
`font-src 'self'` CSP.

### 7.1 Families

| Client | Sans (body) | Serif (brand wordmark) | Monospace (code) |
| --- | --- | --- | --- |
| android | `FontFamily.Default` (Roboto) | `FontFamily.Serif` | `FontFamily.Monospace` |
| ios | system (`-apple-system`) | `.design(.serif)` at 20pt semibold | `.design(.monospaced)` |
| harmony | system default (HarmonyOS Sans) | — | `.fontFamily('monospace')` in `RichMessage.ets` |
| qt | system default via Fusion | — | **none** (Qt's built-in Markdown renderer picks) |
| miniapp | `-apple-system, BlinkMacSystemFont, "PingFang SC", "Helvetica Neue", sans-serif` | `.serif` class: `Georgia, "Times New Roman", "Songti SC", STSong, serif` | **none** |
| htdoc | `system-ui, -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif` | `"Iowan Old Style", "Palatino Linotype", Palatino, Georgia, "Times New Roman", serif` (`config.js` `serifFamily`) | `ui-monospace, SFMono-Regular, Menlo, Consolas, monospace` |

### 7.2 Unified type scale (target)

One named ramp, anchored on android's shipping [`Type.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/theme/Type.kt)
(the only complete scale in the repo; iOS already carries a trimmed copy). Sizes are in a
device-independent unit **u**; §7.2.1 maps u to each client. The user font-scale tiers (§7.4)
offset the **whole ramp**, so steps below are at offset 0.

| Step | Size / line (u) | Weight | Use |
| --- | --- | --- | --- |
| `headline_l` | 26 / 32 | semibold | rare hero text |
| `headline_m` | 22 / 28 | semibold | page titles |
| `headline_s` | 20 / 26 | semibold | dialog titles; serif brand wordmark |
| `title_m` | 16 / 22 | medium | section headers, list-item titles |
| `title_s` | 14 / 20 | medium | card subtitles |
| `body_l` | 16 / 24 | regular | **the standard reading size** — chat body, forms |
| `body_m` | 14 / 20 | regular | secondary text, thinking blocks |
| `body_s` | 12 / 16 | regular | captions; code (in monospace) |
| `label_l` | 14 / 20 | medium | buttons |
| `label_m` | 12 / 16 | medium | chips, tabs |
| `label_s` | 11 / 14 | medium | badges, timestamps |

Rules: pick from the ramp, never invent one-off sizes (harmony's current `12.5` / `13.5` /
`15` / `17` snap to the nearest step when a file is next touched). Android's M3 `display*`
slots (28–36) stay defined there but are not part of the shared ramp. Everything scales
together with the user tier — no step is pinned.

#### 7.2.1 Unit mapping

| Client | 1 u = | body_l at offset 0 |
| --- | --- | --- |
| android | 1 sp | 16 sp (already matches `Type.kt`) |
| ios | 1 pt × `fontScale` | 16 pt |
| harmony | 1 fp | 16 fp |
| htdoc | 1⁄16 rem (root carries the tier) | `1rem` |
| miniapp | 2 rpx | 32 rpx |
| qt | ≈ 0.72 pt (Fusion @ 96 dpi); keep expressing sizes as `Theme.baseSize ± n` | `Theme.baseSize` (11 pt) |

### 7.3 Current per-client size scales

Android ships the full M3 scale in `Type.kt` (display 36/32/28, headline 26/22/20, title
18/16/14, body 16/14/12, label 14/12/11); iOS a trimmed copy in `Theme.swift` (no display /
headlineLarge / titleLarge). harmony uses ad-hoc `fp` literals (11–24, including fractional
one-offs), miniapp raw `rpx` literals, qt `Theme.baseSize ± n`, htdoc `rem`.

### 7.4 User font-scale setting

All clients expose the same five tiers, offset `-4 / -2 / 0 / +2 / +4`
(`fontSmaller … fontLarger`), each applying it its own way:

| Client | Mechanism |
| --- | --- |
| htdoc | root `font-size: (16 + offset)px`, everything in `rem` (`config.js applyFontScale`) |
| qt | `Theme.baseSize = 11 + offset` pt on the `ApplicationWindow`, inherited |
| ios | multiplier `1 + offset/14` (`Theme.swift fontScale`) |
| miniapp | class overrides `fs-small` … `fs-larger` — currently scales **only** the chat bubble + empty state |
| harmony | ArkTS side scales rendered output (math SVG `EX_PX` × scale) and text sizes |

---

## 8. Dark mode

The navy system's dark scheme ships on **android, ios, and htdoc/electron** (htdoc: a
system/light/dark setting in the gear menu; "system" follows `prefers-color-scheme` live);
harmony has full light/dark on its old palette; qt and miniapp are light-only today.

### 8.1 Principles

1. **Dark is not inverted light.** Re-pick every value for perceived brightness, temperature,
   and contrast; never derive it mechanically from the light value.
2. **All UI color goes through a token.** No `.white` / `.black` / raw hex in a view — the
   token decides both modes' values. (Fixed-by-design colors are tokens too: §5.)
3. **Visibility beats aesthetics.** 4.5:1 for body text, 3:1 for large text / graphical
   objects — measured against the surface the color *actually* sits on (§2 was verified this
   way, composited fills included).
4. **Temperature coherence beats raw contrast.** The dark surfaces are cool near-blacks; a
   warm semantic container on them must be a tinted overlay or a deep tinted solid, never the
   light mode's pale warm block.
5. **Keep the brand.** Brighten by raising lightness/saturation at the same hue — navy
   `#1E3A6B` → slate `#A0CDE5`, success `#2A7248` → `#7CC79A` — never by swapping in a
   platform default blue.

### 8.2 What adapts, what stays fixed

| Layer | Behavior |
| --- | --- |
| UI layer — text/icon foregrounds, surfaces, semantic container BGs, button fills, borders | **must** adapt |
| Data layer — chart series (a per-mode ramp like §6's counts as fixed), brand marks, vendor chips, logo | fixed |

Rule of thumb: if the color is chrome, it adapts; if the color *is* the information, it stays.

### 8.3 Patterns

- **Tinted container** — semantic BG on a dark surface = the semantic main color at 15–22%
  opacity over the parent surface (or the precomputed opaque equivalent, like `notice_bg`
  dark `#3A2E1B`). Tuning order: pick the tint (slightly desaturated, hue may lean warmer) →
  set opacity → verify the container's text on the *composited* result at ≥3:1.
- **Brighter sibling** — interactive/brand fills brighten per principle 5; the §2.1/§2.2
  dark values are all built this way.
- **Icon tile** — a brand-colored icon in a chip needs solid adaptive tokens for *both*
  foreground and background. `brand @ 10%` as a tile BG reads fine in light and composites to
  nearly nothing on dark.
- **Floating chip / pill** — an element floating above a surface takes the next surface step
  *up* in dark (e.g. `bg_bubble` over `bg_surface`); same-color means the float disappears.
- **Forced-light region** — if a brand/tier area is pinned light in both modes (hero cards,
  marketing sheets), every descendant must render light too: SwiftUI
  `.environment(\.colorScheme, .light)`, or a scoped token override on the web. Pinning only
  the background leaves adaptive text white-on-white.
- **Native control popup** — a `<select>` list, autofill drop-down or date picker is drawn by
  the platform, out of reach of the styles on the control. `color-scheme` (htdoc sets it on
  `html`) makes them follow the mode, but the option list's fill comes from the **select's own
  `background-color`**: a control left `transparent` to look borderless gets a light popup while
  its options inherit light text. Give every such control the real color of the surface it sits
  on — visually identical, and it hands the popup a color to paint with.
- **System accent** — anything the platform tints from a global accent (iOS `AccentColor`
  asset, system controls) needs a dark variant equal to the `brand` dark fill, or every
  system-tinted control ships light navy on dark surfaces. This is exactly drift item §9.1.

### 8.4 Anti-patterns

| Don't | Because | Instead |
| --- | --- | --- |
| `.white` / `.black` background in a view | light-glare / hole in the other mode | surface tokens |
| fixed brand navy as an icon/text that must be visible in dark | navy-on-dark ≈ invisible | `brand_text` (adaptive) |
| `brand @ 10%` tile or button BG in dark | composites to nothing | solid adaptive BG token |
| light-mode tint hex reused as the dark BG | pale block glares on dark | tinted container (§8.3) |
| semantic main color as its own container BG | text and BG become the same color | tinted container (§8.3) |
| platform default blue for brand fills in dark | brand identity lost | brighter sibling (§8.3) |
| outline/stroke from a half-opacity border token in dark | ~2:1, edge dissolves | a dedicated adaptive border token |
| fixed light-mode semantic foregrounds in dark | forest/caramel at L≈25–40% on near-black ≈ 2–3:1 | the §2.2 dark values |

### 8.5 Verifying a dark-mode change

1. Build the client.
2. Flip the OS/app to dark and look at the real screens (simulator/emulator screenshot, or
   the browser once htdoc grows dark support) — token changes fan out wider than the diff.
3. Flip back to light and regression-check: swapping a fixed token for an adaptive one can
   shift the light value too.
4. Contrast-check every new pair with the §2 script method, against the composited surface it
   sits on.

---

## 9. Known drift & gaps

Kept here so nobody "fixes" one of them accidentally-differently:

1. **iOS `AccentColor` is `#3A78B5`, not brand navy `#1E3A6B`** (single universal value, no dark
   variant) — SwiftUI default tint (toggles, links, `BleDeviceView`) paints an off-brand blue.
   Target per §2.5: `#1E3A6B` + dark appearance `#A0CDE5`.
2. **qt has no dark mode but uses Fusion**, whose widget internals follow the OS — dark-OS users
   get dark widget chrome around cream `Theme.qml` surfaces.
3. **Vendor chip palette duplicated** in `htdoc/src/vendors.js` and `qt/qml/VendorsDialog.qml`.
4. **`#0F1115` wordmark black** re-typed in android, miniapp, and htdoc; `#EFE9DD` / `#44474A`
   exist only in `htdoc/src/index.css`.
5. **harmony still renders the old blue/neutral palette** until the §2.5 step-1 migration
   lands — do not spot-fix individual harmony colors in the meantime; recolor wholesale. Two
   of its current light-mode pairs miss AA (notice_fg 4.1:1, inline_code_fg 4.1:1 on the
   composited fill); the §2 values fix both.
6. **The logo's `#005CF5` is a third blue**, matching neither `#1E3A6B` nor harmony's
   `#3F7FC1` — parked pending a brand decision (§2.5).
7. **htdoc dims disabled controls by opacity**, not with `text_disabled` — the login code field
   / Send code / Sign in (gated until there's an email and six digits) go through
   `widgets.setDisabled`, so one rule covers both modes and fills as well as text. The §2.4
   token appears in `index.css` for exactly one case that opacity can't reach: the ink of a
   disabled `<option>` inside a native popup. Keep the split; don't mix the two on one control.
8. **Type scale non-conformance** (§7.2 is the target): harmony uses ad-hoc `fp` literals
   including fractional one-offs (`12.5` / `13.5`); miniapp uses raw `rpx` literals and its
   font-scale tier only resizes the chat bubble + empty state, not the whole UI. Snap to the
   ramp opportunistically when touching those files.

Resolved 2026-07-28: htdoc/electron charts now load the shared theme (§6); htdoc's misnamed
`color.userBubble` tint became `color.selectedBg` (it was a selection/emphasis wash, used by
`modals.js` / `history.js`), and the user bubble reads `color.brand` instead of a re-typed
hex. Later the same day htdoc got full dark mode (the §2 dark values, an Appearance menu
setting, `html.mb-dark` CSS overrides) and its hardcoded chrome colors were tokenized —
including the off-palette consent-dialog blue `#2563EB` (→ `primary`) and the black OAuth
icon tints (→ `onSurface`); the QR boxes and vendor-logo tiles stay white on purpose
(scanners / third-party marks assume a light ground). One survivor of that pass, fixed after:
htdoc drew the logo as an `<img>` of `assets/mirobody.svg`, whose upper glyph is a flat
`black` — invisible on the dark page (login hero and top bar both). The mark is now inline
SVG (§5), so htdoc matches android's theme-aware `brand_logo_mark`.

---

## 10. Maintenance

- New colors anywhere: take the value from the §2 unified sheet; if the sheet lacks the slot,
  derive it there first (both modes, contrast-checked), then implement.
- Changing a navy-system color: edit `Color.kt` first, then propagate by hand to
  `Theme.swift`, `Theme.qml`, `config.js`/`index.css`, and the miniapp `.wxss` files — grep for
  the old hex across the repo (both cases) to catch every re-typed copy. Update §2 and §3 here.
- Changing a harmony color: edit both `base/` and `dark/` `color.json`, then check the
  hand-mirrored constants in `rawfile/render/render.html`.
- Changing chart colors or marks: edit `ios/Mirobody/Resources/chart-theme.js`, then copy the
  file byte-identical to the android, harmony, and htdoc paths listed in §6.
- Update the tables here in the same change.
