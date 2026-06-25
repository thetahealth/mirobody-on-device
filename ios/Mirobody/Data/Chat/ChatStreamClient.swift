import Foundation

/// POSTs `/api/chat` and decodes the Server-Sent-Events response into a stream of
/// `ChatStreamEvent`s. Mirrors `data/chat/ChatStreamClient.kt`.
///
/// Where Android uses OkHttp's `EventSource`, here we use `URLSession.bytes(for:)`
/// and split on lines. Each SSE `data:` line is one JSON chunk (`RawSseChunk`).
/// The returned `AsyncStream` finishes on the `end` event, on failure (after
/// emitting `.error`), or when the consuming task is cancelled.
final class ChatStreamClient {
    private let api: ApiClient
    private let session: URLSession

    init(api: ApiClient) {
        self.api = api
        // A dedicated session with a long request timeout: an SSE response stays
        // open for the whole turn, so the 60s timeout on the JSON client would cut
        // it off mid-stream.
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 3600
        cfg.timeoutIntervalForResource = 86_400
        self.session = URLSession(configuration: cfg)
    }

    func stream(_ request: ChatStreamRequest) -> AsyncStream<ChatStreamEvent> {
        AsyncStream { continuation in
            let task = Task {
                do {
                    let body = try api.encode(request)
                    let req = try api.makeURLRequest(
                        path: "/api/chat",
                        method: "POST",
                        jsonBody: body,
                        accept: "text/event-stream"
                    )
                    var streamingReq = req
                    streamingReq.setValue("no-cache", forHTTPHeaderField: "Cache-Control")

                    let (bytes, response) = try await session.bytes(for: streamingReq)
                    guard let http = response as? HTTPURLResponse else {
                        continuation.yield(.error(message: "stream failed"))
                        continuation.finish()
                        return
                    }
                    guard (200..<300).contains(http.statusCode) else {
                        continuation.yield(.error(message: "HTTP \(http.statusCode)"))
                        continuation.finish()
                        return
                    }

                    for try await line in bytes.lines {
                        guard line.hasPrefix("data:") else { continue }   // skip event:/id:/comments
                        let payload = line.dropFirst("data:".count)
                            .trimmingCharacters(in: .whitespaces)
                        if payload.isEmpty { continue }
                        let event = Self.parseChunk(payload)
                        continuation.yield(event)
                        if case .end = event { break }
                    }
                    continuation.finish()
                } catch is CancellationError {
                    continuation.finish()
                } catch {
                    continuation.yield(.error(message: error.localizedDescription))
                    continuation.finish()
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    static func parseChunk(_ data: String) -> ChatStreamEvent {
        guard let chunk = try? JSONDecoder().decode(RawSseChunk.self, from: Data(data.utf8)) else {
            return .unknown(rawType: "parse_error", rawData: data)
        }
        func tool(_ make: (String) -> ChatStreamEvent) -> ChatStreamEvent {
            guard let id = chunk.toolId, !id.isEmpty else { return .unknown(rawType: chunk.type, rawData: data) }
            return make(id)
        }
        switch chunk.type {
        case "id":
            return .id(replyId: chunk.replyId ?? "")
        case "thinking":
            return .thinking(delta: chunk.content?.asString ?? "")
        case "reply":
            return .reply(delta: chunk.content?.asString ?? "")
        case "queryTitle":
            return tool { .queryTitle(toolId: $0, title: chunk.content?.asString ?? "") }
        case "queryArguments":
            return tool { .queryArguments(toolId: $0, argumentsJson: chunk.content?.asString ?? "") }
        case "queryDetail":
            return tool { .queryDetail(toolId: $0, detailJson: chunk.content?.asString ?? "") }
        case "image":
            let url = chunk.content?.asString ?? ""
            return url.isEmpty ? .unknown(rawType: chunk.type, rawData: data) : .image(url: url)
        case "chart":
            // The option is a nested JSON object; re-stringify it for echarts.setOption().
            let option = chunk.chart?.asString ?? ""
            return option.isEmpty ? .unknown(rawType: chunk.type, rawData: data) : .chart(optionJson: option)
        case "heartbeat":
            return .heartbeat
        case "error":
            return .error(message: chunk.content?.asString ?? "")
        case "end":
            return .end
        case "costStatistics":
            if let node = chunk.content, let stats = try? node.decoded(CostStatistics.self) {
                return .stats(stats)
            }
            return .unknown(rawType: chunk.type, rawData: data)
        default:
            return .unknown(rawType: chunk.type, rawData: data)
        }
    }
}
