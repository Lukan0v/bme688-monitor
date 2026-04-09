import SwiftUI

struct SensorCardView: View {
    let title: String
    let value: Double
    let unit: String
    let format: String
    let trendPerSec: Double
    let trendUnit: String
    let trendThreshold: Double
    let trendMultiplier: Double
    let color: Color
    let icon: String

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: icon)
                    .font(.subheadline)
                    .foregroundStyle(color)
                Text(title)
                    .font(.caption)
                    .fontWeight(.medium)
                    .foregroundStyle(.secondary)
            }

            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(String(format: format, value))
                    .font(.system(size: 32, weight: .bold, design: .rounded))
                    .foregroundStyle(color)
                    .contentTransition(.numericText())
                    .animation(.spring(duration: 0.3), value: value)
                Text(unit)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            let dir = trendDirection(trendPerSec, threshold: trendThreshold)
            let displayTrend = trendPerSec * trendMultiplier
            HStack(spacing: 4) {
                Image(systemName: dir.sfSymbol)
                    .font(.caption2)
                Text(String(format: "%.2f %@", displayTrend, trendUnit))
                    .font(.caption)
                    .monospacedDigit()
            }
            .foregroundStyle(trendColor(dir))
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background {
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(color.opacity(0.08))
                .overlay(
                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                        .strokeBorder(color.opacity(0.15), lineWidth: 1)
                )
        }
    }

    private func trendColor(_ dir: TrendDirection) -> Color {
        switch dir {
        case .strongUp, .strongDown: return .red
        case .up, .down: return .orange
        case .stable: return .green
        }
    }
}
