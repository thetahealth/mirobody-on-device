import SwiftUI

/// Chat screen — mirrors `ui/chat/ChatScreen.kt`. History is presented as a sheet
/// (the iOS-idiomatic stand-in for Android's modal navigation drawer).
struct ChatView: View {
    @EnvironmentObject private var container: AppContainer
    @EnvironmentObject private var settings: SettingsStore
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    @StateObject private var vm: ChatViewModel

    @State private var showHistory = false
    @State private var showLanguage = false
    @State private var showFontSize = false
    @State private var showBackend = false
    @State private var showHealth = false
    @State private var showAbout = false
    @State private var showSignOut = false

    init(container: AppContainer) {
        _vm = StateObject(wrappedValue: ChatViewModel(
            repo: container.chatRepository,
            settings: container.settings,
            errorBus: container.errorBus
        ))
    }

    var body: some View {
        NavigationStack {
            ZStack {
                colors.background.ignoresSafeArea()
                VStack(spacing: 0) {
                    messageList
                    inputBar
                }
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button { showHistory = true } label: {
                        Image(systemName: "clock.arrow.circlepath").foregroundColor(colors.onSurfaceVariant)
                    }
                    .accessibilityLabel(L("chat_history_cd", lang))
                }
                ToolbarItem(placement: .principal) { providerMenu }
                ToolbarItem(placement: .navigationBarTrailing) { settingsMenu }
            }
        }
        .sheet(isPresented: $showHistory) { sheetEnv { HistoryView(container: container) } }
        .sheet(isPresented: $showLanguage) {
            sheetEnv { LanguageDialog(current: settings.language) { settings.setLanguage($0) } }
        }
        .sheet(isPresented: $showFontSize) { sheetEnv { FontSizeDialog() } }
        .sheet(isPresented: $showBackend) { sheetEnv { BaseUrlDialog() } }
        .sheet(isPresented: $showHealth) { sheetEnv { HealthSyncView(container: container) } }
        .alert(L("chat_about", lang), isPresented: $showAbout) {
            Button(L("common_close", lang), role: .cancel) {}
        } message: {
            Text(L("app_name", lang) + "\n" + L("about_version", lang, appVersion))
        }
        .alert(L("chat_sign_out_confirm_title", lang), isPresented: $showSignOut) {
            Button(L("chat_sign_out", lang), role: .destructive) { container.authRepository.signOut() }
            Button(L("common_cancel", lang), role: .cancel) {}
        } message: {
            Text(L("chat_sign_out_confirm_message", lang))
        }
    }

    // MARK: Message list

    private var messageList: some View {
        Group {
            if vm.messages.isEmpty {
                VStack(spacing: 6) {
                    LText("chat_empty_title").mbFont(.titleMedium).foregroundColor(colors.onSurface)
                    LText("chat_empty_subtitle").mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant)
                }
                .multilineTextAlignment(.center)
                .padding(24)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(spacing: 14) {
                            ForEach(vm.messages) { msg in
                                MessageBubble(message: msg).id(msg.id)
                            }
                        }
                        .padding(.horizontal, 16).padding(.vertical, 16)
                        .frame(maxWidth: contentMaxWidth)
                        .frame(maxWidth: .infinity)
                    }
                    .onChange(of: vm.messages.last?.text) { _ in scrollToBottom(proxy) }
                    .onChange(of: vm.messages.count) { _ in scrollToBottom(proxy) }
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func scrollToBottom(_ proxy: ScrollViewProxy) {
        guard let last = vm.messages.last else { return }
        withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
    }

    // MARK: Input bar

    private var inputBar: some View {
        HStack(alignment: .bottom, spacing: 8) {
            TextField(L("chat_message_hint", lang), text: $vm.input, axis: .vertical)
                .mbFont(.bodyLarge)
                .foregroundColor(colors.onSurface)
                .lineLimit(1...5)
                .padding(.horizontal, 14).padding(.vertical, 10)
                .background(colors.surfaceContainerLow)
                .clipShape(RoundedRectangle(cornerRadius: 16))
                .disabled(vm.sending)
                .onSubmit(vm.send)
            Button(action: vm.send) {
                Image(systemName: "arrow.up.circle.fill")
                    .font(.system(size: 30))
                    .foregroundColor(canSend ? colors.primary : colors.onSurfaceVariant.opacity(0.4))
            }
            .disabled(!canSend)
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
        .frame(maxWidth: contentMaxWidth)
        .frame(maxWidth: .infinity)
        .background(colors.background)
    }

    private var canSend: Bool {
        !vm.sending && !vm.input.trimmingCharacters(in: .whitespaces).isEmpty && vm.selected != nil
    }

    // MARK: Toolbar menus

    private var providerMenu: some View {
        Menu {
            if vm.providers.isEmpty {
                if vm.error != nil {
                    Button(L("common_retry", lang)) { vm.loadProviders() }
                } else {
                    Text(L("chat_no_providers", lang))
                }
            } else {
                ForEach(vm.providers) { provider in
                    Button(provider.name) { vm.onProviderSelected(provider) }
                }
            }
        } label: {
            HStack(spacing: 2) {
                Text(vm.selected?.name.nonBlank ?? L("chat_select_model", lang))
                    .mbFont(.titleSmall).foregroundColor(colors.onSurface)
                Image(systemName: "chevron.down").font(.system(size: 12)).foregroundColor(colors.onSurfaceVariant)
            }
        }
    }

    private var settingsMenu: some View {
        Menu {
            Button(L("chat_language", lang)) { showLanguage = true }
            Button(L("chat_font_size", lang)) { showFontSize = true }
            Button(L("chat_backend", lang)) { showBackend = true }
            // English literal for now; localize via the .lproj tables when wiring i18n.
            Button("Sync health data") { showHealth = true }
            Button(L("chat_about", lang)) { showAbout = true }
            Divider()
            Button(role: .destructive) { showSignOut = true } label: { Text(L("chat_sign_out", lang)) }
        } label: {
            Image(systemName: "gearshape").foregroundColor(colors.onSurfaceVariant)
        }
    }

    private var appVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
    }

    /// Re-injects the app environment onto presented sheets. SwiftUI inherits the
    /// environment into sheets, but doing this explicitly is cheap insurance for the
    /// `@EnvironmentObject`s the settings dialogs rely on.
    @ViewBuilder
    private func sheetEnv<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        content()
            .environmentObject(container)
            .environmentObject(settings)
            .environment(\.mbLanguage, lang)
            .environment(\.mbFontScale, fontScale(forOffset: settings.fontSizeOffset))
    }
}

/// Font-size picker with live preview — mirrors `FontSizeDialog` in ChatScreen.kt.
/// Preview is applied immediately by writing the offset (which drives `mbFontScale`);
/// Cancel restores the value captured on appear.
private struct FontSizeDialog: View {
    @EnvironmentObject private var settings: SettingsStore
    @Environment(\.mbColors) private var colors
    @Environment(\.dismiss) private var dismiss

    private let tiers: [(offset: Int, labelKey: String)] = [
        (-4, "chat_font_size_smaller"),
        (-2, "chat_font_size_small"),
        (0, "chat_font_size_normal"),
        (2, "chat_font_size_large"),
        (4, "chat_font_size_larger"),
    ]
    @State private var stagedIndex = 2
    @State private var original = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            LText("chat_font_size").mbFont(.titleMedium).foregroundColor(colors.onSurface)
            Slider(
                value: Binding(
                    get: { Double(stagedIndex) },
                    set: { newValue in
                        let idx = min(max(Int(newValue.rounded()), 0), tiers.count - 1)
                        if idx != stagedIndex {
                            stagedIndex = idx
                            settings.setFontSizeOffset(tiers[idx].offset)
                        }
                    }
                ),
                in: 0...Double(tiers.count - 1),
                step: 1
            )
            HStack {
                ForEach(Array(tiers.enumerated()), id: \.offset) { index, tier in
                    LText(tier.labelKey)
                        .mbFont(.labelSmall)
                        .foregroundColor(index == stagedIndex ? colors.primary : colors.onSurfaceVariant)
                    if index < tiers.count - 1 { Spacer() }
                }
            }
            HStack {
                Button(L("common_cancel", currentLanguage)) {
                    settings.setFontSizeOffset(original)
                    dismiss()
                }
                Spacer()
                Button(L("common_done", currentLanguage)) { dismiss() }
            }
            .foregroundColor(colors.primary)
        }
        .padding(24)
        .presentationDetents([.height(220)])
        .onAppear {
            original = settings.fontSizeOffset
            stagedIndex = tiers.firstIndex { $0.offset == original } ?? 2
        }
    }

    private var currentLanguage: String { settings.language }
}
