import SwiftUI
import Charts

enum SpectrumColorScheme: Int, CaseIterable {
    case klassik = 0
    case thermal = 1
    case neon = 2

    var label: String {
        switch self {
        case .klassik: return "Klassik"
        case .thermal: return "Thermal"
        case .neon: return "Neon"
        }
    }
}

struct FFTSpectrumView: View {
    @Environment(APIService.self) var api
    @State private var selectedChannel = 0
    @State private var colorScheme: SpectrumColorScheme = .klassik
    @State private var cursorFreq: Double? = nil
    @State private var cursorOpacity: Double = 0

    private let channels = ["Temperatur", "Feuchte", "Druck"]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    Picker("Kanal", selection: $selectedChannel) {
                        ForEach(0..<3) { i in
                            Text(channels[i]).tag(i)
                        }
                    }
                    .pickerStyle(.segmented)

                    Picker("Farbmodus", selection: $colorScheme) {
                        ForEach(SpectrumColorScheme.allCases, id: \.self) { scheme in
                            Text(scheme.label).tag(scheme)
                        }
                    }
                    .pickerStyle(.segmented)

                    if let fft = api.liveData?.fft {
                        let data = channelData(fft)
                        let sampleRate = fft.sampleRate
                        let freqRes = sampleRate / Double(fft.bins * 2)

                        if data.count > 2 {
                            spectrumChart(data: data, freqRes: freqRes)
                                .frame(height: 300)
                                .padding(12)
                                .background {
                                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                                        .fill(.ultraThinMaterial)
                                }

                            let peak = interpolatedPeak(data)
                            let peakFreq = peak.bin * freqRes
                            let peakDb = peak.dB
                            let peakPeriod = peakFreq > 0.001 ? 1.0 / peakFreq : Double.infinity

                            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
                                infoCard(label: "Abtastrate", value: String(format: "%.0f Hz", sampleRate), color: .secondary)
                                infoCard(label: "Peak-Frequenz", value: peakFreq < 0.01 ? "DC" : String(format: "%.4f Hz", peakFreq), color: schemeAccent)
                                infoCard(label: "Peak", value: String(format: "%.1f dB", peakDb), color: .purple)
                                infoCard(label: "Aufloesung", value: String(format: "%.4f Hz", freqRes), color: .secondary)
                                infoCard(label: "Peak-Periode", value: formatPeriod(peakPeriod), color: .mint)
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

    // MARK: - Color scheme accent

    private var schemeAccent: Color {
        switch colorScheme {
        case .klassik: return .orange
        case .thermal: return .red
        case .neon: return .cyan
        }
    }

    // MARK: - Chart

    @ViewBuilder
    private func spectrumChart(data: [Double], freqRes: Double) -> some View {
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

            chartBody(data: data, freqRes: freqRes)

            HStack {
                Text("Frequenz (Hz)")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                Spacer()
            }
        }
    }

    @ViewBuilder
    private func chartBody(data: [Double], freqRes: Double) -> some View {
        let stops = gradientStops(for: colorScheme)

        Chart {
            ForEach(1..<data.count, id: \.self) { i in
                let freq = Double(i) * freqRes
                AreaMark(
                    x: .value("Freq", freq),
                    yStart: .value("Base", -80.0),
                    yEnd: .value("dB", data[i])
                )
                .foregroundStyle(areaStyle(stops: stops))
                LineMark(
                    x: .value("Freq", freq),
                    y: .value("dB", data[i])
                )
                .foregroundStyle(lineColor)
                .lineStyle(StrokeStyle(lineWidth: colorScheme == .klassik ? 1.5 : 1.2))
            }

            // Cursor rule mark
            if let freq = cursorFreq, cursorOpacity > 0 {
                RuleMark(x: .value("Cursor", freq))
                    .foregroundStyle(.white.opacity(0.6 * cursorOpacity))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [5, 3]))
                    .annotation(position: .top, alignment: .center) {
                        cursorLabel(data: data, freqRes: freqRes, freq: freq)
                            .opacity(cursorOpacity)
                    }
            }
        }
        .chartYScale(domain: -80...0)
        .spectrumAxes()
        .chartLegend(.hidden)
        .chartOverlay { proxy in
            GeometryReader { geo in
                Rectangle()
                    .fill(Color.clear)
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onChanged { value in
                                let origin = geo[proxy.plotFrame!].origin
                                let x = value.location.x - origin.x
                                if let freq: Double = proxy.value(atX: x) {
                                    cursorFreq = freq
                                    withAnimation(.easeIn(duration: 0.1)) {
                                        cursorOpacity = 1.0
                                    }
                                }
                            }
                            .onEnded { _ in
                                withAnimation(.easeOut(duration: 3.0)) {
                                    cursorOpacity = 0
                                }
                            }
                    )
            }
        }
    }

    private var lineColor: Color {
        switch colorScheme {
        case .klassik: return .orange
        case .thermal, .neon: return .white.opacity(0.55)
        }
    }

    private func areaStyle(stops: [Gradient.Stop]) -> LinearGradient {
        switch colorScheme {
        case .klassik:
            return LinearGradient(
                colors: [Color.orange.opacity(0.4), Color.orange.opacity(0.02)],
                startPoint: .top, endPoint: .bottom
            )
        default:
            return LinearGradient(stops: stops, startPoint: .bottom, endPoint: .top)
        }
    }

    // MARK: - Cursor label

    @ViewBuilder
    private func cursorLabel(data: [Double], freqRes: Double, freq: Double) -> some View {
        let bin = Int(round(freq / freqRes))
        let clampedBin = max(1, min(bin, data.count - 1))
        let db = data[clampedBin]
        let period = freq > 0.001 ? 1.0 / freq : Double.infinity

        VStack(spacing: 2) {
            Text(String(format: "%.3f Hz", freq))
                .font(.caption2)
                .fontWeight(.semibold)
            Text(String(format: "%.1f dB", db))
                .font(.caption2)
            if !period.isInfinite {
                Text(formatPeriod(period))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .monospacedDigit()
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(.ultraThinMaterial)
                .shadow(color: .black.opacity(0.15), radius: 4, y: 2)
        }
    }

    // MARK: - Gradient stops

    private func gradientStops(for scheme: SpectrumColorScheme) -> [Gradient.Stop] {
        switch scheme {
        case .thermal:
            return [
                .init(color: Color(red: 0, green: 0, blue: 0).opacity(0.8), location: 0.0),
                .init(color: Color(red: 0, green: 0, blue: 0.7).opacity(0.85), location: 0.25),
                .init(color: Color(red: 0.86, green: 0, blue: 0).opacity(0.85), location: 0.5),
                .init(color: Color(red: 0.86, green: 0.78, blue: 0).opacity(0.9), location: 0.75),
                .init(color: Color(red: 1, green: 1, blue: 0.7).opacity(0.95), location: 1.0),
            ]
        case .neon:
            return [
                .init(color: Color(red: 0, green: 0, blue: 0).opacity(0.8), location: 0.0),
                .init(color: Color(red: 0.31, green: 0, blue: 0.63).opacity(0.8), location: 0.33),
                .init(color: Color(red: 0, green: 0.86, blue: 0.78).opacity(0.85), location: 0.66),
                .init(color: Color(red: 1, green: 1, blue: 0.9).opacity(0.95), location: 1.0),
            ]
        default:
            return [
                .init(color: .orange.opacity(0.02), location: 0),
                .init(color: .orange.opacity(0.4), location: 1),
            ]
        }
    }

    // MARK: - 3-Point Parabolic Peak Interpolation

    private struct PeakResult {
        let bin: Double
        let dB: Double
    }

    private func interpolatedPeak(_ data: [Double]) -> PeakResult {
        guard data.count > 2 else { return PeakResult(bin: 0, dB: -80) }

        // Find raw peak (skip DC bin 0)
        var maxIdx = 1
        var maxVal = data[1]
        for i in 2..<data.count {
            if data[i] > maxVal {
                maxVal = data[i]
                maxIdx = i
            }
        }

        // 3-point parabolic interpolation
        if maxIdx > 0 && maxIdx < data.count - 1 {
            let alpha = data[maxIdx - 1]
            let beta = data[maxIdx]
            let gamma = data[maxIdx + 1]
            let denom = 2.0 * beta - alpha - gamma
            if abs(denom) > 1e-10 {
                let delta = 0.5 * (gamma - alpha) / denom
                let interpBin = Double(maxIdx) + delta
                let interpDb = beta - 0.25 * (alpha - gamma) * delta
                return PeakResult(bin: interpBin, dB: interpDb)
            }
        }
        return PeakResult(bin: Double(maxIdx), dB: maxVal)
    }

    // MARK: - Period formatting

    private func formatPeriod(_ seconds: Double) -> String {
        if seconds.isInfinite || seconds > 100000 { return "\u{221E}" }
        if seconds < 1 {
            return String(format: "%.0f ms", seconds * 1000)
        } else if seconds < 60 {
            return String(format: "alle %.1f s", seconds)
        } else if seconds < 3600 {
            return String(format: "alle %.1f min", seconds / 60)
        } else {
            return String(format: "alle %.1f h", seconds / 3600)
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

// MARK: - Chart axis modifier

extension Chart {
    func spectrumAxes() -> some View {
        self
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
    }
}
