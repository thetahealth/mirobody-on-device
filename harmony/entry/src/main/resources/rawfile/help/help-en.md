# Mirobody Help

Hello. I'm **Mirobody** — an assistant that keeps your health data and a language model on **the same device**.

This document is itself a model reply: every kind of formatting in it is something you'll meet in a real conversation. So it doubles as a rendering self-check. If anything below looks wrong, that's a bug worth reporting.

---

## 1. Getting started

Pick a model before you send anything. Tap the model name above the input box to switch.

1. **On-device model** — a `.gguf` file, running entirely on this phone
   1. Choose the file under "add model"; it stays in your own folder, we never copy it
   2. The first turn takes a few seconds to load, then it stays in memory
2. **Cloud model** — your own API key, straight to the provider
   1. Enter the key and `baseUrl` in the key manager
   2. Keys live in the device keystore and never reach our servers — we have no servers

> The difference between the two lanes isn't speed, it's **where your data goes**. On the on-device lane, not one byte of your health data leaves the phone.

### Which to pick

| Situation | Use | Network | Data leaves |
|:--|:--|:--:|:--:|
| Reading your own lab results | on-device | no | nothing |
| Long writing, hard reasoning | cloud | yes | yes |
| On a plane, bad signal | on-device | no | nothing |

---

## 2. What I can read

With health access granted, I can read these from Huawei Health:

- [x] Steps, resting heart rate, heart rate
- [x] Sleep duration and stages
- [ ] Blood glucose, blood pressure *(once the scopes are approved)*
- [ ] Body temperature

Before you grant it, this is **entirely empty** — not read-but-withheld. Not read.

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
  <text x="64" y="65" text-anchor="middle" font-size="14" fill="#ffffff">your device</text>
  <rect x="154" y="34" width="112" height="52" rx="8" fill="#2a7248"/>
  <text x="210" y="65" text-anchor="middle" font-size="14" fill="#ffffff">the model</text>
  <rect x="300" y="34" width="112" height="52" rx="8" fill="#8a5e15"/>
  <text x="356" y="65" text-anchor="middle" font-size="14" fill="#ffffff">the answer</text>
  <path d="M120 60 L154 60" stroke="#52565c" stroke-width="2"/>
  <path d="M266 60 L300 60" stroke="#52565c" stroke-width="2"/>
  <text x="210" y="108" text-anchor="middle" font-size="11" fill="#52565c">none of it leaves</text>
</svg>
```

### 3.4 Formulas

I use them where they help. Body mass index is $BMI = w / h^2$, with $w$ in kg and $h$ in metres; CKD-EPI, for estimated kidney filtration, runs longer:

$$
eGFR = 142 \times \min(S_{cr}/\kappa, 1)^{\alpha} \times \max(S_{cr}/\kappa, 1)^{-1.200} \times 0.9938^{age}
$$

Ions written mid-sentence work the same way: sodium ($Na^+$), potassium ($K^+$), calcium ($Ca^{2+}$).

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

Type a single `/` in the box and the available commands appear, narrowing as you type. Tap one to run it — **nothing to memorize**.

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

1. **On-device model**: question, health data and answer all stay on the phone
2. **Cloud model**: your question goes to the provider *you* configured — your account, not ours
3. **Incognito**: the conversation is not written to history
4. We have **no** servers, so there is nowhere to "upload to us"

> **Note**: I am not a doctor. I can help you read a result, spot a trend, and prepare questions for an appointment — I **cannot** diagnose. Take an abnormal result to a clinician.

---

## 6. Gestures

- **Long-press** any message to copy it — you'll feel a short tick
- The small line under a reply names the **model that wrote it**
- ☰ at the top left opens history, health data and settings

---

<!-- The lines below are the rendering self-check; if they look right, formatting is fine -->

*This document ships inside the app and is never fetched.* Escape check: `\*` must not turn italic — \*the stars around this line should show as written\*. Entity check: AT&amp;T &copy; 2026, body temperature 37&deg;C. ~~This line should be struck through.~~

Visit [thetahealth.ai](https://thetahealth.ai), or just ask me — faster than reading docs.
