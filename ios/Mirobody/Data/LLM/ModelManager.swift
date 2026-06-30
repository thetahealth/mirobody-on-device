import Foundation
import Combine

/// Owns the on-device model file: where it lives, whether it's present, and the
/// download (with pause/resume + progress). Independent of the user's configured
/// server — the download goes straight to Hugging Face. Mirrors Android's
/// `data/llm/ModelManager.kt`.
///
/// Uses a background `URLSessionDownloadTask`; `@Published status` is always mutated
/// on the main queue so SwiftUI observers update safely.
final class ModelManager: NSObject, ObservableObject {

    @Published private(set) var status: OnDeviceModelStatus

    private var task: URLSessionDownloadTask?
    /// Captured when a download is paused/cancelled, to resume from the partial bytes.
    private var resumeData: Data?

    private lazy var session: URLSession = {
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 60
        // A multi-GB transfer must not be killed by the resource timeout.
        config.timeoutIntervalForResource = 7 * 24 * 60 * 60
        config.waitsForConnectivity = true
        return URLSession(configuration: config, delegate: self, delegateQueue: nil)
    }()

    override init() {
        let exists = FileManager.default.fileExists(atPath: ModelManager.modelURL.path)
        status = exists ? .ready : .absent
        super.init()
    }

    static var modelsDir: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("models", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    static var modelURL: URL { modelsDir.appendingPathComponent(OnDeviceModel.fileName) }

    var isReady: Bool { FileManager.default.fileExists(atPath: ModelManager.modelURL.path) }

    /// Start (or resume) the download.
    func startDownload() {
        guard !isReady else { setStatus(.ready); return }
        guard task == nil else { return } // already running
        let newTask: URLSessionDownloadTask
        if let resumeData {
            newTask = session.downloadTask(withResumeData: resumeData)
        } else {
            newTask = session.downloadTask(with: OnDeviceModel.downloadURL)
        }
        task = newTask
        resumeData = nil
        setStatus(.downloading(downloadedBytes: 0, totalBytes: OnDeviceModel.approxBytes))
        newTask.resume()
    }

    /// Pause the download, keeping resume data so `startDownload()` continues from there.
    func pauseDownload() {
        task?.cancel { [weak self] data in
            DispatchQueue.main.async {
                self?.resumeData = data
                self?.task = nil
            }
        }
    }

    /// Remove the model (and reset state) to reclaim ~2.5 GB.
    func delete() {
        task?.cancel()
        task = nil
        resumeData = nil
        try? FileManager.default.removeItem(at: ModelManager.modelURL)
        setStatus(.absent)
    }

    private func setStatus(_ new: OnDeviceModelStatus) {
        if Thread.isMainThread { status = new }
        else { DispatchQueue.main.async { self.status = new } }
    }
}

extension ModelManager: URLSessionDownloadDelegate {
    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didWriteData bytesWritten: Int64,
        totalBytesWritten: Int64,
        totalBytesExpectedToWrite: Int64
    ) {
        let total = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : OnDeviceModel.approxBytes
        setStatus(.downloading(downloadedBytes: totalBytesWritten, totalBytes: total))
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didFinishDownloadingTo location: URL
    ) {
        // The temp file is deleted when this returns; move it to its final home now.
        let dest = ModelManager.modelURL
        try? FileManager.default.removeItem(at: dest)
        do {
            try FileManager.default.moveItem(at: location, to: dest)
            DispatchQueue.main.async { self.task = nil }
            setStatus(.ready)
        } catch {
            setStatus(.failed("Could not finalize model file: \(error.localizedDescription)"))
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error = error as NSError? else { return } // success handled above
        // A user-initiated pause produces resume data and is not a failure.
        if error.code == NSURLErrorCancelled { return }
        DispatchQueue.main.async { self.task = nil }
        setStatus(.failed(error.localizedDescription))
    }
}
