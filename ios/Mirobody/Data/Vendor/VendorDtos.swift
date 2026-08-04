import Foundation

/// Vendor account management DTOs — mirrors `data/vendor/VendorApi.kt`.

/// A connected vendor grant from `GET /vendors` (only the fields the UI needs;
/// unknown keys like `has_token` / `updated_at` are ignored).
struct VendorLink: Decodable {
    let id: String
    let verified: Bool
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(String.self, forKey: .id) ?? ""
        verified = try c.decodeIfPresent(Bool.self, forKey: .verified) ?? false
    }
    enum CodingKeys: String, CodingKey { case id, verified }
}

/// `GET /vendors/{id}/authorize` → the vendor's OAuth consent URL.
struct VendorAuthorize: Decodable {
    let authorizeUrl: String?
    enum CodingKeys: String, CodingKey { case authorizeUrl = "authorize_url" }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        authorizeUrl = try c.decodeIfPresent(String.self, forKey: .authorizeUrl)
    }
}

/// One catalog row for the UI: brand id/name, whether it's connected (and still
/// pending verification), and its server-provided icon data URI (nil → monogram).
/// Mirrors Android's `VendorItem`.
struct VendorItem: Identifiable {
    let id: String
    let name: String
    let connected: Bool
    let pending: Bool
    let iconDataUri: String?
}

/// The static vendor catalog — mirrors `VendorsViewModel` companion + the web
/// client's `CATALOG` (`src/health/vendor/`).
enum VendorCatalog {
    /// Consumer wearables/devices (first tab).
    static let devices = [
        "oura", "whoop", "polar", "fitbit", "withings", "dexcom", "garmin", "huawei",
    ]
    /// B2B aggregation platforms (second tab).
    static let platforms = [
        "terra", "validic", "rook", "spike", "junction", "wefitter", "thryve",
        "human_api", "vitalera", "metriport", "open_wearables", "redox",
        "particle_health", "healthconnect", "lexisnexis",
    ]

    private static let names: [String: String] = [
        "oura": "Oura", "whoop": "WHOOP", "polar": "Polar", "fitbit": "Fitbit",
        "withings": "Withings", "dexcom": "Dexcom", "garmin": "Garmin",
        "huawei": "Huawei Health", "terra": "Terra", "validic": "Validic",
        "rook": "Rook", "spike": "Spike", "junction": "Junction",
        "wefitter": "WeFitter", "thryve": "Thryve", "human_api": "Human API",
        "vitalera": "Vitalera", "metriport": "Metriport",
        "open_wearables": "Open Wearables", "redox": "Redox",
        "particle_health": "Particle Health", "healthconnect": "HealthConnect CoPilot",
        "lexisnexis": "LexisNexis",
    ]

    static func name(of id: String) -> String { names[id] ?? id }
}
