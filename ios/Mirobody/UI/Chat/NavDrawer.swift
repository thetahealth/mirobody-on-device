import SwiftUI

/// Left navigation drawer — the app's ONLY menu, mirroring the web client's
/// `history.js` and Android's `ChatDrawer`. Top: a "New chat" / "Incognito" button
/// row. Middle (the only scrolling area): conversation history (tap to resume,
/// per-row delete). Pinned bottom, three groups: Health & data (Connect EHR,
/// Connected devices, Sync health data, Bluetooth devices), app settings, and the
/// account switcher + Sign out.
///
/// The app-settings group used to be a top-right gear on this screen and on the auth
/// screens. Folding it in here leaves one menu affordance instead of two, and lets the
/// auth screens open the same drawer narrowed to just that group.
///
/// Care circle is intentionally omitted: iOS has no care-circle feature yet, so it
/// is out of today's scope.
struct NavDrawer: View {
    @EnvironmentObject private var container: AppContainer
    @EnvironmentObject private var settings: SettingsStore
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    @ObservedObject var vm: ChatViewModel
    let onDismiss: () -> Void
    /// Show the login view over this session to add another account.
    let onAddAccount: () -> Void

    @StateObject private var historyVM: HistoryViewModel
    @State private var switcherOpen = false
    @State private var pendingDelete: SessionSummary?
    @State private var showEhr = false
    @State private var showVendors = false
    @State private var showHealth = false
    @State private var showBle = false
    @State private var showSignOut = false

    init(container: AppContainer, vm: ChatViewModel, onDismiss: @escaping () -> Void, onAddAccount: @escaping () -> Void) {
        _vm = ObservedObject(wrappedValue: vm)
        self.onDismiss = onDismiss
        self.onAddAccount = onAddAccount
        _historyVM = StateObject(wrappedValue: HistoryViewModel(
            repo: container.chatRepository,
            errorBus: container.errorBus,
            language: container.settings.language
        ))
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            topButtonRow
            historyList
            footer
        }
        .background(colors.background)
        .onAppear { historyVM.refresh() }
        .confirmationDialog(
            L("history_delete_confirm_title", lang),
            isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }),
            titleVisibility: .visible
        ) {
            Button(L("common_delete", lang), role: .destructive) {
                if let item = pendingDelete { historyVM.deleteHistory(sessionId: item.sessionId) }
                pendingDelete = nil
            }
            Button(L("common_cancel", lang), role: .cancel) { pendingDelete = nil }
        } message: {
            Text(L("history_delete_confirm_message", lang))
        }
        .alert(L("chat_sign_out_confirm_title", lang), isPresented: $showSignOut) {
            Button(L("chat_sign_out", lang), role: .destructive) {
                onDismiss()
                container.authRepository.signOut()
            }
            Button(L("common_cancel", lang), role: .cancel) {}
        } message: {
            Text(L("chat_sign_out_confirm_message", lang))
        }
        .sheet(isPresented: $showEhr) { sheetEnv { EhrView(container: container) } }
        .sheet(isPresented: $showVendors) { sheetEnv { VendorsView(container: container) } }
        .sheet(isPresented: $showHealth) { sheetEnv { HealthSyncView(container: container) } }
        .sheet(isPresented: $showBle) { sheetEnv { BleDeviceView(container: container) } }
    }

    // MARK: Header

    private var header: some View { DrawerHeader(onDismiss: onDismiss) }

    // MARK: New chat / Incognito

    private var topButtonRow: some View {
        HStack(spacing: 8) {
            drawerActionButton(titleKey: "chat_new_chat", active: false) {
                vm.newChat(); onDismiss()
            }
            drawerActionButton(titleKey: "chat_incognito_mode", active: vm.incognito) {
                vm.toggleIncognito(); onDismiss()
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 12)
    }

    private func drawerActionButton(titleKey: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            LText(titleKey)
                .mbFont(.bodyMedium)
                .foregroundColor(active ? colors.primary : colors.onSurface)
                .lineLimit(1)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(active ? colors.primary : colors.outlineVariant, lineWidth: 1))
        }
    }

    // MARK: History (the only scrolling area)

    private var historyList: some View {
        Group {
            if historyVM.loading && historyVM.items.isEmpty {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if let err = historyVM.error {
                VStack(spacing: 8) {
                    Text(err).mbFont(.bodyMedium).foregroundColor(colors.error).multilineTextAlignment(.center)
                    Button(L("common_retry", lang)) { historyVM.refresh() }.foregroundColor(colors.primary)
                }
                .padding(24).frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if historyVM.items.isEmpty {
                LText("history_empty").mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)
                    .padding(24).frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(historyVM.items) { item in
                            Divider().background(colors.outlineVariant.opacity(0.4)).padding(.horizontal, 20)
                            historyRow(item)
                        }
                    }
                    .padding(.vertical, 8)
                }
            }
        }
        .frame(maxHeight: .infinity)
    }

    private func historyRow(_ item: SessionSummary) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text(item.summary.nonBlank ?? item.sessionId.nonBlank ?? L("history_untitled", lang))
                    .mbFont(.bodyLarge).foregroundColor(colors.onSurface).lineLimit(2)
                if !item.owned && !item.sharedBy.isEmpty {
                    badge(L("chat_shared_by", lang, item.sharedBy), strong: true)
                } else if item.owned && item.sharedWithCount > 0 {
                    badge(L("chat_shared_with", lang, item.sharedWithCount), strong: false)
                }
                // "2 hours ago · 6 messages", the same subtitle the full history
                // screen shows -- one helper, so the two lists cannot drift apart.
                // Either half may be missing, so the separator is drawn only when both
                // are there rather than leaving a dangling "·".
                let subtitle = historySubtitle(item)
                if !subtitle.isEmpty {
                    Text(subtitle)
                        .mbFont(.labelSmall).foregroundColor(colors.onSurfaceVariant.opacity(0.6))
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
            .onTapGesture { vm.openConversation(sessionId: item.sessionId); onDismiss() }
            Button { pendingDelete = item } label: {
                Image(systemName: "trash").font(.system(size: 18)).foregroundColor(colors.error)
            }
            .frame(width: 40, height: 40)
            .accessibilityLabel(L("history_delete_cd", lang))
        }
        .padding(.leading, 20).padding(.trailing, 8).padding(.vertical, 10)
    }

    private func badge(_ text: String, strong: Bool) -> some View {
        Text(text)
            .mbFont(.labelSmall)
            .foregroundColor(strong ? colors.primary : colors.onSurfaceVariant)
            .padding(.horizontal, 7).padding(.vertical, 1)
            .background(strong ? colors.primaryContainer : colors.surfaceContainerLow)
            .clipShape(Capsule())
    }

    // MARK: Footer (pinned): Health & data + app settings + account + sign out

    /// Three groups, each behind a rule. Kept as separate computed views rather than
    /// one flat list: inlined it comes to exactly ten children, which is ViewBuilder's
    /// ceiling, so the next row anyone adds would fail to compile for a reason that
    /// has nothing to do with the row.
    private var footer: some View {
        VStack(spacing: 0) {
            groupRule
            healthGroup
            groupRule
            // App settings — what the top-bar gear used to hold. Same group the auth
            // screens' drawer shows on its own.
            AppSettingsSection()
            groupRule
            accountSwitcher
            signOutButton
        }
    }

    private var groupRule: some View {
        Divider().background(colors.outlineVariant.opacity(0.5))
    }

    /// These present modally OVER the drawer (like the web's modal-over-drawer); the
    /// drawer stays open behind them and returns on dismiss.
    private var healthGroup: some View {
        VStack(spacing: 0) {
            DrawerRow(titleKey: "chat_ehr") { showEhr = true }
            DrawerRow(titleKey: "chat_vendors") { showVendors = true }
            DrawerRow(titleKey: "chat_sync_health") { showHealth = true }
            DrawerRow(titleKey: "chat_bluetooth") { showBle = true }
        }
    }

    private var signOutButton: some View {
        Button { showSignOut = true } label: {
            LText("chat_sign_out").mbFont(.bodyLarge).foregroundColor(colors.error)
                .frame(maxWidth: .infinity, minHeight: 40)
        }
        .padding(.vertical, 6)
    }

    private var accountSwitcher: some View {
        VStack(spacing: 0) {
            Button { switcherOpen.toggle() } label: {
                HStack {
                    Text(settings.currentEmail?.nonBlank ?? L("chat_account", lang))
                        .mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant).lineLimit(1)
                    Spacer()
                    Image(systemName: switcherOpen ? "chevron.up" : "chevron.down")
                        .foregroundColor(colors.onSurfaceVariant)
                }
                .padding(.horizontal, 20).padding(.vertical, 12).contentShape(Rectangle())
            }
            if switcherOpen {
                ForEach(settings.accounts.filter { !$0.isCurrent }) { acc in
                    Button {
                        onDismiss()
                        settings.switchAccount(acc.sub)
                    } label: {
                        Text(acc.email.nonBlank ?? "#\(acc.sub.prefix(6))")
                            .mbFont(.bodySmall).foregroundColor(colors.onSurface).lineLimit(1)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.horizontal, 20).padding(.vertical, 10).contentShape(Rectangle())
                    }
                }
                Button {
                    onDismiss()
                    onAddAccount()
                } label: {
                    HStack(spacing: 10) {
                        Image(systemName: "plus").foregroundColor(colors.onSurfaceVariant)
                        LText("chat_add_account").mbFont(.bodySmall).foregroundColor(colors.onSurface)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 20).padding(.vertical, 10).contentShape(Rectangle())
                }
            }
        }
    }

    /// Re-injects the app environment onto presented sheets (mirrors ChatView's helper).
    @ViewBuilder
    private func sheetEnv<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        content()
            .environmentObject(container)
            .environmentObject(settings)
            .environment(\.mbLanguage, lang)
            .environment(\.mbFontScale, fontScale(forOffset: settings.fontSizeOffset))
    }
}
