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

# What is Mirobody

<svg class="hero" style="height: 500px" viewBox="0 -32 960 452" font-family="Helvetica, Arial, sans-serif">
  <defs>
    <marker id="dot" viewBox="0 0 10 10" refX="5" refY="5" markerWidth="5" markerHeight="5">
      <circle cx="5" cy="5" r="4" fill="#555"/>
    </marker>
  </defs>
  <text x="480" y="-8" text-anchor="middle" font-size="16" fill="#456">One health AI — runs anywhere, <tspan font-weight="bold" fill="#2f7d4a">your data stays yours.</tspan></text>
  <!-- column dividers -->
  <line x1="320" y1="62" x2="320" y2="378" stroke="#ccc" stroke-width="1.5" stroke-dasharray="5 6"/>
  <line x1="640" y1="62" x2="640" y2="378" stroke="#ccc" stroke-width="1.5" stroke-dasharray="5 6"/>
  <!-- ============ COLUMN 1: on a server ============ -->
  <text x="160" y="42" text-anchor="middle" font-size="18" font-weight="bold" fill="#4072b8">On a server</text>
  <text x="160" y="60" text-anchor="middle" font-size="11" fill="#456">Self-hosted — the whole family</text>
  <!-- stick person -->
  <g stroke="#234" stroke-width="2.5" fill="none" stroke-linecap="round">
    <circle cx="52" cy="180" r="14" fill="#fff"/>
    <line x1="52" y1="194" x2="52" y2="236"/>
    <line x1="52" y1="205" x2="36" y2="222"/>
    <line x1="52" y1="205" x2="76" y2="218"/>
    <line x1="52" y1="236" x2="40" y2="266"/>
    <line x1="52" y1="236" x2="64" y2="266"/>
  </g>
  <path d="M45 184 Q52 190 59 184" stroke="#234" stroke-width="1.8" fill="none"/>
  <!-- speech bubble -->
  <g>
    <path d="M18 98 H90 A10 10 0 0 1 100 108 V128 A10 10 0 0 1 90 138 H66 L54 155 L46 138 H18 A10 10 0 0 1 8 128 V108 A10 10 0 0 1 18 98 Z" fill="#e8eef7" stroke="#4072b8"/>
    <text x="54" y="116" text-anchor="middle" font-size="10.5" fill="#274b73">“How is my</text>
    <text x="54" y="131" text-anchor="middle" font-size="10.5" fill="#274b73">family?”</text>
  </g>
  <!-- arrow person -> server -->
  <line x1="78" y1="212" x2="112" y2="205" stroke="#4072b8" stroke-width="2.5" marker-end="url(#dot)"/>
  <!-- server -->
  <rect x="116" y="120" width="96" height="128" rx="9" fill="#fff" stroke="#234" stroke-width="3"/>
  <line x1="116" y1="148" x2="212" y2="148" stroke="#234" stroke-width="1.5"/>
  <circle cx="130" cy="134" r="3" fill="#2f7d4a"/>
  <circle cx="141" cy="134" r="3" fill="#cfd8e3"/>
  <rect x="126" y="162" width="76" height="30" rx="8" fill="#dff0e4" stroke="#2f7d4a" stroke-width="2.5"/>
  <text x="164" y="182" text-anchor="middle" font-size="12" font-weight="bold" fill="#1c5233">mirobody</text>
  <text x="164" y="216" text-anchor="middle" font-size="10" font-weight="bold" fill="#c08a3e">family's</text>
  <text x="164" y="229" text-anchor="middle" font-size="10" font-weight="bold" fill="#c08a3e">health data</text>
  <!-- family members' devices feeding the server (left-aligned for symmetry) -->
  <g>
    <rect x="247" y="123" width="30" height="44" rx="6" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="251" y="129" width="22" height="30" rx="2" fill="#eef3fa"/>
    <text x="262" y="179" text-anchor="middle" font-size="9" fill="#456">mom</text>
  </g>
  <g>
    <rect x="248" y="186" width="28" height="28" rx="7" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="252" y="190" width="20" height="20" rx="3" fill="#eef3fa"/>
    <text x="262" y="226" text-anchor="middle" font-size="9" fill="#456">dad</text>
  </g>
  <g>
    <rect x="240" y="242" width="44" height="26" rx="3" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="245" y="247" width="34" height="16" rx="2" fill="#eef3fa"/>
    <rect x="234" y="268" width="56" height="5" rx="2" fill="#fff" stroke="#234" stroke-width="1.6"/>
    <text x="262" y="285" text-anchor="middle" font-size="9" fill="#456">kid</text>
  </g>
  <!-- data into the server: dad horizontal; mom & kid mirror about it -->
  <line x1="214" y1="200" x2="244" y2="145" stroke="#4072b8" stroke-width="2.5" stroke-dasharray="4 4" marker-end="url(#dot)"/>
  <line x1="214" y1="200" x2="244" y2="200" stroke="#4072b8" stroke-width="2.5" stroke-dasharray="4 4" marker-end="url(#dot)"/>
  <line x1="214" y1="200" x2="240" y2="248" stroke="#4072b8" stroke-width="2.5" stroke-dasharray="4 4" marker-end="url(#dot)"/>
  <text x="160" y="400" text-anchor="middle" font-size="13" fill="#234">One brain for the family. <tspan fill="#4072b8" font-weight="bold">Shared.</tspan></text>
  <!-- ============ COLUMN 2: on your phone ============ -->
  <text x="485" y="42" text-anchor="middle" font-size="18" font-weight="bold" fill="#2f7d4a">On your phone</text>
  <text x="485" y="60" text-anchor="middle" font-size="11" fill="#456">Just you — works offline</text>
  <!-- stick person -->
  <g stroke="#234" stroke-width="2.5" fill="none" stroke-linecap="round">
    <circle cx="395" cy="180" r="14" fill="#fff"/>
    <line x1="395" y1="194" x2="395" y2="238"/>
    <line x1="395" y1="206" x2="377" y2="224"/>
    <line x1="395" y1="206" x2="420" y2="220"/>
    <line x1="395" y1="238" x2="382" y2="270"/>
    <line x1="395" y1="238" x2="408" y2="270"/>
  </g>
  <path d="M388 184 Q395 190 402 184" stroke="#234" stroke-width="1.8" fill="none"/>
  <!-- speech bubble -->
  <g>
    <path d="M356 98 H434 A10 10 0 0 1 444 108 V128 A10 10 0 0 1 434 138 H400 L386 155 L378 138 H356 A10 10 0 0 1 346 128 V108 A10 10 0 0 1 356 98 Z" fill="#eef6f0" stroke="#2f7d4a"/>
    <text x="395" y="116" text-anchor="middle" font-size="10.5" fill="#1c5233">“How did I</text>
    <text x="395" y="131" text-anchor="middle" font-size="10.5" fill="#1c5233">sleep?”</text>
  </g>
  <!-- arrow person -> phone -->
  <line x1="424" y1="202" x2="464" y2="200" stroke="#2f7d4a" stroke-width="2.5" marker-end="url(#dot)"/>
  <!-- phone -->
  <rect x="468" y="108" width="104" height="184" rx="16" fill="#fff" stroke="#234" stroke-width="3"/>
  <rect x="478" y="126" width="84" height="150" rx="6" fill="#f7faf8"/>
  <circle cx="520" cy="284" r="4" fill="#fff" stroke="#234" stroke-width="2"/>
  <rect x="486" y="146" width="68" height="30" rx="8" fill="#dff0e4" stroke="#2f7d4a" stroke-width="2.5"/>
  <text x="520" y="166" text-anchor="middle" font-size="13" font-weight="bold" fill="#1c5233">mirobody</text>
  <g transform="translate(504,196)">
    <rect x="0" y="8" width="32" height="24" rx="4" fill="#fff3e0" stroke="#c08a3e" stroke-width="2"/>
    <path d="M6 8 V3 a10 10 0 0 1 20 0 V8" fill="none" stroke="#c08a3e" stroke-width="2.2"/>
    <circle cx="16" cy="19" r="3" fill="#c08a3e"/>
  </g>
  <text x="520" y="252" text-anchor="middle" font-size="10" font-weight="bold" fill="#c08a3e">your data stays</text>
  <text x="520" y="265" text-anchor="middle" font-size="10" font-weight="bold" fill="#c08a3e">on the phone</text>
  <text x="485" y="400" text-anchor="middle" font-size="13" fill="#234">Runs on the phone. <tspan fill="#c0392b" font-weight="bold">Private.</tspan></text>
  <!-- ============ COLUMN 3: peer-to-peer (planned) ============ -->
  <text x="800" y="42" text-anchor="middle" font-size="18" font-weight="bold" fill="#8a4fb0">Peer-to-peer</text>
  <text x="800" y="60" text-anchor="middle" font-size="11" fill="#456">No server — free, optional ICE/STUN</text>
  <!-- the user, asking across their own devices -->
  <g stroke="#234" stroke-width="2.5" fill="none" stroke-linecap="round">
    <circle cx="680" cy="215" r="13" fill="#fff"/>
    <line x1="680" y1="228" x2="680" y2="266"/>
    <line x1="680" y1="238" x2="666" y2="253"/>
    <line x1="680" y1="238" x2="700" y2="250"/>
    <line x1="680" y1="266" x2="669" y2="292"/>
    <line x1="680" y1="266" x2="691" y2="292"/>
  </g>
  <path d="M674 219 Q680 224 686 219" stroke="#234" stroke-width="1.8" fill="none"/>
  <g>
    <path d="M670 142 H740 A10 10 0 0 1 750 152 V168 A10 10 0 0 1 740 178 H700 L686 194 L678 178 H670 A10 10 0 0 1 660 168 V152 A10 10 0 0 1 670 142 Z" fill="#f3e8f7" stroke="#8a4fb0"/>
    <text x="705" y="158" text-anchor="middle" font-size="10" fill="#5e2e80">“How is</text>
    <text x="705" y="171" text-anchor="middle" font-size="10" fill="#5e2e80">everyone?”</text>
  </g>
  <line x1="702" y1="253" x2="730" y2="266" stroke="#8a4fb0" stroke-width="2.5" marker-end="url(#dot)"/>
  <!-- mesh links (dashed = not built yet) — equilateral triangle -->
  <line x1="824" y1="158" x2="770" y2="252" stroke="#8a4fb0" stroke-width="2.5" stroke-dasharray="4 4" marker-start="url(#dot)" marker-end="url(#dot)"/>
  <line x1="856" y1="158" x2="908" y2="252" stroke="#8a4fb0" stroke-width="2.5" stroke-dasharray="4 4" marker-start="url(#dot)" marker-end="url(#dot)"/>
  <line x1="790" y1="270" x2="895" y2="270" stroke="#8a4fb0" stroke-width="2.5" stroke-dasharray="4 4" marker-start="url(#dot)" marker-end="url(#dot)"/>
  <!-- node: your phone (top) -->
  <g>
    <text x="840" y="94" text-anchor="middle" font-size="9" fill="#456">your phone</text>
    <rect x="819" y="101" width="42" height="60" rx="9" fill="#fff" stroke="#8a4fb0" stroke-width="2.5" stroke-dasharray="5 4"/>
    <rect x="825" y="111" width="30" height="40" rx="3" fill="#f3e8f7"/>
    <circle cx="840" cy="131" r="5" fill="#2f7d4a"/>
  </g>
  <!-- node: your laptop (bottom-left) -->
  <g>
    <rect x="734" y="255" width="52" height="30" rx="3" fill="#fff" stroke="#8a4fb0" stroke-width="2.5" stroke-dasharray="5 4"/>
    <rect x="739" y="260" width="42" height="20" rx="2" fill="#f3e8f7"/>
    <rect x="726" y="285" width="68" height="5" rx="2" fill="#fff" stroke="#8a4fb0" stroke-width="2" stroke-dasharray="5 4"/>
    <circle cx="760" cy="270" r="5" fill="#2f7d4a"/>
    <text x="760" y="304" text-anchor="middle" font-size="9" fill="#456">your laptop</text>
  </g>
  <!-- node: family (bottom-right) -->
  <g>
    <rect x="899" y="240" width="42" height="60" rx="9" fill="#fff" stroke="#8a4fb0" stroke-width="2.5" stroke-dasharray="5 4"/>
    <rect x="905" y="250" width="30" height="40" rx="3" fill="#f3e8f7"/>
    <circle cx="920" cy="270" r="5" fill="#2f7d4a"/>
    <text x="920" y="316" text-anchor="middle" font-size="9" fill="#456">family</text>
  </g>
  <!-- end-to-end note -->
  <g transform="translate(742,320)">
    <rect x="0" y="5" width="16" height="13" rx="3" fill="#f3e8f7" stroke="#8a4fb0" stroke-width="1.6"/>
    <path d="M4 5 V2 a4 4 0 0 1 8 0 V5" fill="none" stroke="#8a4fb0" stroke-width="1.6"/>
  </g>
  <text x="838" y="332" text-anchor="middle" font-size="10" fill="#5e2e80">end-to-end encrypted</text>
  <text x="800" y="400" text-anchor="middle" font-size="13" fill="#234">Devices link directly. <tspan fill="#8a4fb0" font-weight="bold">No server.</tspan></text>
</svg>

---

# Where your data comes from

<svg class="hero" style="height: 500px" viewBox="0 -32 960 452" font-family="Helvetica, Arial, sans-serif">
  <defs>
    <marker id="fa" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0 0 L10 5 L0 10 z" fill="#7a8aa0"/>
    </marker>
    <marker id="fg" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0 0 L10 5 L0 10 z" fill="#2f7d4a"/>
    </marker>
  </defs>
  <text x="480" y="-8" text-anchor="middle" font-size="16" fill="#456">From wearables to food photos — <tspan font-weight="bold" fill="#c08a3e">one standard format</tspan>, ready for AI.</text>
  <text x="132" y="38" text-anchor="middle" font-size="11" font-weight="bold" fill="#5a6b7a">YOUR DATA SOURCES</text>
  <!-- ===== source cards ===== -->
  <g>
    <rect x="24" y="50" width="216" height="54" rx="10" fill="#fff" stroke="#9bb1c9" stroke-width="1.5"/>
    <rect x="41" y="67" width="16" height="22" rx="5" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="45" y="62" width="8" height="6" rx="2" fill="#234"/>
    <rect x="45" y="88" width="8" height="6" rx="2" fill="#234"/>
    <circle cx="49" cy="78" r="3" fill="#4072b8"/>
    <text x="78" y="74" font-size="12" font-weight="bold" fill="#234">Wearables</text>
    <text x="78" y="90" font-size="9" fill="#5a6b7a">steps · heart rate · sleep</text>
  </g>
  <g>
    <rect x="24" y="112" width="216" height="54" rx="10" fill="#fff" stroke="#9bb1c9" stroke-width="1.5"/>
    <rect x="43" y="126" width="14" height="26" rx="3" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="46" y="130" width="8" height="15" rx="1" fill="#eef3fa"/>
    <text x="78" y="136" font-size="12" font-weight="bold" fill="#234">Phone health</text>
    <text x="78" y="152" font-size="9" fill="#5a6b7a">Apple Health · Health Connect</text>
  </g>
  <g>
    <rect x="24" y="174" width="216" height="54" rx="10" fill="#fff" stroke="#9bb1c9" stroke-width="1.5"/>
    <rect x="44" y="188" width="12" height="4" rx="1" fill="#234"/>
    <rect x="45" y="190" width="10" height="24" rx="5" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="47" y="203" width="6" height="9" rx="2" fill="#9fc1e0"/>
    <text x="78" y="198" font-size="12" font-weight="bold" fill="#234">Lab results</text>
    <text x="78" y="214" font-size="9" fill="#5a6b7a">blood tests · biomarkers</text>
  </g>
  <g>
    <rect x="24" y="236" width="216" height="54" rx="10" fill="#fff" stroke="#9bb1c9" stroke-width="1.5"/>
    <rect x="40" y="251" width="20" height="24" rx="2" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="48" y="256" width="4" height="14" fill="#c0392b"/>
    <rect x="43" y="261" width="14" height="4" fill="#c0392b"/>
    <text x="78" y="260" font-size="12" font-weight="bold" fill="#234">Clinic records</text>
    <text x="78" y="276" font-size="9" fill="#5a6b7a">diagnoses · meds · visits</text>
  </g>
  <g>
    <rect x="24" y="298" width="216" height="54" rx="10" fill="#fff" stroke="#9bb1c9" stroke-width="1.5"/>
    <rect x="40" y="316" width="22" height="16" rx="3" fill="#fff" stroke="#234" stroke-width="2"/>
    <rect x="47" y="312" width="9" height="5" rx="1.5" fill="#234"/>
    <circle cx="51" cy="325" r="3.5" fill="none" stroke="#234" stroke-width="2"/>
    <text x="78" y="322" font-size="12" font-weight="bold" fill="#234">Everyday logging</text>
    <text x="78" y="338" font-size="8.5" fill="#5a6b7a">food photos · voice mood · skin pics</text>
  </g>
  <!-- ===== converging arrows into mirobody ===== -->
  <line x1="244" y1="77" x2="382" y2="166" stroke="#7a8aa0" stroke-width="2" marker-end="url(#fa)"/>
  <line x1="244" y1="139" x2="382" y2="184" stroke="#7a8aa0" stroke-width="2" marker-end="url(#fa)"/>
  <line x1="244" y1="201" x2="382" y2="202" stroke="#7a8aa0" stroke-width="2" marker-end="url(#fa)"/>
  <line x1="244" y1="263" x2="382" y2="220" stroke="#7a8aa0" stroke-width="2" marker-end="url(#fa)"/>
  <line x1="244" y1="325" x2="382" y2="238" stroke="#7a8aa0" stroke-width="2" marker-end="url(#fa)"/>
  <!-- ===== mirobody normalizes to FHIR ===== -->
  <rect x="386" y="120" width="176" height="156" rx="14" fill="#f3faf5" stroke="#2f7d4a" stroke-width="2.5"/>
  <rect x="414" y="138" width="120" height="34" rx="9" fill="#dff0e4" stroke="#2f7d4a" stroke-width="2.5"/>
  <text x="474" y="161" text-anchor="middle" font-size="14" font-weight="bold" fill="#1c5233">mirobody</text>
  <text x="474" y="198" text-anchor="middle" font-size="11" fill="#456">normalizes everything to</text>
  <rect x="420" y="210" width="108" height="30" rx="8" fill="#fff3e0" stroke="#c08a3e" stroke-width="2"/>
  <text x="474" y="231" text-anchor="middle" font-size="16" font-weight="bold" fill="#9a6a1e">FHIR R4</text>
  <text x="474" y="258" text-anchor="middle" font-size="10" fill="#5a6b7a">one standard format</text>
  <!-- ===== output to you + your AI ===== -->
  <!-- mirobody feeds an AI model, which answers you -->
  <line x1="564" y1="199" x2="594" y2="199" stroke="#2f7d4a" stroke-width="2.5" marker-end="url(#fg)"/>
  <rect x="596" y="156" width="112" height="86" rx="12" fill="#f3e8f7" stroke="#8a4fb0" stroke-width="2.5"/>
  <path d="M652 166 L654.5 171.5 L660 174 L654.5 176.5 L652 182 L649.5 176.5 L644 174 L649.5 171.5 Z" fill="#8a4fb0"/>
  <text x="652" y="197" text-anchor="middle" font-size="15" font-weight="bold" fill="#5e2e80">AI model</text>
  <text x="652" y="214" text-anchor="middle" font-size="8.5" font-weight="bold" fill="#2f7d4a">on-device · Gemma</text>
  <text x="652" y="227" text-anchor="middle" font-size="8.5" fill="#8a4fb0">or OpenAI · Gemini</text>
  <line x1="710" y1="199" x2="740" y2="199" stroke="#2f7d4a" stroke-width="2.5" marker-end="url(#fg)"/>
  <g>
    <path d="M678 66 H808 A12 12 0 0 1 820 78 V100 A12 12 0 0 1 808 112 H788 L778 130 L770 112 H678 A12 12 0 0 1 666 100 V78 A12 12 0 0 1 678 66 Z" fill="#f3e8f7" stroke="#8a4fb0"/>
    <text x="743" y="86" text-anchor="middle" font-size="11" fill="#5e2e80">“Am I getting</text>
    <text x="743" y="101" text-anchor="middle" font-size="11" fill="#5e2e80">healthier?”</text>
  </g>
  <g stroke="#234" stroke-width="2.5" fill="none" stroke-linecap="round">
    <circle cx="770" cy="150" r="16" fill="#fff"/>
    <line x1="770" y1="166" x2="770" y2="224"/>
    <line x1="770" y1="180" x2="750" y2="200"/>
    <line x1="770" y1="180" x2="792" y2="196"/>
    <line x1="770" y1="224" x2="754" y2="258"/>
    <line x1="770" y1="224" x2="786" y2="258"/>
  </g>
  <path d="M762 154 Q770 161 778 154" stroke="#234" stroke-width="2" fill="none"/>
  <!-- the answer: plain words + an interactive chart -->
  <rect x="806" y="150" width="138" height="100" rx="10" fill="#fff" stroke="#8a4fb0" stroke-width="2"/>
  <text x="875" y="167" text-anchor="middle" font-size="8.5" font-weight="bold" fill="#5e2e80">Sleep up 12% this week</text>
  <line x1="818" y1="178" x2="818" y2="234" stroke="#d8cce4" stroke-width="1"/>
  <line x1="818" y1="234" x2="936" y2="234" stroke="#d8cce4" stroke-width="1"/>
  <polyline points="824,228 850,218 876,222 902,200 928,184" fill="none" stroke="#8a4fb0" stroke-width="2"/>
  <circle cx="824" cy="228" r="2" fill="#8a4fb0"/>
  <circle cx="850" cy="218" r="2" fill="#8a4fb0"/>
  <circle cx="876" cy="222" r="2" fill="#8a4fb0"/>
  <circle cx="902" cy="200" r="2" fill="#8a4fb0"/>
  <circle cx="928" cy="184" r="2" fill="#8a4fb0"/>
  <text x="875" y="247" text-anchor="middle" font-size="7.5" fill="#8a4fb0">words + interactive charts · ECharts</text>
  <text x="480" y="404" text-anchor="middle" font-size="13" fill="#234">Messy sources in — <tspan font-weight="bold" fill="#2f7d4a">one AI-ready format out.</tspan></text>
</svg>

---

# Your care circle

<svg class="hero" style="height: 500px" viewBox="0 -32 960 452" font-family="Helvetica, Arial, sans-serif">
  <defs>
    <marker id="ca" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0 0 L10 5 L0 10 z" fill="#7a8aa0"/>
    </marker>
  </defs>
  <text x="480" y="-8" text-anchor="middle" font-size="16" fill="#456">Invite the people you trust — then <tspan font-weight="bold" fill="#4072b8">choose what to share.</tspan></text>
  <!-- ============ LEFT: how it works (3 steps) ============ -->
  <text x="120" y="40" text-anchor="middle" font-size="11" font-weight="bold" fill="#5a6b7a">HOW IT WORKS</text>
  <circle cx="48" cy="86" r="13" fill="#4072b8"/>
  <text x="48" y="91" text-anchor="middle" font-size="13" font-weight="bold" fill="#fff">1</text>
  <text x="70" y="82" font-size="12" font-weight="bold" fill="#234">Create a circle</text>
  <text x="70" y="98" font-size="9" fill="#5a6b7a">name it — it's yours</text>
  <circle cx="48" cy="140" r="13" fill="#4072b8"/>
  <text x="48" y="145" text-anchor="middle" font-size="13" font-weight="bold" fill="#fff">2</text>
  <text x="70" y="136" font-size="12" font-weight="bold" fill="#234">Invite by email</text>
  <text x="70" y="152" font-size="9" fill="#5a6b7a">the people you trust</text>
  <circle cx="48" cy="194" r="13" fill="#4072b8"/>
  <text x="48" y="199" text-anchor="middle" font-size="13" font-weight="bold" fill="#fff">3</text>
  <text x="70" y="190" font-size="12" font-weight="bold" fill="#234">They accept</text>
  <text x="70" y="206" font-size="9" fill="#5a6b7a">acceptance required</text>
  <!-- you're in control -->
  <rect x="24" y="232" width="234" height="150" rx="12" fill="#f7f9fc" stroke="#9bb1c9" stroke-width="1.5"/>
  <text x="40" y="256" font-size="11.5" font-weight="bold" fill="#234">You're in control</text>
  <circle cx="46" cy="282" r="3" fill="#4072b8"/>
  <text x="58" y="286" font-size="9" fill="#456">acceptance required to join</text>
  <circle cx="46" cy="308" r="3" fill="#4072b8"/>
  <text x="58" y="312" font-size="9" fill="#456">remove a member anytime</text>
  <circle cx="46" cy="334" r="3" fill="#4072b8"/>
  <text x="58" y="338" font-size="9" fill="#456">unshare a thread anytime</text>
  <circle cx="46" cy="360" r="3" fill="#4072b8"/>
  <text x="58" y="364" font-size="9" fill="#456">health stays off until you allow it</text>
  <line x1="274" y1="209" x2="322" y2="209" stroke="#7a8aa0" stroke-width="2" marker-end="url(#ca)"/>
  <!-- ============ CENTER: the circle of members (centered on y=209) ============ -->
  <ellipse cx="460" cy="209" rx="138" ry="98" fill="#eef3fa" stroke="#4072b8" stroke-width="2" stroke-dasharray="6 5"/>
  <text x="460" y="136" text-anchor="middle" font-size="14" font-weight="bold" fill="#2c4a73">Care circle</text>
  <!-- mutual connections (drawn under the avatars) -->
  <line x1="460" y1="182" x2="410" y2="246" stroke="#9bbbe0" stroke-width="1.5"/>
  <line x1="460" y1="182" x2="512" y2="246" stroke="#9bbbe0" stroke-width="1.5"/>
  <line x1="410" y1="246" x2="512" y2="246" stroke="#9bbbe0" stroke-width="1.5"/>
  <!-- member: you -->
  <circle cx="460" cy="182" r="15" fill="#fff" stroke="#234" stroke-width="2"/>
  <path d="M453 184 Q460 190 467 184" stroke="#234" stroke-width="1.6" fill="none"/>
  <circle cx="471" cy="193" r="6" fill="#2f7d4a"/>
  <path d="M468 193 L470 195 L474 191" stroke="#fff" stroke-width="1.6" fill="none"/>
  <text x="460" y="161" text-anchor="middle" font-size="10" font-weight="bold" fill="#234">you</text>
  <!-- member: mom -->
  <circle cx="410" cy="246" r="15" fill="#fff" stroke="#234" stroke-width="2"/>
  <path d="M403 248 Q410 254 417 248" stroke="#234" stroke-width="1.6" fill="none"/>
  <circle cx="421" cy="257" r="6" fill="#2f7d4a"/>
  <path d="M418 257 L420 259 L424 255" stroke="#fff" stroke-width="1.6" fill="none"/>
  <text x="410" y="280" text-anchor="middle" font-size="10" fill="#456">mom</text>
  <!-- member: dad -->
  <circle cx="512" cy="246" r="15" fill="#fff" stroke="#234" stroke-width="2"/>
  <path d="M505 248 Q512 254 519 248" stroke="#234" stroke-width="1.6" fill="none"/>
  <circle cx="523" cy="257" r="6" fill="#2f7d4a"/>
  <path d="M520 257 L522 259 L526 255" stroke="#fff" stroke-width="1.6" fill="none"/>
  <text x="512" y="280" text-anchor="middle" font-size="10" fill="#456">dad</text>
  <text x="460" y="326" text-anchor="middle" font-size="10" fill="#5a6b7a">accepted members are mutually in the circle</text>
  <line x1="598" y1="209" x2="646" y2="209" stroke="#7a8aa0" stroke-width="2" marker-end="url(#ca)"/>
  <!-- ============ RIGHT: what you can share ============ -->
  <text x="800" y="40" text-anchor="middle" font-size="11" font-weight="bold" fill="#5a6b7a">WHAT YOU CAN SHARE</text>
  <!-- card 1: a conversation, with its access level (ShareAccess: View | Edit) -->
  <rect x="662" y="70" width="278" height="96" rx="12" fill="#f3faf5" stroke="#2f7d4a" stroke-width="2"/>
  <path d="M684 90 H706 A8 8 0 0 1 714 98 V106 A8 8 0 0 1 706 114 H698 L692 122 L688 114 H684 A8 8 0 0 1 676 106 V98 A8 8 0 0 1 684 90 Z" fill="#dff0e4" stroke="#2f7d4a" stroke-width="1.6"/>
  <text x="730" y="100" font-size="12.5" font-weight="bold" fill="#1c5233">Share a conversation</text>
  <text x="730" y="116" font-size="9" fill="#5a6b7a">it shows up in their history</text>
  <text x="730" y="147" font-size="8.5" fill="#5a6b7a">access</text>
  <rect x="772" y="137" width="42" height="15" rx="7.5" fill="#dff0e4" stroke="#2f7d4a" stroke-width="1.3"/>
  <text x="793" y="148" text-anchor="middle" font-size="8.5" font-weight="bold" fill="#1c5233">View</text>
  <rect x="820" y="137" width="42" height="15" rx="7.5" fill="#fff" stroke="#2f7d4a" stroke-width="1.3"/>
  <text x="841" y="148" text-anchor="middle" font-size="8.5" font-weight="bold" fill="#1c5233">Edit</text>
  <!-- card 2: health data (per-person switch) -->
  <rect x="662" y="180" width="278" height="84" rx="12" fill="#fff7ec" stroke="#c08a3e" stroke-width="2"/>
  <rect x="680" y="213" width="36" height="18" rx="9" fill="#fff" stroke="#c08a3e" stroke-width="2"/>
  <circle cx="689" cy="222" r="6.5" fill="#c08a3e"/>
  <text x="732" y="213" font-size="12.5" font-weight="bold" fill="#9a6a1e">Share health data</text>
  <text x="732" y="230" font-size="9" fill="#5a6b7a">your switch — off by default</text>
  <text x="732" y="246" font-size="9" fill="#5a6b7a">mutual — each member controls their own</text>
  <!-- card 3: what sharing health data unlocks (ties back to slide 1) -->
  <rect x="662" y="278" width="278" height="96" rx="12" fill="#f3e8f7" stroke="#8a4fb0" stroke-width="2"/>
  <path d="M692 300 L694.5 305.5 L700 308 L694.5 310.5 L692 316 L689.5 310.5 L684 308 L689.5 305.5 Z" fill="#8a4fb0"/>
  <text x="712" y="304" font-size="12.5" font-weight="bold" fill="#5e2e80">Then the AI can answer</text>
  <text x="712" y="328" font-size="11" fill="#5e2e80">“How is my family doing?”</text>
  <text x="712" y="348" font-size="9" fill="#5a6b7a">reads only what members chose to share</text>
  <text x="480" y="404" text-anchor="middle" font-size="13" fill="#234">Invite-only, acceptance required — <tspan font-weight="bold" fill="#4072b8">every share is your choice.</tspan></text>
</svg>

---

# Mirobody v2 — Deployment

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
  <text x="810" y="172" text-anchor="middle" font-size="14" font-weight="bold" fill="#234">LLM providers</text>
  <text x="810" y="190" text-anchor="middle" font-size="10" font-weight="bold" fill="#2f7d4a">on-device · Gemma</text>
  <text x="810" y="206" text-anchor="middle" font-size="10" fill="#456">OpenAI · Gemini · MiroThinker</text>
  <text x="810" y="221" text-anchor="middle" font-size="10" fill="#456">Qwen · DeepSeek</text>
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

- **Native** (Android · iOS · Electron · Qt) — embed `mirobody` and keep all health data on-device. **<span style="color:#c0392b">Private by default</span>** — and with an **on-device LLM (Gemma)**, even the chat turn can stay on the device.
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

---

## Health data flow — with a backend vs. fully on-device

<svg class="layers" viewBox="0 0 960 400" font-family="Helvetica, Arial, sans-serif">
  <defs>
    <marker id="arrd" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0 0 L10 5 L0 10 z" fill="#555"/>
    </marker>
  </defs>
  <!-- Band 1 : with a backend server -->
  <text x="32" y="30" font-size="13" font-weight="bold" fill="#234">① With a backend server</text>
  <!-- legend: the cylinder marks a data store; plain boxes are processes -->
  <path d="M206 33 V43 A8 3 0 0 0 222 43 V33" fill="#888" fill-opacity="0.5"/>
  <ellipse cx="214" cy="33" rx="8" ry="3" fill="#888"/>
  <text x="230" y="43" font-size="9" fill="#666">= data store (table / object store); plain boxes are processes</text>
  <!-- Device -->
  <rect x="28" y="65" width="118" height="76" rx="12" fill="#eef6f0" stroke="#2f7d4a" stroke-width="1.5"/>
  <rect x="36" y="70" width="102" height="18" rx="6" fill="#2f7d4a"/>
  <text x="87" y="83" text-anchor="middle" font-size="10" font-weight="bold" fill="#fff">Device</text>
  <g font-size="8.5" fill="#456">
    <text x="38" y="106">collect · batch</text>
    <text x="38" y="120">a day of samples</text>
    <text x="38" y="134" fill="#9a7a4a">Apple · HC · Fitbit</text>
  </g>
  <line x1="148" y1="103" x2="225" y2="103" stroke="#555" stroke-width="1.6" marker-end="url(#arrd)"/>
  <text x="186" y="96" text-anchor="middle" font-size="7.5" font-weight="bold" fill="#c0392b">HTTPS</text>
  <!-- health_ingest_staging (transient inbox) -->
  <rect x="227" y="65" width="142" height="76" rx="12" fill="#e9eff7" stroke="#4072b8" stroke-width="1.5"/>
  <path d="M233 92 V114 A10 3.5 0 0 0 253 114 V92" fill="#4072b8" fill-opacity="0.5"/>
  <ellipse cx="243" cy="92" rx="10" ry="3.5" fill="#4072b8"/>
  <text x="258" y="86" font-size="7.5" font-weight="bold" fill="#234f86">health_ingest_staging</text>
  <g font-size="7.5" fill="#456">
    <text x="258" y="101">transient inbox (pg)</text>
    <text x="258" y="114">raw batch · opaque bytes</text>
    <text x="258" y="127" fill="#9a7a4a">deleted after drain</text>
  </g>
  <line x1="371" y1="103" x2="448" y2="103" stroke="#555" stroke-width="1.6" marker-end="url(#arrd)"/>
  <text x="409" y="96" text-anchor="middle" font-size="7" font-weight="bold" fill="#c0392b">claim</text>
  <text x="409" y="116" text-anchor="middle" font-size="6.5" fill="#456">format ≤ max</text>
  <!-- DuckDB worker (drains the inbox; runs the pipeline) -->
  <rect x="450" y="52" width="200" height="102" rx="12" fill="#f5ecfa" stroke="#8a4fb0" stroke-width="1.5"/>
  <rect x="458" y="57" width="184" height="18" rx="6" fill="#8a4fb0"/>
  <text x="550" y="70" text-anchor="middle" font-size="10" font-weight="bold" fill="#fff">Ingest worker</text>
  <text x="550" y="89" text-anchor="middle" font-size="7.5" fill="#5e2e80">decode(format) → pipeline</text>
  <g font-size="8" text-anchor="middle">
    <rect x="458" y="95" width="58" height="20" rx="6" fill="#efe1f6" stroke="#8a4fb0"/><text x="487" y="109" fill="#5e2e80">Validate</text>
    <rect x="520" y="95" width="66" height="20" rx="6" fill="#efe1f6" stroke="#8a4fb0"/><text x="553" y="109" fill="#5e2e80">Normalize</text>
    <rect x="590" y="95" width="52" height="20" rx="6" fill="#efe1f6" stroke="#8a4fb0"/><text x="616" y="109" fill="#5e2e80">Rollup</text>
  </g>
  <text x="550" y="134" text-anchor="middle" font-size="7.5" fill="#5e2e80">reads inbox · writes the three stores →</text>
  <!-- fan-out from the worker's right-middle: middle horizontal, top/bottom symmetric about it -->
  <line x1="650" y1="103" x2="731" y2="49" stroke="#555" stroke-width="1.5" marker-end="url(#arrd)"/>
  <line x1="650" y1="103" x2="731" y2="103" stroke="#555" stroke-width="1.5" marker-end="url(#arrd)"/>
  <line x1="650" y1="103" x2="731" y2="157" stroke="#555" stroke-width="1.4" stroke-dasharray="4 3" marker-end="url(#arrd)"/>
  <!-- fhir_resources (summaries) -->
  <rect x="731" y="28" width="200" height="42" rx="10" fill="#e9eff7" stroke="#4072b8" stroke-width="1.4"/>
  <path d="M737 38 V60 A10 3.5 0 0 0 757 60 V38" fill="#4072b8" fill-opacity="0.5"/>
  <ellipse cx="747" cy="38" rx="10" ry="3.5" fill="#4072b8"/>
  <text x="765" y="45" font-size="9.5" font-weight="bold" fill="#234f86">fhir_resources</text>
  <text x="765" y="61" font-size="7.5" fill="#456">summary Observations · FHIR truth</text>
  <!-- health_facts (hot) — the horizontal arrow lands here -->
  <rect x="731" y="82" width="200" height="42" rx="10" fill="#fff3e0" stroke="#c08a3e" stroke-width="1.4"/>
  <path d="M737 92 V114 A10 3.5 0 0 0 757 114 V92" fill="#c08a3e" fill-opacity="0.5"/>
  <ellipse cx="747" cy="92" rx="10" ry="3.5" fill="#c08a3e"/>
  <text x="765" y="99" font-size="9.5" font-weight="bold" fill="#7a5419">health_facts — hot</text>
  <text x="765" y="115" font-size="7.5" fill="#9a7a4a">trends (SQL) · semantic recall (cosine)</text>
  <!-- Parquet (cold) -->
  <rect x="731" y="136" width="200" height="42" rx="10" fill="#fff3e0" stroke="#c08a3e" stroke-width="1.3" stroke-dasharray="5 3"/>
  <path d="M737 146 V168 A10 3.5 0 0 0 757 168 V146" fill="#c08a3e" fill-opacity="0.5"/>
  <ellipse cx="747" cy="146" rx="10" ry="3.5" fill="#c08a3e"/>
  <text x="765" y="153" font-size="9.5" font-weight="bold" fill="#7a5419">Parquet — raw, cold</text>
  <text x="765" y="169" font-size="7.5" fill="#9a7a4a">every sample · object storage</text>
  <!-- shared-core annotation -->
  <line x1="32" y1="224" x2="928" y2="224" stroke="#ddd" stroke-width="1"/>
  <text x="480" y="243" text-anchor="middle" font-size="10.5" font-weight="bold" fill="#c0392b">Server decouples ingest via a transient inbox + a background worker (the hot path can't link the Parquet engine); on-device runs the same pipeline inline.</text>
  <!-- Band 2 : fully on-device -->
  <text x="32" y="272" font-size="13" font-weight="bold" fill="#234">② Without a backend — fully on-device</text>
  <!-- Device (on-device) -->
  <rect x="28" y="286" width="152" height="90" rx="12" fill="#eef6f0" stroke="#2f7d4a" stroke-width="1.5"/>
  <rect x="36" y="291" width="136" height="19" rx="6" fill="#2f7d4a"/>
  <text x="104" y="305" text-anchor="middle" font-size="10.5" font-weight="bold" fill="#fff">Device</text>
  <g font-size="8.5" fill="#456">
    <text x="40" y="330">collect samples</text>
    <text x="40" y="344">batch · map → FHIR Obs</text>
    <text x="40" y="360" fill="#9a7a4a">no upload needed</text>
  </g>
  <line x1="182" y1="331" x2="278" y2="331" stroke="#555" stroke-width="1.6" marker-end="url(#arrd)"/>
  <text x="230" y="324" text-anchor="middle" font-size="7.5" font-weight="bold" fill="#c0392b">C ABI</text>
  <!-- ingest pipeline (in-process) -->
  <rect x="280" y="286" width="252" height="90" rx="12" fill="#fafafb" stroke="#2f7d4a" stroke-width="1.5"/>
  <rect x="288" y="291" width="236" height="19" rx="6" fill="#2f7d4a"/>
  <text x="406" y="305" text-anchor="middle" font-size="10" font-weight="bold" fill="#fff">ingest pipeline · mirobody_core</text>
  <g font-size="8.5" text-anchor="middle">
    <rect x="288" y="322" width="74" height="22" rx="7" fill="#dff0e4" stroke="#2f7d4a"/><text x="325" y="337" fill="#1c5233">Validate</text>
    <rect x="368" y="322" width="82" height="22" rx="7" fill="#dff0e4" stroke="#2f7d4a"/><text x="409" y="337" fill="#1c5233">Normalize</text>
    <rect x="456" y="322" width="68" height="22" rx="7" fill="#dff0e4" stroke="#2f7d4a"/><text x="490" y="337" fill="#1c5233">Rollup</text>
  </g>
  <text x="406" y="362" text-anchor="middle" font-size="8" fill="#456">the same stages, run in-process</text>
  <line x1="534" y1="331" x2="629" y2="331" stroke="#555" stroke-width="1.6" marker-end="url(#arrd)"/>
  <!-- SQLite -->
  <rect x="631" y="286" width="300" height="90" rx="12" fill="#fff3e0" stroke="#c08a3e" stroke-width="1.5"/>
  <path d="M639 320 V342 A10 3.5 0 0 0 659 342 V320" fill="#c08a3e" fill-opacity="0.5"/>
  <ellipse cx="649" cy="320" rx="10" ry="3.5" fill="#c08a3e"/>
  <text x="667" y="312" font-size="10.5" font-weight="bold" fill="#7a5419">SQLite — on-device</text>
  <g font-size="8.5" fill="#7a5419">
    <text x="667" y="334">fhir_resources · health_indicators · health_facts</text>
    <text x="667" y="349">same schema · owner's rows only</text>
    <text x="667" y="364" fill="#9a7a4a">embeddings ranked by in-process cosine</text>
  </g>
  <text x="781" y="392" text-anchor="middle" font-size="8" font-weight="bold" fill="#c0392b">no inbox · no worker · no cold tier</text>
</svg>

---

## Terminology resolve — normalize → substring match

<svg class="layers" viewBox="0 0 960 372" font-family="Helvetica, Arial, sans-serif">
  <defs>
    <marker id="arr3" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0 0 L10 5 L0 10 z" fill="#555"/>
    </marker>
  </defs>
  <text x="480" y="13" text-anchor="middle" font-size="11.5" fill="#456">Free-text term → standard code — deterministic &amp; lexical. <tspan fill="#c0392b" font-weight="bold">normalize runs on BOTH query and vocabulary.</tspan></text>
  <!-- ===== Build band (offline) ===== -->
  <rect x="40" y="22" width="880" height="92" rx="12" fill="#fbfaf7" stroke="#aab" stroke-width="1.5"/>
  <text x="52" y="40" font-size="11" font-weight="bold" fill="#234">Build · offline — <tspan fill="#4072b8">cli/indicator</tspan> (C++)</text>
  <rect x="52" y="50" width="250" height="56" rx="9" fill="#eef1f5" stroke="#99a"/>
  <text x="60" y="66" font-size="9" font-weight="bold" fill="#234">reference data</text>
  <text x="60" y="80" font-size="8.5" fill="#456">LOINC Part.csv · PartRelatedCodeMapping</text>
  <text x="60" y="94" font-size="8.5" fill="#456">UMLS MRCONSO · LOINC zhCN variant</text>
  <line x1="302" y1="78" x2="330" y2="78" stroke="#555" stroke-width="1.5" marker-end="url(#arr3)"/>
  <rect x="332" y="58" width="140" height="40" rx="9" fill="#e9eff7" stroke="#4072b8"/>
  <text x="402" y="75" text-anchor="middle" font-size="10.5" font-weight="bold" fill="#234f86">build-units</text>
  <text x="402" y="90" text-anchor="middle" font-size="9" fill="#234f86">· build-lexicon</text>
  <line x1="472" y1="78" x2="500" y2="78" stroke="#555" stroke-width="1.5" marker-end="url(#arr3)"/>
  <rect x="504" y="58" width="132" height="40" rx="8" fill="#fff3e0" stroke="#c08a3e"/>
  <text x="570" y="74" text-anchor="middle" font-size="9" font-weight="bold" fill="#7a5419">units.tsv</text>
  <text x="570" y="90" text-anchor="middle" font-size="8.5" fill="#7a5419">unit synonyms</text>
  <rect x="642" y="58" width="132" height="40" rx="8" fill="#fff3e0" stroke="#c08a3e"/>
  <text x="708" y="74" text-anchor="middle" font-size="9" font-weight="bold" fill="#7a5419">abbrev.tsv</text>
  <text x="708" y="90" text-anchor="middle" font-size="8.5" fill="#7a5419">abbrev pairs</text>
  <rect x="780" y="58" width="130" height="40" rx="8" fill="#dff0e4" stroke="#2f7d4a"/>
  <text x="845" y="74" text-anchor="middle" font-size="9" font-weight="bold" fill="#1c5233">fhir_lexicon.bin</text>
  <text x="845" y="90" text-anchor="middle" font-size="8.5" fill="#1c5233">surface → code</text>
  <!-- mapping: which artifact each runtime stage loads -->
  <text x="480" y="132" text-anchor="middle" font-size="9" fill="#456">loaded per query below:  abbrev.tsv → ①normalize   ·   units.tsv → ②match   ·   fhir_lexicon.bin → ③rank</text>
  <!-- ===== Runtime band (per query) ===== -->
  <!-- Stage 1: normalize -->
  <rect x="40" y="148" width="272" height="210" rx="14" fill="#e8eef7" stroke="#4072b8"/>
  <rect x="52" y="156" width="248" height="24" rx="7" fill="#4072b8"/>
  <text x="176" y="173" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">1 · normalize</text>
  <g font-size="10" fill="#274b73">
    <text x="60" y="202">• casefold · full-width → ASCII</text>
    <text x="60" y="221">• collapse whitespace · Greek/punct</text>
    <text x="60" y="240">• expand abbreviations (both sides)</text>
  </g>
  <rect x="56" y="252" width="240" height="42" rx="8" fill="#fff3e0" stroke="#c08a3e"/>
  <text x="176" y="268" text-anchor="middle" font-size="9" font-weight="bold" fill="#7a5419">uses abbrev.tsv</text>
  <text x="176" y="282" text-anchor="middle" font-size="9" fill="#7a5419">deg→degree · 24H→24 hour · &amp;→and</text>
  <text x="60" y="322" font-size="8.5" fill="#c0392b">same normalize() on query + vocab ⇒ forms line up</text>
  <line x1="312" y1="253" x2="344" y2="253" stroke="#555" stroke-width="1.6" marker-end="url(#arr3)"/>
  <!-- Stage 2: substring / unit match -->
  <rect x="344" y="148" width="272" height="210" rx="14" fill="#f5ecfa" stroke="#8a4fb0"/>
  <rect x="356" y="156" width="248" height="24" rx="7" fill="#8a4fb0"/>
  <text x="480" y="173" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">2 · substring / unit match</text>
  <g font-size="10" fill="#5e2e80">
    <text x="364" y="202">• longest-match segmentation</text>
    <text x="364" y="221">• 空腹血糖 → 空腹 · 血糖</text>
    <text x="364" y="240">• bridge units via synonyms</text>
  </g>
  <rect x="360" y="252" width="240" height="42" rx="8" fill="#fff3e0" stroke="#c08a3e"/>
  <text x="480" y="268" text-anchor="middle" font-size="9" font-weight="bold" fill="#7a5419">uses units.tsv</text>
  <text x="480" y="282" text-anchor="middle" font-size="9" fill="#7a5419">血糖 ↔ glucose ↔ 葡萄糖 · 空腹 ↔ fasting</text>
  <text x="364" y="322" font-size="8.5" fill="#456">order-free · cross-language · partial ok</text>
  <line x1="616" y1="253" x2="648" y2="253" stroke="#555" stroke-width="1.6" marker-end="url(#arr3)"/>
  <!-- Stage 3: score and rank -->
  <rect x="648" y="148" width="272" height="210" rx="14" fill="#eef6f0" stroke="#2f7d4a"/>
  <rect x="660" y="156" width="248" height="24" rx="7" fill="#2f7d4a"/>
  <text x="784" y="173" text-anchor="middle" font-size="12" font-weight="bold" fill="#fff">3 · score &amp; rank</text>
  <g font-size="10" fill="#1c5233">
    <text x="668" y="202">• unit coverage vs candidate</text>
    <text x="668" y="221">• modifiers license specificity</text>
    <text x="668" y="240">• dedup → ranked codes</text>
  </g>
  <rect x="664" y="252" width="240" height="42" rx="8" fill="#dff0e4" stroke="#2f7d4a"/>
  <text x="784" y="268" text-anchor="middle" font-size="11" font-weight="bold" fill="#1c5233">LOINC 1558-6</text>
  <text x="784" y="282" text-anchor="middle" font-size="8.5" fill="#1c5233">Fasting glucose [Mass/vol] in Bld</text>
  <text x="668" y="322" font-size="8.5" fill="#c0392b">miss = empty (fixable) — never wrong-but-confident</text>
</svg>

