#include "client/gcp_auth.hpp"

#include "storage/sign.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ctime>
#include <string>
#include <unordered_map>

// The `aws1` workload-identity subject token, checked byte for byte against what
// google-auth produces. Byte-exactness is the whole requirement here and not a
// stylistic one: the token is an AWS SigV4-signed GetCallerIdentity request that
// Google replays to AWS, so a single character out of place is a signature AWS
// refuses, surfacing as an opaque 400 from Google's STS with nothing in it that
// points at the cause.
//
// The expected values were generated from google-auth 2.56.3 by driving
// aws.RequestSigner + aws.Credentials.retrieve_subject_token's serializer with
// _helpers.utcnow pinned to kNow, then emitted as literals rather than
// transcribed. To regenerate after a google-auth upgrade:
//
//     from google.auth import _helpers, aws
//     _helpers.utcnow = lambda: datetime.datetime(2026, 8, 6, 12, 34, 56)
//     signer = aws.RequestSigner(region)
//     opts = signer.get_request_options(
//         aws.AwsSecurityCredentials(key_id, secret, session), url, "POST")
//     headers = opts["headers"]
//     headers["x-goog-cloud-target-resource"] = audience
//     req = {"url": opts["url"], "method": opts["method"],
//            "headers": [{"key": k, "value": headers[k]} for k in headers]}
//     urllib.parse.quote(json.dumps(req, separators=(",", ":"), sort_keys=True))

using mirobody::gcp::aws_signed_request;
using mirobody::gcp::vertex_host;
using mirobody::gcp::vertex_model_location;

namespace {

// 2026-08-06T12:34:56Z. The signer formats it through gmtime, so this is the
// same instant whatever timezone the machine building this is in.
const std::time_t kNow = 1786019696;

const char* const kAudience =
    "//iam.googleapis.com/projects/123456789/locations/global/"
    "workloadIdentityPools/aws-pool/providers/aws-provider";

// regional_cred_verification_url as gcloud writes it into an aws1 credential
// file -- "{region}" and all.
const char* const kDefaultUrl =
    "https://sts.{region}.amazonaws.com?Action=GetCallerIdentity&Version=2011-06-15";

// AWS's own published example secret; the key ids are obviously not real.
const char* const kSecret  = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
// A session token holding the '+', '/' and '=' that make the two encoding layers
// (JSON string, then percent-encoding) worth testing at all.
const char* const kSession = "IQoJb3JpZ2luX2VjEExample//Token+With/Slashes==";

}   // namespace

//------------------------------------------------------------------------------

TEST_CASE("aws_signed_request matches google-auth for an EC2 role", "[gcp][aws][wif]") {
    // The production shape: temporary credentials from the instance metadata
    // server, so an x-amz-security-token rides along and is signed.
    const std::string expected =
        "%7B%22headers%22%3A%5B%7B%22key%22%3A%22Authorization%22%2C%22value%22%3A%22AWS4-HMAC-SH"
        "A256%20Credential%3DASIAEXAMPLEKEYID/20260806/us-east-1/sts/aws4_request%2C%20SignedHead"
        "ers%3Dhost%3Bx-amz-date%3Bx-amz-security-token%2C%20Signature%3Def451732e326eefcbd58bbbc"
        "261bd74498e98e156841af3939695e950e1736f2%22%7D%2C%7B%22key%22%3A%22host%22%2C%22value%22"
        "%3A%22sts.us-east-1.amazonaws.com%22%7D%2C%7B%22key%22%3A%22x-amz-date%22%2C%22value%22%"
        "3A%2220260806T123456Z%22%7D%2C%7B%22key%22%3A%22x-amz-security-token%22%2C%22value%22%3A"
        "%22IQoJb3JpZ2luX2VjEExample//Token%2BWith/Slashes%3D%3D%22%7D%2C%7B%22key%22%3A%22x-goog"
        "-cloud-target-resource%22%2C%22value%22%3A%22//iam.googleapis.com/projects/123456789/loc"
        "ations/global/workloadIdentityPools/aws-pool/providers/aws-provider%22%7D%5D%2C%22method"
        "%22%3A%22POST%22%2C%22url%22%3A%22https%3A//sts.us-east-1.amazonaws.com%3FAction%3DGetCa"
        "llerIdentity%26Version%3D2011-06-15%22%7D";

    REQUIRE(aws_signed_request(kDefaultUrl, "us-east-1", "ASIAEXAMPLEKEYID", kSecret, kSession,
                               kAudience, kNow) == expected);
}

//------------------------------------------------------------------------------

TEST_CASE("aws_signed_request omits the session token for permanent credentials",
          "[gcp][aws][wif]") {
    // AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY with no AWS_SESSION_TOKEN: the
    // x-amz-security-token header is absent, which also changes SignedHeaders and
    // so the signature.
    const std::string expected =
        "%7B%22headers%22%3A%5B%7B%22key%22%3A%22Authorization%22%2C%22value%22%3A%22AWS4-HMAC-SH"
        "A256%20Credential%3DAKIAEXAMPLEKEYID/20260806/ap-southeast-1/sts/aws4_request%2C%20Signe"
        "dHeaders%3Dhost%3Bx-amz-date%2C%20Signature%3Db74b9976ca7f9da6e444ccdf24f30a432513f16d56"
        "cd360efd831b9cabb2f3a6%22%7D%2C%7B%22key%22%3A%22host%22%2C%22value%22%3A%22sts.ap-south"
        "east-1.amazonaws.com%22%7D%2C%7B%22key%22%3A%22x-amz-date%22%2C%22value%22%3A%2220260806"
        "T123456Z%22%7D%2C%7B%22key%22%3A%22x-goog-cloud-target-resource%22%2C%22value%22%3A%22//"
        "iam.googleapis.com/projects/123456789/locations/global/workloadIdentityPools/aws-pool/pr"
        "oviders/aws-provider%22%7D%5D%2C%22method%22%3A%22POST%22%2C%22url%22%3A%22https%3A//sts"
        ".ap-southeast-1.amazonaws.com%3FAction%3DGetCallerIdentity%26Version%3D2011-06-15%22%7D";

    REQUIRE(aws_signed_request(kDefaultUrl, "ap-southeast-1", "AKIAEXAMPLEKEYID", kSecret, "",
                              kAudience, kNow) == expected);
}

//------------------------------------------------------------------------------

TEST_CASE("aws_signed_request canonicalizes the path, query and service name",
          "[gcp][aws][wif]") {
    // Everything the raw URL cannot be passed through for, in one vector: the
    // query parameters arrive out of order and one carries an escape (so they
    // must be decoded, re-encoded and sorted), the path needs posix
    // normalization, and the host is not `sts.*` -- which puts "iam" rather than
    // "sts" in the credential scope and in the signing key.
    //
    // Note the `url` member still holds the URL verbatim: only what is *signed*
    // is canonicalized, and AWS re-derives the canonical form from the URL Google
    // replays. Normalizing the URL itself would break that.
    const char* const url =
        "https://iam.amazonaws.com/a//b/./c/../d/"
        "?Version=2011-06-15&Tag=z%2Fz&Action=GetCallerIdentity";

    const std::string expected =
        "%7B%22headers%22%3A%5B%7B%22key%22%3A%22Authorization%22%2C%22value%22%3A%22AWS4-HMAC-SH"
        "A256%20Credential%3DASIAEXAMPLEKEYID/20260806/eu-west-1/iam/aws4_request%2C%20SignedHead"
        "ers%3Dhost%3Bx-amz-date%3Bx-amz-security-token%2C%20Signature%3D90a0da8ba9c6fff0a0b5f8dd"
        "16c71d83f03ceeacfd105f1b9610e463d1b5156b%22%7D%2C%7B%22key%22%3A%22host%22%2C%22value%22"
        "%3A%22iam.amazonaws.com%22%7D%2C%7B%22key%22%3A%22x-amz-date%22%2C%22value%22%3A%2220260"
        "806T123456Z%22%7D%2C%7B%22key%22%3A%22x-amz-security-token%22%2C%22value%22%3A%22IQoJb3J"
        "pZ2luX2VjEExample//Token%2BWith/Slashes%3D%3D%22%7D%2C%7B%22key%22%3A%22x-goog-cloud-tar"
        "get-resource%22%2C%22value%22%3A%22//iam.googleapis.com/projects/123456789/locations/glo"
        "bal/workloadIdentityPools/aws-pool/providers/aws-provider%22%7D%5D%2C%22method%22%3A%22P"
        "OST%22%2C%22url%22%3A%22https%3A//iam.amazonaws.com/a//b/./c/../d/%3FVersion%3D2011-06-1"
        "5%26Tag%3Dz%252Fz%26Action%3DGetCallerIdentity%22%7D";

    REQUIRE(aws_signed_request(url, "eu-west-1", "ASIAEXAMPLEKEYID", kSecret, kSession,
                               kAudience, kNow) == expected);
}

//------------------------------------------------------------------------------

TEST_CASE("aws_signed_request rejects a URL it cannot sign", "[gcp][aws][wif]") {
    // aws.py raises InvalidResource for these; there is nothing to sign against,
    // and returning "" is what the caller reports as a missing credential.
    REQUIRE(aws_signed_request("http://sts.us-east-1.amazonaws.com?Action=GetCallerIdentity",
                               "us-east-1", "ASIAEXAMPLEKEYID", kSecret, kSession, kAudience,
                               kNow).empty());
    REQUIRE(aws_signed_request("https://?Action=GetCallerIdentity", "us-east-1",
                               "ASIAEXAMPLEKEYID", kSecret, kSession, kAudience, kNow).empty());
    REQUIRE(aws_signed_request("sts.us-east-1.amazonaws.com", "us-east-1", "ASIAEXAMPLEKEYID",
                               kSecret, kSession, kAudience, kNow).empty());
}

//------------------------------------------------------------------------------

TEST_CASE("IRSA credentials come out of the STS reply and sign like any other",
          "[gcp][aws][wif]") {
    // The reply shape aws_web_identity_credentials() reads, abbreviated only in
    // the identifiers. It is picked apart with storage::xml_tag_value -- a flat
    // tag scan, which suffices because each of these occurs exactly once and none
    // is nested inside a repeated element. That is the assumption worth pinning:
    // if AWS ever wrapped Credentials in a list, the scan would still return
    // something and the failure would show up as a rejected signature.
    const std::string reply =
        "<AssumeRoleWithWebIdentityResponse xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">\n"
        "  <AssumeRoleWithWebIdentityResult>\n"
        "    <Audience>sts.amazonaws.com</Audience>\n"
        "    <AssumedRoleUser>\n"
        "      <AssumedRoleId>AROAEXAMPLEID:mirobody</AssumedRoleId>\n"
        "      <Arn>arn:aws:sts::209479309958:assumed-role/eks-wif-sa/mirobody</Arn>\n"
        "    </AssumedRoleUser>\n"
        "    <Provider>arn:aws:iam::209479309958:oidc-provider/oidc.eks.us-west-2.amazonaws.com"
        "/id/EXAMPLE</Provider>\n"
        "    <Credentials>\n"
        "      <AccessKeyId>ASIAEXAMPLEKEYID</AccessKeyId>\n"
        "      <SecretAccessKey>wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY</SecretAccessKey>\n"
        "      <SessionToken>IQoJb3JpZ2luX2VjEExample//Token+With/Slashes==</SessionToken>\n"
        "      <Expiration>2026-08-07T01:23:45Z</Expiration>\n"
        "    </Credentials>\n"
        "    <SubjectFromWebIdentityToken>system:serviceaccount:default:theta"
        "</SubjectFromWebIdentityToken>\n"
        "  </AssumeRoleWithWebIdentityResult>\n"
        "  <ResponseMetadata><RequestId>00000000-0000-0000-0000-000000000000</RequestId>"
        "</ResponseMetadata>\n"
        "</AssumeRoleWithWebIdentityResponse>\n";

    const std::string key_id  = mirobody::storage::xml_tag_value(reply, "AccessKeyId");
    const std::string secret  = mirobody::storage::xml_tag_value(reply, "SecretAccessKey");
    const std::string session = mirobody::storage::xml_tag_value(reply, "SessionToken");

    REQUIRE(key_id  == "ASIAEXAMPLEKEYID");
    REQUIRE(secret  == "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
    REQUIRE(session == "IQoJb3JpZ2luX2VjEExample//Token+With/Slashes==");
    REQUIRE(mirobody::storage::xml_tag_value(reply, "Expiration") == "2026-08-07T01:23:45Z");

    // Where the credentials came from changes nothing about what gets signed:
    // fed through the signer they produce exactly what google-auth produces for
    // the same three values. Expected token generated from google-auth 2.56.3.
    const char* const audience =
        "//iam.googleapis.com/projects/142655879991/locations/global/"
        "workloadIdentityPools/aws-test-pool/providers/aws-test-provider";
    const std::string expected =
        "%7B%22headers%22%3A%5B%7B%22key%22%3A%22Authorization%22%2C%22value%22%3A%22AWS4-HMAC-SH"
        "A256%20Credential%3DASIAEXAMPLEKEYID/20260806/us-west-2/sts/aws4_request%2C%20SignedHead"
        "ers%3Dhost%3Bx-amz-date%3Bx-amz-security-token%2C%20Signature%3D6a67de2b68f14136845c78bb"
        "0fddda228e0e1b5bc93f390cdd29ed364cc518c0%22%7D%2C%7B%22key%22%3A%22host%22%2C%22value%22"
        "%3A%22sts.us-west-2.amazonaws.com%22%7D%2C%7B%22key%22%3A%22x-amz-date%22%2C%22value%22%"
        "3A%2220260806T123456Z%22%7D%2C%7B%22key%22%3A%22x-amz-security-token%22%2C%22value%22%3A"
        "%22IQoJb3JpZ2luX2VjEExample//Token%2BWith/Slashes%3D%3D%22%7D%2C%7B%22key%22%3A%22x-goog"
        "-cloud-target-resource%22%2C%22value%22%3A%22//iam.googleapis.com/projects/142655879991/"
        "locations/global/workloadIdentityPools/aws-test-pool/providers/aws-test-provider%22%7D%5"
        "D%2C%22method%22%3A%22POST%22%2C%22url%22%3A%22https%3A//sts.us-west-2.amazonaws.com%3FA"
        "ction%3DGetCallerIdentity%26Version%3D2011-06-15%22%7D";

    REQUIRE(aws_signed_request(kDefaultUrl, "us-west-2", key_id, secret, session, audience,
                               kNow) == expected);
}

//------------------------------------------------------------------------------

TEST_CASE("aws_signed_request signs the host without userinfo or port", "[gcp][aws][wif]") {
    // urlparse().hostname lowercases the host and drops both, and aws.py signs
    // and sends that value -- so all three of these have to reduce to the same
    // token as the plain host does.
    const std::string plain = aws_signed_request(
        "https://sts.us-east-1.amazonaws.com?Action=GetCallerIdentity&Version=2011-06-15",
        "us-east-1", "ASIAEXAMPLEKEYID", kSecret, kSession, kAudience, kNow);
    REQUIRE(!plain.empty());

    const char* const variants[] = {
        "https://STS.US-EAST-1.amazonaws.com?Action=GetCallerIdentity&Version=2011-06-15",
        "https://sts.us-east-1.amazonaws.com:443?Action=GetCallerIdentity&Version=2011-06-15",
        "https://user@sts.us-east-1.amazonaws.com?Action=GetCallerIdentity&Version=2011-06-15",
    };
    for (std::size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        const std::string got = aws_signed_request(variants[i], "us-east-1", "ASIAEXAMPLEKEYID",
                                                   kSecret, kSession, kAudience, kNow);
        // Only the echoed `url` member differs -- the signature and every signed
        // header must be identical.
        const std::size_t cut = got.find("%2C%22method%22");
        REQUIRE(cut != std::string::npos);
        REQUIRE(got.substr(0, cut) == plain.substr(0, plain.find("%2C%22method%22")));
    }
}

//------------------------------------------------------------------------------

TEST_CASE("vertex_host spells each of the three location shapes", "[gcp][vertex]") {
    // A region is a prefix, the multi-regions are a REP infix, and `global` is
    // the bare host. The multi-region case is the one that regressed: "us" read
    // as a region gives us-aiplatform.googleapis.com, which Google answers with
    // 400 INVALID_ARGUMENT "Invalid hostname" -- there is no such endpoint.
    REQUIRE(vertex_host("us-central1") == "https://us-central1-aiplatform.googleapis.com");
    REQUIRE(vertex_host("europe-west2") == "https://europe-west2-aiplatform.googleapis.com");
    REQUIRE(vertex_host("us") == "https://aiplatform.us.rep.googleapis.com");
    REQUIRE(vertex_host("eu") == "https://aiplatform.eu.rep.googleapis.com");
    REQUIRE(vertex_host("global") == "https://aiplatform.googleapis.com");

    // The Live API socket takes the same three shapes over wss.
    REQUIRE(vertex_host("us", "wss") == "wss://aiplatform.us.rep.googleapis.com");
    REQUIRE(vertex_host("global", "wss") == "wss://aiplatform.googleapis.com");

    // No location, no host to guess at.
    REQUIRE(vertex_host("").empty());
}

TEST_CASE("vertex_model_location overrides the deployment location per model",
          "[gcp][vertex]") {
    // The case this exists for. `us` is what a US-residency chat deployment
    // wants and what gemini-3.5-flash serves; gemini-embedding-001 publishes no
    // multi-region at all, so the same value 404s on the embedding lane. Neither
    // side is wrong -- the models' published lists genuinely do not intersect.
    std::unordered_map<std::string, std::string> per_model;
    per_model["gemini-embedding-001"] = "us-central1";

    REQUIRE(vertex_model_location(per_model, "gemini-embedding-001", "us") == "us-central1");
    REQUIRE(vertex_model_location(per_model, "gemini-3.5-flash", "us") == "us");

    // A model nobody overrode falls back, and so does the empty map -- the
    // shipped default has to be "behave exactly as before".
    REQUIRE(vertex_model_location(per_model, "gemini-2.5-flash", "global") == "global");
    REQUIRE(vertex_model_location(std::unordered_map<std::string, std::string>(),
                                  "gemini-embedding-001", "us") == "us");

    // An override blanked out reads as unset, as every other config value does.
    // Returning "" instead would build a hostless URL from vertex_host("").
    per_model["gemini-3.5-flash"] = "";
    REQUIRE(vertex_model_location(per_model, "gemini-3.5-flash", "us") == "us");

    // Lookup is exact: the map is keyed by the model id sent to Google, and a
    // prefix or case variant is a different model, not a near miss to absorb.
    REQUIRE(vertex_model_location(per_model, "gemini-embedding-2", "us") == "us");
    REQUIRE(vertex_model_location(per_model, "GEMINI-EMBEDDING-001", "us") == "us");
}
