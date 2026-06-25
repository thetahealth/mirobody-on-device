---
marp: true
theme: default
paginate: true
title: Mirobody v2 — On-Device Architecture
style: |
  .columns {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1.5rem;
  }
  .columns h3 { margin-top: 0; }
  svg.hero { width: 100%; height: 360px; display: block; margin: 0 auto; }
  svg.layers { width: 100%; height: auto; display: block; margin: 0 auto; }
  .note { font-size: 0.55em; }
  .note li { margin: 0.25em 0; }
---

# Mirobody v2 Architecture

<svg class="hero" viewBox="0 0 960 430" font-family="Helvetica, Arial, sans-serif">
  <defs>
    <marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0 0 L10 5 L0 10 z" fill="#555"/>
    </marker>
  </defs>
  <text x="480" y="30" text-anchor="middle" font-size="16" font-weight="bold" fill="#c0392b">one core, three forms (each works on its own)</text>

  <!-- ===== Column 1a: Web (thin client) ===== -->
  <rect x="40" y="46" width="230" height="70" rx="14" fill="#e8eef7" stroke="#4072b8"/>
  <text x="155" y="78" text-anchor="middle" font-size="14" font-weight="bold" fill="#234">Web</text>
  <text x="155" y="98" text-anchor="middle" font-size="10" fill="#456">HTML · Miniapp — UI only</text>

  <!-- ===== Column 1b: Native apps (embedded) ===== -->
  <rect x="40" y="148" width="230" height="212" rx="18" fill="#f5f7fa" stroke="#334" stroke-width="2"/>
  <text x="155" y="170" text-anchor="middle" font-size="12" font-weight="bold" fill="#234">Android · iOS · Electron · Qt</text>
  <!-- UI -->
  <rect x="60" y="176" width="190" height="34" rx="8" fill="#e8eef7" stroke="#4072b8"/>
  <text x="155" y="198" text-anchor="middle" font-size="12" font-weight="bold" fill="#234">App UI</text>
  <line x1="155" y1="210" x2="155" y2="244" stroke="#555" stroke-width="1.5" marker-start="url(#arrow)" marker-end="url(#arrow)"/>
  <text x="163" y="231" font-size="12" fill="#456">FFI / C ABI</text>
  <!-- mirobody form 1: embedded -->
  <rect x="60" y="244" width="190" height="48" rx="8" fill="#dff0e4" stroke="#2f7d4a" stroke-width="2.5"/>
  <rect x="121" y="250" width="68" height="16" rx="8" fill="#2f7d4a"/>
  <text x="155" y="262" text-anchor="middle" font-size="9.5" font-weight="bold" fill="#fff">embedded</text>
  <text x="155" y="284" text-anchor="middle" font-size="14" font-weight="bold" fill="#234">mirobody</text>
  <!-- on-device db -->
  <rect x="60" y="300" width="190" height="48" rx="8" fill="#fff3e0" stroke="#c08a3e" stroke-dasharray="4 3"/>
  <text x="155" y="322" text-anchor="middle" font-size="12" font-weight="bold" fill="#234">Your Health Data</text>
  <text x="155" y="338" text-anchor="middle" font-size="9" fill="#456">on-device (optional)</text>

  <!-- ===== Column 2: Server ===== -->
  <rect x="365" y="46" width="230" height="222" rx="18" fill="#f5f7fa" stroke="#334" stroke-width="2"/>
  <text x="480" y="66" text-anchor="middle" font-size="14" font-weight="bold" fill="#234">Server</text>
  <text x="480" y="82" text-anchor="middle" font-size="10" fill="#456">local / cloud host</text>
  <!-- mirobody form 2: standalone executable -->
  <rect x="385" y="92" width="190" height="38" rx="8" fill="#dff0e4" stroke="#2f7d4a" stroke-width="2.5"/>
  <rect x="443" y="96" width="74" height="16" rx="8" fill="#2f7d4a"/>
  <text x="480" y="108" text-anchor="middle" font-size="9.5" font-weight="bold" fill="#fff">executable</text>
  <text x="480" y="125" text-anchor="middle" font-size="13" font-weight="bold" fill="#234">mirobody</text>
  <!-- mirobody form 3: library embedded by language hosts -->
  <rect x="378" y="138" width="204" height="68" rx="10" fill="#eef6f0" stroke="#3a8f57" stroke-dasharray="4 3"/>
  <text x="480" y="152" text-anchor="middle" font-size="9" fill="#456">C# · Java · Go · Rust · Python</text>
  <rect x="398" y="158" width="164" height="38" rx="8" fill="#dff0e4" stroke="#2f7d4a" stroke-width="2.5"/>
  <rect x="450" y="162" width="60" height="16" rx="8" fill="#2f7d4a"/>
  <text x="480" y="174" text-anchor="middle" font-size="9.5" font-weight="bold" fill="#fff">library</text>
  <text x="480" y="191" text-anchor="middle" font-size="13" font-weight="bold" fill="#234">mirobody</text>
  <!-- server db -->
  <rect x="385" y="214" width="190" height="42" rx="8" fill="#fff3e0" stroke="#c08a3e" stroke-dasharray="4 3"/>
  <text x="480" y="234" text-anchor="middle" font-size="12" font-weight="bold" fill="#234">Your Health Data</text>
  <text x="480" y="248" text-anchor="middle" font-size="9" fill="#456">beside the server (optional)</text>

  <!-- ===== Column 3: LLM ===== -->
  <rect x="700" y="150" width="220" height="80" rx="10" fill="#f3e8f7" stroke="#8a4fb0"/>
  <text x="810" y="178" text-anchor="middle" font-size="15" font-weight="bold" fill="#234">LLM providers</text>
  <text x="810" y="200" text-anchor="middle" font-size="10" fill="#456">OpenAI · Gemini · MiroThinker</text>
  <text x="810" y="217" text-anchor="middle" font-size="10" fill="#456">Qwen · DeepSeek</text>

  <!-- ===== Flows (bidirectional, horizontal curves on box edges) ===== -->
  <!-- Web <-> Server -->
  <path d="M270 81 C320 81 320 100 365 100" fill="none" stroke="#555" stroke-width="1.5" marker-start="url(#arrow)" marker-end="url(#arrow)"/>
  <text x="318" y="72" text-anchor="middle" font-size="12" fill="#456">HTTP / WS</text>
  <!-- Native <-> Server -->
  <path d="M270 200 C320 200 320 160 365 160" fill="none" stroke="#555" stroke-width="1.5" marker-start="url(#arrow)" marker-end="url(#arrow)"/>
  <text x="316" y="214" text-anchor="middle" font-size="12" fill="#456">HTTP / WS</text>
  <!-- Server <-> LLM -->
  <path d="M595 136 C650 136 650 185 700 185" fill="none" stroke="#555" stroke-width="1.5" marker-start="url(#arrow)" marker-end="url(#arrow)"/>
  <text x="648" y="127" text-anchor="middle" font-size="12" fill="#456">chat turn</text>
  <!-- Native <-> LLM directly (dips below the server) -->
  <path d="M270 320 C460 320 640 345 700 222" fill="none" stroke="#555" stroke-width="1.5" marker-start="url(#arrow)" marker-end="url(#arrow)"/>
  <text x="478" y="344" text-anchor="middle" font-size="12" fill="#c0392b">chat turn — native apps talk to the LLM directly</text>
</svg>

<div class="note">

**Frontend**

- **Native** (Android · iOS · Electron · Qt) — embed `mirobody` and keep all health data on-device. **<span style="color:#c0392b">Private by default</span>** — nothing leaves but the chat turn.
- **Web** (HTML · Miniapp) — UI only, talks to a `mirobody` server. **<span style="color:#c0392b">Maximum flexibility</span>** — zero install, runs anywhere, instant updates.

</div>

---

## Architecture at a glance

<svg class="layers" viewBox="0 0 960 466" font-family="Helvetica, Arial, sans-serif">
  <defs>
    <marker id="arr2" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0 0 L10 5 L0 10 z" fill="#555"/>
    </marker>
  </defs>

  <!-- ===== Layer 1: Hosts / front doors ===== -->
  <rect x="40" y="8" width="880" height="56" rx="14" fill="#e8eef7" stroke="#4072b8"/>
  <text x="480" y="27" text-anchor="middle" font-size="13" font-weight="bold" fill="#234">Hosts / front doors</text>
  <text x="480" y="44" text-anchor="middle" font-size="10.5" fill="#456">Android (JNI) · iOS (static lib) · Electron (koffi) · Qt · desktop &amp; server binary</text>
  <text x="480" y="58" text-anchor="middle" font-size="10.5" fill="#456">Python wheel · Go · C# · Node · Rust · WeChat miniapp — all via the extern "C" surface</text>

  <!-- seam: C ABI -->
  <line x1="480" y1="64" x2="480" y2="84" stroke="#555" stroke-width="1.5" marker-end="url(#arr2)"/>
  <text x="498" y="78" font-size="10.5" fill="#c0392b" font-weight="bold">src/mirobody.h — plain C ABI (extern "C")</text>

  <!-- ===== Layer 2: mirobody_core, three functional groups ===== -->
  <rect x="30" y="88" width="900" height="244" rx="16" fill="#fafafb" stroke="#aab" stroke-width="1.5"/>
  <text x="480" y="106" text-anchor="middle" font-size="13" font-weight="bold" fill="#234">mirobody_core <tspan font-size="10.5" font-weight="normal" fill="#456">— C++11; compile-time agent &amp; tool registries</tspan></text>

  <!-- ---- Group 1: Health ---- -->
  <rect x="40" y="114" width="280" height="208" rx="12" fill="#eef6f0" stroke="#2f7d4a" stroke-width="1.5"/>
  <rect x="52" y="120" width="256" height="24" rx="7" fill="#2f7d4a"/>
  <text x="180" y="137" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">Health <tspan font-size="9" font-weight="normal">· data ingest &amp; records</tspan></text>
  <g font-size="9.5" text-anchor="middle">
    <rect x="48"  y="150" width="60" height="22" rx="7" fill="#2f7d4a" stroke="#2f7d4a"/><text x="78"  y="165" fill="#fff" font-weight="bold">health</text>
    <rect x="116" y="150" width="60" height="22" rx="7" fill="#2f7d4a" stroke="#2f7d4a"/><text x="146" y="165" fill="#fff" font-weight="bold">vendors</text>
    <rect x="184" y="150" width="60" height="22" rx="7" fill="#dff0e4" stroke="#2f7d4a"/><text x="214" y="165" fill="#1c5233">fhir</text>
    <rect x="252" y="150" width="60" height="22" rx="7" fill="#dff0e4" stroke="#2f7d4a"/><text x="282" y="165" fill="#1c5233">transcode</text>
  </g>
  <g font-size="8.5" fill="#456">
    <text x="52" y="187" font-size="9.5" font-weight="bold" fill="#1c5233">vendors — 15+ brokers (pluggable registry)</text>
    <text x="52" y="200">Platforms: Terra · Validic · Vital · Rook · Spike</text>
    <text x="52" y="212">Thryve · Junction · Metriport · WeFitter</text>
    <text x="52" y="224">Devices: Fitbit · Garmin · Withings · Huawei</text>
    <text x="52" y="236">EHR: Redox · Particle Health · LexisNexis</text>
    <text x="52" y="256" font-size="9.5" font-weight="bold" fill="#1c5233">health — connect &amp; normalize</text>
    <text x="52" y="269">bind → consent → fetch, mapped to FHIR R4</text>
    <text x="52" y="281">wearable · activity · sleep · labs · vitals</text>
    <text x="52" y="301" font-size="9.5" font-weight="bold" fill="#1c5233">fhir / transcode</text>
    <text x="52" y="314">FHIR R4 · UCUM / SNOMED / LOINC / RxNorm</text>
  </g>

  <!-- ---- Group 2: AI ---- -->
  <rect x="340" y="114" width="280" height="208" rx="12" fill="#f5ecfa" stroke="#8a4fb0" stroke-width="1.5"/>
  <rect x="352" y="120" width="256" height="24" rx="7" fill="#8a4fb0"/>
  <text x="480" y="137" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">AI <tspan font-size="9" font-weight="normal">· agents &amp; models</tspan></text>
  <g font-size="9.5" text-anchor="middle">
    <rect x="348" y="150" width="60" height="22" rx="7" fill="#efe1f6" stroke="#8a4fb0"/><text x="378" y="165" fill="#5e2e80">chat</text>
    <rect x="416" y="150" width="60" height="22" rx="7" fill="#8a4fb0" stroke="#8a4fb0"/><text x="446" y="165" fill="#fff" font-weight="bold">llm</text>
    <rect x="484" y="150" width="60" height="22" rx="7" fill="#efe1f6" stroke="#8a4fb0"/><text x="514" y="165" fill="#5e2e80">mcp</text>
    <rect x="552" y="150" width="60" height="22" rx="7" fill="#efe1f6" stroke="#8a4fb0"/><text x="582" y="165" fill="#5e2e80">memory</text>
  </g>
  <g font-size="8.5" fill="#456">
    <text x="352" y="187" font-size="9.5" font-weight="bold" fill="#5e2e80">llm — streaming provider clients</text>
    <text x="352" y="200">Chat: OpenAI · Gemini · MiroThinker</text>
    <text x="352" y="212">Qwen · DeepSeek · Azure OpenAI</text>
    <text x="352" y="224">Realtime: OpenAI Realtime · Gemini Live</text>
    <text x="352" y="236">Embeddings: vector clients</text>
    <text x="352" y="256" font-size="9.5" font-weight="bold" fill="#5e2e80">chat — agent runtime</text>
    <text x="352" y="269">provider pick → system prompt → streamed turn</text>
    <text x="352" y="289" font-size="9.5" font-weight="bold" fill="#5e2e80">mcp / memory</text>
    <text x="352" y="302">MCP JSON-RPC tools (OAuth-gated)</text>
    <text x="352" y="314">durable agent memory</text>
  </g>

  <!-- ---- Group 3: System ---- -->
  <rect x="640" y="114" width="280" height="208" rx="12" fill="#e9eff7" stroke="#4072b8" stroke-width="1.5"/>
  <rect x="652" y="120" width="256" height="24" rx="7" fill="#4072b8"/>
  <text x="780" y="137" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">System <tspan font-size="9" font-weight="normal">· serving · auth · accounts</tspan></text>
  <g font-size="9.5" text-anchor="middle">
    <rect x="648" y="150" width="60" height="22" rx="7" fill="#dde7f5" stroke="#4072b8"/><text x="678" y="165" fill="#234f86">server</text>
    <rect x="716" y="150" width="60" height="22" rx="7" fill="#dde7f5" stroke="#4072b8"/><text x="746" y="165" fill="#234f86">user</text>
    <rect x="784" y="150" width="60" height="22" rx="7" fill="#dde7f5" stroke="#4072b8"/><text x="814" y="165" fill="#234f86">oauth</text>
    <rect x="852" y="150" width="60" height="22" rx="7" fill="#dde7f5" stroke="#4072b8"/><text x="882" y="165" fill="#234f86">jwt</text>
  </g>
  <g font-size="8.5" fill="#456">
    <text x="652" y="187" font-size="9.5" font-weight="bold" fill="#234f86">server — HTTP + WebSocket</text>
    <text x="652" y="200">libwebsockets · REST + WS · static mounts</text>
    <text x="652" y="220" font-size="9.5" font-weight="bold" fill="#234f86">oauth · jwt — auth</text>
    <text x="652" y="233">OAuth 2.0 + PKCE · signed JWT sessions</text>
    <text x="652" y="245">login: password · QR (Tanka)</text>
    <text x="652" y="265" font-size="9.5" font-weight="bold" fill="#234f86">user — accounts</text>
    <text x="652" y="278">profiles · login methods · sessions</text>
    <text x="652" y="298" font-size="9.5" font-weight="bold" fill="#234f86">client · config · platform</text>
    <text x="652" y="311">outbound HTTP · env config · OS shims</text>
  </g>

  <!-- seam: pluggable backends -->
  <line x1="480" y1="332" x2="480" y2="352" stroke="#555" stroke-width="1.5" marker-end="url(#arr2)"/>
  <text x="498" y="346" font-size="10.5" fill="#c0392b" font-weight="bold">one interface each — swappable at build / runtime</text>

  <!-- ===== Layer 3: Pluggable backends ===== -->
  <!-- Database -->
  <rect x="40" y="356" width="280" height="104" rx="12" fill="#fff3e0" stroke="#c08a3e" stroke-width="1.5"/>
  <rect x="56" y="364" width="248" height="22" rx="7" fill="#c08a3e"/>
  <text x="180" y="380" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">database</text>
  <g text-anchor="middle" fill="#7a5419">
    <text x="180" y="404" font-size="10.5">SQLite · Postgres · MySQL</text>
    <text x="180" y="420" font-size="10.5">DuckDB · ClickHouse</text>
    <text x="180" y="442" font-size="9" fill="#9a7a4a">SQL persistence · schema-managed</text>
  </g>

  <!-- Cache -->
  <rect x="340" y="356" width="280" height="104" rx="12" fill="#fff3e0" stroke="#c08a3e" stroke-width="1.5"/>
  <rect x="356" y="364" width="248" height="22" rx="7" fill="#c08a3e"/>
  <text x="480" y="380" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">cache</text>
  <g text-anchor="middle" fill="#7a5419">
    <text x="480" y="404" font-size="10.5">in-process KV · Redis</text>
    <text x="480" y="420" font-size="9" fill="#9a7a4a">Redis-flavored set / get / TTL</text>
    <text x="480" y="442" font-size="9" fill="#9a7a4a">in-process = zero-config default</text>
  </g>

  <!-- Storage -->
  <rect x="640" y="356" width="280" height="104" rx="12" fill="#fff3e0" stroke="#c08a3e" stroke-width="1.5"/>
  <rect x="656" y="364" width="248" height="22" rx="7" fill="#c08a3e"/>
  <text x="780" y="380" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">storage</text>
  <g text-anchor="middle" fill="#7a5419">
    <text x="780" y="404" font-size="10.5">Local FS · AWS S3</text>
    <text x="780" y="420" font-size="10.5">Alibaba OSS · Azure Blob</text>
    <text x="780" y="442" font-size="9" fill="#9a7a4a">object store · signed URLs · CDN</text>
  </g>
</svg>
