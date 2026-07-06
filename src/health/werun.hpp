#pragma once

// WeChat WeRun (微信运动) step ingestion.
//
// WeChat's only health-data surface is WeRun daily step counts — and it is NOT
// Bluetooth. A Mini Program calls wx.getWeRunData(), which returns the user's last
// ~31 days of daily steps as an AES-128-CBC-encrypted blob. This service decrypts
// that blob with the WeChat session_key, maps the steps to FHIR R4 Observations
// (LOINC 55423-8, via health::vendor_json_to_observations), and persists them
// through fhir::FhirStore — the same write path as /vendors/{id}/sync.
//
// The session_key is NOT stored. The Mini Program sends a fresh wx.login() `code`
// alongside the blob; this service exchanges it via jscode2session at decrypt time
// (use-once), reusing the Mini Program credentials WECHAT_APPID / WECHAT_SECRET. The
// mirobody user is taken from the bearer JWT, not from WeChat.
//
//   POST /wechat/werun   {code, encryptedData, iv}  ->  {posted, failed}
//
// `cfg`, `db`, and `jwt` are borrowed (not owned) and must outlive the server.

#include "config/config.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

namespace mirobody { namespace database { class Database; } }

namespace mirobody { namespace health {

class WeRunService {
public:
    WeRunService(server::Router& router, const Config& cfg,
                 database::Database& db, const jwt::Jwt& jwt);

    WeRunService(const WeRunService&) = delete;
    WeRunService& operator=(const WeRunService&) = delete;

private:
    void on_werun(const server::Request& req, server::Response& res);

    const Config&       cfg_;
    database::Database& db_;
    const jwt::Jwt&     jwt_;
};

}}  // namespace mirobody::health
