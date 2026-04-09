import SwiftUI

struct WeatherView: View {
    @Environment(APIService.self) var api

    var body: some View {
        NavigationStack {
            ScrollView {
                if let data = api.liveData {
                    VStack(spacing: 16) {
                        // JETZT + PROGNOSE
                        HStack(spacing: 12) {
                            weatherCard(
                                title: "JETZT",
                                entry: data.weather.jetzt
                            )
                            weatherCard(
                                title: "PROGNOSE",
                                entry: data.weather.prognose
                            )
                        }

                        // Detail cards
                        LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 12) {
                            detailCard(
                                icon: "gauge.with.needle",
                                label: "Luftdruck",
                                value: String(format: "%.1f", data.current.pressure),
                                unit: "hPa",
                                trend: data.trends.pressure,
                                trendUnit: "hPa/h",
                                trendMul: 3600,
                                trendThresh: 0.0005,
                                color: .green
                            )
                            detailCard(
                                icon: "humidity.fill",
                                label: "Feuchte",
                                value: String(format: "%.1f", data.current.humidity),
                                unit: "%",
                                trend: data.trends.humidity,
                                trendUnit: "%/min",
                                trendMul: 60,
                                trendThresh: 0.005,
                                color: .blue
                            )
                            detailCard(
                                icon: "thermometer.medium",
                                label: "Temperatur",
                                value: String(format: "%.1f", data.current.temperature),
                                unit: "\u{00B0}C",
                                trend: data.trends.temperature,
                                trendUnit: "\u{00B0}C/min",
                                trendMul: 60,
                                trendThresh: 0.001,
                                color: .orange
                            )
                        }

                        // Status info
                        if data.demoMode {
                            HStack {
                                Image(systemName: "exclamationmark.triangle.fill")
                                    .foregroundStyle(.orange)
                                Text("Demo-Modus aktiv \u{2013} keine echten Sensordaten")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            .padding(12)
                            .background {
                                RoundedRectangle(cornerRadius: 12, style: .continuous)
                                    .fill(.orange.opacity(0.1))
                            }
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.bottom, 20)
                } else {
                    ContentUnavailableView {
                        Label("Keine Wetterdaten", systemImage: "cloud.slash")
                    } description: {
                        Text("Warte auf Verbindung zum Sensor...")
                    }
                    .frame(minHeight: 400)
                }
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Wetter")
            .refreshable {
                await api.fetchLive()
            }
        }
    }

    // MARK: - Weather card

    private func weatherCard(title: String, entry: WeatherEntry) -> some View {
        VStack(spacing: 12) {
            Text(title)
                .font(.caption)
                .fontWeight(.bold)
                .tracking(2)
                .foregroundStyle(.secondary)

            WeatherIconView(icon: entry.icon, size: 80)

            Text(entry.text)
                .font(.headline)
                .fontWeight(.semibold)
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .minimumScaleFactor(0.7)

            if !entry.detail.isEmpty {
                Text(entry.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .lineLimit(3)
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, minHeight: 220)
        .background {
            RoundedRectangle(cornerRadius: 20, style: .continuous)
                .fill(.ultraThinMaterial)
                .overlay(
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .strokeBorder(.quaternary, lineWidth: 0.5)
                )
        }
    }

    // MARK: - Detail card

    private func detailCard(icon: String, label: String, value: String, unit: String,
                            trend: Double, trendUnit: String, trendMul: Double,
                            trendThresh: Double, color: Color) -> some View {
        VStack(spacing: 6) {
            Image(systemName: icon)
                .font(.title3)
                .foregroundStyle(color)

            HStack(alignment: .firstTextBaseline, spacing: 2) {
                Text(value)
                    .font(.system(.title2, design: .rounded, weight: .bold))
                    .monospacedDigit()
                Text(unit)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .fixedSize()
            .minimumScaleFactor(0.6)

            let dir = trendDirection(trend, threshold: trendThresh)
            HStack(spacing: 3) {
                Image(systemName: dir.sfSymbol)
                    .font(.caption2)
                Text(String(format: "%.2f", trend * trendMul))
                    .font(.caption2)
                    .monospacedDigit()
            }
            .foregroundStyle(trendDisplayColor(dir))

            Text(label)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 14)
        .padding(.horizontal, 8)
        .frame(maxWidth: .infinity)
        .background {
            RoundedRectangle(cornerRadius: 16, style: .continuous)
                .fill(color.opacity(0.06))
                .overlay(
                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .strokeBorder(color.opacity(0.12), lineWidth: 0.5)
                )
        }
    }

    private func trendDisplayColor(_ dir: TrendDirection) -> Color {
        switch dir {
        case .strongUp, .strongDown: return .red
        case .up, .down: return .orange
        case .stable: return .green
        }
    }
}
