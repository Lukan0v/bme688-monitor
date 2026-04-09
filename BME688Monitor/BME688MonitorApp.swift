import SwiftUI

@main
struct BME688MonitorApp: App {
    @State private var apiService = APIService()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(apiService)
                .onAppear { apiService.startPolling() }
                .onDisappear { apiService.stopPolling() }
        }
    }
}
