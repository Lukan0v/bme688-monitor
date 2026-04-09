import SwiftUI

struct DashboardView: View {
    @Environment(APIService.self) var api

    let columns = [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    // Connection status
                    connectionBanner

                    if let data = api.liveData {
                        // Sensor cards
                        LazyVGrid(columns: columns, spacing: 12) {
                            SensorCardView(
                                title: "Temperatur",
                                value: data.current.temperature,
                                unit: "\u{00B0}C",
                                format: "%.1f",
                                trendPerSec: data.trends.temperature,
                                trendUnit: "\u{00B0}C/min",
                                trendThreshold: 0.001,
                                trendMultiplier: 60,
                                color: .orange,
                                icon: "thermometer.medium"
                            )
                            SensorCardView(
                                title: "Luftfeuchtigkeit",
                                value: data.current.humidity,
                                unit: "%",
                                format: "%.1f",
                                trendPerSec: data.trends.humidity,
                                trendUnit: "%/min",
                                trendThreshold: 0.005,
                                trendMultiplier: 60,
                                color: .blue,
                                icon: "humidity.fill"
                            )
                            SensorCardView(
                                title: "Luftdruck",
                                value: data.current.pressure,
                                unit: "hPa",
                                format: "%.1f",
                                trendPerSec: data.trends.pressure,
                                trendUnit: "hPa/h",
                                trendThreshold: 0.0005,
                                trendMultiplier: 3600,
                                color: .green,
                                icon: "gauge.with.needle"
                            )
                            SensorCardView(
                                title: "Luftqualitaet",
                                value: data.current.eco2,
                                unit: "ppm",
                                format: "%.0f",
                                trendPerSec: 0,
                                trendUnit: "",
                                trendThreshold: 1,
                                trendMultiplier: 1,
                                color: .yellow,
                                icon: "aqi.medium"
                            )
                        }

                        // IAQ + Gas info
                        HStack(spacing: 12) {
                            miniInfo(icon: "leaf.fill", label: "IAQ",
                                     value: String(format: "%.0f", data.current.iaq),
                                     color: iaqColor(data.current.iaq))
                            miniInfo(icon: "flame.fill", label: "Gas",
                                     value: String(format: "%.0f k\u{03A9}", data.current.gasResistance),
                                     color: .purple)
                            miniInfo(icon: "clock", label: "Update",
                                     value: formatTime(data.timestamp),
                                     color: .secondary)
                        }

                        // Live chart
                        LiveChartView(history: data.history)
                    } else {
                        noDataView
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 20)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Home Monitor")
            .refreshable {
                await api.fetchLive()
            }
        }
    }

    // MARK: - Subviews

    @ViewBuilder
    private var connectionBanner: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(statusColor)
                .frame(width: 8, height: 8)
            Text(statusText)
                .font(.caption)
                .fontWeight(.medium)
            Spacer()
            if let data = api.liveData, data.demoMode {
                Text("DEMO")
                    .font(.caption2)
                    .fontWeight(.bold)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Capsule().fill(.orange.opacity(0.2)))
                    .foregroundStyle(.orange)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(statusColor.opacity(0.1))
        }
    }

    private var noDataView: some View {
        ContentUnavailableView {
            Label("Keine Verbindung", systemImage: "wifi.slash")
        } description: {
            Text(api.lastError ?? "Verbinde mit \(api.serverURL)...")
        }
        .frame(minHeight: 300)
    }

    private func miniInfo(icon: String, label: String, value: String, color: Color) -> some View {
        VStack(spacing: 4) {
            Image(systemName: icon)
                .font(.caption)
                .foregroundStyle(color)
            Text(value)
                .font(.caption)
                .fontWeight(.semibold)
                .monospacedDigit()
            Text(label)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 10)
        .background {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(.ultraThinMaterial)
        }
    }

    // MARK: - Helpers

    private var statusColor: Color {
        if !api.isConnected { return .red }
        if api.liveData?.demoMode == true { return .orange }
        return .green
    }

    private var statusText: String {
        if !api.isConnected { return "Offline" }
        if api.liveData?.demoMode == true { return "Demo-Modus" }
        if api.liveData?.sensorOk == false { return "Sensor-Fehler" }
        return "Verbunden"
    }

    private func iaqColor(_ value: Double) -> Color {
        if value <= 50 { return .green }
        if value <= 100 { return .yellow }
        if value <= 200 { return .orange }
        return .red
    }

    private func formatTime(_ timestamp: String) -> String {
        // "2026-04-09T14:23:05" -> "14:23"
        let parts = timestamp.split(separator: "T")
        if parts.count == 2 {
            let time = parts[1].prefix(5)
            return String(time)
        }
        return "--:--"
    }
}
