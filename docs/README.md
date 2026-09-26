# Documentation

Which file a given piece of documentation belongs in. Everything under `docs/`
is English; the top-level README also ships in Chinese.

## Here

| File | What it holds |
|---|---|
| [BUILDING.md](BUILDING.md) | Every build target: dependencies per host, the development and mobile profiles, the app sysroots, Windows with vcpkg |
| [privacy-tiers.md](privacy-tiers.md) | The contract for what leaves the device: the model lanes, the storage tiers, what each artifact sends where |
| [on-device-llm.md](on-device-llm.md) | The on-device model: runtimes, formats, quantization, measured phone numbers (a Marp slide deck) |
| [markdown.md](markdown.md) | The Markdown a mirobody client must render, and where each client stands against it |
| [colors-and-fonts.md](colors-and-fonts.md) | The palette and type every client draws from |

## Beside the code

| Where | What it holds |
|---|---|
| `src/<subsystem>/README.md` | How that subsystem works and what it is not. Most subsystems have one; `llm/`, `mcp/`, `platform/` and `server/` do not yet, and their header comments carry it |
| [android/](../android/README.md), [ios/](../ios/README.md), [harmony/](../harmony/README.md) | Building, signing and running each app |
| [cli/README.md](../cli/README.md) | The debug CLIs the development build produces |
| [CONTRIBUTING.md](../CONTRIBUTING.md), [AGENTS.md](../AGENTS.md), [SECURITY.md](../SECURITY.md) | How to contribute, the rules for coding agents, how to report a vulnerability |
| [CHANGELOG.md](../CHANGELOG.md) | What changed, dated |

## Not here

- **The API contract, the SSE wire format, the terminology and the server**:
  the main repo, [mirobody](https://github.com/thetahealth/mirobody), and
  [docs.mirobody.ai](https://docs.mirobody.ai/). This repo matches them and
  does not restate them.
- **The removed clients and server subsystems**: the
  [`v2-full-2026-08`](https://github.com/thetahealth/mirobody-on-device/tree/v2-full-2026-08)
  tag, with the v2 architecture deck and the v2 README.
- **Planning notes**: `internal/`, which is gitignored. Nothing there is a
  promise; a decision that holds gets written into one of the files above.
