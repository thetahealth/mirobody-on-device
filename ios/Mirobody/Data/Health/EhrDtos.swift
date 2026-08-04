import Foundation

/// EHR connect (SMART on FHIR) DTOs — mirrors `data/health/EhrApi.kt`.

/// One tenant from the EHR provider directory.
struct EhrProvider: Decodable, Identifiable {
    let name: String
    let fhirBaseUrl: String

    var id: String { fhirBaseUrl }

    enum CodingKeys: String, CodingKey {
        case name
        case fhirBaseUrl = "fhir_base_url"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
        fhirBaseUrl = try c.decodeIfPresent(String.self, forKey: .fhirBaseUrl) ?? ""
    }
}

struct EhrProviders: Decodable {
    let providers: [EhrProvider]
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        providers = try c.decodeIfPresent([EhrProvider].self, forKey: .providers) ?? []
    }
    enum CodingKeys: String, CodingKey { case providers }
}

struct EhrAuthorizeRequest: Encodable {
    let fhirBaseUrl: String
    enum CodingKeys: String, CodingKey { case fhirBaseUrl = "fhir_base_url" }
}

struct EhrAuthorize: Decodable {
    let authorizeUrl: String?
    enum CodingKeys: String, CodingKey { case authorizeUrl = "authorize_url" }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        authorizeUrl = try c.decodeIfPresent(String.self, forKey: .authorizeUrl)
    }
}

struct EhrSyncResult: Decodable {
    let posted: Int
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        posted = try c.decodeIfPresent(Int.self, forKey: .posted) ?? 0
    }
    enum CodingKeys: String, CodingKey { case posted }
}

/// An empty JSON body (`{}`) for POSTs that carry no fields (sync / unlink).
struct EmptyBody: Encodable {}
