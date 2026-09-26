# Security Policy

mirobody-on-device keeps a person's health record on their phone, and on the
phone it is usually the only copy. A defect here can expose it to another app,
to the local network, or to a model provider the person never chose, so we would
rather hear about a suspected problem that turns out to be nothing than not
hear about a real one.

## Reporting a vulnerability

**Do not open a public issue for a security report.**

Email **security@thetahealth.ai**. Include enough to reproduce: the commit, the
app and its OS version, the device or emulator, the configuration, and the
steps. If a proof of concept touches real health data, redact it: a synthetic
reproduction is worth more to us than a real one, and it is the only kind that
can become a test.

### What to expect

| | |
| --- | --- |
| Acknowledgement | within 3 business days |
| Initial assessment | within 10 business days |
| Fix or mitigation plan | communicated with the assessment |
| Public disclosure | coordinated with you, after a fix ships |

We will credit you in the advisory unless you ask us not to.

## Scope

**In scope**: this repository. That is the C++ core (`src/`, `res/`), the C
ABI and its platform bridges (JNI, the iOS bridge, NAPI), the loopback HTTP
front door, and the Android, iOS and HarmonyOS apps under `android/`, `ios/`
and `harmony/`.

Of particular interest:

- **Reaching the record from off the device.** The loopback front door must
  answer on `127.0.0.1` only. Anything that makes it reachable from the LAN,
  or from another app without the session token, is a vulnerability. The
  Android build bound every interface until September 2026.
- **Reaching the record from another app on the same device.** Exported
  components, content providers, URL schemes, the WebView bridge, files
  written outside the app sandbox.
- **Data leaving in a lane that promised it would not.** The on-device lane
  must send nothing; the BYOK lane must send the turn to the chosen provider
  and nowhere else. [docs/privacy-tiers.md](docs/privacy-tiers.md) is the
  contract, and a gap between it and the code is in scope.
- **Secrets at rest.** The user's own API keys, session tokens, and
  `FILE_ENCRYPTION_KEY` handling for stored uploads.
- **The agent tool surface.** Prompt injection through an uploaded document, a
  health-store value or a model reply that reaches a tool call.
- **Model downloads.** A weights file fetched or loaded without an integrity
  check.

**Out of scope**: the mirobody server, which has its own policy
([mirobody/SECURITY.md](https://github.com/thetahealth/mirobody/blob/main/SECURITY.md));
the hosted products (report those to security@thetahealth.ai as well); findings
that need a rooted or jailbroken device, or an attacker who already has the
unlocked phone; and scanner output without a demonstrated impact.

## Running the core safely

The desktop development build exists to develop against, not to deploy:

- Keep `HTTP_HOST` at `127.0.0.1`. The core holds a health record and has no
  business facing a network.
- `config.example.yml` ships `JWT_SALT`, `JWT_KEY` and `LOCAL_STORAGE_SECRET` as
  `REPLACE_THIS_VALUE_IN_PRODUCTION` placeholders. Replace them on any machine
  another person can reach, and do not uncomment `EMAIL_PREDEFINE_CODES` there:
  those codes are passwords everyone knows.
- Keep `config.yml` and anything under `_local/` out of version control; the
  repo's `.gitignore` covers both.

mirobody is Apache-2.0 licensed and provided without warranty. It is not a
medical device, and its output is not medical advice.

## Supported versions

Fixes land on `main`. The apps are built from `main`; we do not backport, so
please track the latest.
