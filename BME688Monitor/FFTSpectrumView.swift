import SwiftUI
import Charts

struct FFTSpectrumView: View {
    @Environment(APIService.self) var api
    @State private var selectedChannel = 0

    private let channels = ["Temperatur", "Feuchte", "Druck"]
    private let channelColors: [Color] = [.orange, .blue, .green]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    // Channel picker
                    Picker("Kanal", selection: $selectedChannel) {
                        ForEach(0..<3) { i in
                            Text(channels[i]).tag(i)
                        }
                    }
                    .pickerStyle(.segmented)

                    if let fft = api.liveData?.fft {
                        let data = channelData(fft)
                        let sampleRate = fft.sampleRate
                        let freqRes = sampleRate / Double(fft.bins * 2)

                        if data.count > 2 {
                            // Spectrum chart
                            spectrumChart(data: data, freqRes: freqRes, color: channelColors[selectedChannel])
                                .frame(height: 300)
                                .padding(12)
                                .background {
                                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                                        .fill(.ultraThinMaterial)
                                }

                            // Info cards
                            let peakBin = findPeak(data)
                            let peakFreq = Double(peakBin) * freqRes
                            let peakDb = peakBin < data.count ? data[peakBin] : -80

                            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
                                infoCard(label: "Abtastrate", value: String(format: "%.0f Hz", sampleRate), color: .secondary)
                                infoCard(label: "Peak-Frequenz", value: peakFreq < 0.01 ? "DC" : String(format: "%.3f Hz", peakFreq), color: channelColors[selectedChannel])
                                infoCard(label: "Peak", value: String(format: "%.1f dB", peakDb), color: .purple)
                                infoCard(label: "Aufloesung", value: String(format: "%.4f Hz", freqRes), color: .secondary)
                                infoCard(label: "Peak-Periode", value: peakFreq > 0.001 ? String(format: "%.1f s", 1.0 / peakFreq) : "\u{221E}", color: .mint)
                                infoCard(label: "FFT-Punkte", value: "\(fft.bins * 2)", color: .secondary)
                            }
                        } else {
                            noDataPlaceholder("Zu wenig Daten fuer FFT")
                        }
                    } else {
                        noDataPlaceholder("Keine FFT-Daten verfuegbar")
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 20)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("FFT-Spektrum")
        }
    }

    // MARK: - Chart

    @ViewBuilder
    private func spectrumChart(data: [Double], freqRes: Double, color: Color) -> some View {
        let maxFreq = Double(data.count) * freqRes

        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Leistungsspektrum")
                    .font(.subheadline)
                    .fontWeight(.semibold)
                    .foregroundStyle(.secondary)
                Spacer()
                Text("0 \u{2013} \(String(format: "%.2f", maxFreq)) Hz")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }

            Chart {
                // Skip bin 0 (DC component)
                ForEach(1..<data.count, id: \.self) { i in
                    let freq = Double(i) * freqRes
                    AreaMark(
                        x: .value("Freq", freq),
                        y: .value("dB", data[i])
                    )
                    .foregroundStyle(
                        LinearGradient(
                            colors: [color.opacity(0.5), color.opacity(0.05)],
                            startPoint: .top, endPoint: .bottom
                        )
                    )
                    LineMark(
                        x: .value("Freq", freq),
                        y: .value("dB", data[i])
                    )
                    .foregroundStyle(color)
                    .lineStyle(StrokeStyle(lineWidth: 1.5))
                }
            }
            .chartYScale(domain: -80...0)
            .chartXAxis {
                AxisMarks(values: .automatic(desiredCount: 6)) { value in
                    AxisValueLabel {
                        if let v = value.as(Double.self) {
                            Text(String(format: "%.2f", v))
                                .font(.caption2)
                        }
                    }
                    AxisGridLine(stroke: StrokeStyle(lineWidth: 0.3, dash: [4]))
                        .foregroundStyle(.secondary.opacity(0.3))
                }
            }
            .chartYAxis {
                AxisMarks(values: [-80, -60, -40, -20, 0]) { value in
                    AxisValueLabel {
                        if let v = value.as(Int.self) {
                            Text("\(v) dB")
                                .font(.caption2)
                        }
                    }
                    AxisGridLine(stroke: StrokeStyle(lineWidth: 0.3, dash: [4]))
                        .foregroundStyle(.secondary.opacity(0.3))
                }
            }
            .chartLegend(.hidden)

            HStack {
                Text("Frequenz (Hz)")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                Spacer()
            }
        }
    }

    // MARK: - Helpers

    private func channelData(_ fft: FFTData) -> [Double] {
        switch selectedChannel {
        case 0: return fft.temperature
        case 1: return fft.humidity
        case 2: return fft.pressure
        default: return []
        }
    }

    private func findPeak(_ data: [Double]) -> Int {
        guard data.count > 2 else { return 0 }
        var maxIdx = 1
        var maxVal = data[1]
        for i in 2..<data.count {
            if data[i] > maxVal {
                maxVal = data[i]
                maxIdx = i
            }
        }
        return maxIdx
    }

    private func infoCard(label: String, value: String, color: Color) -> some View {
        VStack(spacing: 4) {
            Text(value)
                .font(.system(.callout, design: .rounded, weight: .bold))
                .monospacedDigit()
                .foregroundStyle(color)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
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

    private func noDataPlaceholder(_ message: String) -> some View {
        ContentUnavailableView {
            Label(message, systemImage: "waveform.path.ecg")
        }
        .frame(height: 300)
    }
}
