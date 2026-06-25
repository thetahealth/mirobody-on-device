import Foundation

/// A decoded-but-untyped JSON node. Needed because the `content` field of an SSE
/// chunk is polymorphic on the wire (string for reply/thinking, object for
/// costStatistics, etc.) — the same reason the Android DTO types it as
/// `JsonElement`.
indirect enum JSONValue: Codable {
    case string(String)
    case number(Double)
    case bool(Bool)
    case object([String: JSONValue])
    case array([JSONValue])
    case null

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() {
            self = .null
        } else if let b = try? c.decode(Bool.self) {
            self = .bool(b)
        } else if let n = try? c.decode(Double.self) {
            self = .number(n)
        } else if let s = try? c.decode(String.self) {
            self = .string(s)
        } else if let a = try? c.decode([JSONValue].self) {
            self = .array(a)
        } else if let o = try? c.decode([String: JSONValue].self) {
            self = .object(o)
        } else {
            throw DecodingError.dataCorruptedError(in: c, debugDescription: "Unsupported JSON value")
        }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .string(let s): try c.encode(s)
        case .number(let n): try c.encode(n)
        case .bool(let b):   try c.encode(b)
        case .object(let o): try c.encode(o)
        case .array(let a):  try c.encode(a)
        case .null:          try c.encodeNil()
        }
    }

    /// Backend emits `content` as a string for reply/thinking/error/tool fields;
    /// tolerate either form (matches the Kotlin `asString()` helper).
    var asString: String {
        switch self {
        case .string(let s): return s
        case .null: return ""
        default: return (try? jsonString()) ?? ""
        }
    }

    /// Re-decode this node into a concrete `Decodable` (used for costStatistics).
    func decoded<T: Decodable>(_ type: T.Type) throws -> T {
        let data = try JSONEncoder().encode(self)
        return try JSONDecoder().decode(type, from: data)
    }

    private func jsonString() throws -> String {
        let data = try JSONEncoder().encode(self)
        return String(data: data, encoding: .utf8) ?? ""
    }
}
