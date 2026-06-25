import SwiftUI

/// History list — mirrors `ui/chat/HistoryScreen.kt`. Presented as a sheet from the
/// chat toolbar. View/delete only (tapping a row doesn't reopen the session, same
/// as Android).
struct HistoryView: View {
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang
    @Environment(\.dismiss) private var dismiss

    @StateObject private var vm: HistoryViewModel
    @State private var pendingDelete: SessionSummary?

    init(container: AppContainer) {
        _vm = StateObject(wrappedValue: HistoryViewModel(
            repo: container.chatRepository,
            errorBus: container.errorBus,
            language: container.settings.language
        ))
    }

    var body: some View {
        NavigationStack {
            ZStack {
                colors.background.ignoresSafeArea()
                content
            }
            .navigationTitle(L("history_title", lang))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button { dismiss() } label: { Image(systemName: "chevron.backward") }
                        .tint(colors.onSurfaceVariant)
                }
            }
        }
        .onAppear { vm.refresh() }
        .confirmationDialog(
            L("history_delete_confirm_title", lang),
            isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }),
            titleVisibility: .visible
        ) {
            Button(L("common_delete", lang), role: .destructive) {
                if let item = pendingDelete { vm.deleteHistory(sessionId: item.sessionId) }
                pendingDelete = nil
            }
            Button(L("common_cancel", lang), role: .cancel) { pendingDelete = nil }
        } message: {
            Text(L("history_delete_confirm_message", lang))
        }
    }

    @ViewBuilder
    private var content: some View {
        if vm.loading && vm.items.isEmpty {
            ProgressView()
        } else if let err = vm.error {
            VStack(spacing: 8) {
                Text(err).mbFont(.bodyMedium).foregroundColor(colors.error)
                Button(L("common_retry", lang)) { vm.refresh() }.foregroundColor(colors.primary)
            }
            .padding(24)
        } else if vm.items.isEmpty {
            LText("history_empty").mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant).padding(24)
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(vm.items) { item in
                        HistoryRow(item: item) { pendingDelete = item }
                        Divider().background(colors.outlineVariant.opacity(0.4)).padding(.horizontal, 20)
                    }
                }
                .padding(.vertical, 8)
            }
        }
    }
}

private struct HistoryRow: View {
    let item: SessionSummary
    let onDelete: () -> Void
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text(title).mbFont(.bodyLarge).foregroundColor(colors.onSurface).lineLimit(2)
                if !item.timestamp.isEmpty {
                    Text(formatHistoryTimestamp(item.timestamp))
                        .mbFont(.labelSmall)
                        .foregroundColor(colors.onSurfaceVariant.opacity(0.6))
                }
            }
            Spacer()
            Button(action: onDelete) {
                Image(systemName: "trash").font(.system(size: 18)).foregroundColor(colors.error)
            }
            .accessibilityLabel(L("history_delete_cd", lang))
        }
        .padding(.leading, 20).padding(.trailing, 8).padding(.vertical, 14)
    }

    private var title: String {
        item.summary.nonBlank ?? item.sessionId.nonBlank ?? L("history_untitled", lang)
    }
}

/// Parses the backend's ISO-8601 timestamp (with or without offset; naive strings
/// are treated as UTC) and formats it in the device's local zone. Mirrors
/// `formatTimestamp` in HistoryScreen.kt.
func formatHistoryTimestamp(_ raw: String) -> String {
    let date: Date? = {
        let withFraction = ISO8601DateFormatter()
        withFraction.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let d = withFraction.date(from: raw) { return d }
        let plain = ISO8601DateFormatter()
        plain.formatOptions = [.withInternetDateTime]
        if let d = plain.date(from: raw) { return d }
        // Naive (no offset) — interpret as UTC.
        let naive = DateFormatter()
        naive.locale = Locale(identifier: "en_US_POSIX")
        naive.timeZone = TimeZone(identifier: "UTC")
        for fmt in ["yyyy-MM-dd'T'HH:mm:ss.SSSSSS", "yyyy-MM-dd'T'HH:mm:ss.SSS", "yyyy-MM-dd'T'HH:mm:ss"] {
            naive.dateFormat = fmt
            if let d = naive.date(from: raw) { return d }
        }
        return nil
    }()
    guard let date else { return raw }
    let out = DateFormatter()
    out.dateFormat = "yyyy-MM-dd HH:mm"
    out.timeZone = .current
    return out.string(from: date)
}
