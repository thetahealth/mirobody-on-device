import Foundation
import Combine

/// Owns the on-device model files: where each lives, whether it's present, and the
/// download (with pause/resume + progress) — for every model in `OnDeviceModel.catalog`.
/// Independent of the user's configured server; downloads go straight to Hugging Face.
/// Several models can coexist on disk; the user picks which to run. Mirrors Android's
/// `data/llm/ModelManager.kt`.
///
/// Uses background `URLSessionDownloadTask`s; `@Published statuses` is always mutated on
/// the main queue so SwiftUI observers update safely.
final class ModelManager: NSObject, ObservableObject {

    /// Per-model status, keyed by `OnDeviceModelSpec.id`. Seeded from what's on disk.
    @Published private(set) var statuses: [String: OnDeviceModelStatus] = [:]

    /// Live download tasks, keyed by model id.
    private var tasks: [String: URLSessionDownloadTask] = [:]
    /// Resume data captured on pause/cancel, keyed by model id.
    private var resumeData: [String: Data] = [:]
    /// Maps a URLSession task back to the model id it's downloading.
    private var idByTaskId: [Int: String] = [:]

    private lazy var session: URLSession = {
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 60
        // A multi-GB transfer must not be killed by the resource timeout.
        config.timeoutIntervalForResource = 7 * 24 * 60 * 60
        config.waitsForConnectivity = true
        return URLSession(configuration: config, delegate: self, delegateQueue: nil)
    }()

    override init() {
        super.init()
        var seeded: [String: OnDeviceModelStatus] = [:]
        for spec in OnDeviceModel.catalog {
            seeded[spec.id] = isReady(spec) ? .ready : .absent
        }
        statuses = seeded
    }

    static var modelsDir: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("models", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    func fileURL(_ spec: OnDeviceModelSpec) -> URL {
        ModelManager.modelsDir.appendingPathComponent(spec.fileName)
    }

    func isReady(_ spec: OnDeviceModelSpec) -> Bool {
        FileManager.default.fileExists(atPath: fileURL(spec).path)
    }

    func status(_ spec: OnDeviceModelSpec) -> OnDeviceModelStatus {
        statuses[spec.id] ?? .absent
    }

    /// Start (or resume) the download of `spec`.
    func startDownload(_ spec: OnDeviceModelSpec) {
        guard !isReady(spec) else { setStatus(spec, .ready); return }
        guard tasks[spec.id] == nil else { return } // already running
        let newTask: URLSessionDownloadTask
        if let data = resumeData[spec.id] {
            newTask = session.downloadTask(withResumeData: data)
        } else {
            newTask = session.downloadTask(with: spec.downloadURL)
        }
        tasks[spec.id] = newTask
        idByTaskId[newTask.taskIdentifier] = spec.id
        resumeData[spec.id] = nil
        setStatus(spec, .downloading(downloadedBytes: 0, totalBytes: spec.approxBytes))
        newTask.resume()
    }

    /// Pause `spec`'s download, keeping resume data so `startDownload` continues from there.
    func pauseDownload(_ spec: OnDeviceModelSpec) {
        tasks[spec.id]?.cancel { [weak self] data in
            DispatchQueue.main.async {
                self?.resumeData[spec.id] = data
                self?.tasks[spec.id] = nil
            }
        }
    }

    /// Remove `spec` (and reset state) to reclaim its storage.
    func delete(_ spec: OnDeviceModelSpec) {
        tasks[spec.id]?.cancel()
        tasks[spec.id] = nil
        resumeData[spec.id] = nil
        try? FileManager.default.removeItem(at: fileURL(spec))
        setStatus(spec, .absent)
    }

    private func setStatus(_ spec: OnDeviceModelSpec, _ new: OnDeviceModelStatus) {
        if Thread.isMainThread { statuses[spec.id] = new }
        else { DispatchQueue.main.async { self.statuses[spec.id] = new } }
    }

    /// Resolve the spec a delegate callback belongs to, via its task identifier.
    private func spec(for task: URLSessionTask) -> OnDeviceModelSpec? {
        guard let id = idByTaskId[task.taskIdentifier] else { return nil }
        return OnDeviceModel.byId(id)
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
        guard let spec = spec(for: downloadTask) else { return }
        let total = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : spec.approxBytes
        setStatus(spec, .downloading(downloadedBytes: totalBytesWritten, totalBytes: total))
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didFinishDownloadingTo location: URL
    ) {
        guard let spec = spec(for: downloadTask) else { return }
        // The temp file is deleted when this returns; move it to its final home now.
        let dest = fileURL(spec)
        try? FileManager.default.removeItem(at: dest)
        do {
            try FileManager.default.moveItem(at: location, to: dest)
            DispatchQueue.main.async { self.tasks[spec.id] = nil }
            setStatus(spec, .ready)
        } catch {
            setStatus(spec, .failed("Could not finalize model file: \(error.localizedDescription)"))
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let spec = spec(for: task) else { return }
        DispatchQueue.main.async { self.idByTaskId[task.taskIdentifier] = nil }
        guard let error = error as NSError? else { return } // success handled above
        // A user-initiated pause produces resume data and is not a failure.
        if error.code == NSURLErrorCancelled { return }
        DispatchQueue.main.async { self.tasks[spec.id] = nil }
        setStatus(spec, .failed(error.localizedDescription))
    }
}
