#include "llm/embedding.hpp"

#include "client/gcp_auth.hpp"   // Vertex: application default credentials
#include "client/http_client.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <thread>
#include <unordered_map>

namespace mirobody { namespace embedding {

namespace {

const int kDimensions = 1024;

// Retry policy, mirroring the Python helper.
const int kMaxRetries = 3;
const int kBackoffMs[] = { 1000, 2000, 4000 };
bool is_retry_status(long s) {
    return s == 408 || s == 429 || s == 502 || s == 503 || s == 504;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

//------------------------------------------------------------------------------
// Per-provider request shaping. A provider yields a URL, auth headers, a batch
// limit, and a body builder; the response parser is selected by `kind`.

enum class Kind { Gemini, VertexPredict, OpenAiCompatible };

struct Provider {
    std::string url;
    std::vector<std::string> headers;   // beyond Content-Type (set by HttpRequest)
    int batch_limit = 1;
    Kind kind = Kind::Gemini;
    std::string model;                  // model name for the OpenAI-compatible body
    int dimensions = kDimensions;       // requested output dim; <=0 => omit (model native)
    std::string error;                  // non-empty => misconfigured
};

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

Provider make_provider(const Config& cfg, const std::string& name) {
    Provider p;
    if (name == "gemini") {
        const std::string model = "gemini-embedding-001";

        // Vertex AI: OAuth Bearer auth, one input per :predict request.
        //
        // Selected exactly as the chat lane selects it (res/agents/baseline.cpp):
        // a project AND a location, the two things the endpoint URL is built
        // from. There is no separate on/off switch -- one Vertex configuration
        // decides both lanes, so a deployment cannot end up with its chat on
        // Vertex and its embeddings quietly still going to the consumer endpoint.
        // Neither key may carry a default for that reason: a defaulted location
        // would make "both set" true whenever a project was named.
        //
        // Google's own names -- the same pair the chat lane reads. This lane's old
        // VERTEX_LOCATION / VERTEX_ACCESS_TOKEN / VERTEX_BASE_URL are gone, as are
        // the GCP_PROJECT / GCP_LOCATION fallbacks both lanes used to take: they
        // named concepts that already had names, and two spellings of one setting
        // is how a deployment ends up half-configured.
        const std::string project  = cfg.store.get_str("GOOGLE_CLOUD_PROJECT");
        const std::string location = cfg.store.get_str("GOOGLE_CLOUD_LOCATION");
        if (!project.empty() && !location.empty()) {
            // The token resolves exactly as the chat lane's does -- token file,
            // inline token, then ADC -- through the one class that knows that
            // order. Held across calls so ADC's cache survives; the config it is
            // built from does not change while the process runs.
            static gcp::TokenSource tokens(cfg.store.get_str("GCP_ACCESS_TOKEN_FILE"),
                                           cfg.store.get_str("GCP_ACCESS_TOKEN"));
            const std::string token = tokens.token();
            if (token.empty()) {
                p.error = "gemini embedding (vertex): no access token -- application default "
                          "credentials resolved none, and no GCP_ACCESS_TOKEN(_FILE) is set";
                return p;
            }
            // The pair above is the surface switch and stays deployment-wide.
            // WHERE this model is reached is a separate question, because the
            // embedding models publish locations disjoint from the chat models'
            // -- gemini-embedding-001 serves the US single regions and no
            // multi-region at all, so the `us` that a chat deployment wants is
            // exactly the value that 404s here. One shared resolver, so this
            // lane and the chat lane cannot disagree about a model's location.
            const std::string model_location = gcp::vertex_model_location(
                cfg.store.get_dict("GOOGLE_CLOUD_MODEL_LOCATIONS"), model, location);

            // Region, us / eu multi-region and "global" each spell the host
            // differently; gcp::vertex_host is the one place that knows how.
            const std::string base = trim_trailing_slash(
                cfg.store.get_str("GEMINI_VERTEX_BASE_URL", gcp::vertex_host(model_location)));
            p.url = base + "/v1/projects/" + project + "/locations/" + model_location
                  + "/publishers/google/models/" + model + ":predict";
            p.headers.push_back("Authorization: Bearer " + token);
            p.batch_limit = 1;   // Vertex :predict accepts one input per request
            p.kind = Kind::VertexPredict;
            return p;
        }

        // Google AI Studio (default): x-goog-api-key header (as the Python
        // reference does), keeping the key out of the URL / any URL logging.
        const std::string base = trim_trailing_slash(
            cfg.gemini.base_url.empty() ? std::string("https://generativelanguage.googleapis.com")
                                        : cfg.gemini.base_url);
        const std::string key = cfg.gemini.api_key;   // already falls back to GOOGLE_API_KEY
        if (key.empty()) {
            p.error = "gemini embedding: GEMINI_API_KEY / GOOGLE_API_KEY not set";
            return p;
        }
        p.url = base + "/v1beta/models/" + model + ":batchEmbedContents";
        p.headers.push_back("x-goog-api-key: " + key);
        p.batch_limit = 100;
        p.kind = Kind::Gemini;
        return p;
    } else if (name == "qwen") {
        std::string base = trim_trailing_slash(cfg.dashscope.base_url);
        const std::string key = cfg.dashscope.api_key;
        if (key.empty()) {
            p.error = "qwen embedding: DASHSCOPE_API_KEY not set";
            return p;
        }
        p.url = base + "/embeddings";
        p.headers.push_back("Authorization: Bearer " + key);
        p.model = "text-embedding-v4";
        p.batch_limit = 10;
        p.kind = Kind::OpenAiCompatible;
        return p;
    } else if (name == "gemma" || name == "embeddinggemma") {
        // EmbeddingGemma (Google's open 308M text embedder) served locally over an
        // OpenAI-compatible /embeddings endpoint (Ollama, llama.cpp server, ...).
        // Fully local — pairs with the on-device chat path; no text leaves the host.
        const std::string base = trim_trailing_slash(
            cfg.store.get_str("EMBEDDING_GEMMA_BASE_URL",
                cfg.store.get_str("OLLAMA_BASE_URL", "http://localhost:11434/v1")));
        p.url = base + "/embeddings";
        // Local servers usually need no key; attach one only if configured.
        const std::string key = cfg.store.get_str("EMBEDDING_GEMMA_API_KEY");
        if (!key.empty()) { p.headers.push_back("Authorization: Bearer " + key); }
        p.model = cfg.store.get_str("EMBEDDING_GEMMA_MODEL", "embeddinggemma");
        // EmbeddingGemma is natively 768-dim. Default to the model's native size
        // (0 => omit a dimensions override); set EMBEDDING_GEMMA_DIM to request a
        // Matryoshka-truncated size (512 / 256 / 128) if the endpoint honours it.
        p.dimensions = static_cast<int>(cfg.store.get_int("EMBEDDING_GEMMA_DIM", 0));
        // One input per request, for broad endpoint compatibility (memory recall
        // embeds a single text at a time anyway). Array-capable servers can batch.
        p.batch_limit = 1;
        p.kind = Kind::OpenAiCompatible;
        return p;
    }
    p.error = "unknown embedding provider: '" + name + "' (available: gemini, qwen, gemma)";
    return p;
}

std::string build_body(const Provider& p, const std::vector<std::string>& chunk) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    auto str = [&](const std::string& s) {
        w.String(s.c_str(), static_cast<rapidjson::SizeType>(s.size()));
    };

    if (p.kind == Kind::Gemini) {
        // {"requests":[{"model":"models/gemini-embedding-001",
        //   "content":{"parts":[{"text":"..."}]},"output_dimensionality":1024}, ...]}
        w.StartObject();
        w.Key("requests");
        w.StartArray();
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            w.StartObject();
            w.Key("model"); w.String("models/gemini-embedding-001");
            w.Key("content");
            w.StartObject();
            w.Key("parts");
            w.StartArray();
            w.StartObject(); w.Key("text"); str(chunk[i]); w.EndObject();
            w.EndArray();
            w.EndObject();
            w.Key("output_dimensionality"); w.Int(p.dimensions);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    } else if (p.kind == Kind::VertexPredict) {
        // {"instances":[{"content":"..."}],"parameters":{"outputDimensionality":1024}}
        w.StartObject();
        w.Key("instances");
        w.StartArray();
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            w.StartObject(); w.Key("content"); str(chunk[i]); w.EndObject();
        }
        w.EndArray();
        w.Key("parameters");
        w.StartObject(); w.Key("outputDimensionality"); w.Int(p.dimensions); w.EndObject();
        w.EndObject();
    } else {
        // {"model":"<model>","input":["...", ...],"dimensions":<n>} — `dimensions`
        // omitted when <=0 so the model's native size is used (e.g. EmbeddingGemma 768).
        w.StartObject();
        w.Key("model"); str(p.model);
        w.Key("input");
        w.StartArray();
        for (std::size_t i = 0; i < chunk.size(); ++i) { str(chunk[i]); }
        w.EndArray();
        if (p.dimensions > 0) { w.Key("dimensions"); w.Int(p.dimensions); }
        w.EndObject();
    }
    return std::string(buf.GetString(), buf.GetSize());
}

// Pull a float array out of a JSON value (the per-item vector).
bool read_vector(const rapidjson::Value& arr, std::vector<float>* out) {
    if (!arr.IsArray()) return false;
    out->clear();
    out->reserve(arr.Size());
    for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        if (!arr[i].IsNumber()) return false;
        out->push_back(static_cast<float>(arr[i].GetDouble()));
    }
    return true;
}

// Parse a chunk response into `count` vectors. Returns false (with *err) on a
// malformed payload or a count mismatch.
bool parse_response(Kind kind, const std::string& body, std::size_t count,
                    std::vector<std::vector<float>>* out, std::string* err) {
    rapidjson::Document d;
    if (d.Parse(body.c_str()).HasParseError() || !d.IsObject()) {
        *err = "embedding: malformed JSON response";
        return false;
    }

    const char* key = (kind == Kind::Gemini)        ? "embeddings"
                    : (kind == Kind::VertexPredict) ? "predictions"
                    :                                 "data";
    auto it = d.FindMember(key);
    if (it == d.MemberEnd() || !it->value.IsArray()) {
        *err = std::string("embedding: response missing '") + key + "' array";
        return false;
    }
    const rapidjson::Value& items = it->value;
    if (items.Size() != count) {
        *err = "embedding: provider returned a different number of vectors than requested";
        return false;
    }

    out->clear();
    out->reserve(count);
    for (rapidjson::SizeType i = 0; i < items.Size(); ++i) {
        if (!items[i].IsObject()) {
            *err = "embedding: malformed response item";
            return false;
        }
        // Per-item vector location:
        //   Gemini           -> item.values
        //   Vertex :predict   -> item.embeddings.values
        //   OpenAI-compatible -> item.embedding
        const rapidjson::Value* arr = nullptr;
        if (kind == Kind::Gemini) {
            if (items[i].HasMember("values")) { arr = &items[i]["values"]; }
        } else if (kind == Kind::VertexPredict) {
            if (items[i].HasMember("embeddings") && items[i]["embeddings"].IsObject()
                && items[i]["embeddings"].HasMember("values")) {
                arr = &items[i]["embeddings"]["values"];
            }
        } else {
            if (items[i].HasMember("embedding")) { arr = &items[i]["embedding"]; }
        }

        std::vector<float> vec;
        if (!arr || !read_vector(*arr, &vec)) {
            *err = "embedding: response item missing a numeric vector field";
            return false;
        }
        out->push_back(std::move(vec));
    }
    return true;
}

// POST one chunk with retry/backoff on transient failures. Returns the 200 body,
// or "" with *err set.
bool post_chunk(const Provider& p, const std::string& body,
                const Config& cfg, std::string* out, std::string* err) {
    client::HttpClient http;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        client::HttpRequest req;
        req.url = p.url;
        req.body = body;
        req.headers = p.headers;
        req.connect_timeout_ms = cfg.connect_timeout_ms;
        req.request_timeout_ms = 30000;

        client::HttpResponse res = http.post(req);
        if (res.status == 200) {
            *out = res.body;
            return true;
        }

        const bool transient = (res.status <= 0) || is_retry_status(res.status);
        if (transient && attempt < kMaxRetries - 1) {
            platform::log_warn("embedding: HTTP %ld, retrying in %dms (attempt %d)",
                               res.status, kBackoffMs[attempt], attempt + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffMs[attempt]));
            continue;
        }
        *err = "embedding: HTTP " + std::to_string(res.status) + " " + res.body.substr(0, 300);
        return false;
    }
    *err = "embedding: exhausted retries";
    return false;
}

}  // namespace

//------------------------------------------------------------------------------

EmbeddingResult text_embedding(const Config& cfg, const std::vector<std::string>& texts) {
    EmbeddingResult result;
    result.vectors.assign(texts.size(), std::vector<float>());

    const std::string provider = cfg.store.get_str("EMBEDDING_PROVIDER", "gemini");

    // Keep positional correspondence; only embed non-blank inputs.
    std::vector<std::size_t> valid_index;
    std::vector<std::string> clean;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        std::string t = trim(texts[i]);
        if (!t.empty()) {
            valid_index.push_back(i);
            clean.push_back(t);
        }
    }
    if (clean.empty()) {
        return result;   // nothing to do; all-empty vectors, ok
    }

    Provider p = make_provider(cfg, provider);
    if (!p.error.empty()) {
        result.error = p.error;
        return result;
    }

    // Deduplicate so identical inputs cost one API slot.
    std::vector<std::string> unique;
    std::unordered_map<std::string, std::vector<float>> cache;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        if (cache.find(clean[i]) == cache.end()) {
            cache.emplace(clean[i], std::vector<float>());
            unique.push_back(clean[i]);
        }
    }

    for (std::size_t i = 0; i < unique.size(); i += static_cast<std::size_t>(p.batch_limit)) {
        std::vector<std::string> chunk(
            unique.begin() + static_cast<std::ptrdiff_t>(i),
            unique.begin() + static_cast<std::ptrdiff_t>(
                std::min(unique.size(), i + static_cast<std::size_t>(p.batch_limit))));

        std::string body = build_body(p, chunk);
        std::string resp, err;
        if (!post_chunk(p, body, cfg, &resp, &err)) {
            result.error = err;
            result.vectors.clear();
            return result;
        }
        std::vector<std::vector<float>> vecs;
        if (!parse_response(p.kind, resp, chunk.size(), &vecs, &err)) {
            result.error = err;
            result.vectors.clear();
            return result;
        }
        for (std::size_t j = 0; j < chunk.size(); ++j) {
            cache[chunk[j]] = std::move(vecs[j]);
        }
    }

    for (std::size_t k = 0; k < valid_index.size(); ++k) {
        result.vectors[valid_index[k]] = cache[clean[k]];
    }
    return result;
}

std::vector<float> text_embedding_one(const Config& cfg, const std::string& text, std::string* err) {
    EmbeddingResult r = text_embedding(cfg, std::vector<std::string>{text});
    if (!r.ok()) {
        if (err) { *err = r.error; }
        return std::vector<float>();
    }
    return r.vectors.empty() ? std::vector<float>() : r.vectors[0];
}

}}
