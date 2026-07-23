import SwiftUI
import UniformTypeIdentifiers

/// Chat screen — mirrors `ui/chat/ChatScreen.kt`.
///
/// Top bar (CenterAlignedTopAppBar): LEADING = account avatar that opens the nav
/// drawer; PRINCIPAL = the "Mirobody" brand wordmark; TRAILING = the settings gear
/// (app settings only). The provider/model picker lives in the composer, not the
/// center, so the brand owns the center. Everything session-scoped (history, health
/// connections, incognito, account, sign out) lives in the left nav drawer.
struct ChatView: View {
    @EnvironmentObject private var container: AppContainer
    @EnvironmentObject private var settings: SettingsStore
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    @StateObject private var vm: ChatViewModel

    @State private var showDrawer = false
    @State private var showLanguage = false
    @State private var showFontSize = false
    @State private var showBackend = false
    @State private var showAbout = false
    @State private var showFileImporter = false
    @State private var showOnDeviceModel = false

    // Auto-scroll follows the streaming reply only while the user is parked at the
    // bottom; a manual scroll up detaches it so they can re-read mid-reply without
    // being yanked back down. Re-arms when they return to the bottom or a new turn
    // starts. `viewportHeight` is the scroll area's height, compared against the
    // content's bottom to decide "near bottom".
    @State private var followTail = true
    @State private var viewportHeight: CGFloat = 0

    init(container: AppContainer) {
        _vm = StateObject(wrappedValue: ChatViewModel(
            repo: container.chatRepository,
            settings: container.settings,
            errorBus: container.errorBus,
            modelManager: container.modelManager
        ))
    }

    var body: some View {
        ZStack {
            NavigationStack {
                ZStack {
                    colors.background.ignoresSafeArea()
                    VStack(spacing: 0) {
                        messageList
                        if !vm.readOnly { inputBar }
                    }
                }
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .navigationBarLeading) { accountAvatar }
                    ToolbarItem(placement: .principal) { brand }
                    ToolbarItem(placement: .navigationBarTrailing) { settingsMenu }
                }
            }
            drawerOverlay
        }
        .animation(.easeInOut(duration: 0.25), value: showDrawer)
        .sheet(isPresented: $showLanguage) {
            sheetEnv { LanguageDialog(current: settings.language) { settings.setLanguage($0) } }
        }
        .sheet(isPresented: $showFontSize) { sheetEnv { FontSizeDialog() } }
        .sheet(isPresented: $showBackend) { sheetEnv { BaseUrlDialog() } }
        .sheet(isPresented: $showOnDeviceModel) {
            sheetEnv {
                OnDeviceModelView(
                    statuses: vm.onDeviceStatuses,
                    onDownload: vm.downloadOnDeviceModel,
                    onPause: vm.pauseOnDeviceModel,
                    onDelete: vm.deleteOnDeviceModel
                )
            }
        }
        .alert(L("chat_about", lang), isPresented: $showAbout) {
            Button(L("common_close", lang), role: .cancel) {}
        } message: {
            Text(L("app_name", lang) + "\n" + L("about_version", lang, appVersion))
        }
    }

    // MARK: Nav drawer overlay

    @ViewBuilder
    private var drawerOverlay: some View {
        if showDrawer {
            Color.black.opacity(0.4)
                .ignoresSafeArea()
                .onTapGesture { showDrawer = false }
                .transition(.opacity)
            HStack(spacing: 0) {
                NavDrawer(
                    container: container,
                    vm: vm,
                    onDismiss: { showDrawer = false },
                    onAddAccount: { settings.addingAccount = true }
                )
                .frame(maxWidth: drawerMaxWidth)
                .frame(width: drawerWidth)
                .ignoresSafeArea(edges: .bottom)
                Spacer(minLength: 0)
            }
            .transition(.move(edge: .leading))
        }
    }

    /// 85% of the screen, capped at `drawerMaxWidth` (mirrors the web/Android drawer).
    private var drawerWidth: CGFloat {
        min(UIScreen.main.bounds.width * 0.85, drawerMaxWidth)
    }

    // MARK: Top bar pieces

    private var accountAvatar: some View {
        Button { showDrawer = true } label: {
            ZStack {
                Circle().fill(colors.primary)
                if let initial = avatarInitial {
                    Text(initial).mbFont(.labelLarge).foregroundColor(colors.onPrimary)
                } else {
                    Image(systemName: "person").font(.system(size: 15)).foregroundColor(colors.onPrimary)
                }
            }
            .frame(width: 30, height: 30)
        }
        .accessibilityLabel(L("chat_menu_title", lang))
    }

    private var avatarInitial: String? {
        settings.currentEmail?.trimmingCharacters(in: .whitespaces).first.map { String($0).uppercased() }
    }

    /// Centered brand wordmark — the true CenterAlignedTopAppBar title.
    private var brand: some View {
        Text(L("app_name", lang))
            .font(.system(size: 20 * fontScale(forOffset: settings.fontSizeOffset),
                          weight: .semibold, design: .serif))
            .foregroundColor(colors.onSurface)
    }

    // MARK: Message list

    private var messageList: some View {
        Group {
            if vm.messages.isEmpty {
                emptyState
            } else {
                ScrollViewReader { proxy in
                    ScrollView {
                        if vm.incognito { incognitoBanner }
                        LazyVStack(spacing: 14) {
                            ForEach(vm.messages) { msg in
                                MessageBubble(message: msg).id(msg.id)
                            }
                        }
                        .padding(.horizontal, 16).padding(.vertical, 16)
                        .frame(maxWidth: contentMaxWidth)
                        .frame(maxWidth: .infinity)
                        // Report the content's bottom edge in the scroll viewport's
                        // coordinate space, so we can tell if the user is at the bottom.
                        .background(GeometryReader { geo in
                            Color.clear.preference(
                                key: BottomOffsetKey.self,
                                value: geo.frame(in: .named("chatScroll")).maxY)
                        })
                    }
                    .coordinateSpace(name: "chatScroll")
                    .background(GeometryReader { geo in
                        Color.clear
                            .onAppear { viewportHeight = geo.size.height }
                            .onChange(of: geo.size.height) { viewportHeight = $0 }
                    })
                    .onPreferenceChange(BottomOffsetKey.self) { maxY in
                        // Near the bottom when the content's bottom sits within ~150pt
                        // of the visible area's bottom edge (generous enough that a
                        // single streamed token's growth doesn't detach the follow).
                        followTail = maxY <= viewportHeight + 150
                    }
                    // Follow on ANY change to the last message, not just `text`: the
                    // turn also grows through the `thinking` trace, tool-call cards,
                    // images and charts. Keying on `text` alone missed the thinking
                    // phase, so a long thinking trace scrolled off-screen unfollowed.
                    .onChange(of: vm.messages.last) { _ in
                        if followTail { scrollToBottom(proxy) }
                    }
                    .onChange(of: vm.messages.count) { _ in
                        followTail = true
                        scrollToBottom(proxy)
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    @ViewBuilder
    private var emptyState: some View {
        if vm.incognito {
            // Privacy hero: ghost glyph + "You're incognito" + the not-saved note.
            VStack(spacing: 20) {
                Image(systemName: "eye.slash")
                    .font(.system(size: 52)).foregroundColor(colors.primary)
                LText("chat_incognito_heading").mbFont(.headlineSmall).foregroundColor(colors.onSurface)
                LText("chat_incognito_note").mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)
            }
            .multilineTextAlignment(.center)
            .padding(24)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            VStack(spacing: 6) {
                LText("chat_empty_title").mbFont(.titleMedium).foregroundColor(colors.onSurface)
                LText("chat_empty_subtitle").mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant)
            }
            .multilineTextAlignment(.center)
            .padding(24)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    /// Slim pill pinned above an incognito thread, reminding it won't be saved.
    private var incognitoBanner: some View {
        HStack(spacing: 8) {
            Image(systemName: "eye.slash").font(.system(size: 14)).foregroundColor(colors.primary)
            LText("chat_incognito_note").mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant)
        }
        .padding(.horizontal, 12).padding(.vertical, 6)
        .frame(maxWidth: contentMaxWidth)
        .background(colors.surfaceContainerLow)
        .clipShape(RoundedRectangle(cornerRadius: 10))
        .padding(.horizontal, 16).padding(.top, 8)
    }

    private func scrollToBottom(_ proxy: ScrollViewProxy) {
        guard let last = vm.messages.last else { return }
        withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
    }

    // MARK: Input bar (composer) — the provider picker lives here, not the top bar.

    private var inputBar: some View {
        VStack(spacing: 8) {
            if !vm.attachments.isEmpty { attachmentChips }
            VStack(spacing: 6) {
                TextField(L("chat_message_hint", lang), text: $vm.input, axis: .vertical)
                    .mbFont(.bodyLarge)
                    .foregroundColor(colors.onSurface)
                    .lineLimit(1...5)
                    .padding(.horizontal, 8).padding(.vertical, 6)
                    .disabled(vm.sending)
                    .onSubmit(vm.send)
                HStack(spacing: 4) {
                    Button { showFileImporter = true } label: {
                        Image(systemName: "paperclip")
                            .font(.system(size: 20)).foregroundColor(colors.onSurfaceVariant)
                    }
                    .disabled(vm.sending)
                    .accessibilityLabel(L("chat_attach_file", lang))
                    providerMenu.frame(maxWidth: .infinity)
                    Button(action: vm.send) {
                        Image(systemName: "arrow.up.circle.fill")
                            .font(.system(size: 30))
                            .foregroundColor(canSend ? colors.primary : colors.onSurfaceVariant.opacity(0.4))
                    }
                    .disabled(!canSend)
                }
            }
            .padding(.horizontal, 8).padding(.vertical, 6)
            .background(colors.surfaceContainerLow)
            .overlay(RoundedRectangle(cornerRadius: 24).stroke(colors.outlineVariant, lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 24))
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
        .frame(maxWidth: contentMaxWidth)
        .frame(maxWidth: .infinity)
        .background(colors.background)
        .fileImporter(
            isPresented: $showFileImporter,
            allowedContentTypes: [.item],
            allowsMultipleSelection: true
        ) { result in
            guard case .success(let urls) = result else { return }
            for url in urls {
                if let att = Self.readAttachment(url) { vm.addAttachment(att) }
            }
        }
    }

    /// Staged attachment chips above the composer, removable until the turn is sent.
    private var attachmentChips: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(vm.attachments) { att in
                    HStack(spacing: 6) {
                        Image(systemName: "doc")
                            .font(.system(size: 14)).foregroundColor(colors.onSurfaceVariant)
                        Text(att.fileName)
                            .mbFont(.bodySmall).foregroundColor(colors.onSurface).lineLimit(1)
                        Button { vm.removeAttachment(att.id) } label: {
                            Image(systemName: "xmark")
                                .font(.system(size: 11)).foregroundColor(colors.onSurfaceVariant)
                        }
                        .accessibilityLabel(L("chat_attach_remove", lang))
                    }
                    .padding(.leading, 10).padding(.trailing, 6).padding(.vertical, 6)
                    .background(colors.surfaceContainerLow)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                }
            }
            .padding(.horizontal, 2)
        }
    }

    /// Reads a picked file URL (security-scoped) into an in-memory attachment.
    private static func readAttachment(_ url: URL) -> ChatAttachment? {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url) else { return nil }
        let mime = UTType(filenameExtension: url.pathExtension)?.preferredMIMEType
            ?? "application/octet-stream"
        return ChatAttachment(fileName: url.lastPathComponent, mimeType: mime, data: data)
    }

    private var canSend: Bool {
        !vm.sending
            && (!vm.input.trimmingCharacters(in: .whitespaces).isEmpty || !vm.attachments.isEmpty)
            && vm.selected != nil
    }

    // MARK: Composer provider picker

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
                    Button {
                        if provider.isManageEntry {
                            showOnDeviceModel = true
                        } else {
                            vm.onProviderSelected(provider)
                        }
                    } label: {
                        if provider.isManageEntry {
                            Label(provider.label, systemImage: "slider.horizontal.3")
                        } else if provider.modelSpec != nil {
                            Label(provider.label, systemImage: "lock.fill")
                        } else {
                            Text(provider.label)
                        }
                    }
                }
            }
        } label: {
            HStack(spacing: 2) {
                Text(vm.selected?.label.nonBlank ?? L("chat_select_model", lang))
                    .mbFont(.titleSmall).foregroundColor(colors.onSurface).lineLimit(1)
                Image(systemName: "chevron.down").font(.system(size: 12)).foregroundColor(colors.onSurfaceVariant)
            }
        }
    }

    // MARK: Settings gear — app settings ONLY (identical to the login screen).

    private var settingsMenu: some View {
        Menu {
            Button(L("chat_language", lang)) { showLanguage = true }
            Button(L("chat_font_size", lang)) { showFontSize = true }
            Button(L("chat_backend", lang)) { showBackend = true }
            Button(L("chat_about", lang)) { showAbout = true }
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
/// Cancel restores the value captured on appear. Module-internal so the pre-auth
/// `LoginSettingsMenu` can reuse it too.
struct FontSizeDialog: View {
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

/// Tracks the chat content's bottom edge (in the scroll viewport's coordinate
/// space) so `ChatView` can follow a streaming reply only while the user is
/// parked at the bottom. Takes the last (deepest) reported value.
private struct BottomOffsetKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = nextValue()
    }
}
