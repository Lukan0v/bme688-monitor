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

            let activeCount = (showTemp ? 1 : 0) + (showHum ? 1 : 0) + (showPres ? 1 : 0)
            let chartH: CGFloat = activeCount <= 1 ? 160 : (activeCount == 2 ? 110 : 85)

            // Temperature chart
            if showTemp && !history.temperature.isEmpty {
                tightChart(
                    data: history.temperature,
                    label: "Temperatur (\u{00B0}C)",
                    color: .orange,
                    height: chartH
                )
            }

            // Humidity chart
            if showHum && !history.humidity.isEmpty {
                tightChart(
                    data: history.humidity,
                    label: "Feuchte (%)",
                    color: .blue,
                    height: chartH
                )
            }

            // Pressure chart
            if showPres && !history.pressure.isEmpty {
                tightChart(
                    data: history.pressure,
                    label: "Druck (hPa)",
                    color: .green,
                    height: chartH
                )
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

    @ViewBuilder
    private func tightChart(data: [Double], label: String, color: Color, height: CGFloat) -> some View {
        let dMin = data.min() ?? 0
        let dMax = data.max() ?? 0
        let range = max(dMax - dMin, 0.1)
        let margin = range * 0.3
        let lo = dMin - margin
        let hi = dMax + margin

        VStack(alignment: .leading, spacing: 2) {
            Text(label)
                .font(.caption2)
                .foregroundStyle(.secondary)

            Chart {
                ForEach(Array(data.enumerated()), id: \.offset) { i, v in
                    LineMark(
                        x: .value("Zeit", i),
                        y: .value("V", v)
                    )
                    .foregroundStyle(color)
                    .lineStyle(StrokeStyle(lineWidth: 2))
                }
            }
            .chartXAxis(.hidden)
            .chartYScale(domain: lo...hi)
            .chartYAxis {
                AxisMarks(position: .leading, values: .automatic(desiredCount: 3)) { _ in
                    AxisGridLine(stroke: StrokeStyle(lineWidth: 0.5, dash: [4]))
                        .foregroundStyle(Color.secondary.opacity(0.2))
                    AxisValueLabel()
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .chartLegend(.hidden)
            .frame(height: height)
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
