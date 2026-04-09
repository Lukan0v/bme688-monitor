import Foundation
import Observation

@Observable
final class APIService {
    var liveData: LiveData?
    var historyData: [HistoryPoint] = []
    var isConnected = false
    var lastError: String?
    var isFetchingHistory = false

    var serverURL: String {
        didSet {
            UserDefaults.standard.set(serverURL, forKey: "serverURL")
            isConnected = false
            liveData = nil
        }
    }

    private var pollingTask: Task<Void, Never>?

    init() {
        serverURL = UserDefaults.standard.string(forKey: "serverURL") ?? "http://raspberrypi.local:8080"
    }

    // MARK: - Polling

    func startPolling() {
        stopPolling()
        pollingTask = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                await self?.fetchLive()
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    func stopPolling() {
        pollingTask?.cancel()
        pollingTask = nil
    }

    // MARK: - Live data

    @MainActor
    func fetchLive() async {
        guard let url = URL(string: "\(serverURL)/api/live") else {
            lastError = "Ungueltige URL"
            isConnected = false
            return
        }

        do {
            var request = URLRequest(url: url, timeoutInterval: 5)
            request.cachePolicy = .reloadIgnoringLocalCacheData
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                isConnected = false
                lastError = "Server antwortet nicht"
                return
            }
            liveData = try JSONDecoder().decode(LiveData.self, from: data)
            isConnected = true
            lastError = nil
        } catch is CancellationError {
            // Task cancelled, ignore
        } catch {
            isConnected = false
            lastError = error.localizedDescription
        }
    }

    // MARK: - History

    @MainActor
    func fetchHistory(days: Int = 1) async {
        isFetchingHistory = true
        defer { isFetchingHistory = false }

        guard let url = URL(string: "\(serverURL)/api/history?days=\(days)") else { return }

        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            historyData = try JSONDecoder().decode([HistoryPoint].self, from: data)
        } catch {
            historyData = []
        }
    }

    // MARK: - Settings

    @MainActor
    func fetchSettings() async -> EditableSettings? {
        guard let url = URL(string: "\(serverURL)/api/settings") else { return nil }

        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            let raw = try JSONDecoder().decode(RawSettings.self, from: data)
            return EditableSettings(from: raw)
        } catch {
            return nil
        }
    }

    @MainActor
    func saveSettings(_ settings: EditableSettings) async -> Bool {
        guard let url = URL(string: "\(serverURL)/api/settings") else { return false }

        do {
            var request = URLRequest(url: url)
            request.httpMethod = "POST"
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try JSONSerialization.data(withJSONObject: settings.toDict())
            let (data, _) = try await URLSession.shared.data(for: request)
            if let result = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               result["ok"] as? Bool == true {
                return true
            }
            return false
        } catch {
            return false
        }
    }
}
