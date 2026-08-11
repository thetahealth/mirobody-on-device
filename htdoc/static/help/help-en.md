# Mirobody Help

Hello. I'm **Mirobody** — an assistant that reads the health data you've connected and answers questions about it.

This document is itself a model reply: every kind of formatting in it is something you'll meet in a real conversation. So it doubles as a rendering self-check. If anything below looks wrong, that's a bug worth reporting.

---

## 1. Getting started

Pick a model before you send anything. Click the model name under the input box to switch.

1. **Server model** — whichever models this Mirobody server offers
   1. This is the lane with tools, so it is the one that can read your health data
   2. Your question goes to the server, and from there to the provider it is configured with
2. **On-device model** — desktop app only, a `.gguf` file running on this machine
   1. "Manage on-device AI…" at the end of the model list downloads one, or points at a file you already have
   2. It answers from the conversation alone: no tools, no health data, nothing over the network

> The difference between the two lanes isn't speed, it's **what the answer is made of**. The on-device lane never leaves this machine — and never sees one of your measurements either.

### Which to pick

| Situation | Use | Network | Reads your data |
|:--|:--|:--:|:--:|
| Reading your own lab results | server | yes | yes |
| Long writing, hard reasoning | server | yes | yes |
| A question you'd rather keep here | on-device | no | no |

---

## 2. What I can read

Nothing at all, until you connect a source. **☰** at the top left, then:

- **Connected devices** — a wearable or a health service: steps, heart rate, sleep, weight, whatever that vendor publishes
- **Connect EHR** — a hospital or clinic over SMART on FHIR, which is where lab results come from
- **Care circles** — someone who shared their data with you. The picker beside the input box chooses whose data a question is about; it appears only once somebody has shared

Before you connect one, this is **entirely empty** — not read-but-withheld. Not read.

---

## 3. How I answer

### 3.1 Tables and numbers

Ask "how did I sleep this week" and you get something like:

| Day | Asleep | Duration | Deep |
|:--|--:|--:|--:|
| Mon | 23:40 | 7h10m | 21% |
| Tue | 00:15 | 6h25m | 17% |
| Wed | 23:05 | 7h55m | 24% |

### 3.2 Charts

When there's too much data to see a trend by eye, I draw it:

```echarts
{"backgroundColor":"transparent","grid":{"left":48,"right":24,"top":32,"bottom":36},"xAxis":{"type":"category","data":["Mon","Tue","Wed","Thu","Fri","Sat","Sun"]},"yAxis":{"type":"value","name":"hours"},"series":[{"type":"bar","name":"sleep","data":[7.17,6.42,7.92,6.83,7.33,8.5,8.05]}]}
```

### 3.3 Diagrams

When the point is a structure rather than a set of numbers, I draw that instead:

```svg
<svg viewBox="0 0 420 120" xmlns="http://www.w3.org/2000/svg">
  <rect x="8" y="34" width="112" height="52" rx="8" fill="#1e3a6b"/>
  <text x="64" y="65" text-anchor="middle" font-size="14" fill="#ffffff">your question</text>
  <rect x="154" y="34" width="112" height="52" rx="8" fill="#2a7248"/>
  <text x="210" y="65" text-anchor="middle" font-size="14" fill="#ffffff">the model</text>
  <rect x="300" y="34" width="112" height="52" rx="8" fill="#8a5e15"/>
  <text x="356" y="65" text-anchor="middle" font-size="14" fill="#ffffff">the answer</text>
  <path d="M120 60 L154 60" stroke="#52565c" stroke-width="2"/>
  <path d="M266 60 L300 60" stroke="#52565c" stroke-width="2"/>
  <text x="210" y="108" text-anchor="middle" font-size="11" fill="#52565c">your account, no one else's</text>
</svg>
```

### 3.4 Formulas

I use them where they help. Body mass index is $BMI = w / h^2$, with $w$ in kg and $h$ in metres; CKD-EPI, for estimated kidney filtration, runs longer:

$$
eGFR = 142 \times \min(S_{cr}/\kappa, 1)^{\alpha} \times \max(S_{cr}/\kappa, 1)^{-1.200} \times 0.9938^{age}
$$

Ions written mid-sentence work the same way: sodium ($Na^+$), potassium ($K^+$), calcium ($Ca^{2+}$).

A dollar sign is also money, so it isn't taken at face value: a test that costs $100 and one that costs $200 stay as prose.

### 3.5 Code

Ask something technical and code gets its own block, scrollable sideways:

```python
def bmi(weight_kg: float, height_m: float) -> float:
    """Body mass index. Height in metres, not centimetres."""
    return weight_kg / (height_m ** 2)
```

Inline bits like `variable_name` and `config.yml` get the fixed-width face too.

---

## 4. Commands you can type

Type a single `/` in the box and the available commands appear, narrowing as you type. Click one to run it — **nothing to memorize**.

| Command | What it does |
|:--|:--|
| `/help` | Show this document |
| `/new` | Start a new conversation |
| `/incognito` | Toggle incognito — this conversation is not saved |

All three are **also** in the menu. The command line is a shortcut for people who like one, not a syntax you have to learn.

Commands are **not** sent to the model, take up none of the conversation's context, and are never written to history — including the `/help` you typed to get here.

---

## 5. Privacy

This section is a description, not a disclaimer.

1. **Server model**: the question, the health data the answer needed and the reply are kept with your account, which is what lets you reopen a conversation later
2. **On-device model** (desktop app): the exchange stays on this machine, and there is nothing for it to read anyway
3. **Incognito**: nothing is written to history and nothing is added to memory — the conversation ends when you leave it
4. Links I write open only over `http`, `https` and `mailto` — a model doesn't get to choose what your browser launches

> **Note**: I am not a doctor. I can help you read a result, spot a trend, and prepare questions for an appointment — I **cannot** diagnose. Take an abnormal result to a clinician.

---

## 6. Around the app

- **☰** at the top left opens history, your health connections and settings
- The small row under a reply names the **model that wrote it** and holds a copy button — plus a **$** for what the turn cost, when the model reports it
- **Enter** sends, **Shift + Enter** starts a new line
- Attach a file with the paperclip, or drop it straight onto the input box

---

<!-- The lines below are the rendering self-check; if they look right, formatting is fine -->

*This document ships with the app and is served from the same address as this page.* Escape check: `\*` must not turn italic — \*the stars around this line should show as written\*. Entity check: AT&amp;T &copy; 2026, body temperature 37&deg;C. ~~This line should be struck through.~~

Visit [thetahealth.ai](https://thetahealth.ai), or just ask me — faster than reading docs.
