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

    /// User-imported models (picked from the Files app, live OUTSIDE the app sandbox so
    /// they survive an app reinstall). Drives the imported section of the picker.
    @Published private(set) var imported: [OnDeviceModelSpec] = []

    /// Security-scoped URLs we hold open for the session (imported files), keyed by model
    /// id, so the engine can read them; released on `deleteImported` / `deinit`.
    private var scopedURLs: [String: URL] = [:]

    private let importsKey = "ondevice_imports"

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
        loadImports()
    }

    deinit {
        for url in scopedURLs.values { url.stopAccessingSecurityScopedResource() }
    }

    static var modelsDir: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("models", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    func fileURL(_ spec: OnDeviceModelSpec) -> URL {
        if let path = spec.localPath { return URL(fileURLWithPath: path) }
        return ModelManager.modelsDir.appendingPathComponent(spec.fileName)
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

    // MARK: - Import (a model file the user already has, from the Files app)

    /// Import the model at `pickedURL` (from the document picker) as a new on-device model.
    /// The file stays OUTSIDE the app sandbox — referenced in place via a security-scoped
    /// bookmark, so it survives an app reinstall and is never copied. Returns the new spec,
    /// or nil on failure. `pickedURL` is expected to be a security-scoped URL from SwiftUI's
    /// `.fileImporter`.
    func importModel(from pickedURL: URL) -> OnDeviceModelSpec? {
        let didScope = pickedURL.startAccessingSecurityScopedResource()
        // Persist a bookmark so we can re-resolve (and re-scope) after relaunch.
        guard let bookmark = try? pickedURL.bookmarkData(
            options: [], includingResourceValuesForKeys: nil, relativeTo: nil
        ) else {
            if didScope { pickedURL.stopAccessingSecurityScopedResource() }
            return nil
        }

        let attrs = try? FileManager.default.attributesOfItem(atPath: pickedURL.path)
        let size = (attrs?[.size] as? NSNumber)?.int64Value ?? 0
        let id = OnDeviceModel.importedIdPrefix + String(UUID().uuidString.prefix(8))
        let name = pickedURL.deletingPathExtension().lastPathComponent
        let label = name.isEmpty ? "Imported model" : name

        let spec = OnDeviceModel.importedSpec(
            id: id, displayName: label, path: pickedURL.path, sizeBytes: size
        )
        OnDeviceModel.registerImported(spec)
        scopedURLs[id] = pickedURL
        setStatus(spec, .ready)
        imported.append(spec)
        persistImports(appending: (id: id, name: label, bookmark: bookmark))
        return spec
    }

    /// Forget an imported model. The user's file on disk is left untouched — we only drop
    /// the bookmark and release the security-scoped access.
    func deleteImported(_ spec: OnDeviceModelSpec) {
        if let url = scopedURLs[spec.id] { url.stopAccessingSecurityScopedResource() }
        scopedURLs[spec.id] = nil
        OnDeviceModel.unregisterImported(spec.id)
        statuses[spec.id] = nil
        imported.removeAll { $0.id == spec.id }
        var stored = (UserDefaults.standard.array(forKey: importsKey) as? [[String: Any]]) ?? []
        stored.removeAll { ($0["id"] as? String) == spec.id }
        UserDefaults.standard.set(stored, forKey: importsKey)
    }

    private func persistImports(appending entry: (id: String, name: String, bookmark: Data)) {
        var stored = (UserDefaults.standard.array(forKey: importsKey) as? [[String: Any]]) ?? []
        stored.append(["id": entry.id, "name": entry.name, "bookmark": entry.bookmark])
        UserDefaults.standard.set(stored, forKey: importsKey)
    }

    /// Restore persisted imports: resolve each bookmark, re-open security-scoped access,
    /// and register the spec. Entries whose file has since disappeared are dropped.
    private func loadImports() {
        let stored = (UserDefaults.standard.array(forKey: importsKey) as? [[String: Any]]) ?? []
        var kept: [[String: Any]] = []
        for entry in stored {
            guard let id = entry["id"] as? String,
                  let name = entry["name"] as? String,
                  let bookmark = entry["bookmark"] as? Data else { continue }
            var stale = false
            guard let url = try? URL(
                resolvingBookmarkData: bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale
            ), url.startAccessingSecurityScopedResource(),
                  FileManager.default.fileExists(atPath: url.path) else { continue }

            let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
            let size = (attrs?[.size] as? NSNumber)?.int64Value ?? 0
            let spec = OnDeviceModel.importedSpec(id: id, displayName: name, path: url.path, sizeBytes: size)
            OnDeviceModel.registerImported(spec)
            scopedURLs[id] = url
            statuses[id] = .ready
            imported.append(spec)
            kept.append(entry)
        }
        if kept.count != stored.count { UserDefaults.standard.set(kept, forKey: importsKey) }
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
