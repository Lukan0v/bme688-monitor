import SwiftUI
import Charts

struct LiveChartView: View {
    let history: HistoryArrays
    @State private var showTemp = true
    @State private var showHum = true
    @State private var showPres = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Live-Trend")
                    .font(.subheadline)
                    .fontWeight(.semibold)
                    .foregroundStyle(.secondary)
                Spacer()
                HStack(spacing: 8) {
                    ChartToggle(label: "Temp", color: .orange, isOn: $showTemp)
                    ChartToggle(label: "Feuchte", color: .blue, isOn: $showHum)
                    ChartToggle(label: "Druck", color: .green, isOn: $showPres)
                }
            }

            // Temperature + Humidity chart (similar scale ~0-100)
            if showTemp || showHum {
                Chart {
                    if showTemp {
                        ForEach(Array(history.temperature.enumerated()), id: \.offset) { i, v in
                            LineMark(
                                x: .value("Zeit", i),
                                y: .value("Temp", v),
                                series: .value("Typ", "Temperatur")
                            )
                            .foregroundStyle(Color.orange)
                            .lineStyle(StrokeStyle(lineWidth: 2))
                        }
                    }
                    if showHum {
                        ForEach(Array(history.humidity.enumerated()), id: \.offset) { i, v in
                            LineMark(
                                x: .value("Zeit", i),
                                y: .value("Hum", v),
                                series: .value("Typ", "Feuchte")
                            )
                            .foregroundStyle(Color.blue)
                            .lineStyle(StrokeStyle(lineWidth: 2))
                        }
                    }
                }
                .chartXAxis(.hidden)
                .chartYAxis {
                    AxisMarks(position: .leading) { _ in
                        AxisGridLine(stroke: StrokeStyle(lineWidth: 0.5, dash: [4]))
                            .foregroundStyle(Color.secondary.opacity(0.2))
                        AxisValueLabel()
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
                .chartLegend(.hidden)
                .frame(height: showPres ? 140 : 200)
            }

            // Pressure chart (separate, tight scale)
            if showPres && !history.pressure.isEmpty {
                let pMin = history.pressure.min() ?? 1013
                let pMax = history.pressure.max() ?? 1013
                let pRange = max(pMax - pMin, 0.5)
                let margin = pRange * 0.3
                let lo = pMin - margin
                let hi = pMax + margin

                VStack(alignment: .leading, spacing: 4) {
                    Text("Druck (hPa)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)

                    Chart {
                        ForEach(Array(history.pressure.enumerated()), id: \.offset) { i, v in
                            LineMark(
                                x: .value("Zeit", i),
                                y: .value("Pres", v)
                            )
                            .foregroundStyle(Color.green)
                            .lineStyle(StrokeStyle(lineWidth: 2))
                        }
                    }
                    .chartXAxis(.hidden)
                    .chartYScale(domain: lo...hi)
                    .chartYAxis {
                        AxisMarks(position: .leading, values: .automatic(desiredCount: 4)) { _ in
                            AxisGridLine(stroke: StrokeStyle(lineWidth: 0.5, dash: [4]))
                                .foregroundStyle(Color.secondary.opacity(0.2))
                            AxisValueLabel()
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .chartLegend(.hidden)
                    .frame(height: (showTemp || showHum) ? 100 : 200)
                }
            }

            HStack {
                Text("\u{2190} 60s")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                Spacer()
                Text("jetzt")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(16)
        .background {
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(.ultraThinMaterial)
        }
    }
}

struct ChartToggle: View {
    let label: String
    let color: Color
    @Binding var isOn: Bool

    var body: some View {
        Button {
            withAnimation(.spring(duration: 0.2)) { isOn.toggle() }
        } label: {
            Text(label)
                .font(.caption2)
                .fontWeight(.medium)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background {
                    Capsule()
                        .fill(isOn ? color.opacity(0.2) : Color.secondary.opacity(0.1))
                        .overlay(
                            Capsule().strokeBorder(isOn ? color.opacity(0.4) : Color.clear, lineWidth: 1)
                        )
                }
                .foregroundStyle(isOn ? color : .secondary)
        }
        .buttonStyle(.plain)
    }
}
