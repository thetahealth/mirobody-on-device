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
                // Either half can be missing — a row with no timestamp, or a backend
                // that does not send message_count yet — so the separator is drawn only
                // when both are there rather than leaving a dangling "·".
                if !subtitle.isEmpty {
                    Text(subtitle)
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

    private var subtitle: String { historySubtitle(item) }
}

/// "2 hours ago · 6 messages" for a history row, in either list.
///
/// Shared because the drawer and the full history screen render the same rows, and the
/// version where each formatted its own is how one of them ends up a release behind.
///
/// The count goes through `.stringsdict`, not string interpolation: "1 messages" is
/// wrong in English and the rule differs again in Russian (few/many) and Arabic (six
/// categories). Foundation applies the locale's own rule; the forms come from the same
/// reviewed translations Android ships.
func historySubtitle(_ item: SessionSummary) -> String {
    let stamp = item.timestamp > 0 ? relativeHistoryStamp(item.timestamp) : ""
    let count = item.messageCount > 0
        ? String.localizedStringWithFormat(
            NSLocalizedString("history_message_count", comment: "turns in a conversation"),
            item.messageCount)
        : ""
    return [stamp, count].filter { !$0.isEmpty }.joined(separator: " · ")
}

/// How long ago, not when. Mirrors `relativeStamp` in HistoryScreen.kt.
///
/// A history list is scanned for "which conversation was that", and "2 hours ago"
/// answers it where "2026-08-09 14:31" has to be decoded first. Past a week the
/// relative form stops helping ("37 days ago" is not a date anyone pictures), so it
/// switches to an absolute one.
///
/// `RelativeDateTimeFormatter` rather than the hand-rolled ladder Android needs:
/// Foundation already knows every locale's wording AND its plural rules, so there are
/// no `history_mins_ago` strings to write or keep in step across ten languages.
///
/// Likewise the absolute form is built from a TEMPLATE, not a pattern. "MMM d" hardcodes
/// the English field order; `setLocalizedDateFormatFromTemplate` asks the locale for its
/// own, which is how ja gets 8月9日 rather than 8 9. The year is carried only when it is
/// not the current one — always omitting it leaves "Mar 15" ambiguous once a
/// conversation is more than a year old.
func relativeHistoryStamp(_ millis: Int64) -> String {
    let date = Date(timeIntervalSince1970: Double(millis) / 1000.0)
    let now = Date()
    // Clamped at zero: a server clock a few seconds ahead of the phone would otherwise
    // render "in 1 minute".
    let elapsed = max(0, now.timeIntervalSince(date))

    if elapsed < 7 * 24 * 3600 {
        let rel = RelativeDateTimeFormatter()
        // .named so the locale may say "yesterday" where it has a word for it; it falls
        // back to the numeric form by itself everywhere else.
        rel.dateTimeStyle = .named
        rel.unitsStyle = .short
        return rel.localizedString(for: now.addingTimeInterval(-elapsed), relativeTo: now)
    }

    let sameYear = Calendar.current.component(.year, from: date)
        == Calendar.current.component(.year, from: now)
    let out = DateFormatter()
    out.timeZone = .current
    out.setLocalizedDateFormatFromTemplate(sameYear ? "MMMd" : "MMMdyyyy")
    return out.string(from: date)
}
