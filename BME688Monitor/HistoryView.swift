import SwiftUI
import Charts

struct HistoryView: View {
    @Environment(APIService.self) var api
    @State private var selectedRange = "24h"
    @State private var showTemp = true
    @State private var showHum = true
    @State private var showPres = false
    @State private var showCo2 = false

    private let rangeOptions = ["3h", "24h", "3d", "7d"]

    private var days: Int {
        switch selectedRange {
        case "3h", "24h": return 1
        case "3d": return 3
        case "7d": return 7
        default: return 1
        }
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    // Time range picker
                    Picker("Zeitraum", selection: $selectedRange) {
                        ForEach(rangeOptions, id: \.self) { opt in
                            Text(opt).tag(opt)
                        }
                    }
                    .pickerStyle(.segmented)
                    .onChange(of: selectedRange) {
                        Task { await api.fetchHistory(days: days) }
                    }

                    // Series toggles
                    HStack(spacing: 8) {
                        ChartToggle(label: "Temp", color: .orange, isOn: $showTemp)
                        ChartToggle(label: "Feuchte", color: .blue, isOn: $showHum)
                        ChartToggle(label: "Druck", color: .green, isOn: $showPres)
                        ChartToggle(label: "eCO2", color: .yellow, isOn: $showCo2)
                    }

                    // Chart
                    if api.isFetchingHistory {
                        ProgressView("Lade Verlaufsdaten...")
                            .frame(height: 300)
                    } else if api.historyData.isEmpty {
                        ContentUnavailableView {
                            Label("Keine Verlaufsdaten", systemImage: "chart.xyaxis.line")
                        } description: {
                            Text("Fuer diesen Zeitraum sind keine Daten vorhanden.")
                        }
                        .frame(height: 300)
                    } else {
                        // Temperature + Humidity + eCO2 chart
                        if showTemp || showHum || showCo2 {
                            chartView
                                .frame(height: showPres ? 240 : 320)
                                .padding(16)
                                .background {
                                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                                        .fill(.ultraThinMaterial)
                                }
                        }

                        // Pressure chart (separate, tight scale)
                        if showPres {
                            pressureChartView
                                .frame(height: (showTemp || showHum || showCo2) ? 180 : 320)
                                .padding(16)
                                .background {
                                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                                        .fill(.ultraThinMaterial)
                                }
                        }
                    }

                    // Stats
                    if !api.historyData.isEmpty {
                        statsGrid
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 20)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Verlauf")
            .task { await api.fetchHistory(days: days) }
            .refreshable { await api.fetchHistory(days: days) }
        }
    }

    // MARK: - Chart

    private var filteredData: [HistoryPoint] {
        let sorted = api.historyData.sorted {
            if $0.date != $1.date { return $0.date < $1.date }
            return $0.minute < $1.minute
        }
        if selectedRange == "3h" {
            // Keep only the last 3 hours (180 minutes worth of points)
            // Each point is ~1min, so last 180 points
            let count = sorted.count
            if count > 180 {
                return Array(sorted.suffix(180))
            }
        }
        return sorted
    }

    private var sortedData: [HistoryPoint] {
        filteredData.sorted {
            if $0.date != $1.date { return $0.date < $1.date }
            return $0.minute < $1.minute
        }
    }

    private func tempMarks(_ data: [HistoryPoint]) -> some ChartContent {
        ForEach(Array(data.enumerated()), id: \.offset) { i, p in
            LineMark(
                x: .value("Index", i),
                y: .value("Temp", p.temperature),
                series: .value("S", "T")
            )
            .foregroundStyle(.orange)
            .lineStyle(StrokeStyle(lineWidth: 1.5))
        }
    }

    private func humMarks(_ data: [HistoryPoint]) -> some ChartContent {
        ForEach(Array(data.enumerated()), id: \.offset) { i, p in
            LineMark(
                x: .value("Index", i),
                y: .value("Hum", p.humidity),
                series: .value("S", "H")
            )
            .foregroundStyle(.blue)
            .lineStyle(StrokeStyle(lineWidth: 1.5))
        }
    }

    private func co2Marks(_ data: [HistoryPoint]) -> some ChartContent {
        ForEach(Array(data.enumerated()), id: \.offset) { i, p in
            LineMark(
                x: .value("Index", i),
                y: .value("CO2", p.eco2),
                series: .value("S", "C")
            )
            .foregroundStyle(.yellow)
            .lineStyle(StrokeStyle(lineWidth: 1.5))
        }
    }

    private var chartView: some View {
        let sorted = sortedData
        return Chart {
            if showTemp { tempMarks(sorted) }
            if showHum { humMarks(sorted) }
            if showCo2 { co2Marks(sorted) }
        }
        .chartXAxis {
            AxisMarks(values: .automatic(desiredCount: 6)) { value in
                if let idx = value.as(Int.self), idx < sorted.count {
                    AxisValueLabel {
                        Text(days == 1 ? sorted[idx].time : "\(sorted[idx].date.suffix(5))")
                            .font(.caption2)
                    }
                }
                AxisGridLine(stroke: StrokeStyle(lineWidth: 0.3, dash: [4]))
                    .foregroundStyle(.secondary.opacity(0.3))
            }
        }
        .chartYAxis {
            AxisMarks(position: .leading) { _ in
                AxisGridLine(stroke: StrokeStyle(lineWidth: 0.3, dash: [4]))
                    .foregroundStyle(.secondary.opacity(0.3))
                AxisValueLabel()
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .chartLegend(.hidden)
    }

    // MARK: - Pressure Chart

    private var pressureChartView: some View {
        let sorted = sortedData
        let pressVals = sorted.map(\.pressure)
        let pMin = pressVals.min() ?? 1013
        let pMax = pressVals.max() ?? 1013
        let pRange = max(pMax - pMin, 0.5)
        let margin = pRange * 0.3
        let lo = pMin - margin
        let hi = pMax + margin

        return VStack(alignment: .leading, spacing: 4) {
            Text("Druck (hPa)")
                .font(.caption2)
                .foregroundStyle(.secondary)

            Chart {
                ForEach(Array(sorted.enumerated()), id: \.offset) { i, p in
                    LineMark(
                        x: .value("Index", i),
                        y: .value("Pres", p.pressure)
                    )
                    .foregroundStyle(.green)
                    .lineStyle(StrokeStyle(lineWidth: 1.5))
                }
            }
            .chartYScale(domain: lo...hi)
            .chartXAxis {
                AxisMarks(values: .automatic(desiredCount: 6)) { value in
                    if let idx = value.as(Int.self), idx < sorted.count {
                        AxisValueLabel {
                            Text(days == 1 ? sorted[idx].time : "\(sorted[idx].date.suffix(5))")
                                .font(.caption2)
                        }
                    }
                    AxisGridLine(stroke: StrokeStyle(lineWidth: 0.3, dash: [4]))
                        .foregroundStyle(.secondary.opacity(0.3))
                }
            }
            .chartYAxis {
                AxisMarks(position: .leading, values: .automatic(desiredCount: 5)) { _ in
                    AxisGridLine(stroke: StrokeStyle(lineWidth: 0.3, dash: [4]))
                        .foregroundStyle(.secondary.opacity(0.3))
                    AxisValueLabel()
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .chartLegend(.hidden)
        }
    }

    // MARK: - Stats

    private var statsGrid: some View {
        let data: [HistoryPoint] = api.historyData
        let temps: [Double] = data.map(\.temperature)
        let hums: [Double] = data.map(\.humidity)
        let press: [Double] = data.map(\.pressure)

        return LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
            statCard(label: "Temp Min", value: String(format: "%.1f\u{00B0}C", temps.min() ?? 0), color: .blue)
            statCard(label: "Temp \u{00D8}", value: String(format: "%.1f\u{00B0}C", temps.isEmpty ? 0 : temps.reduce(0, +) / Double(temps.count)), color: .orange)
            statCard(label: "Temp Max", value: String(format: "%.1f\u{00B0}C", temps.max() ?? 0), color: .red)

            statCard(label: "Feuchte Min", value: String(format: "%.0f%%", hums.min() ?? 0), color: .cyan)
            statCard(label: "Feuchte \u{00D8}", value: String(format: "%.0f%%", hums.isEmpty ? 0 : hums.reduce(0, +) / Double(hums.count)), color: .blue)
            statCard(label: "Feuchte Max", value: String(format: "%.0f%%", hums.max() ?? 0), color: .indigo)

            statCard(label: "Druck Min", value: String(format: "%.1f", press.min() ?? 0), color: .mint)
            statCard(label: "Druck \u{00D8}", value: String(format: "%.1f", press.isEmpty ? 0 : press.reduce(0, +) / Double(press.count)), color: .green)
            statCard(label: "Druck Max", value: String(format: "%.1f", press.max() ?? 0), color: .teal)
        }
    }

    private func statCard(label: String, value: String, color: Color) -> some View {
        VStack(spacing: 4) {
            Text(value)
                .font(.system(.callout, design: .rounded, weight: .bold))
                .monospacedDigit()
                .foregroundStyle(color)
            Text(label)
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity)
        .background {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(color.opacity(0.06))
        }
    }
}
