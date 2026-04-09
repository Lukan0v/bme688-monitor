import SwiftUI

struct ContentView: View {
    @Environment(APIService.self) var api

    var body: some View {
        TabView {
            Tab("Dashboard", systemImage: "gauge.with.dots.needle.33percent") {
                DashboardView()
            }
            Tab("Wetter", systemImage: "cloud.sun.fill") {
                WeatherView()
            }
            Tab("Verlauf", systemImage: "chart.xyaxis.line") {
                HistoryView()
            }
            Tab("Einstellungen", systemImage: "gearshape.fill") {
                SettingsView()
            }
        }
        .tint(.blue)
    }
}
