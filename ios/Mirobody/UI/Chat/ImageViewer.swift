import SwiftUI
import Photos

/// Full-screen image viewer with pinch/double-tap zoom and save-to-Photos — mirrors
/// `ImageViewerDialog.kt`. Presented as a full-screen cover from the bubble.
struct ImageViewer: View {
    let content: String
    let onDismiss: () -> Void

    @Environment(\.mbLanguage) private var lang
    @State private var scale: CGFloat = 1
    @State private var lastScale: CGFloat = 1
    @State private var offset: CGSize = .zero
    @State private var toast: String?

    private let minScale: CGFloat = 1
    private let maxScale: CGFloat = 5

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            imageContent
                .scaleEffect(scale)
                .offset(offset)
                .gesture(
                    MagnificationGesture()
                        .onChanged { value in
                            scale = min(max(lastScale * value, minScale), maxScale)
                        }
                        .onEnded { _ in
                            lastScale = scale
                            if scale <= 1 { offset = .zero }
                        }
                )
                .simultaneousGesture(
                    DragGesture()
                        .onChanged { value in
                            if scale > 1 { offset = value.translation }
                        }
                )
                .onTapGesture(count: 2) {
                    withAnimation {
                        if scale > 1 { scale = 1; lastScale = 1; offset = .zero }
                        else { scale = 2.5; lastScale = 2.5 }
                    }
                }

            VStack {
                HStack {
                    Button(action: onDismiss) {
                        Image(systemName: "xmark").foregroundColor(.white).padding(12)
                    }
                    Spacer()
                    if !isRawSVG(content) {
                        Button(action: save) {
                            Image(systemName: "square.and.arrow.down").foregroundColor(.white).padding(12)
                        }
                    }
                }
                Spacer()
                if let toast {
                    Text(toast)
                        .mbFont(.bodyMedium)
                        .foregroundColor(.white)
                        .padding()
                        .background(Color.black.opacity(0.6))
                        .clipShape(Capsule())
                        .padding(.bottom, 24)
                }
            }
        }
    }

    @ViewBuilder
    private var imageContent: some View {
        if isRawSVG(content) {
            SVGWebView(svg: content)
        } else if let url = URL(string: content) {
            AsyncImage(url: url) { image in
                image.resizable().scaledToFit()
            } placeholder: {
                ProgressView().tint(.white)
            }
        }
    }

    private func save() {
        guard let url = URL(string: content) else { return }
        Task {
            do {
                let (data, _) = try await URLSession.shared.data(from: url)
                guard let image = UIImage(data: data) else { throw URLError(.cannotDecodeContentData) }
                // Completion-based performChanges wrapped in a continuation — present
                // on all supported SDKs. Triggers the add-only Photos permission prompt
                // (NSPhotoLibraryAddUsageDescription).
                try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
                    PHPhotoLibrary.shared().performChanges({
                        PHAssetChangeRequest.creationRequestForAsset(from: image)
                    }, completionHandler: { success, error in
                        if let error { cont.resume(throwing: error) }
                        else if success { cont.resume(returning: ()) }
                        else { cont.resume(throwing: URLError(.unknown)) }
                    })
                }
                toast = L("chat_image_saved", lang)
            } catch {
                toast = L("chat_image_save_failed", lang)
            }
        }
    }
}
