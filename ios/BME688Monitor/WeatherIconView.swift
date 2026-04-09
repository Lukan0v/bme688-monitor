import SwiftUI

struct WeatherIconView: View {
    let icon: String
    let size: CGFloat

    var body: some View {
        Canvas { context, canvasSize in
            let cx = canvasSize.width / 2
            let cy = canvasSize.height / 2
            let r = min(canvasSize.width, canvasSize.height) * 0.3

            switch icon {
            case "sun":
                drawSun(in: &context, cx: cx, cy: cy, r: r, withRays: false)
            case "sun_rays":
                drawSun(in: &context, cx: cx, cy: cy, r: r, withRays: true)
            case "sun_cloud":
                drawSun(in: &context, cx: cx + r * 0.3, cy: cy - r * 0.4, r: r * 0.65, withRays: false)
                drawCloud(in: &context, cx: cx - r * 0.1, cy: cy + r * 0.15, r: r * 0.9)
            case "cloud":
                drawCloud(in: &context, cx: cx, cy: cy, r: r * 1.1)
            case "rain":
                drawCloud(in: &context, cx: cx, cy: cy - r * 0.25, r: r * 0.95)
                drawRainDrops(in: &context, cx: cx, cy: cy + r * 0.2, r: r)
            case "storm":
                drawCloud(in: &context, cx: cx, cy: cy - r * 0.35, r: r * 0.95, dark: true)
                drawLightning(in: &context, cx: cx, cy: cy + r * 0.05, r: r)
                drawRainDrops(in: &context, cx: cx, cy: cy + r * 0.2, r: r, count: 2)
            case "stable":
                drawArrow(in: &context, cx: cx, cy: cy, r: r)
            default:
                drawArrow(in: &context, cx: cx, cy: cy, r: r)
            }
        }
        .frame(width: size, height: size)
    }

    // MARK: - Drawing helpers

    private func drawSun(in context: inout GraphicsContext, cx: CGFloat, cy: CGFloat, r: CGFloat, withRays: Bool) {
        let sunColor = Color(red: 1.0, green: 0.82, blue: 0.38)
        context.fill(Circle().path(in: CGRect(x: cx - r, y: cy - r, width: r * 2, height: r * 2)),
                     with: .color(sunColor))

        if withRays {
            for i in 0..<8 {
                let angle = Double(i) / 8.0 * .pi * 2
                let x1 = cx + CGFloat(cos(angle)) * (r + 5)
                let y1 = cy + CGFloat(sin(angle)) * (r + 5)
                let x2 = cx + CGFloat(cos(angle)) * (r + 14)
                let y2 = cy + CGFloat(sin(angle)) * (r + 14)
                var path = Path()
                path.move(to: CGPoint(x: x1, y: y1))
                path.addLine(to: CGPoint(x: x2, y: y2))
                context.stroke(path, with: .color(sunColor), lineWidth: 3)
            }
        }
    }

    private func drawCloud(in context: inout GraphicsContext, cx: CGFloat, cy: CGFloat, r: CGFloat, dark: Bool = false) {
        let color = dark ? Color(red: 0.38, green: 0.41, blue: 0.47) : Color(red: 0.69, green: 0.72, blue: 0.78)
        let puffs: [(CGFloat, CGFloat, CGFloat)] = [
            (cx - r * 0.4, cy, r * 0.48),
            (cx + r * 0.35, cy + r * 0.05, r * 0.42),
            (cx, cy - r * 0.22, r * 0.52),
        ]
        for (px, py, pr) in puffs {
            context.fill(Circle().path(in: CGRect(x: px - pr, y: py - pr, width: pr * 2, height: pr * 2)),
                         with: .color(color))
        }
        let base = CGRect(x: cx - r * 0.7, y: cy, width: r * 1.4, height: r * 0.42)
        context.fill(RoundedRectangle(cornerRadius: 4).path(in: base), with: .color(color))
    }

    private func drawRainDrops(in context: inout GraphicsContext, cx: CGFloat, cy: CGFloat, r: CGFloat, count: Int = 3) {
        let blue = Color(red: 0.31, green: 0.63, blue: 1.0)
        let offsets: [CGFloat] = count == 2 ? [-0.4, 0.4] : [-0.45, 0, 0.45]
        for dx in offsets {
            var path = Path()
            let x = cx + dx * r
            path.move(to: CGPoint(x: x, y: cy + r * 0.3))
            path.addLine(to: CGPoint(x: x - 3, y: cy + r * 0.7))
            context.stroke(path, with: .color(blue), lineWidth: 2.5)
        }
    }

    private func drawLightning(in context: inout GraphicsContext, cx: CGFloat, cy: CGFloat, r: CGFloat) {
        let yellow = Color(red: 1.0, green: 0.88, blue: 0.25)
        var path = Path()
        path.move(to: CGPoint(x: cx + 2, y: cy - r * 0.15))
        path.addLine(to: CGPoint(x: cx - r * 0.22, y: cy + r * 0.25))
        path.addLine(to: CGPoint(x: cx + 4, y: cy + r * 0.2))
        path.addLine(to: CGPoint(x: cx - r * 0.12, y: cy + r * 0.65))
        context.stroke(path, with: .color(yellow), lineWidth: 3)
    }

    private func drawArrow(in context: inout GraphicsContext, cx: CGFloat, cy: CGFloat, r: CGFloat) {
        let gray = Color(red: 0.63, green: 0.69, blue: 0.75)
        var line = Path()
        line.move(to: CGPoint(x: cx - r, y: cy))
        line.addLine(to: CGPoint(x: cx + r, y: cy))
        context.stroke(line, with: .color(gray), lineWidth: 3)

        var head = Path()
        head.move(to: CGPoint(x: cx + r, y: cy))
        head.addLine(to: CGPoint(x: cx + r * 0.6, y: cy - r * 0.3))
        head.move(to: CGPoint(x: cx + r, y: cy))
        head.addLine(to: CGPoint(x: cx + r * 0.6, y: cy + r * 0.3))
        context.stroke(head, with: .color(gray), lineWidth: 3)
    }
}
