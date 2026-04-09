import SwiftUI

struct ContentView: View {
    @Environment(APIService.self) var api

    var body: some View {
        TabView {
            DashboardView()
                .tabItem {
                    Label("Dashboard", systemImage: "gauge.with.dots.needle.33percent")
                }
            WeatherView()
                .tabItem {
                    Label("Wetter", systemImage: "cloud.sun.fill")
                }
            HistoryView()
                .tabItem {
                    Label("Verlauf", systemImage: "chart.xyaxis.line")
                }
            FFTSpectrumView()
                .tabItem {
                    Label("FFT", systemImage: "waveform.path.ecg")
                }
            SettingsView()
                .tabItem {
                    Label("Einstellungen", systemImage: "gearshape.fill")
                }
        }
        .tint(.blue)
    }
}
