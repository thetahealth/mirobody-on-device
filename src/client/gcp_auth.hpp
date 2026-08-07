#pragma once

// Google Application Default Credentials -> OAuth 2.0 access tokens.
//
// The C++ analog of what the Python stack does before every Vertex call:
//
//     creds, _ = google.auth.default(scopes=["...cloud-platform"])
//     creds.refresh(google.auth.transport.requests.Request())
//     headers["Authorization"] = f"Bearer {creds.token}"
//
// with one deliberate difference: a token is minted when the cached one is
// about to expire, not on every request. Google's tokens last about an hour, so
// refreshing per call costs a round trip per LLM turn and buys nothing.
//
// Sources, resolved once in the order google.auth.default() uses:
//
//   1. A service-account JSON key, from GOOGLE_APPLICATION_CREDENTIALS or the
//      well-known ADC path. Minting signs a JWT with the key's private half and
//      exchanges it at the key's token_uri (RFC 7523).
//   2. An authorized-user JSON at the well-known ADC path -- what
//      `gcloud auth application-default login` leaves behind. Minting is a
//      refresh_token grant. This is the developer-machine case.
//   2b. An external-account JSON -- workload identity federation. This is how a
//      workload OUTSIDE Google (an EKS pod, an Azure VM) authenticates without a
//      key: it proves its own cloud's identity, and Google's STS exchanges that
//      proof for an access token. Two proofs are supported, matching the two
//      credential_source shapes google-auth defines: a token FILE or URL (a
//      projected OIDC token, the usual Kubernetes case), and `aws1`, where the
//      proof is a SigV4-signed AWS GetCallerIdentity request that Google calls
//      on the caller's behalf. An optional impersonation step then swaps the
//      federated token for a service account's.
//
//      The AWS credentials that sign `aws1` come from the environment, else an
//      IRSA role, else EC2's metadata server -- in that order. The IRSA step goes
//      beyond what google-auth's built-in credential source does, and has to: in
//      a pod the metadata server is normally unreachable, so the projected
//      web-identity token is the only identity there is. google-auth leaves that
//      case to AwsSecurityCredentialsSupplier, a hook each application implements
//      for itself; doing it here means a deployment configures it once.
//   3. The GCE / GKE / Cloud Run metadata server, which hands out tokens for the
//      instance's attached service account. This is the production case ON GCP:
//      nothing is configured, and nothing has to be rotated by hand.
//
// Everything is best-effort: a source that cannot be resolved or a mint that
// fails yields "" (logged), which the caller reports as a missing credential.
// Failures are not retried immediately -- an unreachable metadata server would
// otherwise add its connect timeout to every turn.
//
// Thread-safe: turns run on worker threads and share one instance.

#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace gcp {

// Which of the three sources above is in use. Resolved on the first mint.
enum class CredentialSource {
    None,             // nothing usable was found
    ServiceAccount,   // service-account JSON key (signed JWT grant)
    AuthorizedUser,   // gcloud ADC login (refresh_token grant)
    ExternalAccount,  // workload identity federation (STS token exchange)
    Metadata,         // instance metadata server
};

// "service account" / "authorized user" / "metadata server" / "none".
const char* source_name(CredentialSource s);

//------------------------------------------------------------------------------

class AccessTokens {
public:
    // `scope` is the OAuth scope requested for the JSON-key sources; the
    // metadata server serves whatever scopes the instance's account carries.
    explicit AccessTokens(std::string scope = "https://www.googleapis.com/auth/cloud-platform");

    // A currently-valid access token, minted on demand and reused until shortly
    // before it expires. "" when no source is available or minting failed.
    std::string token();

    // The resolved source. None until the first token() call (or when nothing
    // usable was found). For logging what a deployment actually picked up.
    CredentialSource source();

private:
    // Pick a source and load whatever it needs. Runs once, under mu_.
    void resolve();
    // Mint from the resolved source into token_ / expires_at_. Under mu_.
    bool refresh();

    bool refresh_service_account();
    bool refresh_authorized_user();
    bool refresh_external_account();
    bool refresh_metadata();

    // The proof of identity handed to Google's STS, per credential_source.
    // "" when it cannot be produced (the file is missing, AWS did not answer).
    std::string subject_token();
    std::string aws_subject_token();

    // Store a token endpoint's {access_token, expires_in} reply.
    bool accept_token_response(const std::string& body, const char* what);

    std::mutex   mu_;
    std::string  scope_;

    bool             resolved_ = false;
    CredentialSource source_   = CredentialSource::None;

    std::string  token_;
    std::int64_t expires_at_  = 0;   // unix seconds, already minus the skew
    std::int64_t retry_after_ = 0;   // unix seconds; set after a failed mint

    // Service-account key fields.
    std::string sa_client_email_, sa_private_key_, sa_token_uri_;
    // Authorized-user (gcloud ADC) fields.
    std::string au_client_id_, au_client_secret_, au_refresh_token_;

    // External-account (workload identity federation) fields, named as the
    // credential file names them.
    std::string ea_audience_, ea_subject_token_type_, ea_token_url_;
    std::string ea_impersonation_url_;
    //   credential_source: a file or url holding the token...
    std::string ea_cs_file_, ea_cs_url_, ea_cs_format_, ea_cs_field_;
    //   ...with headers, when the url wants a credential of its own first.
    std::vector<std::string> ea_cs_headers_;
    //   ...or the aws1 environment, which is signed rather than read.
    std::string ea_cs_environment_id_, ea_cs_region_url_, ea_cs_security_creds_url_;
    std::string ea_cs_imdsv2_url_, ea_cs_regional_verification_url_;
};

//------------------------------------------------------------------------------

// Where a Vertex bearer token comes from, for every caller that needs one. The
// order is fixed so a deployment configures it once and both the chat and the
// embedding lane resolve identically:
//
//   1. a token FILE, re-read on every call -- for something that rotates the
//      token under a running process (a projected service-account token, a
//      sidecar). Nothing has to restart.
//   2. a token INLINE, exactly as given -- read once by whoever built this, so
//      it only tracks a rotation that restarts the process. Convenient for a
//      local `gcloud auth print-access-token`.
//   3. Application Default Credentials -- the normal case, and the only one that
//      needs no configuration at all. Lazily constructed, so a deployment using
//      1 or 2 never touches ADC.
//
// Thread-safe. Cheap to call per request: 1 is a small file read, 3 is a cached
// lookup that only mints when the token is near expiry.
class TokenSource {
public:
    TokenSource() {}
    TokenSource(std::string token_file, std::string inline_token)
        : token_file_(std::move(token_file)), inline_token_(std::move(inline_token)) {}

    std::string token();

    // Which of the three is in play, for a startup log: "file, re-read per
    // request" / "inline, fixed at startup" / "application default credentials".
    const char* describe() const;

private:
    std::mutex                    mu_;
    std::string                   token_file_;
    std::string                   inline_token_;
    std::shared_ptr<AccessTokens> adc_;   // built on first use
};

// The contents of a token file, whitespace-trimmed, or "" when it cannot be
// read. Exposed because a caller may want to check at startup whether the path
// it was given is readable yet.
std::string read_token_file(const std::string& path);

//------------------------------------------------------------------------------

// The Vertex AI endpoint host for a GOOGLE_CLOUD_LOCATION. Three URL shapes,
// and the location string alone says which -- guessing wrong does not fall back
// to anything, it is a 400 "Invalid hostname" from Google's frontend:
//
//   region        us-central1  ->  https://us-central1-aiplatform.googleapis.com
//   multi-region  us, eu       ->  https://aiplatform.us.rep.googleapis.com
//   global        global       ->  https://aiplatform.googleapis.com
//
// The multi-regions are the odd ones: they are Representative Endpoints (REP),
// so the location is an infix on aiplatform.*.rep.googleapis.com rather than a
// prefix. Only the host differs -- the path still carries /locations/{location}
// exactly as the regional form does.
//
// `scheme` is "https" for the REST lanes and "wss" for the Live API socket.
// An empty `location` yields "", since there is no sane default to invent.
std::string vertex_host(const std::string& location, const char* scheme = "https");

// The location one publisher model should be reached at: its entry in the
// GOOGLE_CLOUD_MODEL_LOCATIONS map when it has a non-empty one, else the
// deployment-wide GOOGLE_CLOUD_LOCATION that `fallback` carries.
//
// This exists because a location is a property of the MODEL, not of the
// deployment, and the published lists do not merely differ -- they invert.
// `gemini-embedding-001` serves the US single regions and NO multi-region;
// `gemini-embedding-2` serves `us` / `eu` and NO US single region. Chat is the
// same shape: `gemini-3.5-flash` serves the multi-regions, `gemini-2.5-flash`
// the single ones, and both are offered from one deployment. So a single
// location necessarily 404s some model the server offers -- not a
// misconfiguration, an unsatisfiable constraint.
//
// The map is configuration rather than a table compiled in here on purpose:
// Google's per-model lists move, and a stale built-in table would be a 404 no
// operator could override. What ships is the fallback; the map is how a
// deployment corrects one model without disturbing the rest.
//
// Both lanes that call Vertex resolve through this one function so they cannot
// drift. It does not validate: a location the model does not serve is Google's
// 404 to report, and guessing a "nearby" region here would silently move data
// across a residency boundary the operator chose deliberately.
std::string vertex_model_location(
    const std::unordered_map<std::string, std::string>& per_model,
    const std::string& model,
    const std::string& fallback);

//------------------------------------------------------------------------------

// The subject token the `aws1` credential source hands to Google's STS: an AWS
// SigV4-signed GetCallerIdentity request, serialized as {url, method, headers}
// and percent-encoded. Nothing sends it -- Google replays it to AWS, and AWS
// answering with the caller's identity is the proof of who this workload is.
//
// `verification_url` is the credential file's regional_cred_verification_url,
// with "{region}" still in it. `session_token` is empty for permanent AWS
// credentials. `audience` rides along as x-goog-cloud-target-resource, outside
// the signature, as aws.py sends it.
//
// Exposed, and taking `now` rather than reading the clock, because that is what
// makes it checkable against vectors google-auth generates -- every byte has to
// agree or AWS rejects the replayed request (tests/client/gcp_auth_test.cpp).
// "" when `verification_url` is not an https URL with a host.
std::string aws_signed_request(const std::string& verification_url,
                               const std::string& region,
                               const std::string& access_key_id,
                               const std::string& secret_access_key,
                               const std::string& session_token,
                               const std::string& audience,
                               std::time_t now);

}}
