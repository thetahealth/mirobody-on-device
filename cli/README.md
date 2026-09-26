# Debug tools

The development build produces a small family of standalone CLIs under `cli/`,
most wrapping one LLM client in [src/llm/](../src/llm/); the rest exercise one
subsystem each (`mcp`, `agent`, `fhir`, `indicator`, `image` / `document` /
`file_parser`, `file_rekey`, the JWT helpers). They bypass the loopback HTTP
front door — handy for poking at request parameters or wire-format quirks
without rebuilding the core. Toggle with `-DMIROBODY_BUILD_TOOLS=OFF`.

The LLM CLIs share the same UX:

- **Configuration.** Each CLI loads a YAML key-value store on startup using
  the same plumbing as the server's [load_config](../src/config/config.cpp) — local
  file (`--config <path>` > `MIROBODY_CONFIG` env > `./config.yml`) and/or
  a remote source (`CONFIG_SERVER` + `CONFIG_TOKEN` + `ENV` env vars).
  Fernet-encrypted (`gAAAA…`) values are decrypted on load when
  `CONFIG_ENCRYPTION_KEY` is set. **Precedence per option:** `--flag` > env
  var > YAML > built-in default.
- **Input.** Positional argument, `--message` (repeatable), or stdin.
- **System prompt.** `--system "..."`.
- **Output.** Default is colored / incremental; `--raw` emits one TSV line per
  event (`<type>\t<tool_id>\t<content>`) for piping into `jq`; `--no-color`
  drops the ANSI escapes but keeps the labels.
- **Cost stats.** `--input-price` / `--output-price` in $/M tokens; emits a
  terminal `[cost]` line.
- `--help` lists every flag.

| Binary | Endpoint | Required credentials |
|---|---|---|
| `mirothinker` | `api.miromind.ai/v1/chat/completions` (`mcp_servers` vendor extension) | `MIROTHINKER_API_KEY` + `MCP_PUBLIC_URL` |
| `openai_chat` | any OpenAI-compatible `/chat/completions` — flip `--base-url` for OpenRouter, Nebula, DashScope, Ark, vLLM, … | `OPENAI_API_KEY` (+ optional `OPENAI_BASE_URL`) |
| `openai_responses` | OpenAI `/responses` (named-event SSE, supports `previous_response_id`) | `OPENAI_API_KEY` |
| `gemini` | `streamGenerateContent` — `--mode ai-studio` or `--mode vertex` | AI Studio: `GOOGLE_API_KEY`. Vertex: `GCP_ACCESS_TOKEN` (`gcloud auth print-access-token`) + `GOOGLE_CLOUD_PROJECT` + `GOOGLE_CLOUD_LOCATION` |

## YAML keys

Drop these into your `config.yml` (or push them through your remote config
service) instead of exporting env vars one by one. Start from
[`config.example.yml`](../config.example.yml), copied to `config.yml`; the provider
keys each CLI honors (model, timeouts, pricing) are listed in that CLI's source
header.

Each CLI's source header lists every key it honors (timeouts,
min-chunk-size, etc.). Secrets like `*_API_KEY` / `*_TOKEN` can be stored as
Fernet tokens (`gAAAA…`) and decrypted on load via `CONFIG_ENCRYPTION_KEY`.

```sh
# Once config.yml is populated, every CLI just works:
./mirothinker "summarize this in one sentence"
./openai_chat "hi"
./openai_responses "explain SSE in one sentence"
./gemini "hi"

# Flags still override anything in YAML / env:
./openai_chat --base-url https://openrouter.ai/api/v1 \
                  --model openai/gpt-4o-mini "hi"
./gemini --mode vertex "hi"   # GEMINI_MODE may also live in YAML

# Chain a Responses-API turn onto a prior stored response:
./openai_responses --previous-response-id resp_abc \
                       "now translate that to Chinese"

# Point at a non-default YAML file:
./mirothinker --config /tmp/dev.yaml "hi"

# Pipe to jq:
echo "hi" | ./openai_chat --raw \
  | jq -R 'split("\t") | {type:.[0], tool_id:.[1], content:.[2]}'
```

## Notes on the Gemini paths

The Python reference's `GeminiClient` uses Google's experimental
**Interactions API** on the AI Studio path so it can pass `mcp_server` tools
natively. That API surface isn't on a stable HTTP endpoint, so the C++ port
uses the standard `streamGenerateContent` endpoint for *both* modes — the wire
format is identical between AI Studio and Vertex, only host + auth differ.
This loses Gemini-side native MCP; if you need it, do MCP locally as function
declarations or add an `Interactions` client later.

## file_rekey

Re-encrypts stored uploads under the **newest** `FILE_ENCRYPTION_KEY`, in place,
so a retired key can be dropped from the list after a rotation (see the
key-rotation notes in [src/storage/README.md](../src/storage/README.md)). It
decrypts each object / `.meta` / `.trans` with whatever configured key fits and
rewrites it under the newest one; paths are unchanged (they derive from
`FILE_KEY_SEED`), so the `files` table and URLs are untouched. Reads the object
store + `FILE_ENCRYPTION_KEY` / `FILE_KEY_SEED` from config.

| Flag | Default | Action |
| ---- | ------- | ------ |
| `--config PATH` | discovery | YAML config (else `MIROBODY_CONFIG` / `./config.yml`) |
| `--user ID`     | all      | re-encrypt only this user's objects |
| `--max N`       | 1000000  | cap the object scan (warns if hit) |
| `--dry-run`     | *(off)*  | report counts without writing |

Rotate, flush, then drop the old key:

```sh
# 1. append a new key to FILE_ENCRYPTION_KEY (newest = encrypts), restart.
# 2. preview, then flush every object onto the new key:
file_rekey --dry-run
file_rekey
# 3. once it reports 0 failed, remove the old key from FILE_ENCRYPTION_KEY.
```

## jwt_keygen

Generates an RSA keypair for **RS256** JWT signing. Set `JWT_PRIVATE_KEY` and the
server signs tokens with RS256 and publishes the public key as a JWKS at
`/.well-known/jwks.json` for third-party validators (see
[src/oauth/README.md](../src/oauth/README.md)). The tool also prints the key id
(RFC 7638 JWK thumbprint) the server will advertise. Needs no config or network.

| Flag | Default | Action |
| ---- | ------- | ------ |
| `--bits N`     | `2048`  | RSA modulus size (e.g. `3072`, `4096`) |
| `--out PREFIX` | *(off)* | write `PREFIX.key` (private, 0600 on POSIX) + `PREFIX.pub` (public) |
| `--pem`        | *(off)* | print raw private then public PEM to stdout |
| `--jwks`       | *(off)* | also print the JWKS document (the jwks.json body) |

With no `--out`/`--pem`, it prints a ready-to-paste `config.yml` snippet to
stdout (`JWT_PRIVATE_KEY:` / `JWT_PUBLIC_KEY:` block scalars); the `kid` always
goes to stderr so it stays out of the piped artifact.

```sh
# Print a config.yml block (kid on stderr); append it to your config:
jwt_keygen >> config.local.yaml

# Write key files instead, 3072-bit, and show the JWKS that will be served:
jwt_keygen --bits 3072 --out jwt_signing --jwks

# For rotation, generate a second key and add its .pub as JWT_PUBLIC_KEY_2.
```
