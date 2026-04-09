import Foundation

// MARK: - Live Data (from /api/live)

struct LiveData: Codable {
    let timestamp: String
    let demoMode: Bool
    let sensorOk: Bool
    let current: SensorReadings
    let trends: SensorTrends
    let weather: WeatherData
    let settings: SettingsData
    let history: HistoryArrays
    let fft: FFTData?

    enum CodingKeys: String, CodingKey {
        case timestamp
        case demoMode = "demo_mode"
        case sensorOk = "sensor_ok"
        case current, trends, weather, settings, history, fft
    }
}

struct FFTData: Codable {
    let sampleRate: Double
    let bins: Int
    let temperature: [Double]
    let humidity: [Double]
    let pressure: [Double]

    enum CodingKeys: String, CodingKey {
        case sampleRate = "sample_rate"
        case bins, temperature, humidity, pressure
    }
}

struct SensorReadings: Codable {
    let temperature: Double
    let humidity: Double
    let pressure: Double
    let eco2: Double
    let iaq: Double
    let gasResistance: Double

    enum CodingKeys: String, CodingKey {
        case temperature, humidity, pressure, eco2, iaq
        case gasResistance = "gas_resistance"
    }
}

struct SensorTrends: Codable {
    let temperature: Double
    let humidity: Double
    let pressure: Double
}

struct WeatherData: Codable {
    let jetzt: WeatherEntry
    let prognose: WeatherEntry
}

struct WeatherEntry: Codable {
    let text: String
    let detail: String
    let icon: String
}

struct SettingsData: Codable {
    let utcOffsetMin: Int
    let sensorIntervalMs: Int
    let logDays: Int
    let gasIntervalS: Int
    let heaterFilter: Bool
    let heaterBlankingMs: Int
    let nightModeAuto: Bool
    let nightStartH: Int
    let nightEndH: Int
    let nightBrightness: Int

    enum CodingKeys: String, CodingKey {
        case utcOffsetMin = "utc_offset_min"
        case sensorIntervalMs = "sensor_interval_ms"
        case logDays = "log_days"
        case gasIntervalS = "gas_interval_s"
        case heaterFilter = "heater_filter"
        case heaterBlankingMs = "heater_blanking_ms"
        case nightModeAuto = "night_mode_auto"
        case nightStartH = "night_start_h"
        case nightEndH = "night_end_h"
        case nightBrightness = "night_brightness"
    }
}

struct HistoryArrays: Codable {
    let temperature: [Double]
    let humidity: [Double]
    let pressure: [Double]
}

// MARK: - History Point (from /api/history)

struct HistoryPoint: Codable, Identifiable {
    var id: String { "\(date)_\(minute)" }
    let date: String
    let time: String
    let minute: Int
    let temperature: Double
    let humidity: Double
    let pressure: Double
    let eco2: Double
}

// MARK: - Settings response from GET /api/settings (string values)

struct RawSettings: Codable {
    var utcOffsetMin: String?
    var sensorIntervalMs: String?
    var logDays: String?
    var gasIntervalS: String?
    var heaterFilter: String?
    var heaterBlankingMs: String?
    var nightModeAuto: String?
    var nightStartH: String?
    var nightEndH: String?
    var nightBrightness: String?
    var spikeThreshold: String?

    enum CodingKeys: String, CodingKey {
        case utcOffsetMin = "utc_offset_min"
        case sensorIntervalMs = "sensor_interval_ms"
        case logDays = "log_days"
        case gasIntervalS = "gas_interval_s"
        case heaterFilter = "heater_filter"
        case heaterBlankingMs = "heater_blanking_ms"
        case nightModeAuto = "night_mode_auto"
        case nightStartH = "night_start_h"
        case nightEndH = "night_end_h"
        case nightBrightness = "night_brightness"
        case spikeThreshold = "spike_threshold"
    }
}

// MARK: - Editable settings model for the UI

struct EditableSettings {
    var utcOffsetMin: Int = 60
    var sensorIntervalMs: Int = 100
    var logDays: Int = 7
    var gasIntervalS: Int = 10
    var heaterFilter: Bool = false
    var heaterBlankingMs: Int = 3500
    var nightModeAuto: Bool = true
    var nightStartH: Int = 22
    var nightEndH: Int = 7
    var nightBrightness: Int = 30

    init() {}

    init(from raw: RawSettings) {
        utcOffsetMin = Int(raw.utcOffsetMin ?? "60") ?? 60
        sensorIntervalMs = Int(raw.sensorIntervalMs ?? "100") ?? 100
        logDays = Int(raw.logDays ?? "7") ?? 7
        gasIntervalS = Int(raw.gasIntervalS ?? "10") ?? 10
        heaterFilter = raw.heaterFilter == "1"
        heaterBlankingMs = Int(raw.heaterBlankingMs ?? "3500") ?? 3500
        nightModeAuto = raw.nightModeAuto == "1"
        nightStartH = Int(raw.nightStartH ?? "22") ?? 22
        nightEndH = Int(raw.nightEndH ?? "7") ?? 7
        nightBrightness = Int(raw.nightBrightness ?? "30") ?? 30
    }

    func toDict() -> [String: Any] {
        [
            "utc_offset_min": utcOffsetMin,
            "sensor_interval_ms": sensorIntervalMs,
            "log_days": logDays,
            "gas_interval_s": gasIntervalS,
            "heater_filter": heaterFilter ? 1 : 0,
            "heater_blanking_ms": heaterBlankingMs,
            "night_mode_auto": nightModeAuto ? 1 : 0,
            "night_start_h": nightStartH,
            "night_end_h": nightEndH,
            "night_brightness": nightBrightness,
        ]
    }
}

// MARK: - Trend helpers

enum TrendDirection {
    case strongUp, up, stable, down, strongDown

    var arrow: String {
        switch self {
        case .strongUp: return "\u{2191}"
        case .up: return "\u{2197}"
        case .stable: return "\u{2192}"
        case .down: return "\u{2198}"
        case .strongDown: return "\u{2193}"
        }
    }

    var sfSymbol: String {
        switch self {
        case .strongUp: return "arrow.up"
        case .up: return "arrow.up.right"
        case .stable: return "arrow.right"
        case .down: return "arrow.down.right"
        case .strongDown: return "arrow.down"
        }
    }
}

func trendDirection(_ value: Double, threshold: Double) -> TrendDirection {
    if value > threshold * 2 { return .strongUp }
    if value > threshold { return .up }
    if value < -threshold * 2 { return .strongDown }
    if value < -threshold { return .down }
    return .stable
}
