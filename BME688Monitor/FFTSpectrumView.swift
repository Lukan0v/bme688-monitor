import SwiftUI
import Charts

enum SpectrumColorScheme: Int, CaseIterable {
    case klassik = 0
    case thermal = 1
    case neon = 2
    case polar = 3

    var label: String {
        switch self {
        case .klassik: return "Klassik"
        case .thermal: return "Thermal"
        case .neon: return "Neon"
        case .polar: return "Polar"
        }
    }
}

struct FFTSpectrumView: View {
    @Environment(APIService.self) var api
    @State private var selectedChannel = 0
    @State private var colorScheme: SpectrumColorScheme = .klassik
    @State private var cursorFreq: Double? = nil
    @State private var cursorVisible = false
    @State private var fadeTask: Task<Void, Never>? = nil

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
                            if colorScheme == .polar {
                                polarView(data: data, freqRes: freqRes)
                                    .frame(height: 320)
                                    .padding(12)
                                    .background {
                                        RoundedRectangle(cornerRadius: 18, style: .continuous)
                                            .fill(.ultraThinMaterial)
                                    }
                            } else {
                                spectrumChart(data: data, freqRes: freqRes)
                                    .frame(height: 300)
                                    .padding(12)
                                    .background {
                                        RoundedRectangle(cornerRadius: 18, style: .continuous)
                                            .fill(.ultraThinMaterial)
                                    }
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
        case .polar: return .blue
        }
    }

    // MARK: - Polar Radial View

    @ViewBuilder
    private func polarView(data: [Double], freqRes: Double) -> some View {
        let maxFreq = Double(data.count) * freqRes
        let peaks = findTop3Peaks(data)

        VStack(spacing: 4) {
            HStack {
                Text("Polar-Spektrum")
                    .font(.subheadline)
                    .fontWeight(.semibold)
                    .foregroundStyle(.secondary)
                Spacer()
                Text("0 \u{2013} \(String(format: "%.2f", maxFreq)) Hz")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }

            Canvas { context, size in
                let cx = size.width / 2
                let cy = size.height / 2
                let maxR = min(cx, cy) - 24
                let minR: CGFloat = 12 // center hole

                // dB grid circles
                for db in stride(from: -80, through: 0, by: 20) {
                    let norm = CGFloat((Double(db) + 80) / 80)
                    let r = minR + norm * (maxR - minR)
                    let circle = Path(ellipseIn: CGRect(x: cx - r, y: cy - r, width: r * 2, height: r * 2))
                    context.stroke(circle, with: .color(.secondary.opacity(0.15)), lineWidth: 0.5)
                    // dB label
                    let labelPt = CGPoint(x: cx + 4, y: cy - r - 1)
                    context.draw(Text("\(db)").font(.system(size: 8)).foregroundColor(.secondary.opacity(0.5)), at: labelPt, anchor: .bottomLeading)
                }

                // 0 dB outer ring (stronger)
                let outerCircle = Path(ellipseIn: CGRect(x: cx - maxR, y: cy - maxR, width: maxR * 2, height: maxR * 2))
                context.stroke(outerCircle, with: .color(.secondary.opacity(0.3)), lineWidth: 1)

                // Radial frequency axis lines (every 90 degrees)
                let count = data.count
                for q in 0..<4 {
                    let angle = Double(q) * .pi / 2 - .pi / 2
                    let x1 = cx + minR * CGFloat(cos(angle))
                    let y1 = cy + minR * CGFloat(sin(angle))
                    let x2 = cx + maxR * CGFloat(cos(angle))
                    let y2 = cy + maxR * CGFloat(sin(angle))
                    var line = Path()
                    line.move(to: CGPoint(x: x1, y: y1))
                    line.addLine(to: CGPoint(x: x2, y: y2))
                    context.stroke(line, with: .color(.secondary.opacity(0.15)), lineWidth: 0.5)

                    // Freq labels at quadrant points
                    let freqAtQ = maxFreq * Double(q) / 4.0
                    let lx = cx + (maxR + 14) * CGFloat(cos(angle))
                    let ly = cy + (maxR + 14) * CGFloat(sin(angle))
                    context.draw(
                        Text(String(format: "%.2f", freqAtQ)).font(.system(size: 9)).foregroundColor(.secondary.opacity(0.6)),
                        at: CGPoint(x: lx, y: ly),
                        anchor: .center
                    )
                }

                // Build filled path (center → data points → center) and line path
                var fillPath = Path()
                var linePath = Path()
                let center = CGPoint(x: cx, y: cy)

                fillPath.move(to: center)
                for i in 1..<count {
                    let angle = Double(i) / Double(count) * 2 * .pi - .pi / 2
                    let norm = CGFloat(max(0, (data[i] + 80) / 80))
                    let r = minR + norm * (maxR - minR)
                    let pt = CGPoint(x: cx + r * CGFloat(cos(angle)), y: cy + r * CGFloat(sin(angle)))

                    fillPath.addLine(to: pt)
                    if i == 1 {
                        linePath.move(to: pt)
                    } else {
                        linePath.addLine(to: pt)
                    }
                }
                // Close: connect last point back to first data point, then to center
                let firstAngle = 1.0 / Double(count) * 2 * .pi - .pi / 2
                let firstNorm = CGFloat(max(0, (data[1] + 80) / 80))
                let firstR = minR + firstNorm * (maxR - minR)
                let firstPt = CGPoint(x: cx + firstR * CGFloat(cos(firstAngle)), y: cy + firstR * CGFloat(sin(firstAngle)))
                fillPath.addLine(to: firstPt)
                fillPath.closeSubpath()
                linePath.addLine(to: firstPt)

                // Draw filled area with radial gradient
                context.fill(fillPath, with: .radialGradient(
                    Gradient(stops: [
                        .init(color: Color.blue.opacity(0.02), location: 0),
                        .init(color: Color.cyan.opacity(0.15), location: 0.3),
                        .init(color: Color.blue.opacity(0.25), location: 0.6),
                        .init(color: Color.purple.opacity(0.4), location: 0.85),
                        .init(color: Color.pink.opacity(0.5), location: 1.0),
                    ]),
                    center: CGPoint(x: cx, y: cy),
                    startRadius: minR,
                    endRadius: maxR
                ))

                // Draw spectrum line
                context.stroke(linePath, with: .linearGradient(
                    Gradient(colors: [.cyan, .blue, .purple, .pink, .cyan]),
                    startPoint: CGPoint(x: 0, y: 0),
                    endPoint: CGPoint(x: size.width, y: size.height)
                ), lineWidth: 1.5)

                // Peak markers as small diamonds
                for peak in peaks {
                    let angle = Double(peak.bin) / Double(count) * 2 * .pi - .pi / 2
                    let norm = CGFloat(max(0, (peak.dB + 80) / 80))
                    let r = minR + norm * (maxR - minR)
                    let px = cx + r * CGFloat(cos(angle))
                    let py = cy + r * CGFloat(sin(angle))
                    let sz: CGFloat = peak.rank == 0 ? 6 : 4

                    var diamond = Path()
                    diamond.move(to: CGPoint(x: px, y: py - sz))
                    diamond.addLine(to: CGPoint(x: px + sz, y: py))
                    diamond.addLine(to: CGPoint(x: px, y: py + sz))
                    diamond.addLine(to: CGPoint(x: px - sz, y: py))
                    diamond.closeSubpath()

                    context.fill(diamond, with: .color(.white))
                    if peak.rank == 0 {
                        // Glow for strongest peak
                        context.fill(diamond, with: .color(.white.opacity(0.5)))
                    }
                }

                // Center label
                context.draw(
                    Text("-80").font(.system(size: 8)).foregroundColor(.secondary.opacity(0.4)),
                    at: CGPoint(x: cx, y: cy),
                    anchor: .center
                )
            }
            .frame(maxWidth: .infinity)

            Text("Frequenz im Uhrzeigersinn, Amplitude = Radius")
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
    }

    // MARK: - Linear Chart

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
        let lineWidth: CGFloat = colorScheme == .klassik ? 1.5 : 1.2
        let curLineColor: Color = lineColor
        let curAreaGradient: LinearGradient = areaStyle(stops: gradientStops(for: colorScheme))
        let showCursor = cursorFreq != nil && cursorVisible
        let safeCursorFreq = cursorFreq ?? 0
        let peaks = findTop3Peaks(data)
        let peakColor: Color = peakMarkerColor

        Chart {
            ForEach(1..<data.count, id: \.self) { i in
                AreaMark(
                    x: .value("Freq", Double(i) * freqRes),
                    yStart: .value("Base", -80.0),
                    yEnd: .value("dB", data[i])
                )
                .foregroundStyle(curAreaGradient)

                LineMark(
                    x: .value("Freq", Double(i) * freqRes),
                    y: .value("dB", data[i])
                )
                .foregroundStyle(curLineColor)
                .lineStyle(StrokeStyle(lineWidth: lineWidth))
            }

            ForEach(peaks, id: \.bin) { peak in
                PointMark(
                    x: .value("Freq", Double(peak.bin) * freqRes),
                    y: .value("dB", peak.dB)
                )
                .symbol(.diamond)
                .symbolSize(peak.rank == 0 ? 60 : 36)
                .foregroundStyle(peakColor)
            }

            if showCursor {
                let cursorBin = max(1, min(Int(round(safeCursorFreq / freqRes)), data.count - 1))
                let cursorDb = data[cursorBin]

                RuleMark(x: .value("Cursor", safeCursorFreq))
                    .foregroundStyle(Color.white.opacity(0.6))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [5, 3]))

                PointMark(
                    x: .value("Freq", safeCursorFreq),
                    y: .value("dB", cursorDb)
                )
                .symbol(.circle)
                .symbolSize(50)
                .foregroundStyle(Color.white)
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
        .overlay(alignment: .topLeading) {
            if showCursor {
                cursorOverlay(data: data, freqRes: freqRes)
            }
        }
        .chartOverlay { proxy in
            chartGestureOverlay(proxy: proxy)
        }
        .animation(.easeInOut(duration: 0.15), value: cursorVisible)
    }

    private var peakMarkerColor: Color {
        switch colorScheme {
        case .klassik: return .white
        case .thermal: return .yellow
        case .neon: return .mint
        case .polar: return .white
        }
    }

    // MARK: - Cursor overlay

    @ViewBuilder
    private func cursorOverlay(data: [Double], freqRes: Double) -> some View {
        let freq = cursorFreq ?? 0
        let bin = max(1, min(Int(round(freq / freqRes)), data.count - 1))
        let db = data[bin]
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
        .transition(.opacity.animation(.easeOut(duration: 0.25)))
        .padding(8)
    }

    // MARK: - Gesture overlay

    private func chartGestureOverlay(proxy: ChartProxy) -> some View {
        GeometryReader { geo in
            Rectangle()
                .fill(Color.clear)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { value in
                            guard let plotFrame = proxy.plotFrame else { return }
                            let origin = geo[plotFrame].origin
                            let x = value.location.x - origin.x
                            if let freq: Double = proxy.value(atX: x) {
                                cursorFreq = freq
                                cursorVisible = true
                                fadeTask?.cancel()
                                fadeTask = nil
                            }
                        }
                        .onEnded { _ in
                            fadeTask?.cancel()
                            fadeTask = Task { @MainActor in
                                try? await Task.sleep(for: .seconds(1.5))
                                guard !Task.isCancelled else { return }
                                withAnimation(.easeOut(duration: 0.4)) {
                                    cursorVisible = false
                                }
                            }
                        }
                )
        }
    }

    private var lineColor: Color {
        switch colorScheme {
        case .klassik: return .orange
        case .thermal, .neon: return .white.opacity(0.55)
        case .polar: return .cyan
        }
    }

    private func areaStyle(stops: [Gradient.Stop]) -> LinearGradient {
        switch colorScheme {
        case .klassik:
            return LinearGradient(
                colors: [Color.orange.opacity(0.25), Color.orange.opacity(0.02)],
                startPoint: .top, endPoint: .bottom
            )
        default:
            return LinearGradient(stops: stops, startPoint: .bottom, endPoint: .top)
        }
    }

    // MARK: - Gradient stops

    private func gradientStops(for scheme: SpectrumColorScheme) -> [Gradient.Stop] {
        switch scheme {
        case .thermal:
            return [
                .init(color: Color(red: 0, green: 0, blue: 0).opacity(0.05), location: 0.0),
                .init(color: Color(red: 0, green: 0, blue: 0.7).opacity(0.5), location: 0.25),
                .init(color: Color(red: 0.86, green: 0, blue: 0).opacity(0.55), location: 0.5),
                .init(color: Color(red: 0.86, green: 0.78, blue: 0).opacity(0.6), location: 0.75),
                .init(color: Color(red: 1, green: 1, blue: 0.7).opacity(0.7), location: 1.0),
            ]
        case .neon:
            return [
                .init(color: Color(red: 0, green: 0, blue: 0).opacity(0.05), location: 0.0),
                .init(color: Color(red: 0.31, green: 0, blue: 0.63).opacity(0.5), location: 0.33),
                .init(color: Color(red: 0, green: 0.86, blue: 0.78).opacity(0.55), location: 0.66),
                .init(color: Color(red: 1, green: 1, blue: 0.9).opacity(0.7), location: 1.0),
            ]
        default:
            return [
                .init(color: .orange.opacity(0.02), location: 0),
                .init(color: .orange.opacity(0.25), location: 1),
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

        var maxIdx = 1
        var maxVal = data[1]
        for i in 2..<data.count {
            if data[i] > maxVal {
                maxVal = data[i]
                maxIdx = i
            }
        }

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

    // MARK: - Top 3 Peaks

    private struct PeakMarker: Hashable {
        let bin: Int
        let dB: Double
        let rank: Int

        func hash(into hasher: inout Hasher) {
            hasher.combine(bin)
        }
    }

    private func findTop3Peaks(_ data: [Double]) -> [PeakMarker] {
        guard data.count > 3 else { return [] }

        var localMaxima: [(bin: Int, dB: Double)] = []
        for i in 1..<(data.count - 1) {
            if data[i] > data[i - 1] && data[i] > data[i + 1] && data[i] > -75 {
                localMaxima.append((bin: i, dB: data[i]))
            }
        }

        localMaxima.sort { $0.dB > $1.dB }
        let top = localMaxima.prefix(3)

        return top.enumerated().map { idx, peak in
            PeakMarker(bin: peak.bin, dB: peak.dB, rank: idx)
        }
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
