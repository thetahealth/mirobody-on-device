// Standalone CLI for the health-data vendor clients under src/health/vendor/.
//
// Unlike the storage CLIs (one executable per backend), this is a single
// multiplexed driver: all 15 vendors share the same VendorConfig shape and
// differ only by id, so the id is the first positional argument. Splitting into
// per-vendor executables later is a one-line foreach in CMakeLists.txt.
//
//   vendor list                                  # all registered vendors
//   vendor <id> info                             # full metadata for one vendor
//   vendor <id> auth-url   --redirect URL [--state S]
//   vendor <id> providers
//   vendor <id> fetch      --user U --domain sleep [--start ISO] [--end ISO]
//   vendor <id> webhook    --file payload.json   # body from --file/--data/stdin
//   vendor <id> revoke     --user U
//
// Every operation except `list`/`info` is a stub today and will print
// "<id>: <op> not implemented (stub)". Credentials come from --api-key /
// --client-id / --client-secret / --base-url, else from the environment:
//   MIROBODY_VENDOR_<ID>_API_KEY / _CLIENT_ID / _CLIENT_SECRET / _BASE_URL

#include "client/http_client.hpp"
#include "health/vendor/registry.hpp"
#include "event_printer.hpp"   // prepare_windows_console, stdin_is_tty

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace mv = mirobody::vendor;

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s list\n"
        "       %s <id> <command> [options]\n"
        "\n"
        "Commands:\n"
        "  list                 List all registered vendors.\n"
        "  info                 Print full metadata for <id>.\n"
        "  auth-url             Print the consumer-consent entry point (--redirect, --state,\n"
        "                       --user, --provider — which apply depends on the vendor).\n"
        "  providers            List connectable data sources (--user for user-scoped vendors).\n"
        "  fetch                Fetch data (--user, --domain, --start, --end).\n"
        "  webhook              Parse a webhook delivery (body via --file/--data/stdin).\n"
        "  revoke               Revoke a user's authorization (--user, --provider).\n"
        "\n"
        "Options:\n"
        "  --user <id>          End-user id from the consent flow.\n"
        "  --provider <slug>    Single data source, for per-provider authorize/revoke.\n"
        "  --domain <name>      activity|sleep|heart_rate|glucose|nutrition|body_metrics|labs|clinical\n"
        "  --start <iso>        ISO-8601 start of the fetch window.\n"
        "  --end <iso>          ISO-8601 end of the fetch window.\n"
        "  --redirect <url>     Redirect URL for auth-url.\n"
        "  --state <s>          Opaque state echoed back to the redirect.\n"
        "  --file <path>        Webhook body from a file.\n"
        "  --data <string>      Webhook body inline.\n"
        "  --api-key <k>        Vendor API key (else MIROBODY_VENDOR_<ID>_API_KEY).\n"
        "  --client-id <k>      OAuth client id.\n"
        "  --client-secret <k>  OAuth client secret.\n"
        "  --base-url <url>     API host override.\n"
        "  -h, --help           Show this help.\n",
        prog, prog);
}

void print_list_field(const char* label, const std::string& value) {
    if (!value.empty()) std::fprintf(stdout, "  %-14s %s\n", label, value.c_str());
}

void print_info(const mv::VendorInfo& v) {
    std::fprintf(stdout, "%s  (%s)\n", v.display_name.c_str(), v.id.c_str());
    print_list_field("positioning", v.positioning);
    print_list_field("customers", v.target_customers);
    print_list_field("coverage", v.data_source_coverage);
    print_list_field("integration", v.integration_method);
    print_list_field("compliance", v.compliance_summary);
    print_list_field("edge", v.differentiator);
    print_list_field("docs", v.docs_url);
    std::fprintf(stdout, "  %-14s %s%s\n", "region", mv::to_string(v.region),
                 v.open_source ? " (open source)" : "");

    std::string domains;
    for (mv::DataDomain d : v.domains) { if (!domains.empty()) domains += ", "; domains += mv::to_string(d); }
    print_list_field("domains", domains);

    std::string integrations;
    for (mv::Integration g : v.integrations) { if (!integrations.empty()) integrations += ", "; integrations += mv::to_string(g); }
    print_list_field("apis", integrations);

    std::string certs;
    for (const std::string& c : v.compliance) { if (!certs.empty()) certs += ", "; certs += c; }
    print_list_field("certs", certs);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !in.bad();
}

std::string read_stdin() {
    std::string out;
    char buf[65536];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), stdin)) out.append(buf, n);
    return out;
}

}

int main(int argc, char** argv) {
    mirobody::client::HttpClient::global_init();
    mirobody::tools::prepare_windows_console();
    int rc = 0;

    std::string id, command;
    std::string user, provider, domain_s, start, end, redirect, state, file, data;
    bool have_data = false;
    mv::VendorConfig flag_cfg;

    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help")  { usage(argv[0]); mirobody::client::HttpClient::global_cleanup(); return 0; }
        else if (a == "--user")               { user = need(i, "--user"); }
        else if (a == "--provider")           { provider = need(i, "--provider"); }
        else if (a == "--domain")             { domain_s = need(i, "--domain"); }
        else if (a == "--start")              { start = need(i, "--start"); }
        else if (a == "--end")                { end = need(i, "--end"); }
        else if (a == "--redirect")           { redirect = need(i, "--redirect"); }
        else if (a == "--state")              { state = need(i, "--state"); }
        else if (a == "--file")               { file = need(i, "--file"); }
        else if (a == "--data")               { data = need(i, "--data"); have_data = true; }
        else if (a == "--api-key")            { flag_cfg.api_key = need(i, "--api-key"); }
        else if (a == "--client-id")          { flag_cfg.client_id = need(i, "--client-id"); }
        else if (a == "--client-secret")      { flag_cfg.client_secret = need(i, "--client-secret"); }
        else if (a == "--base-url")           { flag_cfg.base_url = need(i, "--base-url"); }
        else if (!a.empty() && a.front() == '-') {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
            usage(argv[0]); mirobody::client::HttpClient::global_cleanup(); return 2;
        }
        else if (id.empty())                  { id = std::string(a); }
        else if (command.empty())             { command = std::string(a); }
        else {
            std::fprintf(stderr, "unexpected argument: %.*s\n", static_cast<int>(a.size()), a.data());
            mirobody::client::HttpClient::global_cleanup(); return 2;
        }
    }

    // `vendor list` — id slot holds the command.
    if (id == "list") {
        std::fprintf(stdout, "%-16s %-26s %-12s %s\n", "ID", "NAME", "REGION", "OPEN-SOURCE");
        for (const mv::VendorInfo& v : mv::all_vendor_info()) {
            std::fprintf(stdout, "%-16s %-26s %-12s %s\n",
                         v.id.c_str(), v.display_name.c_str(),
                         mv::to_string(v.region), v.open_source ? "yes" : "no");
        }
        mirobody::client::HttpClient::global_cleanup();
        return 0;
    }

    if (id.empty() || command.empty()) {
        usage(argv[0]);
        mirobody::client::HttpClient::global_cleanup();
        return 2;
    }

    try {
        // Flags win over env; fall back to MIROBODY_VENDOR_<ID>_* for anything unset.
        mv::VendorConfig cfg = mv::VendorConfig::from_env(id);
        if (!flag_cfg.api_key.empty())       cfg.api_key = flag_cfg.api_key;
        if (!flag_cfg.client_id.empty())     cfg.client_id = flag_cfg.client_id;
        if (!flag_cfg.client_secret.empty()) cfg.client_secret = flag_cfg.client_secret;
        if (!flag_cfg.base_url.empty())      cfg.base_url = flag_cfg.base_url;

        std::unique_ptr<mv::Vendor> vendor = mv::open_vendor(id, cfg);   // throws if unknown

        if (command == "info") {
            print_info(vendor->info());
        } else if (command == "auth-url") {
            std::fprintf(stdout, "%s\n", vendor->authorize_url(redirect, state, user, provider).c_str());
        } else if (command == "providers") {
            std::fprintf(stdout, "%s\n", vendor->list_providers(user).c_str());
        } else if (command == "fetch") {
            mv::DataDomain domain;
            if (!mv::parse_domain(domain_s, domain)) {
                std::fprintf(stderr, "vendor: invalid or missing --domain '%s'\n", domain_s.c_str());
                rc = 2;
            } else {
                std::fprintf(stdout, "%s\n", vendor->fetch(user, domain, start, end).c_str());
            }
        } else if (command == "webhook") {
            std::string body;
            if (have_data) body = data;
            else if (!file.empty()) {
                if (!read_file(file, body)) { std::fprintf(stderr, "vendor: cannot read --file '%s'\n", file.c_str()); rc = 1; }
            } else if (!mirobody::tools::stdin_is_tty()) body = read_stdin();
            if (rc == 0) std::fprintf(stdout, "%s\n", vendor->handle_webhook("", body).c_str());
        } else if (command == "revoke") {
            vendor->revoke(user, provider);
            std::fprintf(stderr, "revoked %s%s for %s\n", user.c_str(),
                         provider.empty() ? "" : (" / " + provider).c_str(), id.c_str());
        } else {
            std::fprintf(stderr, "unknown command: %s\n\n", command.c_str());
            usage(argv[0]);
            rc = 2;
        }
    } catch (const mv::VendorError& e) {
        std::fprintf(stderr, "vendor: %s\n", e.what());
        rc = 1;
    }

    mirobody::client::HttpClient::global_cleanup();
    return rc;
}
