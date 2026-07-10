import SwiftUI

private struct ImageItem: Identifiable {
    let id = UUID()
    let content: String
}

/// One chat message — mirrors `MessageBubble` in `ChatScreen.kt`. User messages get
/// a tinted bubble; assistant messages render plain on the background with an
/// optional cost-stats button.
struct MessageBubble: View {
    let message: ChatMessage
    @Environment(\.mbColors) private var colors
    @State private var showStats = false
    @State private var viewerItem: ImageItem?

    private var isUser: Bool { message.role == .user }

    var body: some View {
        VStack(alignment: isUser ? .trailing : .leading, spacing: 0) {
            if !message.thinking.isEmpty {
                Text(message.thinking)
                    .mbFont(.bodySmall)
                    .italic()
                    .foregroundColor(colors.onSurfaceVariant)
                    .padding(.horizontal, 14).padding(.vertical, 10)
                    .frame(maxWidth: 320, alignment: .leading)
                    .background(colors.surfaceContainerLow)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                    .padding(.bottom, 6)
            }

            if isUser {
                // Solid navy bubble with light text — the web client's user-turn style.
                bubbleContent
                    .frame(maxWidth: 320, alignment: .leading)
                    .background(brandBlue)
                    .clipShape(RoundedRectangle(cornerRadius: 14))
            } else {
                VStack(alignment: .leading, spacing: 0) {
                    bubbleContent
                    if message.costStats != nil && !message.streaming {
                        HStack {
                            Spacer()
                            Button { showStats = true } label: {
                                Image(systemName: "info.circle")
                                    .font(.system(size: 16))
                                    .foregroundColor(colors.onSurfaceVariant.opacity(0.35))
                            }
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .frame(maxWidth: .infinity, alignment: isUser ? .trailing : .leading)
        .sheet(isPresented: $showStats) {
            if let stats = message.costStats { CostStatsSheet(stats: stats) }
        }
        .fullScreenCover(item: $viewerItem) { item in
            ImageViewer(content: item.content) { viewerItem = nil }
        }
    }

    private var bubbleContent: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Attached file names (user turns only). White-ish on the navy bubble.
            ForEach(message.attachmentNames, id: \.self) { name in
                HStack(spacing: 6) {
                    Image(systemName: "doc").font(.system(size: 12))
                    Text(name).mbFont(.bodySmall).lineLimit(1)
                }
                .foregroundColor(isUser ? Color.white.opacity(0.9) : colors.onSurfaceVariant)
                .padding(.bottom, 4)
            }
            ForEach(Array(message.toolCalls.enumerated()), id: \.element.id) { index, tool in
                ToolCallCard(tool: tool).padding(.top, index > 0 ? 6 : 0)
            }
            if !message.text.isEmpty {
                MarkdownText(text: message.text, color: isUser ? .white : nil,
                             streaming: message.streaming)
                    .padding(.top, message.toolCalls.isEmpty ? 0 : 8)
            } else if message.streaming && message.error == nil
                        && message.toolCalls.isEmpty && message.imageUrls.isEmpty
                        && message.chartOptions.isEmpty {
                TypingDots()
            }
            ForEach(Array(message.chartOptions.enumerated()), id: \.offset) { index, option in
                let needsTopPad = index > 0 || !message.text.isEmpty || !message.toolCalls.isEmpty
                EChartsView(option: option)
                    .frame(maxWidth: .infinity)
                    .padding(.top, needsTopPad ? 8 : 0)
            }
            ForEach(Array(message.imageUrls.enumerated()), id: \.offset) { index, url in
                let needsTopPad = index > 0 || !message.text.isEmpty
                    || !message.toolCalls.isEmpty || !message.chartOptions.isEmpty
                ChatImage(content: url)
                    .frame(maxWidth: .infinity)
                    .padding(.top, needsTopPad ? 8 : 0)
                    .onTapGesture { viewerItem = ImageItem(content: url) }
            }
            if let err = message.error {
                Text(err).mbFont(.bodySmall).foregroundColor(colors.error).padding(.top, 4)
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
    }
}

private struct ToolCallCard: View {
    let tool: ToolCall
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang
    @State private var expanded = false

    private var hasDetails: Bool { !tool.argumentsJson.isEmpty || !tool.resultJson.isEmpty }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "wrench.and.screwdriver")
                    .font(.system(size: 14)).foregroundColor(colors.onSurfaceVariant)
                Text(tool.title.isEmpty ? L("chat_tool", lang) : tool.title)
                    .mbFont(.labelMedium).foregroundColor(colors.onSurface)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if !tool.resultReceived {
                    ProgressView().controlSize(.mini)
                } else if hasDetails {
                    Image(systemName: expanded ? "chevron.up" : "chevron.down")
                        .font(.system(size: 12)).foregroundColor(colors.onSurfaceVariant)
                }
            }
            if expanded {
                if !tool.argumentsJson.isEmpty { section(L("chat_tool_args", lang), tool.argumentsJson) }
                if !tool.resultJson.isEmpty { section(L("chat_tool_result", lang), tool.resultJson) }
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(colors.surfaceContainerLow)
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .contentShape(Rectangle())
        .onTapGesture { if hasDetails { expanded.toggle() } }
    }

    private func section(_ label: String, _ content: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label).mbFont(.labelSmall).foregroundColor(colors.onSurfaceVariant)
            Text(content)
                .mbFont(.bodySmall, monospaced: true)
                .foregroundColor(colors.onSurface)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 8).padding(.vertical, 6)
                .background(colors.surface)
                .clipShape(RoundedRectangle(cornerRadius: 6))
        }
        .padding(.top, 8)
    }
}

private struct TypingDots: View {
    @Environment(\.mbColors) private var colors
    @State private var animating = false

    var body: some View {
        HStack(spacing: 4) {
            ForEach(0..<3, id: \.self) { i in
                Circle()
                    .fill(colors.onSurfaceVariant.opacity(animating ? 1 : 0.25))
                    .frame(width: 6, height: 6)
                    .animation(
                        .easeInOut(duration: 0.7).repeatForever(autoreverses: true)
                            .delay(Double(i) * 0.18),
                        value: animating
                    )
            }
        }
        .padding(.vertical, 4)
        .onAppear { animating = true }
    }
}

private struct CostStatsSheet: View {
    let stats: CostStatistics
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            LText("chat_stats_title").mbFont(.titleMedium).foregroundColor(colors.onSurface)
            row(L("chat_stats_model", lang), stats.model)
            row(L("chat_stats_input_tokens", lang), "\(stats.inputTokens)")
            row(L("chat_stats_output_tokens", lang), "\(stats.outputTokens)")
            if stats.thoughtTokens > 0 {
                row(L("chat_stats_thought_tokens", lang), "\(stats.thoughtTokens)")
            }
            row(L("chat_stats_total_tokens", lang), "\(stats.totalTokens)")
            row(L("chat_stats_total_cost", lang), String(format: "$%.4f", stats.totalCost))
            Spacer()
            Button { dismiss() } label: { LText("common_close").foregroundColor(colors.primary) }
                .frame(maxWidth: .infinity)
        }
        .padding(24)
        .presentationDetents([.medium])
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)
            Spacer()
            Text(value).mbFont(.bodyMedium).foregroundColor(colors.onSurface)
        }
    }
}
