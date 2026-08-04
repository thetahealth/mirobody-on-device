---
marp: true
theme: default
paginate: true
title: On-Device LLM — Mirobody
style: |
  /* Palette: the navy/warm-cream sheet in docs/colors-and-fonts.md §2 — the same
     values htdoc renders (see htdoc/src/index.css, which defers to that sheet).
     Set through the theme's OWN GitHub-markdown variables so tables, hairlines,
     code fills and body text all follow at once instead of being patched rule by
     rule. !important is required, not decoration: the theme declares these on
     `div#:$p > svg > foreignObject > section`, which outranks a bare `section`. */
  section {
    --bgColor-default: #F2EFE9 !important;       /* bg_page — warm cream */
    --bgColor-muted: #EDE8DE !important;         /* bg_bubble — table header, code */
    --bgColor-neutral-muted: rgba(0,0,0,0.08) !important;  /* bg_inline_code (ARGB) */
    --borderColor-default: #DDD6C9 !important;   /* border — warm hairline */
    --borderColor-muted: #E7E1D5 !important;     /* divider */
    --fgColor-default: #1A1C1E !important;       /* text_primary */
    --fgColor-muted: #52565C !important;         /* text_secondary */
    --fgColor-accent: #1E3A6B !important;        /* brand_text — links */
    background: #F2EFE9;
    color: #1A1C1E;
  }
  /* The sheet keeps two code inks apart: crimson for inline spans, near-black for
     blocks. This deck only has inline spans, but pin both so a future fence is right. */
  code { color: #9F3048; }
  pre code { color: #2A2E36; }
  .columns {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1.5rem;
  }
  .columns h3 { margin-top: 0; }
  .step { display: grid; grid-template-columns: 160px minmax(0, 1fr); gap: 1rem; align-items: start; margin: .5em 0; }
  .step > div { min-width: 0; }
  .step small { color: #63666B; font-size: 0.6em; line-height: 1.05; display: block; margin-top: 0.5em; }
  .step p { margin: 0.15em 0; line-height: 1.2; }
  .step table { display: table !important; table-layout: fixed !important; width: 100% !important; margin: 0 !important; }
  section.secs p { margin: 1.0em 0 0.2em; }
  svg.hero { width: 100%; height: 380px; display: block; margin: 0 auto; }
  table { font-size: 0.62em; margin: 0 auto; }
  .note { font-size: 0.52em; }
  .note li { margin: 0.2em 0; }
  section.divider { background: #1E3A6B; }
  section.divider h1 { color: #FFFFFF; font-size: 2.1em; border: none; }
  section.divider p { color: #A0CDE5; font-size: 1.05em; }
---

# On-Device LLM

<svg class="hero" style="height: 450px" viewBox="0 0 960 360" font-family="Helvetica, Arial, sans-serif">
  <text x="480" y="24" text-anchor="middle" font-size="16" fill="#52565C">One private chat model, running on the hardware you already own.</text>
  <!-- ===== center device ===== -->
  <rect x="360" y="66" width="170" height="250" rx="22" fill="#FAF7F1" stroke="#1A1C1E" stroke-width="3"/>
  <rect x="374" y="90" width="142" height="200" rx="8" fill="#F2EFE9" stroke="#DDD6C9"/>
  <text x="445" y="110" text-anchor="middle" font-size="10.5" font-weight="bold" fill="#2A7248">on your device</text>
  <!-- LLM chip -->
  <rect x="386" y="120" width="118" height="46" rx="8" fill="#EDE8DE" stroke="#1E3A6B"/>
  <text x="445" y="140" text-anchor="middle" font-size="12" font-weight="bold" fill="#1E3A6B">on-device LLM</text>
  <text x="445" y="156" text-anchor="middle" font-size="8.5" fill="#52565C">Gemma · GGUF / LiteRT-LM</text>
  <!-- data chip -->
  <rect x="386" y="176" width="118" height="40" rx="8" fill="#FBEEDD" stroke="#B0762A"/>
  <text x="445" y="200" text-anchor="middle" font-size="11" font-weight="bold" fill="#8A5E15">your health data</text>
  <!-- shield / lock -->
  <path d="M445 226 L473 236 V256 C473 270 460 278 445 284 C430 278 417 270 417 256 V236 Z" fill="#FAF7F1" stroke="#2A7248" stroke-width="2"/>
  <rect x="438" y="250" width="14" height="12" rx="2" fill="#2A7248"/>
  <path d="M441 250 V245 A4 4 0 0 1 449 245 V250" fill="none" stroke="#2A7248" stroke-width="2"/>
  <text x="445" y="302" text-anchor="middle" font-size="9.5" font-weight="bold" fill="#1E3A6B">private by default</text>
  <!-- ===== left: no cloud ===== -->
  <g>
    <circle cx="110" cy="150" r="24" fill="#DDD6C9"/>
    <circle cx="146" cy="138" r="31" fill="#DDD6C9"/>
    <circle cx="180" cy="152" r="22" fill="#DDD6C9"/>
    <rect x="94" y="140" width="94" height="34" rx="17" fill="#DDD6C9"/>
    <text x="142" y="160" text-anchor="middle" font-size="11" fill="#52565C">server</text>
    <line x1="96" y1="186" x2="192" y2="118" stroke="#BA1A1A" stroke-width="4" stroke-linecap="round"/>
  </g>
  <text x="142" y="212" text-anchor="middle" font-size="12" font-weight="bold" fill="#BA1A1A">no server round-trip</text>
  <!-- blocked link device -> cloud -->
  <line x1="356" y1="180" x2="204" y2="180" stroke="#C9C2B4" stroke-width="1.5" stroke-dasharray="5 5"/>
  <g stroke="#BA1A1A" stroke-width="2.5" stroke-linecap="round">
    <line x1="273" y1="172" x2="287" y2="188"/>
    <line x1="287" y1="172" x2="273" y2="188"/>
  </g>
  <!-- ===== right: Part 1 landscape (vertical list) ===== -->
  <text x="600" y="60" font-size="14" font-weight="bold" fill="#1A1C1E">Part 1 · The landscape</text>
  <text x="600" y="78" font-size="10.5" fill="#52565C">how a chat model runs on a phone or laptop</text>
  <g>
    <rect x="600" y="92" width="344" height="70" rx="10" fill="#FAF7F1" stroke="#DDD6C9"/>
    <rect x="600" y="92" width="8" height="70" rx="4" fill="#2A78D6"/>
    <text x="632" y="124" font-size="16" font-weight="bold" fill="#1A1C1E">Small enough to fit</text>
    <text x="632" y="146" font-size="11" fill="#52565C">quantized to run in device memory</text>
    <rect x="600" y="174" width="344" height="70" rx="10" fill="#FAF7F1" stroke="#DDD6C9"/>
    <rect x="600" y="174" width="8" height="70" rx="4" fill="#EB6834"/>
    <text x="632" y="206" font-size="16" font-weight="bold" fill="#1A1C1E">Fast enough to use</text>
    <text x="632" y="228" font-size="11" fill="#52565C">GPU-accelerated</text>
    <rect x="600" y="256" width="344" height="70" rx="10" fill="#FAF7F1" stroke="#DDD6C9"/>
    <rect x="600" y="256" width="8" height="70" rx="4" fill="#1BAF7A"/>
    <text x="632" y="288" font-size="16" font-weight="bold" fill="#1A1C1E">Yours to choose or swap</text>
    <text x="632" y="310" font-size="11" fill="#52565C">not locked to one vendor's model</text>
  </g>
</svg>

---

## Where the model runs

<table style="width:90%;table-layout:fixed;margin:0 auto">
<colgroup>
<col style="width:10%"><col style="width:15%"><col style="width:25%"><col style="width:25%"><col style="width:25%">
</colgroup>
<thead>
<tr><th colspan="2"></th><th>CPU</th><th>GPU</th><th>NPU</th></tr>
</thead>
<tbody>
<tr><td colspan="2"><b>Speed</b></td><td><span style="color:#BA1A1A">slow</span></td><td><span style="color:#8A5E15">fast</span></td><td><span style="color:#2A7248">fastest</span></td></tr>
<tr><td colspan="2"><b>Power</b></td><td><span style="color:#BA1A1A">high</span></td><td><span style="color:#8A5E15">medium</span></td><td><span style="color:#2A7248">low</span></td></tr>
<tr><td colspan="2"><b>Built for</b></td><td>general compute</td><td>parallel math / graphics</td><td>vision &amp; speech</td></tr>
<tr><td colspan="2"><b>Examples</b></td><td>Intel · AMD<br>Apple / Arm cores</td><td>NVIDIA · AMD<br>Adreno · Mali<br>Apple GPU</td><td>Hexagon (Qualcomm)<br>ANE (Apple)<br>TPU (Pixel)</td></tr>
<tr><td colspan="2"><b>LLM today</b></td><td><span style="color:#BA1A1A">last resort</span></td><td><span style="color:#2A7248"><b>the practical default</b></span></td><td><span style="color:#8A5E15">early, restricted</span></td></tr>
<tr><td rowspan="5"><b>Runtimes</b></td><td>llama.cpp</td><td><span style="color:#2A7248;font-size:1.2em">●</span></td><td><span style="color:#2A7248;font-size:1.2em">●</span></td><td></td></tr>
<tr><td>LiteRT-LM</td><td></td><td><span style="color:#2A7248;font-size:1.2em">●</span></td><td><span style="color:#8A5E15">◐</span> early</td></tr>
<tr><td>ONNX Runtime</td><td><span style="color:#2A7248;font-size:1.2em">●</span></td><td><span style="color:#2A7248;font-size:1.2em">●</span></td><td><span style="color:#2A7248;font-size:1.2em">●</span> QNN</td></tr>
<tr><td>MLC-LLM</td><td></td><td><span style="color:#2A7248;font-size:1.2em">●</span></td><td></td></tr>
<tr><td>system service</td><td></td><td></td><td><span style="color:#2A7248;font-size:1.2em">●</span></td></tr>
</tbody>
</table>

---

## Getting a model onto a device

<div class="step">
<div>

**1 · Quantize**
<small>fewer bits, smaller file</small>

</div>
<div>

<table style="width:100%;table-layout:fixed;display:table">
<colgroup><col style="width:28%"><col style="width:36%"><col style="width:36%"></colgroup>
<thead><tr><th>Precision</th><th>3B model</th><th>Used in</th></tr></thead>
<tbody>
<tr><td>FP16</td><td>~6 GB</td><td>servers</td></tr>
<tr><td>8-bit</td><td>~3 GB</td><td>desktop</td></tr>
<tr><td><b>4-bit</b></td><td><b>~1.5–2 GB</b></td><td>mobile / desktop sweet spot</td></tr>
<tr><td>2-bit (QAT)</td><td>~1 GB</td><td>aggressive</td></tr>
</tbody>
</table>

</div>
</div>

<div class="step">
<div>

**2 · Package**
<small>wrap in a runnable format</small>

</div>
<div>

<table style="width:100%;table-layout:fixed;display:table">
<colgroup><col style="width:28%"><col style="width:24%"><col style="width:24%"><col style="width:24%"></colgroup>
<thead><tr><th>Format</th><th>What</th><th>Ecosystem</th><th>Best at</th></tr></thead>
<tbody>
<tr><td><b>GGUF</b></td><td>weights file</td><td>llama.cpp / desktop</td><td>biggest library</td></tr>
<tr><td><b>ONNX</b></td><td>compute graph</td><td>ONNX Runtime</td><td>widest backends (NPUs)</td></tr>
<tr><td><b>LiteRT-LM</b></td><td>mobile package</td><td>Google AI Edge</td><td>mobile, GPU-accelerated</td></tr>
</tbody>
</table>

</div>
</div>

<div class="step run">
<div>

**3 · Run**
<small>bring your own engine, or the system's</small>

</div>
<div>

<table style="width:100%;table-layout:fixed;display:table">
<colgroup><col style="width:28%"><col style="width:30%"><col style="width:42%"></colgroup>
<thead><tr><th>Approach</th><th>How</th><th>Trade-off</th></tr></thead>
<tbody>
<tr><td><b>Bring your own engine</b></td><td>app ships runtime + model</td><td>any model, cross-platform · bigger download</td></tr>
<tr><td><b>Call the system model</b></td><td>app calls an OS API</td><td>tiny app, best hardware · one fixed model</td></tr>
</tbody>
</table>

</div>
</div>

---

<!-- _class: secs -->

## What you can actually run today

**On the phone** — 1B–4B open models at 4-bit:

| Family (maker) | Sizes | Strength | Best for | Also used by |
|----------------|-------|----------|----------|--------------|
| **Gemma** (Google) | 270M · 1B · 4B | multimodal + tool-calling | our default | Gemini Nano · LiteRT-LM |
| **Llama 3.2** (Meta) | 1B · 3B | best tool-calling | safe default | Meta AI · ONNX GenAI · Genie |
| **Phi-4-mini** (Microsoft) | 3.8B | math & reasoning | reasoning | Windows Copilot+ |
| **Qwen 3** (Alibaba) | 1.7B · 4B | 35+ languages | multilingual | Ollama · LM Studio · MLC |
| **SmolLM** (Hugging Face) | 1.7B · 3B | tiny, fully open | low-memory | hobbyist / research |

**On the NPU, even less** — industry-wide limits:

| Limit | Detail |
|-------|--------|
| **Tiny model menu** | Phi-3.5 mini, Llama 3.2 3B (ONNX+QNN) · Gemma3-1B (LiteRT-LM) |
| **Pre-compiled per chip** | can't drop in any model like GGUF |
| **Fragile** | NPU init can crash (drivers) — so **GPU** stays the default; NPU is an experimental toggle |

---

<!-- _class: secs -->

## Our approach: bundle the engine, not the model

**Vendors vs Mirobody:**

| | **Google · Gemini Nano** | **Apple · Foundation Models** | **Mirobody** |
|---|---|---|---|
| Model | Gemini Nano (1.8 / 3.25B) | ~3B, 2-bit QAT | any GGUF / Gemma |
| Runs in | AICore (NPU/TPU) | Neural Engine | bundled engine (llama.cpp / LiteRT-LM) |
| You get | one fixed model | one fixed model | pick & swap, cross-platform |
| Available on | recent flagships | Apple silicon | any desktop, most phones |

**What we run, per tier:**

| Tier | Runtime · Format | Models |
|------|------------------|--------|
| **Desktop** — Qt · Electron | llama.cpp · GGUF | any GGUF — Gemma, Qwen, Llama, Phi… |
| **Mobile** — Android · iOS | LiteRT-LM · `.litertlm` | Gemma 4 · Qwen |
| **Mobile** — HarmonyOS | llama.cpp · GGUF | any GGUF — Qwen3.5 2B · 4B |

<div class="note" style="margin-top:1em">

Engine bundled in-app; the model is downloaded on demand and managed in **⚙ → On-device AI** — swap or update without shipping a new app.

</div>

---

<!-- _class: secs -->

## HarmonyOS — and where the compute actually went

<style scoped>
  p { font-size: 0.9em; line-height: 1.35; margin: 0.45em 0 0.3em; }
  h2 { margin-bottom: 0.25em; }
  table { font-size: 0.56em; }
  /* The theme sizes tables to max-content, so centering has to be stated rather
     than assumed -- an auto width would fill and there would be nothing to centre. */
  table.setup { margin-left: auto !important; margin-right: auto !important; }
  .note { font-size: 0.5em; line-height: 1.3; }
</style>

**Experimental setup:**

<table class="setup">
<tbody>
<tr><td><b>Device</b></td><td>Kirin 9020 · 16 GB</td></tr>
<tr><td><b>Runtime · Format</b></td><td>llama.cpp · GGUF — no Google services, so no LiteRT-LM</td></tr>
<tr><td><b>Engine</b></td><td>in the shared C++ core, same interface as the cloud providers</td></tr>
<tr><td><b>Model</b></td><td>Qwen3.5 2B · 4B — <code>Q4_K_M</code>, 4.5 bit/weight → 1.28 / 2.74 GB</td></tr>
<tr><td><b>Model file</b></td><td>a folder the user picks, outside app storage — survives an uninstall</td></tr>
<tr><td><b>Baseline</b></td><td>2B — ~43 tok/s prefill · <b>11.2 tok/s decode</b> · 4B — 5.2 decode</td></tr>
</tbody>
</table>

**Also the one platform where "the GPU is the practical default" proved false:**

| | Measured | Why |
|---|---|---|
| **CPU** + `i8mm` kernels | **2.9× prefill · 2.2× decode** — what ships | a cross build silently falls back to baseline `armv8-a` |
| **GPU** — Vulkan on Maleoon | **3.5× slower**, +50 MB app size | offload copies every tensor; the iGPU shares the same DRAM |
| **NPU** — NNRt / MindSpore Lite | **ceiling below today's CPU** | no INT4 → 2× bytes per token, and decode is bandwidth-bound |

<div class="note" style="margin-top:0.5em">

**One number closes both:** measured read bandwidth is **19.5 GB/s** and a single core already saturates it. Decode reads every weight for every token, so a 1.28 GB model can never exceed **15.2 tok/s** — we already get 11.2. Another compute unit does not move that wall, and running there at INT8 instead of 4-bit would lower it to **8.6**. An NPU could still help *prefill*, which is compute-bound — worth revisiting only for long-input work.

</div>

