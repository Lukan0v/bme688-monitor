#include "bme688.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <thread>
#include <mutex>
#include <atomic>
#include <dirent.h>

// Dark theme colors
struct Color { uint8_t r, g, b, a; };

static const Color BG           = {18, 18, 24, 255};
static const Color PANEL_BG     = {30, 30, 42, 255};
static const Color CARD_BG      = {40, 40, 56, 255};
static const Color TEXT_PRIMARY  = {230, 230, 240, 255};
static const Color TEXT_DIM      = {140, 140, 160, 255};
static const Color ACCENT_TEMP   = {255, 100, 80, 255};
static const Color ACCENT_HUM   = {80, 180, 255, 255};
static const Color ACCENT_PRES  = {120, 220, 140, 255};
static const Color ACCENT_CO2   = {255, 200, 60, 255};
static const Color EXIT_BG      = {180, 50, 50, 255};
static const Color EXIT_HOVER   = {220, 70, 70, 255};
static const Color GRID_LINE    = {50, 50, 70, 255};
static const Color ASTRO_BG     = {50, 50, 80, 255};
static const Color ASTRO_HOVER  = {70, 70, 100, 255};
static const Color SETTINGS_BG  = {60, 55, 80, 255};
static const Color SETTINGS_HOVER = {80, 75, 100, 255};
static const Color BTN_GREEN    = {40, 140, 40, 255};
static const Color BTN_GREEN_H  = {50, 170, 50, 255};

// --- Settings ---
static const char* CONFIG_PATH = "/home/luka/bme688-monitor/settings.conf";

struct Settings {
    int utc_offset_min = 60;        // UTC+1 (CET) in minutes
    int sensor_interval_ms = 100;   // fast read interval ms (sleep between reads)
    int log_days = 7;               // how many days to keep logs
    float spike_threshold = 0.05f;  // max relative jump per reading (5%)
    int gas_interval_s = 10;        // seconds between CO2/gas measurements
    bool heater_filter = false;     // blank temperature during heater cooldown
    int heater_blanking_ms = 3500;  // how long to blank temp after gas read (ms)
    bool night_mode_auto = true;    // automatic night mode
    int night_start_h = 22;         // night mode start hour (0-23)
    int night_end_h = 7;            // night mode end hour (0-23)
    int night_brightness = 30;      // brightness percentage during night (10-100)

    void save() {
        FILE* f = fopen(CONFIG_PATH, "w");
        if (!f) return;
        fprintf(f, "utc_offset_min=%d\n", utc_offset_min);
        fprintf(f, "sensor_interval_ms=%d\n", sensor_interval_ms);
        fprintf(f, "log_days=%d\n", log_days);
        fprintf(f, "spike_threshold=%.2f\n", spike_threshold);
        fprintf(f, "gas_interval_s=%d\n", gas_interval_s);
        fprintf(f, "heater_filter=%d\n", heater_filter ? 1 : 0);
        fprintf(f, "heater_blanking_ms=%d\n", heater_blanking_ms);
        fprintf(f, "night_mode_auto=%d\n", night_mode_auto ? 1 : 0);
        fprintf(f, "night_start_h=%d\n", night_start_h);
        fprintf(f, "night_end_h=%d\n", night_end_h);
        fprintf(f, "night_brightness=%d\n", night_brightness);
        fclose(f);
    }

    void load() {
        FILE* f = fopen(CONFIG_PATH, "r");
        if (!f) return;
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            int iv; float fv;
            if (sscanf(line, "utc_offset_min=%d", &iv) == 1) utc_offset_min = iv;
            else if (sscanf(line, "sensor_interval_ms=%d", &iv) == 1) sensor_interval_ms = std::clamp(iv, 50, 2000);
            else if (sscanf(line, "log_days=%d", &iv) == 1) log_days = std::clamp(iv, 1, 30);
            else if (sscanf(line, "spike_threshold=%f", &fv) == 1) spike_threshold = std::clamp(fv, 0.05f, 1.0f);
            else if (sscanf(line, "gas_interval_s=%d", &iv) == 1) gas_interval_s = std::clamp(iv, 5, 300);
            else if (sscanf(line, "heater_filter=%d", &iv) == 1) heater_filter = (iv != 0);
            else if (sscanf(line, "fft_filter=%d", &iv) == 1) heater_filter = (iv != 0);  // compat
            else if (sscanf(line, "heater_blanking_ms=%d", &iv) == 1) heater_blanking_ms = std::clamp(iv, 250, 6000);
            else if (sscanf(line, "night_mode_auto=%d", &iv) == 1) night_mode_auto = (iv != 0);
            else if (sscanf(line, "night_start_h=%d", &iv) == 1) night_start_h = std::clamp(iv, 0, 23);
            else if (sscanf(line, "night_end_h=%d", &iv) == 1) night_end_h = std::clamp(iv, 0, 23);
            else if (sscanf(line, "night_brightness=%d", &iv) == 1) night_brightness = std::clamp(iv, 10, 100);
        }
        fclose(f);
    }

    void reset() {
        utc_offset_min = 60;
        sensor_interval_ms = 100;
        log_days = 7;
        spike_threshold = 0.05f;
        gas_interval_s = 10;
        heater_filter = false;
        heater_blanking_ms = 3500;
        night_mode_auto = true;
        night_start_h = 22;
        night_end_h = 7;
        night_brightness = 30;
    }
};

static Settings g_settings;

// --- 7-day data logging ---
static const char* DATA_DIR = "/home/luka/bme688-monitor/data";

struct LogPoint {
    int minute_of_day;  // 0-1439
    int day_offset;     // 0 = today, -1 = yesterday, etc.
    float temperature, humidity, pressure, eco2;
};

static const char* WEB_DATA_PATH = "/home/luka/bme688-monitor/web_data.json";
static const char* SETTINGS_RELOAD_PATH = "/home/luka/bme688-monitor/settings_reload";
static const char* SYNC_STATUS_PATH = "/home/luka/bme688-monitor/sync_status";

static void save_log_point(float temp, float hum, float pres, float eco2) {
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char fname[256];
    snprintf(fname, sizeof(fname), "%s/%04d-%02d-%02d.csv", DATA_DIR,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    FILE* f = fopen(fname, "a");
    if (!f) return;
    fprintf(f, "%02d:%02d,%.2f,%.2f,%.2f,%.0f\n",
            tm->tm_hour, tm->tm_min, temp, hum, pres, eco2);
    fclose(f);
}

static std::vector<LogPoint> load_log_history() {
    std::vector<LogPoint> log;
    time_t now = time(nullptr);
    struct tm* tm_now = localtime(&now);
    (void)tm_now;

    DIR* dir = opendir(DATA_DIR);
    if (!dir) return log;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/%s", DATA_DIR, entry->d_name);

        // Parse date from filename YYYY-MM-DD.csv
        int y, m, d;
        if (sscanf(entry->d_name, "%d-%d-%d.csv", &y, &m, &d) != 3) continue;

        struct tm file_tm = {};
        file_tm.tm_year = y - 1900;
        file_tm.tm_mon = m - 1;
        file_tm.tm_mday = d;
        file_tm.tm_isdst = -1;
        time_t file_t = mktime(&file_tm);
        int day_diff = (int)difftime(now, file_t) / 86400;

        // Skip files older than configured days, delete them
        if (day_diff > g_settings.log_days) {
            remove(fpath);
            continue;
        }
        // Calculate day_offset (0=today, -1=yesterday, etc.)
        int day_offset = -day_diff;

        FILE* f = fopen(fpath, "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            int hh, mm;
            float temp, hum, pres, eco2;
            if (sscanf(line, "%d:%d,%f,%f,%f,%f", &hh, &mm, &temp, &hum, &pres, &eco2) == 6) {
                log.push_back({hh * 60 + mm, day_offset, temp, hum, pres, eco2});
            }
        }
        fclose(f);
    }
    closedir(dir);

    // Sort by day_offset then minute
    std::sort(log.begin(), log.end(), [](const LogPoint& a, const LogPoint& b) {
        if (a.day_offset != b.day_offset) return a.day_offset < b.day_offset;
        return a.minute_of_day < b.minute_of_day;
    });
    return log;
}

static const int HISTORY_SIZE = 120;      // 60s at 0.5s/point for temp/hum/press

// --- FFT-based notch filter for heater interference removal ---
// Radix-2 Cooley-Tukey FFT (in-place, iterative)
static const int FFT_N = 512;  // must be power of 2

struct Complex { float re, im; };

static void fft(Complex* x, int n, bool inverse) {
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    // Butterfly
    for (int len = 2; len <= n; len <<= 1) {
        float angle = 2.0f * M_PI / len * (inverse ? -1 : 1);
        Complex wlen = {cosf(angle), sinf(angle)};
        for (int i = 0; i < n; i += len) {
            Complex w = {1, 0};
            for (int j = 0; j < len / 2; j++) {
                Complex u = x[i + j];
                Complex v = {w.re * x[i + j + len/2].re - w.im * x[i + j + len/2].im,
                             w.re * x[i + j + len/2].im + w.im * x[i + j + len/2].re};
                x[i + j] = {u.re + v.re, u.im + v.im};
                x[i + j + len/2] = {u.re - v.re, u.im - v.im};
                float tmp = w.re * wlen.re - w.im * wlen.im;
                w.im = w.re * wlen.im + w.im * wlen.re;
                w.re = tmp;
            }
        }
    }
    if (inverse) {
        for (int i = 0; i < n; i++) { x[i].re /= n; x[i].im /= n; }
    }
}



struct Co2Point {
    float value;
    int minute_of_day;  // 0-1439
};

static const int RAW_BUF_SIZE = 1024;  // ~100s at 10Hz

struct SensorHistory {
    std::deque<float> temperature;
    std::deque<float> humidity;
    std::deque<float> pressure;
    std::deque<Co2Point> eco2;  // 24h daily monitor with timestamps

    // Raw sample ring buffers (at sensor rate ~10Hz) for FFT
    std::deque<float> raw_temp;
    std::deque<float> raw_hum;
    std::deque<float> raw_pres;

    // 5-minute trend buffers (at chart rate ~2Hz = 600 samples)
    static const int TREND_BUF_SIZE = 600;
    std::deque<float> trend_temp;
    std::deque<float> trend_hum;
    std::deque<float> trend_pres;

    // Accumulator for compressing fast reads into 0.5s points
    float acc_temp = 0, acc_hum = 0, acc_pres = 0;
    int acc_count = 0;
    int acc_temp_count = 0;  // separate count for temp (may skip heater samples)

    void push_raw(float t, float h, float p) {
        auto add_raw = [](std::deque<float>& q, float v) {
            q.push_back(v);
            if ((int)q.size() > RAW_BUF_SIZE) q.pop_front();
        };
        add_raw(raw_temp, t);
        add_raw(raw_hum, h);
        add_raw(raw_pres, p);
    }

    void accumulate(const BME688Data& d) {
        acc_temp += d.temperature;
        acc_temp_count++;
        acc_hum += d.humidity;
        acc_pres += d.pressure;
        acc_count++;
        push_raw(d.temperature, d.humidity, d.pressure);
    }

    void accumulate_no_temp(const BME688Data& d) {
        // Skip temperature (heater interference), keep hum/pres
        acc_hum += d.humidity;
        acc_pres += d.pressure;
        acc_count++;
        // For raw buffer: hold last temp, store hum/pres
        float last_t = raw_temp.empty() ? 0.0f : raw_temp.back();
        push_raw(last_t, d.humidity, d.pressure);
    }

    void flush() {
        if (acc_count == 0) return;
        auto add = [](std::deque<float>& q, float v, int max_size) {
            q.push_back(v);
            if ((int)q.size() > max_size) q.pop_front();
        };
        // Temperature: use temp-specific count, or interpolate from last known
        float temp_val;
        if (acc_temp_count > 0) {
            temp_val = acc_temp / acc_temp_count;
        } else if (!temperature.empty()) {
            temp_val = temperature.back();  // hold last good value
        } else {
            temp_val = 0;
        }
        add(temperature, temp_val, HISTORY_SIZE);
        float hum_val = acc_hum / acc_count;
        float pres_val = acc_pres / acc_count;
        add(humidity, hum_val, HISTORY_SIZE);
        add(pressure, pres_val, HISTORY_SIZE);
        // Also push to 5-minute trend buffers
        add(trend_temp, temp_val, TREND_BUF_SIZE);
        add(trend_hum, hum_val, TREND_BUF_SIZE);
        add(trend_pres, pres_val, TREND_BUF_SIZE);
        acc_temp = acc_hum = acc_pres = 0;
        acc_count = 0;
        acc_temp_count = 0;
    }

    void push_co2(float eco2_val) {
        time_t t = time(nullptr);
        struct tm* tm = localtime(&t);
        int mod = tm->tm_hour * 60 + tm->tm_min;
        // Remove old points from previous day cycle or same minute
        while (!eco2.empty() && eco2.front().minute_of_day > mod + 1 && eco2.size() > 1) {
            eco2.pop_front();  // wrapped past midnight
        }
        eco2.push_back({eco2_val, mod});
    }
};

static void save_web_data(const BME688Data& vals, const SensorHistory& hist,
                           bool is_demo, bool sensor_working,
                           float trend_t, float trend_h, float trend_p,
                           const char* jetzt_text, const char* jetzt_detail, const char* jetzt_icon,
                           const char* prog_text, const char* prog_detail, const char* prog_icon) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", WEB_DATA_PATH);
    FILE* f = fopen(tmp_path, "w");
    if (!f) return;

    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char timestr[64];
    snprintf(timestr, sizeof(timestr), "%04d-%02d-%02dT%02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    fprintf(f, "{\n");
    fprintf(f, "  \"timestamp\": \"%s\",\n", timestr);
    fprintf(f, "  \"demo_mode\": %s,\n", is_demo ? "true" : "false");
    fprintf(f, "  \"sensor_ok\": %s,\n", sensor_working ? "true" : "false");
    fprintf(f, "  \"current\": {\n");
    fprintf(f, "    \"temperature\": %.2f,\n", vals.temperature);
    fprintf(f, "    \"humidity\": %.2f,\n", vals.humidity);
    fprintf(f, "    \"pressure\": %.2f,\n", vals.pressure);
    fprintf(f, "    \"eco2\": %.0f,\n", vals.eco2);
    fprintf(f, "    \"iaq\": %.1f,\n", vals.iaq);
    fprintf(f, "    \"gas_resistance\": %.1f\n", vals.gas_resistance);
    fprintf(f, "  },\n");
    fprintf(f, "  \"trends\": {\n");
    fprintf(f, "    \"temperature\": %.6f,\n", trend_t);
    fprintf(f, "    \"humidity\": %.6f,\n", trend_h);
    fprintf(f, "    \"pressure\": %.6f\n", trend_p);
    fprintf(f, "  },\n");
    fprintf(f, "  \"weather\": {\n");
    fprintf(f, "    \"jetzt\": {\"text\": \"%s\", \"detail\": \"%s\", \"icon\": \"%s\"},\n",
            jetzt_text ? jetzt_text : "", jetzt_detail ? jetzt_detail : "", jetzt_icon ? jetzt_icon : "");
    fprintf(f, "    \"prognose\": {\"text\": \"%s\", \"detail\": \"%s\", \"icon\": \"%s\"}\n",
            prog_text ? prog_text : "", prog_detail ? prog_detail : "", prog_icon ? prog_icon : "");
    fprintf(f, "  },\n");
    fprintf(f, "  \"settings\": {\n");
    fprintf(f, "    \"utc_offset_min\": %d,\n", g_settings.utc_offset_min);
    fprintf(f, "    \"sensor_interval_ms\": %d,\n", g_settings.sensor_interval_ms);
    fprintf(f, "    \"log_days\": %d,\n", g_settings.log_days);
    fprintf(f, "    \"gas_interval_s\": %d,\n", g_settings.gas_interval_s);
    fprintf(f, "    \"heater_filter\": %s,\n", g_settings.heater_filter ? "true" : "false");
    fprintf(f, "    \"heater_blanking_ms\": %d,\n", g_settings.heater_blanking_ms);
    fprintf(f, "    \"night_mode_auto\": %s,\n", g_settings.night_mode_auto ? "true" : "false");
    fprintf(f, "    \"night_start_h\": %d,\n", g_settings.night_start_h);
    fprintf(f, "    \"night_end_h\": %d,\n", g_settings.night_end_h);
    fprintf(f, "    \"night_brightness\": %d\n", g_settings.night_brightness);
    fprintf(f, "  },\n");

    int n_hist = std::min((int)hist.temperature.size(), 120);
    int start = (int)hist.temperature.size() - n_hist;
    fprintf(f, "  \"history\": {\n");
    fprintf(f, "    \"temperature\": [");
    for (int i = 0; i < n_hist; i++) fprintf(f, "%s%.2f", i ? "," : "", hist.temperature[start + i]);
    fprintf(f, "],\n    \"humidity\": [");
    for (int i = 0; i < n_hist; i++) fprintf(f, "%s%.2f", i ? "," : "", hist.humidity[start + i]);
    fprintf(f, "],\n    \"pressure\": [");
    for (int i = 0; i < n_hist; i++) fprintf(f, "%s%.2f", i ? "," : "", hist.pressure[start + i]);
    fprintf(f, "]\n  },\n");

    // FFT spectrum data (computed from chart-rate 2Hz data)
    float sample_rate = 2.0f;
    const char* fft_names[] = {"temperature", "humidity", "pressure"};
    const std::deque<float>* fft_sources[] = {&hist.temperature, &hist.humidity, &hist.pressure};
    fprintf(f, "  \"fft\": {\n");
    fprintf(f, "    \"sample_rate\": %.1f,\n", sample_rate);
    fprintf(f, "    \"bins\": %d,\n", FFT_N / 2);
    for (int ch = 0; ch < 3; ch++) {
        const std::deque<float>& src = *fft_sources[ch];
        int sn = (int)src.size();
        fprintf(f, "    \"%s\": [", fft_names[ch]);
        if (sn >= 16) {
            int use_n = std::min(sn, FFT_N);
            Complex buf[FFT_N] = {};
            float mean = 0;
            for (int i = 0; i < use_n; i++) mean += src[sn - use_n + i];
            mean /= use_n;
            for (int i = 0; i < use_n; i++) {
                float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (use_n - 1)));
                buf[i] = {(src[sn - use_n + i] - mean) * w, 0};
            }
            fft(buf, FFT_N, false);
            int num_bins = FFT_N / 2;
            float max_mag = 0;
            float mags[FFT_N / 2];
            for (int i = 1; i < num_bins; i++) {
                mags[i] = sqrtf(buf[i].re * buf[i].re + buf[i].im * buf[i].im) * 2.0f / FFT_N;
                if (mags[i] > max_mag) max_mag = mags[i];
            }
            mags[0] = 0;
            if (max_mag < 1e-9f) max_mag = 1e-9f;
            // Output dB values (capped at -80dB)
            for (int i = 0; i < num_bins; i++) {
                float db = mags[i] > 1e-12f ? 20.0f * log10f(mags[i] / max_mag) : -80.0f;
                if (db < -80.0f) db = -80.0f;
                fprintf(f, "%s%.1f", i ? "," : "", db);
            }
        }
        fprintf(f, "]%s\n", ch < 2 ? "," : "");
    }
    fprintf(f, "  }\n}\n");
    fclose(f);
    rename(tmp_path, WEB_DATA_PATH);
}

static void set_color(SDL_Renderer* r, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void fill_rounded_rect(SDL_Renderer* renderer, SDL_Rect rect, int radius, Color c) {
    set_color(renderer, c);
    SDL_Rect center = {rect.x + radius, rect.y, rect.w - 2 * radius, rect.h};
    SDL_RenderFillRect(renderer, &center);
    SDL_Rect left = {rect.x, rect.y + radius, radius, rect.h - 2 * radius};
    SDL_RenderFillRect(renderer, &left);
    SDL_Rect right = {rect.x + rect.w - radius, rect.y + radius, radius, rect.h - 2 * radius};
    SDL_RenderFillRect(renderer, &right);
    auto fill_circle = [&](int cx, int cy, int rad) {
        for (int dy = -rad; dy <= rad; dy++) {
            int dx = (int)sqrt(rad * rad - dy * dy);
            SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
        }
    };
    fill_circle(rect.x + radius, rect.y + radius, radius);
    fill_circle(rect.x + rect.w - radius - 1, rect.y + radius, radius);
    fill_circle(rect.x + radius, rect.y + rect.h - radius - 1, radius);
    fill_circle(rect.x + rect.w - radius - 1, rect.y + rect.h - radius - 1, radius);
}

// Text texture cache to avoid re-rendering text every frame
#include <unordered_map>

struct TextCacheEntry {
    SDL_Texture* tex;
    int w, h;
    uint32_t last_used;
};

static std::unordered_map<uint64_t, TextCacheEntry> text_cache;
static uint32_t text_frame = 0;

static uint64_t text_hash(TTF_Font* font, const char* text, Color c) {
    uint64_t h = (uint64_t)(uintptr_t)font;
    h ^= ((uint64_t)c.r << 24) | ((uint64_t)c.g << 16) | ((uint64_t)c.b << 8) | c.a;
    for (const char* p = text; *p; p++) {
        h = h * 31 + (uint8_t)*p;
    }
    return h;
}

static void text_cache_cleanup(uint32_t frame) {
    // Every 300 frames (~5s), remove entries unused for 600 frames (~10s)
    if (frame % 300 != 0) return;
    for (auto it = text_cache.begin(); it != text_cache.end(); ) {
        if (frame - it->second.last_used > 600) {
            SDL_DestroyTexture(it->second.tex);
            it = text_cache.erase(it);
        } else {
            ++it;
        }
    }
}

static void text_cache_destroy() {
    for (auto& [k, e] : text_cache) SDL_DestroyTexture(e.tex);
    text_cache.clear();
}

static void draw_text(SDL_Renderer* renderer, TTF_Font* font, const char* text, int x, int y, Color c, bool center = false) {
    if (!text || !text[0]) return;
    uint64_t key = text_hash(font, text, c);
    auto it = text_cache.find(key);
    if (it != text_cache.end()) {
        it->second.last_used = text_frame;
        SDL_Rect dst = {center ? x - it->second.w / 2 : x, center ? y - it->second.h / 2 : y, it->second.w, it->second.h};
        SDL_RenderCopy(renderer, it->second.tex, nullptr, &dst);
        return;
    }
    SDL_Color sc = {c.r, c.g, c.b, c.a};
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text, sc);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) return;
    int w, h;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    text_cache[key] = {tex, w, h, text_frame};
    SDL_Rect dst = {center ? x - w / 2 : x, center ? y - h / 2 : y, w, h};
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
}

// Health zone definition: up to 3 zones (green/yellow/red)
// Each zone is [low, high] in sensor units
struct HealthZone {
    float low, high;
    Color color;
};

struct HealthZones {
    HealthZone zones[5];
    int count;
};

// Schlafzimmer-Empfehlungen (UBA / Gesundheitsministerium):
// Temperatur: 16-20°C gut, 14-16 & 20-24 mäßig, <14 & >24 schlecht
// Feuchte: 40-60% gut, 30-40 & 60-70 mäßig, <30 & >70 schlecht
// CO2: <1000 gut, 1000-1500 mäßig, >1500 schlecht
static const HealthZones ZONES_TEMP = {{
    {0, 14,   {200, 60, 60, 20}},   // rot: zu kalt
    {14, 16,  {200, 180, 40, 20}},  // gelb
    {16, 20,  {40, 180, 60, 20}},   // grün: optimal
    {20, 24,  {200, 180, 40, 20}},  // gelb
    {24, 50,  {200, 60, 60, 20}},   // rot: zu warm
}, 5};

static const HealthZones ZONES_HUM = {{
    {0, 30,   {200, 60, 60, 20}},   // rot: zu trocken
    {30, 40,  {200, 180, 40, 20}},  // gelb
    {40, 60,  {40, 180, 60, 20}},   // grün: optimal
    {60, 70,  {200, 180, 40, 20}},  // gelb
    {70, 100, {200, 60, 60, 20}},   // rot: zu feucht
}, 5};

static const HealthZones ZONES_CO2 = {{
    {0, 1000,    {40, 180, 60, 20}},   // grün: gut
    {1000, 1500, {200, 180, 40, 20}},  // gelb: lüften
    {1500, 5000, {200, 60, 60, 20}},   // rot: ungesund
}, 3};

static const HealthZones ZONES_NONE = {{}, 0};

static void draw_chart(SDL_Renderer* renderer, TTF_Font* font_small,
                       const std::deque<float>& data, SDL_Rect area, Color color,
                       const char* label, const char* unit,
                       float fixed_min = NAN, float fixed_max = NAN,
                       const HealthZones& zones = ZONES_NONE,
                       float interp_tip = NAN, float interp_frac = 0.0f,
                       int max_points = HISTORY_SIZE,
                       float min_range = 1.0f) {
    fill_rounded_rect(renderer, area, 8, CARD_BG);

    // Chart title in accent color (bold look via font_small at natural size)
    draw_text(renderer, font_small, label, area.x + 10, area.y + 6, color);

    // Show interpolated value if available, otherwise last data point
    float display_val = !std::isnan(interp_tip) ? interp_tip : (data.empty() ? 0 : data.back());
    if (!data.empty()) {
        char val_str[32];
        snprintf(val_str, sizeof(val_str), "%.1f %s", display_val, unit);
        draw_text(renderer, font_small, val_str, area.x + area.w - 10 - 8 * (int)strlen(val_str), area.y + 6, color);
    }

    int chart_x = area.x + 50;
    int chart_y = area.y + 28;
    int chart_w = area.w - 60;
    int chart_h = area.h - 40;

    if (data.size() < 2 || chart_w < 10 || chart_h < 10) return;

    float min_val, max_val;
    if (!std::isnan(fixed_min) && !std::isnan(fixed_max)) {
        min_val = fixed_min;
        max_val = fixed_max;
    } else {
        min_val = *std::min_element(data.begin(), data.end());
        max_val = *std::max_element(data.begin(), data.end());
        // Enforce minimum range
        float range = max_val - min_val;
        if (range < min_range) {
            float mid = (min_val + max_val) / 2.0f;
            min_val = mid - min_range / 2.0f;
            max_val = mid + min_range / 2.0f;
        }
        float margin = (max_val - min_val) * 0.1f;
        if (margin < 0.5f) margin = 0.5f;
        min_val -= margin;
        max_val += margin;
    }

    // Draw health zones as background bands
    if (zones.count > 0) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        for (int z = 0; z < zones.count; z++) {
            float z_low = std::clamp((zones.zones[z].low - min_val) / (max_val - min_val), 0.0f, 1.0f);
            float z_high = std::clamp((zones.zones[z].high - min_val) / (max_val - min_val), 0.0f, 1.0f);
            if (z_low >= z_high) continue;
            int y_top = chart_y + chart_h - (int)(z_high * chart_h);
            int y_bot = chart_y + chart_h - (int)(z_low * chart_h);
            SDL_Rect zone_rect = {chart_x, y_top, chart_w, y_bot - y_top};
            set_color(renderer, zones.zones[z].color);
            SDL_RenderFillRect(renderer, &zone_rect);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Grid lines
    set_color(renderer, GRID_LINE);
    for (int i = 0; i <= 3; i++) {
        int gy = chart_y + chart_h - (i * chart_h / 3);
        SDL_RenderDrawLine(renderer, chart_x, gy, chart_x + chart_w, gy);
        float gv = min_val + (max_val - min_val) * i / 3.0f;
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%.0f", gv);
        draw_text(renderer, font_small, lbl, area.x + 4, gy - 6, {160, 180, 220, 255});
    }

    int n = (int)data.size();
    float step = (float)chart_w / (max_points - 1);

    // Smooth scrolling: all points slide left continuously with interp_frac
    float smooth_offset = (float)(max_points - n) - interp_frac;

    // Interpolated tip always sits at the right edge
    bool has_tip = !std::isnan(interp_tip) && n >= 1;
    float tip_norm = 0;
    float tip_x_f = (float)(max_points - 1) * step;  // right edge
    if (has_tip) {
        tip_norm = std::clamp((interp_tip - min_val) / (max_val - min_val), 0.0f, 1.0f);
    }

    // Filled area - continuous fill between all data points
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    set_color(renderer, {color.r, color.g, color.b, 40});
    int bottom = chart_y + chart_h;
    // Fill between consecutive data points pixel by pixel
    for (int i = 0; i < n; i++) {
        float norm0 = std::clamp((data[i] - min_val) / (max_val - min_val), 0.0f, 1.0f);
        int x0 = chart_x + (int)((i + smooth_offset) * step);
        float norm1;
        int x1;
        if (i + 1 < n) {
            norm1 = std::clamp((data[i + 1] - min_val) / (max_val - min_val), 0.0f, 1.0f);
            x1 = chart_x + (int)((i + 1 + smooth_offset) * step);
        } else if (has_tip) {
            norm1 = tip_norm;
            x1 = chart_x + (int)tip_x_f;
        } else {
            // Last point, just draw one line
            if (x0 >= chart_x && x0 <= chart_x + chart_w) {
                int y = bottom - (int)(norm0 * chart_h);
                SDL_RenderDrawLine(renderer, x0, y, x0, bottom);
            }
            continue;
        }
        // Draw vertical lines for each pixel between x0 and x1
        int px_start = std::max(x0, chart_x);
        int px_end = std::min(x1, chart_x + chart_w);
        for (int px = px_start; px <= px_end; px++) {
            float t = (x0 == x1) ? 0.0f : (float)(px - x0) / (float)(x1 - x0);
            float yn = norm0 + (norm1 - norm0) * t;
            int y = bottom - (int)(yn * chart_h);
            SDL_RenderDrawLine(renderer, px, y, px, bottom);
        }
    }

    // Line
    set_color(renderer, color);
    for (int i = 1; i < n; i++) {
        float norm0 = std::clamp((data[i - 1] - min_val) / (max_val - min_val), 0.0f, 1.0f);
        float norm1 = std::clamp((data[i] - min_val) / (max_val - min_val), 0.0f, 1.0f);
        int x0 = chart_x + (int)((i - 1 + smooth_offset) * step);
        int y0 = chart_y + chart_h - (int)(norm0 * chart_h);
        int x1 = chart_x + (int)((i + smooth_offset) * step);
        int y1 = chart_y + chart_h - (int)(norm1 * chart_h);
        if (x1 < chart_x) continue;
        if (x0 < chart_x) {
            float t = (float)(chart_x - x0) / (float)(x1 - x0);
            y0 = y0 + (int)(t * (y1 - y0));
            x0 = chart_x;
        }
        SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
        SDL_RenderDrawLine(renderer, x0, y0 + 1, x1, y1 + 1);
    }
    // Line to tip
    if (has_tip && n >= 1) {
        float last_norm = std::clamp((data.back() - min_val) / (max_val - min_val), 0.0f, 1.0f);
        int x0 = std::max(chart_x, chart_x + (int)((n - 1 + smooth_offset) * step));
        int y0 = chart_y + chart_h - (int)(last_norm * chart_h);
        int x1 = chart_x + (int)tip_x_f;
        int y1 = chart_y + chart_h - (int)(tip_norm * chart_h);
        SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
        SDL_RenderDrawLine(renderer, x0, y0 + 1, x1, y1 + 1);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// --- Sun/Moon symbols ---
static void draw_sun_symbol(SDL_Renderer* renderer, int cx, int cy, int r, Color c) {
    set_color(renderer, c);
    // Filled circle
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt(r * r - dy * dy);
        SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
    // 8 rays
    for (int i = 0; i < 8; i++) {
        float angle = i * M_PI / 4.0f;
        int x1 = cx + (int)(cosf(angle) * (r + 2));
        int y1 = cy + (int)(sinf(angle) * (r + 2));
        int x2 = cx + (int)(cosf(angle) * (r + 5));
        int y2 = cy + (int)(sinf(angle) * (r + 5));
        SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
    }
}

static void draw_moon_symbol(SDL_Renderer* renderer, int cx, int cy, int r, Color c) {
    set_color(renderer, c);
    // Filled circle
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt(r * r - dy * dy);
        SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
    // Cut out crescent with dark circle offset to the right
    set_color(renderer, CARD_BG);
    int off = r * 2 / 3;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt(r * r - dy * dy);
        int start = cx + off - dx;
        int end = cx + off + dx;
        if (start < cx - r) start = cx - r;
        // Only draw the overlap part
        int left = std::max(start, cx - (int)sqrt(r * r - dy * dy));
        int right = std::min(end, cx + (int)sqrt(r * r - dy * dy));
        if (left < right) SDL_RenderDrawLine(renderer, left, cy + dy, right, cy + dy);
    }
}

// --- Gear symbol for settings button ---
static void draw_gear_symbol(SDL_Renderer* renderer, int cx, int cy, int r, Color c,
                             Color bg = {0, 0, 0, 0}) {
    set_color(renderer, c);
    // Outer ring + teeth as one solid shape
    int outer_r = r * 3 / 4;
    // Fill the entire outer circle first
    for (int dy = -outer_r; dy <= outer_r; dy++) {
        int dx = (int)sqrt(outer_r * outer_r - dy * dy);
        SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
    // 8 teeth radiating outward
    int teeth = 8;
    for (int i = 0; i < teeth; i++) {
        float angle = i * M_PI * 2.0f / teeth;
        float half_w = M_PI / teeth * 0.5f;
        for (int d = outer_r - 1; d <= r; d++) {
            float a1 = angle - half_w;
            float a2 = angle + half_w;
            int x1 = cx + (int)(cosf(a1) * d);
            int y1 = cy + (int)(sinf(a1) * d);
            int x2 = cx + (int)(cosf(a2) * d);
            int y2 = cy + (int)(sinf(a2) * d);
            SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
        }
    }
    // Cut out center hole (use bg color if provided, otherwise default dark)
    int hole_r = r * 2 / 7;
    if (hole_r > 1) {
        Color hole_c = (bg.a > 0) ? bg : Color{26, 26, 38, 255};
        set_color(renderer, hole_c);
        for (int dy = -hole_r; dy <= hole_r; dy++) {
            int dx = (int)sqrt(hole_r * hole_r - dy * dy);
            SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
        }
    }
}

// --- Solar/Lunar position calculations ---
static const double LAT = 50.0;  // ~central Germany
static const double LON = 10.0;

static double solar_altitude(int day_of_year, double hour) {
    double decl = 23.45 * sin((360.0 / 365.0) * (284 + day_of_year) * M_PI / 180.0);
    double ha = 15.0 * (hour - 12.0) + LON - 15.0;  // rough solar time offset
    double lat_r = LAT * M_PI / 180.0;
    double decl_r = decl * M_PI / 180.0;
    double ha_r = ha * M_PI / 180.0;
    double alt = asin(sin(lat_r) * sin(decl_r) + cos(lat_r) * cos(decl_r) * cos(ha_r));
    return alt * 180.0 / M_PI;
}

static double lunar_altitude(int day_of_year, double hour) {
    // Simplified: moon moves ~13° per day relative to sun, ~50min later each day
    double moon_offset = fmod(day_of_year * 12.37, 360.0);  // rough lunar phase shift
    double decl = 23.45 * sin((360.0 / 365.0) * (284 + day_of_year) * M_PI / 180.0 + moon_offset * M_PI / 180.0);
    double ha = 15.0 * (hour - 12.0) + LON - 15.0 + moon_offset;
    double lat_r = LAT * M_PI / 180.0;
    double decl_r = decl * M_PI / 180.0;
    double ha_r = ha * M_PI / 180.0;
    double alt = asin(sin(lat_r) * sin(decl_r) + cos(lat_r) * cos(decl_r) * cos(ha_r));
    return alt * 180.0 / M_PI;
}

static void draw_co2_daily(SDL_Renderer* renderer, TTF_Font* font_small,
                           const std::deque<Co2Point>& data, SDL_Rect area, Color color) {
    fill_rounded_rect(renderer, area, 8, CARD_BG);
    draw_text(renderer, font_small, "CO\xE2\x82\x82 (24h)", area.x + 10, area.y + 6, TEXT_DIM);

    if (!data.empty()) {
        char val_str[32];
        snprintf(val_str, sizeof(val_str), "%.0f ppm", data.back().value);
        draw_text(renderer, font_small, val_str, area.x + area.w - 10 - 8 * (int)strlen(val_str), area.y + 6, color);
    }

    int chart_x = area.x + 50;
    int chart_y = area.y + 28;
    int chart_w = area.w - 60;
    int chart_h = area.h - 52;  // room for time labels at bottom
    int bottom = chart_y + chart_h;

    if (chart_w < 10 || chart_h < 10) return;

    float min_val = 0, max_val = 2000;

    // Health zone backgrounds
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const HealthZones& zones = ZONES_CO2;
    for (int z = 0; z < zones.count; z++) {
        float z_low = std::clamp((zones.zones[z].low - min_val) / (max_val - min_val), 0.0f, 1.0f);
        float z_high = std::clamp((zones.zones[z].high - min_val) / (max_val - min_val), 0.0f, 1.0f);
        if (z_low >= z_high) continue;
        int y_top = bottom - (int)(z_high * chart_h);
        int y_bot = bottom - (int)(z_low * chart_h);
        SDL_Rect zone_rect = {chart_x, y_top, chart_w, y_bot - y_top};
        set_color(renderer, zones.zones[z].color);
        SDL_RenderFillRect(renderer, &zone_rect);
    }

    // Grid lines + Y labels
    set_color(renderer, GRID_LINE);
    for (int i = 0; i <= 3; i++) {
        int gy = bottom - (i * chart_h / 3);
        SDL_RenderDrawLine(renderer, chart_x, gy, chart_x + chart_w, gy);
        float gv = min_val + (max_val - min_val) * i / 3.0f;
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%.0f", gv);
        draw_text(renderer, font_small, lbl, area.x + 4, gy - 6, {160, 180, 220, 255});
    }

    // Time labels at bottom: 0, 3, 6, 9, 12, 15, 18, 21, 24
    for (int h = 0; h <= 24; h += 3) {
        int x = chart_x + (int)((float)h / 24.0f * chart_w);
        set_color(renderer, GRID_LINE);
        SDL_RenderDrawLine(renderer, x, chart_y, x, bottom);
        char tlbl[8];
        snprintf(tlbl, sizeof(tlbl), "%d", h);
        draw_text(renderer, font_small, tlbl, x, bottom + 4, {140, 180, 220, 255}, true);
    }

    // Get current day info for sun/moon
    time_t now_t = time(nullptr);
    struct tm* tm_now = localtime(&now_t);
    int day_of_year = tm_now->tm_yday + 1;
    int cur_minute = tm_now->tm_hour * 60 + tm_now->tm_min;

    // Draw sun altitude curve (yellow, dashed)
    // Scale: 0° at bottom, 90° at top (but we use chart_h * 0.3 for the curve area at the bottom)
    int astro_h = chart_h;  // full height for visibility
    float alt_scale = 90.0f;  // max altitude

    // Find sunrise/sunset
    int sunrise_min = -1, sunset_min = -1;
    for (int m = 0; m < 1440; m++) {
        double alt = solar_altitude(day_of_year, m / 60.0);
        if (alt > 0 && sunrise_min < 0) sunrise_min = m;
        if (alt <= 0 && sunrise_min >= 0 && sunset_min < 0) sunset_min = m;
    }

    // Draw sun curve
    set_color(renderer, {255, 200, 50, 60});
    int prev_sx = -1, prev_sy = -1;
    for (int m = 0; m < 1440; m += 2) {
        double alt = solar_altitude(day_of_year, m / 60.0);
        if (alt < 0) alt = 0;
        int sx = chart_x + (int)((float)m / 1440.0f * chart_w);
        int sy = bottom - (int)(alt / alt_scale * astro_h * 0.4f);
        if (prev_sx >= 0 && alt > 0) {
            SDL_RenderDrawLine(renderer, prev_sx, prev_sy, sx, sy);
        }
        prev_sx = sx;
        prev_sy = sy;
    }

    // Draw moon curve (silver/gray)
    set_color(renderer, {180, 180, 220, 40});
    prev_sx = -1; prev_sy = -1;
    for (int m = 0; m < 1440; m += 2) {
        double alt = lunar_altitude(day_of_year, m / 60.0);
        if (alt < 0) alt = 0;
        int sx = chart_x + (int)((float)m / 1440.0f * chart_w);
        int sy = bottom - (int)(alt / alt_scale * astro_h * 0.4f);
        if (prev_sx >= 0 && alt > 0) {
            SDL_RenderDrawLine(renderer, prev_sx, prev_sy, sx, sy);
        }
        prev_sx = sx;
        prev_sy = sy;
    }

    // Sunrise/sunset markers
    if (sunrise_min >= 0) {
        int sx = chart_x + (int)((float)sunrise_min / 1440.0f * chart_w);
        set_color(renderer, {255, 180, 50, 100});
        SDL_RenderDrawLine(renderer, sx, chart_y, sx, bottom);
        draw_sun_symbol(renderer, sx, bottom + 14, 5, {255, 200, 50, 255});
        char sr[16];
        snprintf(sr, sizeof(sr), "%d:%02d", sunrise_min / 60, sunrise_min % 60);
        draw_text(renderer, font_small, sr, sx + 10, bottom + 8, {255, 180, 50, 200});
    }
    if (sunset_min >= 0) {
        int sx = chart_x + (int)((float)sunset_min / 1440.0f * chart_w);
        set_color(renderer, {255, 120, 50, 100});
        SDL_RenderDrawLine(renderer, sx, chart_y, sx, bottom);
        draw_moon_symbol(renderer, sx, bottom + 14, 5, {200, 200, 240, 255});
        char ss[16];
        snprintf(ss, sizeof(ss), "%d:%02d", sunset_min / 60, sunset_min % 60);
        draw_text(renderer, font_small, ss, sx - 50, bottom + 8, {255, 120, 50, 200});
    }

    // Current time marker (vertical white dashed line)
    {
        int cx = chart_x + (int)((float)cur_minute / 1440.0f * chart_w);
        set_color(renderer, {255, 255, 255, 150});
        for (int y = chart_y; y < bottom; y += 4) {
            SDL_RenderDrawLine(renderer, cx, y, cx, std::min(y + 2, bottom));
        }
        // Small triangle at top
        SDL_RenderDrawLine(renderer, cx - 3, chart_y, cx + 3, chart_y);
        SDL_RenderDrawLine(renderer, cx - 2, chart_y + 1, cx + 2, chart_y + 1);
        SDL_RenderDrawLine(renderer, cx - 1, chart_y + 2, cx + 1, chart_y + 2);
    }

    // CO2 data - filled area + line
    int n = (int)data.size();
    if (n >= 2) {
        // Filled area
        set_color(renderer, {color.r, color.g, color.b, 40});
        for (int i = 0; i < n - 1; i++) {
            float norm0 = std::clamp((data[i].value - min_val) / (max_val - min_val), 0.0f, 1.0f);
            float norm1 = std::clamp((data[i + 1].value - min_val) / (max_val - min_val), 0.0f, 1.0f);
            int x0 = chart_x + (int)((float)data[i].minute_of_day / 1440.0f * chart_w);
            int x1 = chart_x + (int)((float)data[i + 1].minute_of_day / 1440.0f * chart_w);
            if (x1 <= x0) continue;
            for (int px = x0; px <= x1; px++) {
                float t = (float)(px - x0) / (float)(x1 - x0);
                float yn = norm0 + (norm1 - norm0) * t;
                int y = bottom - (int)(yn * chart_h);
                SDL_RenderDrawLine(renderer, px, y, px, bottom);
            }
        }

        // Line
        set_color(renderer, color);
        for (int i = 1; i < n; i++) {
            float norm0 = std::clamp((data[i - 1].value - min_val) / (max_val - min_val), 0.0f, 1.0f);
            float norm1 = std::clamp((data[i].value - min_val) / (max_val - min_val), 0.0f, 1.0f);
            int x0 = chart_x + (int)((float)data[i - 1].minute_of_day / 1440.0f * chart_w);
            int y0 = bottom - (int)(norm0 * chart_h);
            int x1 = chart_x + (int)((float)data[i].minute_of_day / 1440.0f * chart_w);
            int y1 = bottom - (int)(norm1 * chart_h);
            SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
            SDL_RenderDrawLine(renderer, x0, y0 + 1, x1, y1 + 1);
        }
    } else if (n == 1) {
        float norm = std::clamp((data[0].value - min_val) / (max_val - min_val), 0.0f, 1.0f);
        int x = chart_x + (int)((float)data[0].minute_of_day / 1440.0f * chart_w);
        int y = bottom - (int)(norm * chart_h);
        set_color(renderer, color);
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -2; dy <= 2; dy++)
                SDL_RenderDrawPoint(renderer, x + dx, y + dy);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// --- Fullscreen Astro View ---
static void draw_astro_view(SDL_Renderer* renderer, TTF_Font* font_big, TTF_Font* font_med,
                            TTF_Font* font_small, int win_w, int win_h,
                            const std::vector<LogPoint>& log, int view_day,
                            const BME688Data& current, const bool show[4]) {
    set_color(renderer, BG);
    SDL_RenderClear(renderer);

    time_t now_t = time(nullptr);
    struct tm* tm_now = localtime(&now_t);
    int day_of_year = tm_now->tm_yday + 1 + view_day;
    int cur_minute = tm_now->tm_hour * 60 + tm_now->tm_min;

    // Title
    char title[64];
    if (view_day == 0) {
        snprintf(title, sizeof(title), "Astro & Sensoren - Heute");
    } else {
        time_t view_t = now_t + view_day * 86400;
        struct tm* vtm = localtime(&view_t);
        snprintf(title, sizeof(title), "Astro & Sensoren - %02d.%02d.",
                 vtm->tm_mday, vtm->tm_mon + 1);
    }
    draw_text(renderer, font_med, title, 20, 16, TEXT_PRIMARY);

    int margin = 10;
    int chart_x = 10 + margin;
    int chart_w = win_w - chart_x - margin;
    int chart_y = 50;
    int chart_h = win_h - chart_y - 25;
    int bottom = chart_y + chart_h;

    fill_rounded_rect(renderer, {margin, chart_y - 4, win_w - 2 * margin, chart_h + 28}, 8, CARD_BG);

    // Find sunrise/sunset
    int sunrise_min = -1, sunset_min = -1;
    for (int m = 0; m < 1440; m++) {
        double alt = solar_altitude(day_of_year, m / 60.0);
        if (alt > 0 && sunrise_min < 0) sunrise_min = m;
        if (alt <= 0 && sunrise_min >= 0 && sunset_min < 0) sunset_min = m;
    }

    // Fill daytime area (subtle)
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (sunrise_min >= 0 && sunset_min >= 0) {
        int x0 = chart_x + (int)((float)sunrise_min / 1440.0f * chart_w);
        int x1 = chart_x + (int)((float)sunset_min / 1440.0f * chart_w);
        set_color(renderer, {255, 200, 50, 10});
        SDL_Rect day_rect = {x0, chart_y, x1 - x0, chart_h};
        SDL_RenderFillRect(renderer, &day_rect);
    }

    // Time grid
    set_color(renderer, GRID_LINE);
    for (int h = 0; h <= 24; h += 3) {
        int x = chart_x + (int)((float)h / 24.0f * chart_w);
        SDL_RenderDrawLine(renderer, x, chart_y, x, bottom);
        char tlbl[8];
        snprintf(tlbl, sizeof(tlbl), "%d", h);
        draw_text(renderer, font_small, tlbl, x, bottom + 4, {140, 180, 220, 255}, true);
    }

    // Sun curve (fills upper portion, normalized 0-90° → full chart height)
    float alt_scale = 90.0f;
    set_color(renderer, {255, 200, 50, 140});
    int prev_sx = -1, prev_sy = -1;
    for (int m = 0; m < 1440; m += 2) {
        double alt = solar_altitude(day_of_year, m / 60.0);
        if (alt < 0) { prev_sx = -1; continue; }
        int sx = chart_x + (int)((float)m / 1440.0f * chart_w);
        int sy = bottom - (int)((float)alt / alt_scale * chart_h * 0.95f);
        if (prev_sx >= 0) SDL_RenderDrawLine(renderer, prev_sx, prev_sy, sx, sy);
        prev_sx = sx; prev_sy = sy;
    }

    // Moon curve
    set_color(renderer, {180, 180, 230, 80});
    prev_sx = -1; prev_sy = -1;
    for (int m = 0; m < 1440; m += 2) {
        double alt = lunar_altitude(day_of_year, m / 60.0);
        if (alt < 0) { prev_sx = -1; continue; }
        int sx = chart_x + (int)((float)m / 1440.0f * chart_w);
        int sy = bottom - (int)((float)alt / alt_scale * chart_h * 0.95f);
        if (prev_sx >= 0) SDL_RenderDrawLine(renderer, prev_sx, prev_sy, sx, sy);
        prev_sx = sx; prev_sy = sy;
    }

    // Sunrise/sunset symbols
    if (sunrise_min >= 0) {
        int sx = chart_x + (int)((float)sunrise_min / 1440.0f * chart_w);
        set_color(renderer, {255, 180, 50, 60});
        SDL_RenderDrawLine(renderer, sx, chart_y, sx, bottom);
        draw_sun_symbol(renderer, sx, chart_y + 12, 7, {255, 200, 50, 255});
        char sr[16]; snprintf(sr, sizeof(sr), "%d:%02d", sunrise_min / 60, sunrise_min % 60);
        draw_text(renderer, font_small, sr, sx + 14, chart_y + 6, {255, 180, 50, 220});
    }
    if (sunset_min >= 0) {
        int sx = chart_x + (int)((float)sunset_min / 1440.0f * chart_w);
        set_color(renderer, {255, 120, 50, 60});
        SDL_RenderDrawLine(renderer, sx, chart_y, sx, bottom);
        draw_moon_symbol(renderer, sx, chart_y + 12, 7, {200, 200, 240, 255});
        char ss[16]; snprintf(ss, sizeof(ss), "%d:%02d", sunset_min / 60, sunset_min % 60);
        draw_text(renderer, font_small, ss, sx - 55, chart_y + 6, {255, 120, 50, 220});
    }

    // Filter log for the selected day
    std::vector<const LogPoint*> day_data;
    for (const auto& p : log) {
        if (p.day_offset == view_day) day_data.push_back(&p);
    }

    // --- Overlay all 4 sensor values as lines, each normalized to its own range ---
    struct SensorLine {
        const char* label;
        const char* unit;
        Color color;
        float range_min, range_max;
        int field;  // 0=temp,1=hum,2=pres,3=co2
    };
    SensorLine lines[] = {
        {"Temp", "\xC2\xB0""C",  ACCENT_TEMP, 10, 35, 0},
        {"Feuchte", "%",         ACCENT_HUM,  0, 100, 1},
        {"Druck", "hPa",         ACCENT_PRES, 980, 1040, 2},
        {"CO\xE2\x82\x82", "ppm", ACCENT_CO2, 0, 2000, 3},
    };

    auto get_val = [](const LogPoint* p, int field) -> float {
        switch (field) {
            case 0: return p->temperature;
            case 1: return p->humidity;
            case 2: return p->pressure;
            default: return p->eco2;
        }
    };

    // Draw each sensor line + find min/max
    for (int s = 0; s < 4; s++) {
        if (!show[s]) continue;
        if (day_data.size() < 2) continue;

        // Find actual min/max for this sensor
        float vmin = 1e9, vmax = -1e9;
        int min_idx = 0, max_idx = 0;
        for (size_t i = 0; i < day_data.size(); i++) {
            float v = get_val(day_data[i], lines[s].field);
            if (v < vmin) { vmin = v; min_idx = (int)i; }
            if (v > vmax) { vmax = v; max_idx = (int)i; }
        }

        float smin = lines[s].range_min, smax = lines[s].range_max;

        // Draw line - gaps >3 min shown as gray connecting line
        for (size_t i = 1; i < day_data.size(); i++) {
            float v0 = get_val(day_data[i - 1], lines[s].field);
            float v1 = get_val(day_data[i], lines[s].field);
            float n0 = std::clamp((v0 - smin) / (smax - smin), 0.0f, 1.0f);
            float n1 = std::clamp((v1 - smin) / (smax - smin), 0.0f, 1.0f);
            int x0 = chart_x + (int)((float)day_data[i - 1]->minute_of_day / 1440.0f * chart_w);
            int y0 = bottom - (int)(n0 * chart_h);
            int x1 = chart_x + (int)((float)day_data[i]->minute_of_day / 1440.0f * chart_w);
            int y1 = bottom - (int)(n1 * chart_h);
            int gap = day_data[i]->minute_of_day - day_data[i - 1]->minute_of_day;
            if (gap > 3) {
                // Semi-transparent sensor-colored line for gap
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                set_color(renderer, {lines[s].color.r, lines[s].color.g, lines[s].color.b, 60});
                SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
            } else {
                set_color(renderer, lines[s].color);
                SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
            }
        }

        // Mark MIN with dot + value
        {
            float n = std::clamp((vmin - smin) / (smax - smin), 0.0f, 1.0f);
            int mx = chart_x + (int)((float)day_data[min_idx]->minute_of_day / 1440.0f * chart_w);
            int my = bottom - (int)(n * chart_h);
            // Small dot
            for (int dy = -3; dy <= 3; dy++)
                for (int dx = -3; dx <= 3; dx++)
                    if (dx * dx + dy * dy <= 9)
                        SDL_RenderDrawPoint(renderer, mx + dx, my + dy);
            char lbl[32];
            if (lines[s].field == 3 || lines[s].field == 2)
                snprintf(lbl, sizeof(lbl), "%.0f", vmin);
            else
                snprintf(lbl, sizeof(lbl), "%.1f", vmin);
            // Position label below the dot, offset to avoid overlap
            draw_text(renderer, font_small, lbl, mx + 5, my + 5, lines[s].color);
        }

        // Mark MAX with dot + value
        {
            float n = std::clamp((vmax - smin) / (smax - smin), 0.0f, 1.0f);
            int mx = chart_x + (int)((float)day_data[max_idx]->minute_of_day / 1440.0f * chart_w);
            int my = bottom - (int)(n * chart_h);
            for (int dy = -3; dy <= 3; dy++)
                for (int dx = -3; dx <= 3; dx++)
                    if (dx * dx + dy * dy <= 9)
                        SDL_RenderDrawPoint(renderer, mx + dx, my + dy);
            char lbl[32];
            if (lines[s].field == 3 || lines[s].field == 2)
                snprintf(lbl, sizeof(lbl), "%.0f", vmax);
            else
                snprintf(lbl, sizeof(lbl), "%.1f", vmax);
            draw_text(renderer, font_small, lbl, mx + 5, my - 16, lines[s].color);
        }
    }

    // Current time marker
    if (view_day == 0) {
        int cx = chart_x + (int)((float)cur_minute / 1440.0f * chart_w);
        set_color(renderer, {255, 255, 255, 180});
        for (int y = chart_y; y < bottom; y += 4)
            SDL_RenderDrawLine(renderer, cx, y, cx, std::min(y + 2, bottom));
        char now_str[16]; snprintf(now_str, sizeof(now_str), "%02d:%02d", cur_minute / 60, cur_minute % 60);
        draw_text(renderer, font_small, now_str, cx, chart_y - 12, TEXT_PRIMARY, true);
    }

    // Legend (bottom-left) - only visible sensors
    int lx = chart_x + 5;
    int ly = bottom - 65;
    int leg_row = 0;
    for (int s = 0; s < 4; s++) {
        Color lc = show[s] ? lines[s].color : Color{80, 80, 100, 255};
        set_color(renderer, lc);
        SDL_RenderDrawLine(renderer, lx, ly + leg_row * 15 + 6, lx + 15, ly + leg_row * 15 + 6);
        SDL_RenderDrawLine(renderer, lx, ly + leg_row * 15 + 7, lx + 15, ly + leg_row * 15 + 7);
        char leg[64];
        snprintf(leg, sizeof(leg), "%s (%s)", lines[s].label, lines[s].unit);
        draw_text(renderer, font_small, leg, lx + 20, ly + leg_row * 15, lc);
        leg_row++;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// --- Spike detection ---
struct SpikeInfo {
    int log_index;        // index into data_log
    int minute_of_day;
    int day_offset;
    float value;
    float prev_value;
    float jump_pct;       // relative jump
    int field;            // which sensor field (0-3)
};

static std::vector<SpikeInfo> find_spikes(const std::vector<LogPoint>& log, int field, float threshold) {
    std::vector<SpikeInfo> spikes;
    auto get = [field](const LogPoint& p) -> float {
        switch (field) {
            case 0: return p.temperature;
            case 1: return p.humidity;
            case 2: return p.pressure;
            default: return p.eco2;
        }
    };
    for (size_t i = 1; i < log.size(); i++) {
        // Only compare consecutive points on same day or close in time
        if (log[i].day_offset != log[i-1].day_offset) continue;
        int gap = log[i].minute_of_day - log[i-1].minute_of_day;
        if (gap <= 0 || gap > 5) continue;  // skip gaps >5min

        float v0 = get(log[i-1]);
        float v1 = get(log[i]);
        float range;
        switch (field) {
            case 0: range = 25.0f; break;  // temp: 10-35
            case 1: range = 100.0f; break; // hum: 0-100
            case 2: range = 60.0f; break;  // pres: 980-1040
            default: range = 2000.0f; break; // co2: 0-2000
        }
        float jump = fabsf(v1 - v0) / range;
        if (jump >= threshold) {
            spikes.push_back({(int)i, log[i].minute_of_day, log[i].day_offset, v1, v0, jump, field});
        }
    }
    return spikes;
}

static void remove_spike_from_log(std::vector<LogPoint>& log, int index) {
    if (index >= 0 && index < (int)log.size()) {
        // Also remove from disk file
        const LogPoint& p = log[index];
        time_t now = time(nullptr);
        time_t file_t = now + p.day_offset * 86400;
        struct tm* ftm = localtime(&file_t);
        char fname[256];
        snprintf(fname, sizeof(fname), "%s/%04d-%02d-%02d.csv", DATA_DIR,
                 ftm->tm_year + 1900, ftm->tm_mon + 1, ftm->tm_mday);

        // Read file, skip the matching line, rewrite
        FILE* f = fopen(fname, "r");
        if (f) {
            std::vector<std::string> lines;
            char line[256];
            int target_hh = p.minute_of_day / 60;
            int target_mm = p.minute_of_day % 60;
            bool removed = false;
            while (fgets(line, sizeof(line), f)) {
                int hh, mm;
                float t, h, pr, c;
                if (!removed && sscanf(line, "%d:%d,%f,%f,%f,%f", &hh, &mm, &t, &h, &pr, &c) == 6 &&
                    hh == target_hh && mm == target_mm) {
                    removed = true;  // skip this line
                    continue;
                }
                lines.push_back(line);
            }
            fclose(f);
            f = fopen(fname, "w");
            if (f) {
                for (const auto& l : lines) fputs(l.c_str(), f);
                fclose(f);
            }
        }
        log.erase(log.begin() + index);
    }
}

// Settings menu modes
enum SettingsPage { SETTINGS_MAIN, SETTINGS_SPIKE_SELECT, SETTINGS_SPIKE_REVIEW, SETTINGS_SPECTRUM, SETTINGS_SENSOR };

// --- Sensor retry tracking (persistent across app restarts) ---
static const char* RETRY_PATH = "/home/luka/bme688-monitor/sensor_retries.txt";

static int load_retry_count() {
    FILE* f = fopen(RETRY_PATH, "r");
    if (!f) return 0;
    int n = 0;
    if (fscanf(f, "%d", &n) != 1) n = 0;
    fclose(f);
    return n;
}

static void save_retry_count(int n) {
    FILE* f = fopen(RETRY_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", n);
    fclose(f);
}

static void clear_retry_count() {
    remove(RETRY_PATH);
}

static const char* FIELD_NAMES[] = {"Temperatur", "Luftfeuchtigkeit", "Luftdruck", "CO\xE2\x82\x82"};
static const char* TZ_NAMES[] = {
    "UTC-12", "UTC-11", "UTC-10", "UTC-9", "UTC-8", "UTC-7", "UTC-6", "UTC-5",
    "UTC-4", "UTC-3", "UTC-2", "UTC-1", "UTC", "UTC+1", "UTC+2", "UTC+3",
    "UTC+4", "UTC+5", "UTC+6", "UTC+7", "UTC+8", "UTC+9", "UTC+10", "UTC+11", "UTC+12"
};

// Check if current time is in night mode window
static bool is_night_time() {
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    int h = tm->tm_hour;
    if (g_settings.night_start_h > g_settings.night_end_h) {
        // Wraps midnight: e.g. 22-7
        return h >= g_settings.night_start_h || h < g_settings.night_end_h;
    } else {
        return h >= g_settings.night_start_h && h < g_settings.night_end_h;
    }
}

// Apply night mode red filter overlay
static void apply_night_dim(SDL_Renderer* renderer, int win_w, int win_h) {
    if (!g_settings.night_mode_auto || !is_night_time()) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect full = {0, 0, win_w, win_h};
    // True red filter: absorb green and blue channels with a dark overlay
    // First pass: kill blue+green (dark cyan/teal overlay absorbs non-red)
    float strength = 1.0f - g_settings.night_brightness / 100.0f;
    int gb_kill = (int)(strength * 220);  // how much to absorb green+blue
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, (uint8_t)gb_kill);
    SDL_RenderFillRect(renderer, &full);
    // Second pass: add a subtle red tint back to keep things visible
    SDL_SetRenderDrawColor(renderer, 60, 0, 0, (uint8_t)(strength * 120));
    SDL_RenderFillRect(renderer, &full);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// Weather icon types for programmatic drawing
enum WeatherIcon { ICON_SUN = 0, ICON_SUN_RAYS, ICON_SUN_CLOUD, ICON_CLOUD, ICON_RAIN, ICON_STORM, ICON_STABLE };

static const char* weather_icon_name(WeatherIcon icon) {
    switch (icon) {
        case ICON_SUN: return "sun";
        case ICON_SUN_RAYS: return "sun_rays";
        case ICON_SUN_CLOUD: return "sun_cloud";
        case ICON_CLOUD: return "cloud";
        case ICON_RAIN: return "rain";
        case ICON_STORM: return "storm";
        case ICON_STABLE: return "stable";
        default: return "stable";
    }
}

static void draw_weather_icon(SDL_Renderer* renderer, int cx, int cy, int size, WeatherIcon icon, Color tint) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    int r = size;

    auto fill_circle = [&](int x0, int y0, int rad, Color c) {
        set_color(renderer, c);
        for (int dy = -rad; dy <= rad; dy++) {
            int dx = (int)sqrtf((float)(rad * rad - dy * dy));
            SDL_RenderDrawLine(renderer, x0 - dx, y0 + dy, x0 + dx, y0 + dy);
        }
    };

    auto draw_cloud = [&](int x0, int y0, int w, Color c) {
        // Cloud = overlapping circles
        fill_circle(x0 - w/3, y0, w/3, c);
        fill_circle(x0 + w/4, y0, w/3, c);
        fill_circle(x0, y0 - w/4, w/3, c);
        // Bottom flat
        set_color(renderer, c);
        SDL_Rect base = {x0 - w/2, y0, w, w/4};
        SDL_RenderFillRect(renderer, &base);
    };

    switch (icon) {
    case ICON_SUN: {
        // Sun body only, no rays
        fill_circle(cx, cy, r, {255, 210, 60, 255});
        break;
    }
    case ICON_SUN_RAYS: {
        // Sun body with short rays — only for very good weather
        fill_circle(cx, cy, r, {255, 210, 60, 255});
        set_color(renderer, {255, 230, 100, 200});
        for (int a = 0; a < 8; a++) {
            float angle = a * 3.14159f / 4.0f;
            int x1 = cx + (int)(cosf(angle) * (r + 2));
            int y1 = cy + (int)(sinf(angle) * (r + 2));
            int x2 = cx + (int)(cosf(angle) * (r + r/3 + 2));
            int y2 = cy + (int)(sinf(angle) * (r + r/3 + 2));
            SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
            SDL_RenderDrawLine(renderer, x1 + 1, y1, x2 + 1, y2);
        }
        break;
    }
    case ICON_SUN_CLOUD: {
        // Small sun behind (upper right), no rays
        fill_circle(cx + r/2, cy - r/2, r*2/3, {255, 210, 60, 255});
        // Cloud in front (lower left)
        draw_cloud(cx - r/4, cy + r/4, r, {210, 220, 240, 240});
        break;
    }
    case ICON_CLOUD: {
        draw_cloud(cx, cy, r * 4/3, {180, 190, 210, 255});
        break;
    }
    case ICON_RAIN: {
        // Cloud
        draw_cloud(cx, cy - r/3, r, {160, 170, 190, 255});
        // Rain drops
        set_color(renderer, {100, 160, 255, 220});
        for (int d = 0; d < 3; d++) {
            int dx = (d - 1) * r/2;
            SDL_RenderDrawLine(renderer, cx + dx, cy + r/2, cx + dx - 2, cy + r);
            SDL_RenderDrawLine(renderer, cx + dx + 1, cy + r/2, cx + dx - 1, cy + r);
        }
        break;
    }
    case ICON_STORM: {
        // Dark cloud
        draw_cloud(cx, cy - r/3, r, {120, 120, 140, 255});
        // Lightning bolt
        set_color(renderer, {255, 240, 80, 255});
        int lx = cx, ly = cy + r/4;
        SDL_RenderDrawLine(renderer, lx, ly, lx - r/3, ly + r/2);
        SDL_RenderDrawLine(renderer, lx + 1, ly, lx - r/3 + 1, ly + r/2);
        SDL_RenderDrawLine(renderer, lx - r/3, ly + r/2, lx + r/6, ly + r/2);
        SDL_RenderDrawLine(renderer, lx + r/6, ly + r/2, lx - r/4, ly + r);
        SDL_RenderDrawLine(renderer, lx + r/6 + 1, ly + r/2, lx - r/4 + 1, ly + r);
        // Rain drops (fewer)
        set_color(renderer, {100, 160, 255, 180});
        SDL_RenderDrawLine(renderer, cx - r/2, cy + r/3, cx - r/2 - 2, cy + r*2/3);
        SDL_RenderDrawLine(renderer, cx + r/2, cy + r/3, cx + r/2 - 2, cy + r*2/3);
        break;
    }
    case ICON_STABLE: {
        // Horizontal arrow →
        set_color(renderer, tint);
        int aw = r;
        SDL_RenderDrawLine(renderer, cx - aw, cy, cx + aw, cy);
        SDL_RenderDrawLine(renderer, cx - aw, cy + 1, cx + aw, cy + 1);
        // Arrowhead
        SDL_RenderDrawLine(renderer, cx + aw, cy, cx + aw - r/2, cy - r/3);
        SDL_RenderDrawLine(renderer, cx + aw, cy + 1, cx + aw - r/2, cy + r/3 + 1);
        SDL_RenderDrawLine(renderer, cx + aw + 1, cy, cx + aw - r/2 + 1, cy - r/3);
        SDL_RenderDrawLine(renderer, cx + aw + 1, cy + 1, cx + aw - r/2 + 1, cy + r/3 + 1);
        break;
    }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// Weather classification result
struct WeatherClass {
    const char* text;
    const char* detail;
    Color color;
    WeatherIcon icon;
};

// Classify weather from pressure, humidity, pressure change, and humidity change
// pres_delta = pressure change over the analysis window (e.g. 45min or 3h)
// hum_delta = humidity change over same window (positive = getting more humid)
// window_h = size of the analysis window in hours (for scaling thresholds)
static WeatherClass classify_weather(float pressure, float humidity, float pres_delta, float window_h,
                                      float hum_delta = 0) {
    // Scale thresholds relative to 3h window
    float scale = window_h / 3.0f;
    if (scale < 0.1f) scale = 0.1f;
    float th_fast = 2.0f * scale;
    float th_rise = 0.5f * scale;

    bool high_p = pressure > 1022;
    bool low_p = pressure < 1015;
    bool rising_fast = pres_delta > th_fast;
    bool rising = pres_delta > th_rise;
    bool falling_fast = pres_delta < -th_fast;
    bool falling = pres_delta < -th_rise;
    bool stable = !rising && !falling;
    // Indoor sensor thresholds (BME688 inside — outdoor humidity arrives dampened)
    bool humid = humidity > 65.0f;
    bool very_humid = humidity > 78.0f;
    // Humidity rising fast = outdoor wet event (rain pulls indoor RH up)
    bool hum_rising = hum_delta > 3.0f * scale;   // >3% in 45min or >12% in 3h
    bool hum_spike = hum_delta > 6.0f * scale;     // dramatic rise = storm

    // Priority 1: humidity spike = active precipitation regardless of pressure
    if (hum_spike && falling_fast) return {"Gewitter!", "Feuchte-Spike + Druckabfall", {255, 40, 40, 255}, ICON_STORM};
    if (hum_spike && falling) return {"Starkregen", "Feuchte steigt stark + Druck f\xC3\xA4llt", {255, 80, 60, 255}, ICON_STORM};
    if (hum_spike) return {"Regen / Gewitter", "Feuchte steigt rapide an", {255, 100, 60, 255}, ICON_STORM};

    // Priority 2: high indoor humidity = likely precipitation outside
    if (very_humid && falling_fast) return {"Gewitter / Starkregen", "Feucht + schneller Druckabfall", {255, 60, 60, 255}, ICON_STORM};
    if (very_humid && falling) return {"Regen / Schauer", "Hohe Feuchte + fallender Druck", {255, 120, 80, 255}, ICON_RAIN};
    if (very_humid && stable) return {"Anhaltender Regen", "Hohe Feuchte, Druck stabil", {200, 150, 120, 255}, ICON_RAIN};
    if (very_humid && rising && !rising_fast) return {"Nachlassender Regen", "Druck steigt, noch feucht", {200, 180, 120, 255}, ICON_RAIN};
    if (very_humid && rising_fast) return {"Okklusion / Regen", "Frontdurchgang, noch feucht", {220, 160, 100, 255}, ICON_RAIN};

    // Humidity rising + humid = precipitation starting/ongoing
    if (hum_rising && humid && falling) return {"Regen setzt ein", "Feuchte steigt + Druck f\xC3\xA4llt", {255, 140, 70, 255}, ICON_RAIN};
    if (hum_rising && humid) return {"Niederschlag", "Feuchte steigt deutlich", {240, 160, 90, 255}, ICON_RAIN};

    // Priority 2: high pressure
    if (high_p && stable && !humid) return {"Best\xC3\xA4ndig sch\xC3\xB6n", "Hochdruck, stabil, trockene Luft", {100, 220, 140, 255}, ICON_SUN_RAYS};
    if (high_p && stable && humid) return {"Schw\xC3\xBCl, bew\xC3\xB6lkt", "Hochdruck + hohe Feuchtigkeit", {220, 200, 100, 255}, ICON_SUN_CLOUD};
    if (high_p && rising) return {"Sch\xC3\xB6nes Wetter", "Hoher und steigender Druck", {100, 220, 140, 255}, ICON_SUN_RAYS};
    if (high_p && falling && !humid) return {"Wechselhaft", "Druck f\xC3\xA4llt vom Hochdruck", {255, 200, 80, 255}, ICON_SUN_CLOUD};
    if (high_p && falling) return {"Bew\xC3\xB6lkung nimmt zu", "Fallender Druck + Feuchte", {255, 180, 60, 255}, ICON_CLOUD};
    if (high_p && falling_fast) return {"Verschlechterung", "Schneller Druckabfall", {255, 160, 60, 255}, ICON_CLOUD};

    // Priority 3: low pressure
    if (low_p && falling_fast) return {"Sturm m\xC3\xB6glich!", "Tiefdruck + schneller Abfall", {255, 80, 80, 255}, ICON_STORM};
    if (low_p && falling) return {"Regen wahrscheinlich", "Tief verst\xC3\xA4rkt sich", {255, 120, 80, 255}, ICON_RAIN};
    if (low_p && stable && humid) return {"Regen / Nieselregen", "Tiefdruck + hohe Feuchte", {200, 160, 120, 255}, ICON_RAIN};
    if (low_p && stable) return {"Bew\xC3\xB6lkt", "Tiefdruckgebiet h\xC3\xA4lt an", {200, 180, 140, 255}, ICON_CLOUD};
    if (low_p && rising && humid) return {"Nachlassender Regen", "Druck steigt, noch feucht", {200, 190, 130, 255}, ICON_RAIN};
    if (low_p && rising) return {"Besserung in Sicht", "Druck steigt aus dem Tief", {180, 220, 140, 255}, ICON_SUN_CLOUD};
    if (low_p && rising_fast && humidity > 50.0f) return {"Okklusion / Schauer", "Schneller Druckanstieg + feucht", {220, 170, 90, 255}, ICON_RAIN};
    if (low_p && rising_fast) return {"Rasche Aufklarung", "Schneller Druckanstieg", {140, 220, 180, 255}, ICON_SUN};

    // Priority 4: normal pressure — humidity is key
    if (falling_fast && humid) return {"Gewitter m\xC3\xB6glich", "Schneller Druckabfall + feucht", {255, 100, 60, 255}, ICON_STORM};
    if (falling_fast) return {"Verschlechterung", "Schneller Druckabfall", {255, 120, 60, 255}, ICON_CLOUD};
    if (falling && humid) return {"Regen m\xC3\xB6glich", "Fallender Druck + hohe Feuchte", {255, 170, 80, 255}, ICON_RAIN};
    if (falling) return {"Zunehmend bew\xC3\xB6lkt", "Langsam fallender Druck", {255, 200, 100, 255}, ICON_CLOUD};
    if (rising_fast && humidity > 50.0f) return {"Okklusion / Schauer", "Schneller Druckanstieg + feucht", {220, 170, 90, 255}, ICON_RAIN};
    if (rising_fast) return {"Aufklarung", "Schnell steigender Druck", {140, 220, 160, 255}, ICON_SUN};
    if (rising && humid) return {"Bew\xC3\xB6lkt, wird trockener", "Druck steigt, noch feucht", {200, 200, 140, 255}, ICON_CLOUD};
    if (rising) return {"Leichte Besserung", "Druck steigt langsam", {180, 220, 140, 255}, ICON_SUN_CLOUD};
    if (stable && humid) return {"Tr\xC3\xBC""b und feucht", "Stabiler Druck, hohe Feuchte", {200, 180, 140, 255}, ICON_CLOUD};
    return {"Keine \xC3\x84nderung", "Stabiler Luftdruck", {180, 200, 220, 255}, ICON_STABLE};
}

// Compute trend from deque: uses all samples, returns gradient per second (positive = rising)
// Data is at ~2Hz (0.5s apart)
static float calc_trend(const std::deque<float>& data) {
    int n = (int)data.size();
    if (n < 8) return 0;
    float sx = 0, sy = 0, sxy = 0, sxx = 0;
    for (int i = 0; i < n; i++) {
        float x = (float)i;
        float y = data[i];
        sx += x; sy += y; sxy += x * y; sxx += x * x;
    }
    float denom = n * sxx - sx * sx;
    if (fabsf(denom) < 1e-9f) return 0;
    float slope = (n * sxy - sx * sy) / denom;  // per 0.5s sample
    return slope * 2.0f;  // convert to per-second
}

// Returns UTF-8 arrow character based on trend strength
static const char* trend_arrow(float trend_per_s, float threshold) {
    if (trend_per_s > threshold * 2) return "\xe2\x86\x91";       // ↑ strong rise
    if (trend_per_s > threshold) return "\xe2\x86\x97";           // ↗ rising
    if (trend_per_s < -threshold * 2) return "\xe2\x86\x93";      // ↓ strong fall
    if (trend_per_s < -threshold) return "\xe2\x86\x98";           // ↘ falling
    return "\xe2\x86\x92";                                         // → stable
}

static Color trend_color(float trend_per_s, float threshold) {
    if (fabsf(trend_per_s) > threshold * 2) return {255, 120, 80, 255};
    if (fabsf(trend_per_s) > threshold) return {255, 200, 100, 255};
    return {140, 200, 140, 255};
}

static void draw_value_card(SDL_Renderer* renderer, TTF_Font* font_big, TTF_Font* font_small,
                            SDL_Rect area, const char* label, float value, const char* unit,
                            Color color, const char* fmt = "%.1f",
                            const char* arrow = nullptr, Color arrow_col = {0,0,0,0}) {
    fill_rounded_rect(renderer, area, 10, CARD_BG);

    draw_text(renderer, font_small, label, area.x + area.w / 2, area.y + 12, TEXT_DIM, true);

    char val_str[32];
    snprintf(val_str, sizeof(val_str), fmt, value);
    draw_text(renderer, font_big, val_str, area.x + area.w / 2, area.y + area.h / 2, color, true);

    // Trend arrow (right side of card)
    if (arrow) {
        draw_text(renderer, font_big, arrow, area.x + area.w - 24, area.y + area.h / 2, arrow_col, true);
    }

    draw_text(renderer, font_small, unit, area.x + area.w / 2, area.y + area.h - 16, TEXT_DIM, true);
}

int main(int argc, char* argv[]) {
    std::string i2c_dev = "/dev/i2c-1";
    uint8_t i2c_addr = 0x77;
    bool demo_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) demo_mode = true;
        else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) i2c_dev = argv[++i];
        else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) i2c_addr = (uint8_t)strtol(argv[++i], nullptr, 0);
    }

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        return 1;
    }

    SDL_DisplayMode dm;
    SDL_GetCurrentDisplayMode(0, &dm);
    int win_w = dm.w;
    int win_h = dm.h;

    SDL_Window* window = SDL_CreateWindow("Luftqualit\xC3\xA4t",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        win_w, win_h, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_ShowCursor(SDL_DISABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Log renderer info for debugging
    SDL_RendererInfo rinfo;
    if (SDL_GetRendererInfo(renderer, &rinfo) == 0) {
        fprintf(stderr, "Renderer: %s (flags: 0x%x)\n", rinfo.name, rinfo.flags);
    }

    const char* font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char* font_bold_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    TTF_Font* font_big = TTF_OpenFont(font_bold_path, 36);
    TTF_Font* font_med = TTF_OpenFont(font_path, 20);
    TTF_Font* font_small = TTF_OpenFont(font_path, 14);

    if (!font_big || !font_med || !font_small) {
        fprintf(stderr, "Failed to load font: %s\n", TTF_GetError());
        return 1;
    }

    BME688 sensor(i2c_dev, i2c_addr);
    bool sensor_ok = false;
    bool sensor_user_forced_demo = false;  // user explicitly chose --demo
    if (demo_mode) {
        sensor_user_forced_demo = true;
    } else {
        sensor_ok = sensor.init();
        if (!sensor_ok) {
            fprintf(stderr, "Sensor nicht gefunden – starte im Demo-Modus\n");
            demo_mode = true;
        }
    }

    // Sensor retry tracking
    int sensor_retry_count = load_retry_count();
    bool sensor_warning_shown = false;   // true = popup visible
    bool sensor_warning_dismissed = false; // user clicked OK
    uint32_t sensor_warning_time = 0;     // when warning appeared (for 60s countdown)
    const char* sensor_test_result = nullptr;  // null = no result, else text

    if (sensor_ok && !sensor_user_forced_demo) {
        // Sensor works — reset retry counter
        if (sensor_retry_count > 0) clear_retry_count();
        sensor_retry_count = 0;
    } else if (!sensor_user_forced_demo) {
        // Sensor failed — show warning popup
        sensor_warning_shown = true;
        sensor_warning_time = SDL_GetTicks();
    }

    // Threaded sensor reading to avoid blocking the render loop
    // Fast reads (temp/hum/press) every ~100ms, full gas read every ~1s
    std::mutex sensor_mtx;
    BME688Data sensor_latest = {};
    std::atomic<bool> sensor_new_data{false};
    std::atomic<bool> sensor_running{true};
    float last_gas_resistance = 0;
    float last_iaq = 0;
    float last_eco2 = 400;

    std::atomic<bool> sensor_gas_new{false};
    float sensor_gas_eco2 = 400;
    // Heater timing: main loop uses this to know when to skip temp readings
    std::atomic<uint32_t> last_gas_read_tick{0};  // SDL_GetTicks at last gas read

    std::thread sensor_thread([&]() {
        float dt = 0;
        float last_clean_temp = 22.0f;  // last temperature not affected by heater
        auto last_gas_time = std::chrono::steady_clock::now() - std::chrono::seconds(999);
        while (sensor_running.load()) {
            BME688Data reading;
            auto now_tp = std::chrono::steady_clock::now();
            int gas_interval_ms = g_settings.gas_interval_s * 1000;
            bool gas_due = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_gas_time).count() >= gas_interval_ms;

            if (demo_mode) {
                dt += 0.01f;
                reading.temperature = 22.0f + 3.0f * sinf(dt);
                reading.humidity = 45.0f + 10.0f * sinf(dt * 0.7f);
                reading.pressure = 1013.0f + 2.0f * sinf(dt * 0.3f);
                reading.gas_resistance = 50.0f + 20.0f * sinf(dt * 0.5f);
                reading.eco2 = 600.0f + 200.0f * sinf(dt * 0.4f);
                reading.iaq = 30.0f + 20.0f * sinf(dt * 0.4f);
                reading.valid = true;
                if (gas_due) {
                    last_gas_time = now_tp;
                    std::lock_guard<std::mutex> lock(sensor_mtx);
                    sensor_gas_eco2 = reading.eco2;
                    sensor_gas_new.store(true);
                }
            } else if (gas_due) {
                // When heater filter is on, do one last fast read and wait 1s
                // so the chart gets a clean temp sample right before heater fires
                if (g_settings.heater_filter) {
                    BME688Data pre = sensor.read_fast();
                    if (pre.valid) {
                        last_clean_temp = pre.temperature;
                        pre.gas_resistance = last_gas_resistance;
                        pre.iaq = last_iaq;
                        pre.eco2 = last_eco2;
                        std::lock_guard<std::mutex> lock(sensor_mtx);
                        sensor_latest = pre;
                        sensor_new_data.store(true);
                    }
                    // 1s settle time before heater kicks in
                    for (int i = 0; i < 100 && sensor_running.load(); i++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                // Full read with gas
                reading = sensor.read();
                last_gas_read_tick.store(SDL_GetTicks());  // set AFTER read completes
                last_gas_time = now_tp;
                if (reading.valid) {
                    last_gas_resistance = reading.gas_resistance;
                    last_iaq = reading.iaq;
                    last_eco2 = reading.eco2;
                    // Replace heated temperature with last clean value
                    if (g_settings.heater_filter) {
                        reading.temperature = last_clean_temp;
                    }
                    {
                        std::lock_guard<std::mutex> lock(sensor_mtx);
                        sensor_gas_eco2 = reading.eco2;
                        sensor_gas_new.store(true);
                    }
                }
            } else {
                // Fast read: temp/hum/press only (~35ms)
                reading = sensor.read_fast();
                if (reading.valid) {
                    last_clean_temp = reading.temperature;
                    reading.gas_resistance = last_gas_resistance;
                    reading.iaq = last_iaq;
                    reading.eco2 = last_eco2;
                }
            }
            {
                std::lock_guard<std::mutex> lock(sensor_mtx);
                sensor_latest = reading;
                sensor_new_data.store(true);
            }
            // Short sleep between fast reads
            if (demo_mode) {
                for (int i = 0; i < 10 && sensor_running.load(); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                int sleep_ms = g_settings.sensor_interval_ms;
                for (int i = 0; i < sleep_ms / 10 && sensor_running.load(); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });

    SensorHistory history;
    BME688Data latest = {};
    BME688Data prev_reading = {};
    BME688Data display_vals = {};  // interpolated values for smooth display
    uint32_t last_read = 0;
    uint32_t read_interval = 500;  // chart flush interval for interpolation
    bool running = true;
    bool exit_hover = false;
    bool power_hover = false;
    bool screen_off = false;

    // ECO mode: 0=OFF, 1=reduced fps, 2=full powersave+screen off
    int eco_target = 0;   // what button shows (immediate)
    int eco_active = 0;   // what's actually running (delayed)
    uint32_t eco_switch_time = 0;  // when mode change was requested
    bool eco_pending = false;
    uint32_t eco_idle_start = 0;

    // Button strip: right-to-left with 6px gaps, y=8, h=40
    // EXIT(70) _ MIN(42) _ ECO(80) _ ASTRO(80) _ WETTER(70) _ SET(42)
    int bx = win_w - 8;
    SDL_Rect exit_btn    = {bx -= 70, 8, 70, 40};
    SDL_Rect min_btn     = {bx -= 48, 8, 42, 40};
    SDL_Rect power_btn   = {bx -= 86, 8, 80, 40};
    SDL_Rect astro_btn   = {bx -= 86, 8, 80, 40};
    SDL_Rect weather_btn = {bx -= 76, 8, 70, 40};
    SDL_Rect settings_btn= {bx -= 48, 8, 42, 40};
    bool min_hover = false;
    bool settings_hover = false;
    bool weather_hover = false;
    bool weather_mode = false;

    bool astro_mode = false;
    bool astro_hover = false;
    int astro_view_day = 0;
    bool astro_show[4] = {true, true, true, true};  // temp, hum, pres, co2
    bool astro_settings_open = false;
    SDL_Rect astro_back_btn = {win_w - 100, 2, 88, 40};
    SDL_Rect astro_prev_btn = {win_w - 200, 2, 40, 40};
    SDL_Rect astro_next_btn = {win_w - 150, 2, 40, 40};

    // Settings state
    bool settings_mode = false;
    SettingsPage settings_page = SETTINGS_MAIN;
    int spike_field = 0;
    int spectrum_field = 0;       // 0=temp,1=hum,2=pres,3=co2(log)
    int spectrum_colorscheme = 0; // 0=klassik,1=thermal,2=neon
    int spectrum_nyquist = 0;     // 0=1Hz(chart 2Hz), 1=5Hz(raw 10Hz)
    bool spectrum_x_period = false; // false=Hz, true=period (alle Xs)
    std::vector<SpikeInfo> spike_results;
    int spike_current = 0;  // which spike we're reviewing

    // Load settings
    g_settings.load();

    // Load historical data from disk + populate CO2 chart with today's data
    std::vector<LogPoint> data_log = load_log_history();
    for (const auto& p : data_log) {
        if (p.day_offset == 0) {
            history.eco2.push_back({p.eco2, p.minute_of_day});
        }
    }
    uint32_t last_log_save = 0;
    uint32_t last_web_save = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)
                    running = false;
            } else if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_FINGERMOTION) {
                int mx, my;
                if (ev.type == SDL_FINGERMOTION) {
                    mx = (int)(ev.tfinger.x * win_w);
                    my = (int)(ev.tfinger.y * win_h);
                } else {
                    mx = ev.motion.x;
                    my = ev.motion.y;
                }
                SDL_Point p = {mx, my};
                exit_hover = SDL_PointInRect(&p, &exit_btn);
                min_hover = SDL_PointInRect(&p, &min_btn);
                power_hover = SDL_PointInRect(&p, &power_btn);
                astro_hover = !astro_mode && !weather_mode && !settings_mode && SDL_PointInRect(&p, &astro_btn);
                weather_hover = !astro_mode && !weather_mode && !settings_mode && SDL_PointInRect(&p, &weather_btn);
                settings_hover = !astro_mode && !weather_mode && !settings_mode && SDL_PointInRect(&p, &settings_btn);
                // Slider drag for spectrum filter strength
                if (settings_mode && settings_page == SETTINGS_SPECTRUM &&
                    (ev.type == SDL_MOUSEMOTION ? (ev.motion.state & SDL_BUTTON_LMASK) : true)) {
                    int spw = win_w * 80 / 100, sph = win_h * 80 / 100;
                    int spx = (win_w - spw) / 2, spy = (win_h - sph) / 2;
                    int sl_x = spx + spw - 300, sl_w = 250;
                    int csy = spy + sph - 50;
                    SDL_Rect sl_hit = {sl_x, csy - 4, sl_w, 44};
                    if (SDL_PointInRect(&p, &sl_hit)) {
                        float frac = (float)(mx - sl_x) / (float)sl_w;
                        frac = std::clamp(frac, 0.0f, 1.0f);
                        int val = 250 + (int)(frac * (6000 - 250));
                        g_settings.heater_blanking_ms = std::clamp(((val + 125) / 250) * 250, 250, 6000);
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_FINGERDOWN) {
                int mx, my;
                if (ev.type == SDL_FINGERDOWN) {
                    mx = (int)(ev.tfinger.x * win_w);
                    my = (int)(ev.tfinger.y * win_h);
                } else {
                    mx = ev.button.x;
                    my = ev.button.y;
                }
                // Wake screen on any touch when screen is off (ECO 2)
                if (screen_off) {
                    screen_off = false;
                    system("echo 1 > /sys/class/backlight/11-0045/brightness");
                    eco_idle_start = SDL_GetTicks();
                    continue;
                }
                SDL_Point p = {mx, my};

                // Sensor warning popup click (intercepts everything)
                if (sensor_warning_shown && !sensor_warning_dismissed) {
                    int wpw = 460, wph = 260;
                    int wpx = (win_w - wpw) / 2, wpy = (win_h - wph) / 2;
                    SDL_Rect warn_ok = {wpx + wpw/2 - 80, wpy + wph - 70, 160, 50};
                    if (SDL_PointInRect(&p, &warn_ok)) {
                        sensor_warning_dismissed = true;
                        sensor_warning_shown = false;
                    }
                    continue;  // block all other clicks while popup visible
                }

                if (settings_mode) {
                    // Overlay panel dimensions (centered)
                    // Panel size depends on page
                    int pw = 560, ph = 600;
                    if (settings_page == SETTINGS_MAIN) {
                        // 56 header + rows*46 + buttons area
                        int n_extra = 2;  // heater toggle + night toggle always shown
                        if (g_settings.heater_filter) n_extra++;
                        if (g_settings.night_mode_auto) n_extra++;
                        ph = 56 + (5 + n_extra) * 46 + 250;
                        ph = std::min(ph, win_h - 20);  // never exceed screen
                    }
                    if (settings_page == SETTINGS_SPIKE_REVIEW) ph = 540;
                    else if (settings_page == SETTINGS_SPIKE_SELECT) ph = 420;
                    else if (settings_page == SETTINGS_SENSOR) ph = sensor_retry_count >= 3 ? 500 : 440;
                    int px = (win_w - pw) / 2, py = (win_h - ph) / 2;
                    int pad = 20;
                    int row_w = pw - 2 * pad;
                    int rx = px + pad;

                    // Close button (X) top-right of panel
                    SDL_Rect close_btn = {px + pw - 44, py + 6, 38, 32};
                    if (SDL_PointInRect(&p, &close_btn)) {
                        if (settings_page == SETTINGS_MAIN) {
                            settings_mode = false;
                            g_settings.save();
                        } else {
                            if (settings_page == SETTINGS_SENSOR) sensor_test_result = nullptr;
                            settings_page = SETTINGS_MAIN;
                        }
                    } else if (settings_page == SETTINGS_MAIN) {
                        int base_y = py + 56;
                        int rh = 46;
                        int rx = px + pad;

                        // Touch-friendly arrows: 48x44, with 110px gap between them for value text
                        int aw = 48, ah = 44;
                        // Left arrow right edge at rx+row_w-110-aw = rx+row_w-158
                        // Right arrow left edge at rx+row_w-aw = rx+row_w-48
                        auto arrow_rects = [&](int row) -> std::pair<SDL_Rect, SDL_Rect> {
                            int ay = base_y + row * rh;
                            return {{rx + row_w - aw - 110 - aw, ay, aw, ah},
                                    {rx + row_w - aw, ay, aw, ah}};
                        };

                        // Timezone
                        auto [tz_l, tz_r] = arrow_rects(0);
                        if (SDL_PointInRect(&p, &tz_l))
                            g_settings.utc_offset_min = std::max(-720, g_settings.utc_offset_min - 60);
                        else if (SDL_PointInRect(&p, &tz_r))
                            g_settings.utc_offset_min = std::min(720, g_settings.utc_offset_min + 60);

                        // Sensor interval
                        auto [rr_l, rr_r] = arrow_rects(1);
                        if (SDL_PointInRect(&p, &rr_l))
                            g_settings.sensor_interval_ms = std::max(50, g_settings.sensor_interval_ms - 50);
                        else if (SDL_PointInRect(&p, &rr_r))
                            g_settings.sensor_interval_ms = std::min(2000, g_settings.sensor_interval_ms + 50);

                        // Log days
                        auto [ld_l, ld_r] = arrow_rects(2);
                        if (SDL_PointInRect(&p, &ld_l))
                            g_settings.log_days = std::max(1, g_settings.log_days - 1);
                        else if (SDL_PointInRect(&p, &ld_r))
                            g_settings.log_days = std::min(30, g_settings.log_days + 1);

                        // Spike threshold
                        auto [st_l, st_r] = arrow_rects(3);
                        if (SDL_PointInRect(&p, &st_l))
                            g_settings.spike_threshold = std::max(0.01f, g_settings.spike_threshold - 0.01f);
                        else if (SDL_PointInRect(&p, &st_r))
                            g_settings.spike_threshold = std::min(1.0f, g_settings.spike_threshold + 0.01f);

                        // Gas/CO2 interval
                        auto [gi_l, gi_r] = arrow_rects(4);
                        if (SDL_PointInRect(&p, &gi_l))
                            g_settings.gas_interval_s = std::max(5, g_settings.gas_interval_s - 5);
                        else if (SDL_PointInRect(&p, &gi_r))
                            g_settings.gas_interval_s = std::min(300, g_settings.gas_interval_s + 5);

                        // Heater filter toggle (row 5)
                        {
                            int ry = base_y + 5 * rh;
                            int tbw = 70, tbh = 36;
                            int tbx = rx + row_w - tbw - 8;
                            SDL_Rect fft_btn = {tbx, ry + (44 - tbh) / 2, tbw, tbh};
                            if (SDL_PointInRect(&p, &fft_btn))
                                g_settings.heater_filter = !g_settings.heater_filter;
                        }

                        // Blanking time (row 6, only when filter is on)
                        int extra_rows = 0;
                        if (g_settings.heater_filter) {
                            auto [bl_l, bl_r] = arrow_rects(6);
                            if (SDL_PointInRect(&p, &bl_l))
                                g_settings.heater_blanking_ms = std::max(250, g_settings.heater_blanking_ms - 250);
                            else if (SDL_PointInRect(&p, &bl_r))
                                g_settings.heater_blanking_ms = std::min(6000, g_settings.heater_blanking_ms + 250);
                            extra_rows = 1;
                        }

                        // Night mode toggle
                        {
                            int nrow = 6 + extra_rows;
                            int ry = base_y + nrow * rh;
                            int tbw = 70, tbh = 36;
                            int tbx = rx + row_w - tbw - 8;
                            SDL_Rect nm_btn = {tbx, ry + (44 - tbh) / 2, tbw, tbh};
                            if (SDL_PointInRect(&p, &nm_btn))
                                g_settings.night_mode_auto = !g_settings.night_mode_auto;
                            extra_rows += 1;
                        }

                        // Night hours (only when night mode on)
                        // Left arrow: shift both start and end earlier
                        // Right arrow: shift both start and end later
                        if (g_settings.night_mode_auto) {
                            int nrow = 6 + extra_rows;
                            auto [nh_l, nh_r] = arrow_rects(nrow);
                            if (SDL_PointInRect(&p, &nh_l)) {
                                g_settings.night_start_h = (g_settings.night_start_h + 23) % 24;
                                g_settings.night_end_h = (g_settings.night_end_h + 23) % 24;
                            } else if (SDL_PointInRect(&p, &nh_r)) {
                                g_settings.night_start_h = (g_settings.night_start_h + 1) % 24;
                                g_settings.night_end_h = (g_settings.night_end_h + 1) % 24;
                            }
                            extra_rows += 1;
                        }

                        // Spektrum button
                        int btn_base = base_y + (6 + extra_rows) * rh + 10;
                        SDL_Rect spec_btn = {rx, btn_base, row_w, 50};
                        if (SDL_PointInRect(&p, &spec_btn))
                            settings_page = SETTINGS_SPECTRUM;

                        // Spike filter button
                        SDL_Rect spike_btn = {rx, btn_base + 60, row_w, 50};
                        if (SDL_PointInRect(&p, &spike_btn))
                            settings_page = SETTINGS_SPIKE_SELECT;

                        // Sensor diagnostics button
                        SDL_Rect sensor_btn = {rx, btn_base + 120, row_w, 50};
                        if (SDL_PointInRect(&p, &sensor_btn))
                            settings_page = SETTINGS_SENSOR;

                        // Reset button
                        SDL_Rect reset_btn = {rx, btn_base + 180, row_w, 50};
                        if (SDL_PointInRect(&p, &reset_btn)) {
                            g_settings.reset();
                            g_settings.save();
                        }

                    } else if (settings_page == SETTINGS_SPIKE_SELECT) {
                        int base_y = py + 56;
                        int rx = px + pad;
                        int bh = 52;
                        for (int i = 0; i < 4; i++) {
                            SDL_Rect btn = {rx, base_y + i * (bh + 8), row_w, bh};
                            if (SDL_PointInRect(&p, &btn)) {
                                spike_field = i;
                                data_log = load_log_history();
                                spike_results = find_spikes(data_log, spike_field, g_settings.spike_threshold);
                                spike_current = 0;
                                settings_page = SETTINGS_SPIKE_REVIEW;
                            }
                        }
                        // "Alle analysieren" button
                        SDL_Rect all_btn = {rx, base_y + 4 * (bh + 8) + 8, row_w, bh};
                        if (SDL_PointInRect(&p, &all_btn)) {
                            spike_field = -1;  // all fields
                            data_log = load_log_history();
                            spike_results.clear();
                            for (int f = 0; f < 4; f++) {
                                auto s = find_spikes(data_log, f, g_settings.spike_threshold);
                                spike_results.insert(spike_results.end(), s.begin(), s.end());
                            }
                            // Sort by log_index so we review chronologically
                            std::sort(spike_results.begin(), spike_results.end(),
                                [](const SpikeInfo& a, const SpikeInfo& b) { return a.log_index < b.log_index; });
                            spike_current = 0;
                            settings_page = SETTINGS_SPIKE_REVIEW;
                        }

                    } else if (settings_page == SETTINGS_SPIKE_REVIEW) {
                        if (!spike_results.empty() && spike_current < (int)spike_results.size()) {
                            int btn_y = py + ph - 60;
                            SDL_Rect del_btn = {px + pw / 2 - 165, btn_y, 160, 52};
                            SDL_Rect skip_btn = {px + pw / 2 + 5, btn_y, 160, 52};
                            if (SDL_PointInRect(&p, &del_btn)) {
                                remove_spike_from_log(data_log, spike_results[spike_current].log_index);
                                // Rebuild CO2 daily chart from updated log (today's entries)
                                history.eco2.clear();
                                for (auto& lp : data_log) {
                                    if (lp.day_offset == 0)
                                        history.eco2.push_back({lp.eco2, lp.minute_of_day});
                                }
                                // Recalculate spikes
                                if (spike_field == -1) {
                                    spike_results.clear();
                                    for (int f = 0; f < 4; f++) {
                                        auto s = find_spikes(data_log, f, g_settings.spike_threshold);
                                        spike_results.insert(spike_results.end(), s.begin(), s.end());
                                    }
                                    std::sort(spike_results.begin(), spike_results.end(),
                                        [](const SpikeInfo& a, const SpikeInfo& b) { return a.log_index < b.log_index; });
                                } else {
                                    spike_results = find_spikes(data_log, spike_field, g_settings.spike_threshold);
                                }
                            } else if (SDL_PointInRect(&p, &skip_btn)) {
                                spike_current++;
                            }
                        }
                    } else if (settings_page == SETTINGS_SPECTRUM) {
                        // Spectrum uses large overlay (~80% of screen)
                        int spw = win_w * 80 / 100, sph = win_h * 80 / 100;
                        int spx = (win_w - spw) / 2, spy = (win_h - sph) / 2;

                        // Close button (X) top-right
                        SDL_Rect sp_close = {spx + spw - 44, spy + 6, 38, 32};
                        if (SDL_PointInRect(&p, &sp_close)) {
                            settings_page = SETTINGS_MAIN;
                        }

                        // Data source buttons (top row, left side)
                        int dbtn_y = spy + 50;
                        int dbtn_w = (spw - 300) / 4;  // leave room for right-side buttons
                        for (int i = 0; i < 4; i++) {
                            SDL_Rect dbtn = {spx + 16 + i * (dbtn_w + 4), dbtn_y, dbtn_w, 36};
                            if (SDL_PointInRect(&p, &dbtn))
                                spectrum_field = i;
                        }

                        // Nyquist + Hz/Period buttons (top row, right side)
                        int right_x = spx + spw - 260;
                        for (int i = 0; i < 2; i++) {
                            SDL_Rect nqb = {right_x + i * 66, dbtn_y, 60, 36};
                            if (SDL_PointInRect(&p, &nqb))
                                spectrum_nyquist = i;
                        }
                        // Hz/Period toggle
                        SDL_Rect hz_btn = {right_x + 140, dbtn_y, 100, 36};
                        if (SDL_PointInRect(&p, &hz_btn))
                            spectrum_x_period = !spectrum_x_period;

                        // Bottom bar clicks
                        int csy = spy + sph - 50;
                        // Color scheme buttons (3 modes)
                        int csw = 80;
                        for (int i = 0; i < 3; i++) {
                            SDL_Rect cbtn = {spx + 20 + i * (csw + 8), csy, csw, 36};
                            if (SDL_PointInRect(&p, &cbtn))
                                spectrum_colorscheme = i;
                        }

                        // Filter strength slider
                        int sl_x = spx + spw - 300;
                        int sl_w = 250;
                        SDL_Rect sl_hit = {sl_x, csy, sl_w, 36};
                        if (SDL_PointInRect(&p, &sl_hit)) {
                            float frac = (float)(mx - sl_x) / (float)sl_w;
                            frac = std::clamp(frac, 0.0f, 1.0f);
                            int val = 250 + (int)(frac * (6000 - 250));
                            g_settings.heater_blanking_ms = std::clamp(((val + 125) / 250) * 250, 250, 6000);
                        }
                    } else if (settings_page == SETTINGS_SENSOR) {
                        int sy = py + 56;
                        int rh_s = 34;

                        // Calculate sy to match rendering layout
                        sy += rh_s; // status
                        sy += rh_s - 6; // i2c
                        sy += rh_s - 6; // modus
                        sy += rh_s; // retry
                        if (display_vals.valid && !demo_mode) sy += rh_s - 6; // last reading
                        if (sensor_test_result) sy += rh_s - 6; // test result
                        sy += 10;

                        // Sensor test button
                        SDL_Rect test_btn = {rx, sy, row_w, 50};
                        if (SDL_PointInRect(&p, &test_btn)) {
                            // Try to init sensor
                            BME688 test_sensor(i2c_dev, i2c_addr);
                            if (test_sensor.init()) {
                                BME688Data td = test_sensor.read_fast();
                                if (td.valid) {
                                    sensor_test_result = "Test OK! Sensor antwortet.";
                                } else {
                                    sensor_test_result = "Init OK, aber Messung fehlgeschlagen.";
                                }
                            } else {
                                sensor_test_result = "FEHLER: Sensor nicht erreichbar!";
                            }
                        }
                        sy += 60;

                        // App restart button
                        SDL_Rect restart_btn = {rx, sy, row_w, 50};
                        if (SDL_PointInRect(&p, &restart_btn)) {
                            save_retry_count(0);  // manual restart = reset counter
                            sensor_running.store(false);
                            sensor_thread.join();
                            text_cache_destroy();
                            TTF_CloseFont(font_big);
                            TTF_CloseFont(font_med);
                            TTF_CloseFont(font_small);
                            TTF_Quit();
                            SDL_DestroyRenderer(renderer);
                            SDL_DestroyWindow(window);
                            SDL_Quit();
                            execv(argv[0], argv);
                            return 1;
                        }
                        sy += 60;

                        // System reboot (only if 3+ retries)
                        if (sensor_retry_count >= 3) {
                            SDL_Rect reboot_btn = {rx, sy, row_w, 50};
                            if (SDL_PointInRect(&p, &reboot_btn)) {
                                save_retry_count(0);
                                system("sudo reboot");
                            }
                        }
                    }

                    // Click outside panel closes settings
                    if (settings_page == SETTINGS_SPECTRUM) {
                        int spw = win_w * 80 / 100, sph = win_h * 80 / 100;
                        int spx = (win_w - spw) / 2, spy = (win_h - sph) / 2;
                        SDL_Rect panel = {spx, spy, spw, sph};
                        if (!SDL_PointInRect(&p, &panel)) {
                            settings_page = SETTINGS_MAIN;
                        }
                    } else if (settings_page == SETTINGS_SENSOR) {
                        SDL_Rect panel = {px, py, pw, ph};
                        if (!SDL_PointInRect(&p, &panel)) {
                            settings_page = SETTINGS_MAIN;
                            sensor_test_result = nullptr;
                        }
                    } else if (settings_page == SETTINGS_MAIN) {
                        SDL_Rect panel = {px, py, pw, ph};
                        if (!SDL_PointInRect(&p, &panel)) {
                            settings_mode = false;
                            g_settings.save();
                        }
                    }
                } else if (weather_mode) {
                    // Weather screen: only back button
                    SDL_Rect weather_back = {win_w - 100, 2, 88, 40};
                    if (SDL_PointInRect(&p, &weather_back)) {
                        weather_mode = false;
                    }
                } else if (astro_mode) {
                    // Gear button
                    SDL_Rect astro_gear_btn = {310, 2, 42, 40};
                    if (SDL_PointInRect(&p, &astro_gear_btn)) {
                        astro_settings_open = !astro_settings_open;
                    } else if (astro_settings_open) {
                        // Sensor toggle buttons
                        int opx = 310, opy = 46;
                        bool clicked_toggle = false;
                        for (int i = 0; i < 4; i++) {
                            SDL_Rect tbtn = {opx + 8, opy + 8 + i * 56, 244, 50};
                            if (SDL_PointInRect(&p, &tbtn)) {
                                astro_show[i] = !astro_show[i];
                                clicked_toggle = true;
                            }
                        }
                        // Click outside overlay closes it
                        if (!clicked_toggle) {
                            SDL_Rect overlay = {opx, opy, 260, 4 * 56 + 16};
                            if (!SDL_PointInRect(&p, &overlay))
                                astro_settings_open = false;
                        }
                    } else if (SDL_PointInRect(&p, &astro_back_btn)) {
                        astro_mode = false;
                        astro_settings_open = false;
                    } else if (SDL_PointInRect(&p, &astro_prev_btn)) {
                        if (astro_view_day > -6) astro_view_day--;
                    } else if (SDL_PointInRect(&p, &astro_next_btn)) {
                        if (astro_view_day < 0) astro_view_day++;
                    }
                } else {
                    if (SDL_PointInRect(&p, &exit_btn))
                        running = false;
                    if (SDL_PointInRect(&p, &min_btn))
                        SDL_MinimizeWindow(window);
                    if (SDL_PointInRect(&p, &power_btn)) {
                        eco_target = (eco_target + 1) % 3;
                        eco_switch_time = SDL_GetTicks();
                        eco_pending = true;
                    }
                    if (SDL_PointInRect(&p, &astro_btn)) {
                        astro_mode = true;
                        astro_view_day = 0;
                        data_log = load_log_history();
                    }
                    if (SDL_PointInRect(&p, &weather_btn)) {
                        weather_mode = true;
                        data_log = load_log_history();
                    }
                    if (SDL_PointInRect(&p, &settings_btn)) {
                        settings_mode = true;
                        settings_page = SETTINGS_MAIN;
                    }
                }
                // Reset idle timer on any touch in eco2 mode
                if (eco_active == 2) eco_idle_start = SDL_GetTicks();
            }
        }

        uint32_t now = SDL_GetTicks();

        // Delayed ECO mode activation (1s after click)
        if (eco_pending && (now - eco_switch_time >= 1000)) {
            eco_pending = false;
            int old_active = eco_active;
            eco_active = eco_target;

            if (eco_active == 2 && old_active != 2) {
                // Entering ECO 2: dim + powersave
                system("echo 1 > /sys/class/backlight/11-0045/brightness");
                system("echo powersave | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null");
                eco_idle_start = now;
            } else if (eco_active != 2 && old_active == 2) {
                // Leaving ECO 2: restore brightness + governor
                screen_off = false;
                system("echo 31 > /sys/class/backlight/11-0045/brightness");
                system("echo ondemand | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null");
            }
            if (eco_active == 0 && old_active == 2) {
                // Already handled above
            }
        }

        // ECO 2: screen off after 20s idle
        if (eco_active == 2 && !screen_off && (now - eco_idle_start > 20000)) {
            screen_off = true;
            system("echo 0 > /sys/class/backlight/11-0045/brightness");
        }

        // Pick up new data from sensor thread (non-blocking)
        if (sensor_new_data.load()) {
            std::lock_guard<std::mutex> lock(sensor_mtx);
            prev_reading = latest;
            latest = sensor_latest;
            sensor_new_data.store(false);
            if (latest.valid) {
                // Heater compensation: skip temperature during cooldown
                if (g_settings.heater_filter) {
                    uint32_t gas_tick = last_gas_read_tick.load();
                    if (gas_tick > 0 && (now - gas_tick) < (uint32_t)g_settings.heater_blanking_ms) {
                        history.accumulate_no_temp(latest);
                    } else {
                        history.accumulate(latest);
                    }
                } else {
                    history.accumulate(latest);
                }
                // Flush to chart every ~0.5s
                if (now - last_read >= 500) {
                    history.flush();
                    last_read = now;
                }
            }
        }
        // Pick up CO2 data for daily monitor
        if (sensor_gas_new.load()) {
            float eco2_val;
            {
                std::lock_guard<std::mutex> lock(sensor_mtx);
                eco2_val = sensor_gas_eco2;
                sensor_gas_new.store(false);
            }
            history.push_co2(eco2_val);
        }

        // Save to log file every 60s (never save demo data)
        if (!demo_mode && latest.valid && (now - last_log_save >= 60000 || last_log_save == 0)) {
            last_log_save = now;
            save_log_point(latest.temperature, latest.humidity, latest.pressure, latest.eco2);
            // Also add to in-memory log
            time_t t = time(nullptr);
            struct tm* tm = localtime(&t);
            data_log.push_back({tm->tm_hour * 60 + tm->tm_min, 0,
                               latest.temperature, latest.humidity, latest.pressure, latest.eco2});
        }

        // Write web data JSON every 2s
        if (display_vals.valid && (now - last_web_save >= 2000)) {
            last_web_save = now;
            float wt = calc_trend(history.trend_temp);
            float wh = calc_trend(history.trend_hum);
            float wp = calc_trend(history.trend_pres);

            // Compute weather for web export (same logic as weather screen)
            float wpres = display_vals.pressure;
            float whum = display_vals.humidity;
            // 45min JETZT
            float wpres45 = wpres; bool whas45 = false;
            float whum45 = whum;
            // 3h PROGNOSE
            float wtrend3h = 0; bool whas3h = false;
            float whum3h = whum; bool whasHum3h = false;
            if (!data_log.empty()) {
                time_t wt2 = time(nullptr);
                struct tm* wtm = localtime(&wt2);
                int wcm = wtm->tm_hour * 60 + wtm->tm_min;
                int w45d = 999, w3hd = 999, wh3d = 999;
                float wp3h = wpres;
                for (auto& lp : data_log) {
                    if (lp.day_offset != 0) continue;
                    int d = abs((wcm - 45) - lp.minute_of_day);
                    if (d < w45d) { w45d = d; wpres45 = lp.pressure; whum45 = lp.humidity; whas45 = true; }
                    int d3 = abs((wcm - 180) - lp.minute_of_day);
                    if (d3 < w3hd) { w3hd = d3; wp3h = lp.pressure; whas3h = true; }
                    if (d3 < wh3d) { wh3d = d3; whum3h = lp.humidity; whasHum3h = true; }
                }
                if (w45d > 10) whas45 = false;
                if (w3hd > 15) whas3h = false;
                if (wh3d > 15) whasHum3h = false;
                wtrend3h = whas3h ? (wpres - wp3h) : 0;
            }
            float wd45 = whas45 ? (wpres - wpres45) : 0;
            float whd45 = whas45 ? (whum - whum45) : 0;
            float whd3h = whasHum3h ? (whum - whum3h) : 0;
            WeatherClass wjetzt = (wpres > 0 && whas45)
                ? classify_weather(wpres, whum, wd45, 0.75f, whd45)
                : WeatherClass{"Keine Daten", "", TEXT_DIM, ICON_STABLE};
            WeatherClass wprog = (wpres > 0 && whas3h)
                ? classify_weather(wpres, whum, wtrend3h, 3.0f, whd3h)
                : WeatherClass{"Noch zu wenig Daten", "Min. 3h Druckdaten", TEXT_DIM, ICON_STABLE};

            save_web_data(display_vals, history, demo_mode, sensor_ok,
                          wt, wh, wp,
                          wjetzt.text, wjetzt.detail, weather_icon_name(wjetzt.icon),
                          wprog.text, wprog.detail, weather_icon_name(wprog.icon));
        }

        // Check for settings reload from web interface
        {
            FILE* rf = fopen(SETTINGS_RELOAD_PATH, "r");
            if (rf) { fclose(rf); remove(SETTINGS_RELOAD_PATH); g_settings.load(); }
        }

        text_frame++;
        text_cache_cleanup(text_frame);

        // Smooth interpolation between readings for display (60fps)
        if (latest.valid && prev_reading.valid) {
            float t = std::min((float)(now - last_read) / (float)read_interval, 1.0f);
            // Ease-out for smooth feel
            float s = 1.0f - (1.0f - t) * (1.0f - t);
            display_vals.temperature = prev_reading.temperature + (latest.temperature - prev_reading.temperature) * s;
            display_vals.humidity = prev_reading.humidity + (latest.humidity - prev_reading.humidity) * s;
            display_vals.pressure = prev_reading.pressure + (latest.pressure - prev_reading.pressure) * s;
            display_vals.eco2 = prev_reading.eco2 + (latest.eco2 - prev_reading.eco2) * s;
            display_vals.iaq = prev_reading.iaq + (latest.iaq - prev_reading.iaq) * s;
            display_vals.gas_resistance = latest.gas_resistance;
            display_vals.valid = true;
        } else if (latest.valid) {
            display_vals = latest;
        }

        // --- Render ---
        if (weather_mode) {
            // === Weather Forecast Screen ===
            set_color(renderer, BG);
            SDL_RenderClear(renderer);

            // Title bar
            fill_rounded_rect(renderer, {8, 2, win_w - 120, 40}, 8, PANEL_BG);
            draw_text(renderer, font_med, "Wettervorhersage", 20, 12, TEXT_PRIMARY);

            // Back button
            SDL_Rect weather_back = {win_w - 100, 2, 88, 40};
            fill_rounded_rect(renderer, weather_back, 8, EXIT_BG);
            draw_text(renderer, font_med, "BACK", weather_back.x + weather_back.w / 2,
                     weather_back.y + weather_back.h / 2, TEXT_PRIMARY, true);

            // Clock
            {
                time_t t = time(nullptr);
                struct tm* tm = localtime(&t);
                char ts[32];
                strftime(ts, sizeof(ts), "%H:%M", tm);
                draw_text(renderer, font_small, ts, win_w - 200, 14, TEXT_DIM);
            }

            // === Pressure trend analysis ===
            // Get pressure readings from last 3 hours from log
            float pres_now = display_vals.valid ? display_vals.pressure : 0;
            float pres_1h = 0, pres_3h = 0;
            bool has_1h = false, has_3h = false;

            if (!data_log.empty()) {
                time_t t = time(nullptr);
                struct tm* tm_n = localtime(&t);
                int cur_min = tm_n->tm_hour * 60 + tm_n->tm_min;

                // Find pressure from ~1h and ~3h ago
                int best_1h_diff = 999, best_3h_diff = 999;
                for (auto& lp : data_log) {
                    if (lp.day_offset != 0) continue;
                    int diff_1h = abs((cur_min - 60) - lp.minute_of_day);
                    int diff_3h = abs((cur_min - 180) - lp.minute_of_day);
                    if (diff_1h < best_1h_diff) { best_1h_diff = diff_1h; pres_1h = lp.pressure; has_1h = true; }
                    if (diff_3h < best_3h_diff) { best_3h_diff = diff_3h; pres_3h = lp.pressure; has_3h = true; }
                }
                // Only valid if we found data within 15min of target
                if (best_1h_diff > 15) has_1h = false;
                if (best_3h_diff > 15) has_3h = false;
            }

            float trend_3h = has_3h ? (pres_now - pres_3h) : 0;
            float trend_1h = has_1h ? (pres_now - pres_1h) : 0;

            // === Weather classification ===
            float hum_now = display_vals.valid ? display_vals.humidity : 50.0f;

            // "JETZT" — current conditions from last 45min of data
            // Use pressure + humidity delta over ~45min window
            float pres_45 = 0, hum_45 = 0;
            bool has_45 = false;
            // Also get humidity from 3h ago for PROGNOSE
            float hum_3h = 50.0f; bool has_hum_3h = false;
            if (!data_log.empty()) {
                time_t t = time(nullptr);
                struct tm* tm_n2 = localtime(&t);
                int cur_min2 = tm_n2->tm_hour * 60 + tm_n2->tm_min;
                int best_45_diff = 999, best_h3_diff = 999;
                for (auto& lp : data_log) {
                    if (lp.day_offset != 0) continue;
                    int d45 = abs((cur_min2 - 45) - lp.minute_of_day);
                    if (d45 < best_45_diff) { best_45_diff = d45; pres_45 = lp.pressure; hum_45 = lp.humidity; has_45 = true; }
                    int d3h = abs((cur_min2 - 180) - lp.minute_of_day);
                    if (d3h < best_h3_diff) { best_h3_diff = d3h; hum_3h = lp.humidity; has_hum_3h = true; }
                }
                if (best_45_diff > 10) has_45 = false;
                if (best_h3_diff > 15) has_hum_3h = false;
            }
            float delta_45 = has_45 ? (pres_now - pres_45) : 0;
            float hum_delta_45 = has_45 ? (hum_now - hum_45) : 0;
            float hum_delta_3h = has_hum_3h ? (hum_now - hum_3h) : 0;

            WeatherClass now_wx = (pres_now > 0 && has_45)
                ? classify_weather(pres_now, hum_now, delta_45, 0.75f, hum_delta_45)
                : WeatherClass{"Keine Daten", "", TEXT_DIM, ICON_STABLE};

            // "PROGNOSE" — future from 3h trend
            WeatherClass future_wx = (pres_now > 0 && has_3h)
                ? classify_weather(pres_now, hum_now, trend_3h, 3.0f, hum_delta_3h)
                : WeatherClass{"Noch zu wenig Daten", "Min. 3h Druckdaten", TEXT_DIM, ICON_STABLE};

            // === Layout ===
            int margin = 16;
            int left_w = win_w * 45 / 100;
            int right_x = left_w + margin;
            int right_w = win_w - right_x - margin;
            int content_y = 52;

            // Left panel: JETZT (big) + PROGNOSE (smaller) side by side at top
            fill_rounded_rect(renderer, {margin, content_y, left_w - margin, win_h - content_y - margin}, 12, CARD_BG);

            int lcx = margin + (left_w - margin) / 2;  // left center x

            // --- JETZT section (top, dominant) ---
            draw_text(renderer, font_med, "JETZT", lcx, content_y + 10, {220, 220, 240, 255}, true);
            draw_weather_icon(renderer, lcx, content_y + 56, 22, now_wx.icon, now_wx.color);
            draw_text(renderer, font_big, now_wx.text, lcx, content_y + 105, now_wx.color, true);
            draw_text(renderer, font_small, now_wx.detail, lcx, content_y + 145, TEXT_DIM, true);

            // Pressure + humidity
            if (pres_now > 0) {
                char pres_str[48];
                snprintf(pres_str, sizeof(pres_str), "%.1f hPa  |  %.0f%% RH", pres_now, hum_now);
                draw_text(renderer, font_small, pres_str, lcx, content_y + 170, ACCENT_PRES, true);
            }

            // Separator
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 80, 80, 120, 100);
            SDL_RenderDrawLine(renderer, margin + 20, content_y + 192, margin + left_w - margin - 20, content_y + 192);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            // --- PROGNOSE section (smaller, below) ---
            draw_text(renderer, font_small, "PROGNOSE", lcx, content_y + 200, {180, 180, 200, 255}, true);
            draw_weather_icon(renderer, lcx, content_y + 224, 10, future_wx.icon, future_wx.color);
            draw_text(renderer, font_med, future_wx.text, lcx, content_y + 244, future_wx.color, true);
            draw_text(renderer, font_small, future_wx.detail, lcx, content_y + 268, TEXT_DIM, true);

            // 12h trend graph (pressure + humidity dual axis)
            {
                int tg_x = margin + 12, tg_y = content_y + 282;
                int tg_w = left_w - margin - 24, tg_h = win_h - content_y - margin - 296;
                if (tg_h > 60) {
                    draw_text(renderer, font_small, "12h Verlauf", tg_x, tg_y - 16, {160, 180, 220, 255});

                    fill_rounded_rect(renderer, {tg_x - 2, tg_y - 2, tg_w + 4, tg_h + 4}, 6, {26, 26, 38, 255});

                    // Collect 12h data from log
                    time_t t = time(nullptr);
                    struct tm* tm_n = localtime(&t);
                    int cur_min = tm_n->tm_hour * 60 + tm_n->tm_min;

                    std::vector<std::pair<int, float>> p12_pts;  // pressure
                    std::vector<std::pair<int, float>> h12_pts;  // humidity
                    for (auto& lp : data_log) {
                        if (lp.day_offset != 0) continue;
                        if (lp.minute_of_day >= cur_min - 720 && lp.minute_of_day <= cur_min) {
                            p12_pts.push_back({lp.minute_of_day, lp.pressure});
                            h12_pts.push_back({lp.minute_of_day, lp.humidity});
                        }
                    }

                    if (p12_pts.size() >= 2) {
                        // Pressure range
                        float p_min = 9999, p_max = -9999;
                        for (auto& pp : p12_pts) { p_min = std::min(p_min, pp.second); p_max = std::max(p_max, pp.second); }
                        float p_range = p_max - p_min;
                        if (p_range < 2.0f) { float mid = (p_min + p_max) / 2; p_min = mid - 1; p_max = mid + 1; p_range = 2.0f; }
                        p_min -= p_range * 0.1f; p_max += p_range * 0.1f;

                        int cx = tg_x + 34, cy = tg_y + 4, cw = tg_w - 68, ch = tg_h - 24;
                        int start_min_12 = cur_min - 720;

                        // Grid
                        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                        for (int g = 0; g <= 3; g++) {
                            int gy = cy + ch * g / 3;
                            SDL_SetRenderDrawColor(renderer, 60, 60, 80, 40);
                            SDL_RenderDrawLine(renderer, cx, gy, cx + cw, gy);
                            // Pressure scale (left)
                            float gv = p_max - (p_max - p_min) * g / 3.0f;
                            char gl[16]; snprintf(gl, sizeof(gl), "%.0f", gv);
                            draw_text(renderer, font_small, gl, cx - 4, gy - 7, {140, 220, 160, 255}, false);
                        }
                        // Humidity scale (right) — fixed 0-100%
                        for (int g = 0; g <= 2; g++) {
                            int gy = cy + ch * g / 2;
                            float hv = 100.0f - 100.0f * g / 2.0f;
                            char hl[8]; snprintf(hl, sizeof(hl), "%.0f%%", hv);
                            draw_text(renderer, font_small, hl, cx + cw + 4, gy - 7, {100, 180, 255, 255}, false);
                        }

                        // Pressure line (green)
                        int ppx2 = -1, ppy2 = -1;
                        for (auto& pp : p12_pts) {
                            float fx = (float)(pp.first - start_min_12) / 720.0f;
                            float fy = (pp.second - p_min) / (p_max - p_min);
                            int lx = cx + (int)(fx * cw);
                            int ly = cy + ch - (int)(fy * ch);
                            if (ppx2 >= 0) {
                                set_color(renderer, ACCENT_PRES);
                                SDL_RenderDrawLine(renderer, ppx2, ppy2, lx, ly);
                                SDL_RenderDrawLine(renderer, ppx2, ppy2 + 1, lx, ly + 1);
                            }
                            ppx2 = lx; ppy2 = ly;
                        }

                        // Humidity line (blue, semi-transparent)
                        ppx2 = -1; ppy2 = -1;
                        for (auto& hp : h12_pts) {
                            float fx = (float)(hp.first - start_min_12) / 720.0f;
                            float fy = hp.second / 100.0f;
                            int lx = cx + (int)(fx * cw);
                            int ly = cy + ch - (int)(fy * ch);
                            if (ppx2 >= 0) {
                                set_color(renderer, {80, 160, 255, 180});
                                SDL_RenderDrawLine(renderer, ppx2, ppy2, lx, ly);
                            }
                            ppx2 = lx; ppy2 = ly;
                        }

                        // Time labels
                        for (int h = 0; h <= 4; h++) {
                            int lx = cx + (int)((float)(h * 180) / 720.0f * cw);
                            char tl[8]; snprintf(tl, sizeof(tl), "-%dh", 12 - h * 3);
                            if (h == 4) snprintf(tl, sizeof(tl), "jetzt");
                            draw_text(renderer, font_small, tl, lx, cy + ch + 4, {140, 180, 220, 255}, true);
                        }

                        // Weather classification markers every 3h along the graph
                        for (int slot = 0; slot < 4; slot++) {
                            // Find pressure and humidity at this time point
                            int target_min = start_min_12 + (slot + 1) * 180;  // -9h, -6h, -3h, now
                            float slot_p = 0, slot_h = 50;
                            int best_d = 999;
                            for (size_t j = 0; j < p12_pts.size(); j++) {
                                int d = abs(p12_pts[j].first - target_min);
                                if (d < best_d) { best_d = d; slot_p = p12_pts[j].second; slot_h = h12_pts[j].second; }
                            }
                            if (best_d > 15 || slot_p < 900) continue;
                            // Get pressure + humidity delta for a ~45min window around this point
                            float slot_p_prev = 0, slot_h_prev = slot_h;
                            int best_pd = 999;
                            for (size_t j2 = 0; j2 < p12_pts.size(); j2++) {
                                int d = abs(p12_pts[j2].first - (target_min - 45));
                                if (d < best_pd) { best_pd = d; slot_p_prev = p12_pts[j2].second; slot_h_prev = h12_pts[j2].second; }
                            }
                            float slot_delta = (best_pd < 15) ? (slot_p - slot_p_prev) : 0;
                            float slot_hd = (best_pd < 15) ? (slot_h - slot_h_prev) : 0;
                            WeatherClass wc = classify_weather(slot_p, slot_h, slot_delta, 0.75f, slot_hd);
                            // Draw mini icon above the time label
                            float fx = (float)((slot + 1) * 180) / 720.0f;
                            int ix = cx + (int)(fx * cw);
                            draw_weather_icon(renderer, ix, cy + ch + 18, 6, wc.icon, wc.color);
                        }

                        // Legend
                        set_color(renderer, ACCENT_PRES);
                        SDL_RenderDrawLine(renderer, tg_x + 4, tg_y + tg_h - 8, tg_x + 20, tg_y + tg_h - 8);
                        draw_text(renderer, font_small, "hPa", tg_x + 24, tg_y + tg_h - 14, ACCENT_PRES);
                        set_color(renderer, {80, 160, 255, 255});
                        SDL_RenderDrawLine(renderer, tg_x + 64, tg_y + tg_h - 8, tg_x + 80, tg_y + tg_h - 8);
                        draw_text(renderer, font_small, "%RH", tg_x + 84, tg_y + tg_h - 14, {80, 160, 255, 255});

                        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                    } else {
                        draw_text(renderer, font_small, "Noch zu wenig Daten",
                                 tg_x + tg_w / 2, tg_y + tg_h / 2, TEXT_DIM, true);
                    }
                }
            }

            // Right panel: sensor data + pressure chart
            int rp_y = content_y;
            int rp_h = win_h - rp_y - margin;
            fill_rounded_rect(renderer, {right_x, rp_y, right_w, rp_h}, 12, CARD_BG);

            int tx = right_x + 16;
            int ty = rp_y + 14;

            // Current sensor readings
            draw_text(renderer, font_med, "Aktuelle Messwerte", tx, ty, TEXT_PRIMARY);
            ty += 28;

            char tbuf[64];
            if (display_vals.valid) {
                // Temperature
                snprintf(tbuf, sizeof(tbuf), "Temperatur:  %.1f \xC2\xB0""C", display_vals.temperature);
                draw_text(renderer, font_small, tbuf, tx, ty, ACCENT_TEMP);
                ty += 20;
                // Humidity
                snprintf(tbuf, sizeof(tbuf), "Feuchtigkeit:  %.1f %%RH", display_vals.humidity);
                draw_text(renderer, font_small, tbuf, tx, ty, ACCENT_HUM);
                ty += 20;
                // Dewpoint calculation
                float a_dp = 17.271f, b_dp = 237.7f;
                float gamma_dp = (a_dp * display_vals.temperature) / (b_dp + display_vals.temperature) + logf(display_vals.humidity / 100.0f);
                float dewpoint = (b_dp * gamma_dp) / (a_dp - gamma_dp);
                snprintf(tbuf, sizeof(tbuf), "Taupunkt:  %.1f \xC2\xB0""C", dewpoint);
                draw_text(renderer, font_small, tbuf, tx, ty, {160, 200, 255, 255});
                ty += 20;
                // CO2
                snprintf(tbuf, sizeof(tbuf), "CO\xE2\x82\x82:  %.0f ppm", display_vals.eco2);
                draw_text(renderer, font_small, tbuf, tx, ty, ACCENT_CO2);
                ty += 24;
            }

            // Separator
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 80, 80, 120, 100);
            SDL_RenderDrawLine(renderer, right_x + 12, ty, right_x + right_w - 12, ty);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            ty += 8;

            // Pressure trends
            draw_text(renderer, font_med, "Drucktrend", tx, ty, TEXT_PRIMARY);
            ty += 26;

            if (has_1h) {
                snprintf(tbuf, sizeof(tbuf), "\xce\x94 1h:  %+.1f hPa  (%.1f \xe2\x86\x92 %.1f)", trend_1h, pres_1h, pres_now);
                Color tc = fabsf(trend_1h) > 1.0f ? Color{255, 160, 80, 255} : ACCENT_PRES;
                draw_text(renderer, font_small, tbuf, tx, ty, tc);
            } else {
                draw_text(renderer, font_small, "\xce\x94 1h:  keine Daten", tx, ty, TEXT_DIM);
            }
            ty += 20;

            if (has_3h) {
                snprintf(tbuf, sizeof(tbuf), "\xce\x94 3h:  %+.1f hPa  (%.1f \xe2\x86\x92 %.1f)", trend_3h, pres_3h, pres_now);
                Color tc = fabsf(trend_3h) > 2.0f ? Color{255, 120, 60, 255} : ACCENT_PRES;
                draw_text(renderer, font_small, tbuf, tx, ty, tc);
            } else {
                draw_text(renderer, font_small, "\xce\x94 3h:  keine Daten", tx, ty, TEXT_DIM);
            }
            ty += 24;

            // Pressure chart (last 3h from log) — fills remaining space
            int pc_x = right_x + 16, pc_y = ty;
            int pc_w = right_w - 32, pc_h = rp_y + rp_h - ty - 16;
            if (pc_h > 40) {
                fill_rounded_rect(renderer, {pc_x - 2, pc_y - 2, pc_w + 4, pc_h + 4}, 6, {30, 30, 44, 255});
                draw_text(renderer, font_med, "Druck (3h)", pc_x + 4, pc_y + 2, ACCENT_PRES);

                // Collect last 3h of pressure data from log
                std::vector<std::pair<int, float>> pres_pts;
                time_t t = time(nullptr);
                struct tm* tm_n = localtime(&t);
                int cur_min = tm_n->tm_hour * 60 + tm_n->tm_min;
                for (auto& lp : data_log) {
                    if (lp.day_offset != 0) continue;
                    if (lp.minute_of_day >= cur_min - 180 && lp.minute_of_day <= cur_min)
                        pres_pts.push_back({lp.minute_of_day, lp.pressure});
                }

                if (pres_pts.size() >= 2) {
                    float pmin = 9999, pmax = -9999;
                    for (auto& pp : pres_pts) {
                        pmin = std::min(pmin, pp.second);
                        pmax = std::max(pmax, pp.second);
                    }
                    float range = pmax - pmin;
                    if (range < 1.0f) { float mid = (pmin + pmax) / 2; pmin = mid - 0.5f; pmax = mid + 0.5f; range = 1.0f; }
                    pmin -= range * 0.1f;
                    pmax += range * 0.1f;

                    int cx = pc_x + 36, cy = pc_y + 20, cw = pc_w - 44, ch = pc_h - 36;

                    // Grid
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    for (int g = 0; g <= 3; g++) {
                        int gy = cy + ch * g / 3;
                        SDL_SetRenderDrawColor(renderer, 60, 60, 80, 50);
                        SDL_RenderDrawLine(renderer, cx, gy, cx + cw, gy);
                        float gv = pmax - (pmax - pmin) * g / 3.0f;
                        char gl[16]; snprintf(gl, sizeof(gl), "%.0f", gv);
                        draw_text(renderer, font_small, gl, cx - 4, gy - 7, {140, 220, 160, 255}, false);
                    }

                    // Data line
                    int start_min = cur_min - 180;
                    int ppx = -1, ppy = -1;
                    for (auto& pp : pres_pts) {
                        float fx = (float)(pp.first - start_min) / 180.0f;
                        float fy = (pp.second - pmin) / (pmax - pmin);
                        int lx = cx + (int)(fx * cw);
                        int ly = cy + ch - (int)(fy * ch);
                        if (ppx >= 0) {
                            set_color(renderer, ACCENT_PRES);
                            SDL_RenderDrawLine(renderer, ppx, ppy, lx, ly);
                            SDL_RenderDrawLine(renderer, ppx, ppy + 1, lx, ly + 1);
                        }
                        ppx = lx; ppy = ly;
                    }

                    // Least-squares regression line (dashed)
                    if (pres_pts.size() >= 3) {
                        float rsx = 0, rsy = 0, rsxy = 0, rsxx = 0;
                        int rn = (int)pres_pts.size();
                        for (auto& pp : pres_pts) {
                            float rx = (float)(pp.first - start_min);
                            rsx += rx; rsy += pp.second; rsxy += rx * pp.second; rsxx += rx * rx;
                        }
                        float rd = rn * rsxx - rsx * rsx;
                        if (fabsf(rd) > 1e-6f) {
                            float r_slope = (rn * rsxy - rsx * rsy) / rd;
                            float r_inter = (rsy - r_slope * rsx) / rn;
                            // Draw dashed line from x=0 to x=180
                            set_color(renderer, {255, 200, 100, 180});
                            for (int px = 0; px < cw; px += 2) {
                                bool dash = ((px / 8) % 2 == 0);
                                if (!dash) continue;
                                float min_at = (float)px / (float)cw * 180.0f;
                                float pval = r_inter + r_slope * min_at;
                                float fy = (pval - pmin) / (pmax - pmin);
                                int ly = cy + ch - (int)(fy * ch);
                                ly = std::clamp(ly, cy, cy + ch);
                                SDL_RenderDrawPoint(renderer, cx + px, ly);
                                SDL_RenderDrawPoint(renderer, cx + px, ly + 1);
                            }
                        }
                    }

                    // Time labels
                    for (int h = 0; h <= 3; h++) {
                        int lx = cx + (int)((float)(h * 60) / 180.0f * cw);
                        char tl[8]; snprintf(tl, sizeof(tl), "-%dh", 3 - h);
                        if (h == 3) snprintf(tl, sizeof(tl), "jetzt");
                        draw_text(renderer, font_small, tl, lx, cy + ch + 4, {140, 180, 220, 255}, true);
                    }
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                }
            }

            apply_night_dim(renderer, win_w, win_h);
            SDL_RenderPresent(renderer);
            if (eco_active >= 1) SDL_Delay(48);
            continue;
        } else if (astro_mode) {
            draw_astro_view(renderer, font_big, font_med, font_small,
                           win_w, win_h, data_log, astro_view_day, display_vals, astro_show);
            // Astro view buttons
            fill_rounded_rect(renderer, astro_back_btn, 8, EXIT_BG);
            draw_text(renderer, font_med, "BACK", astro_back_btn.x + astro_back_btn.w / 2,
                     astro_back_btn.y + astro_back_btn.h / 2, TEXT_PRIMARY, true);
            fill_rounded_rect(renderer, astro_prev_btn, 8, PANEL_BG);
            draw_text(renderer, font_med, "<", astro_prev_btn.x + astro_prev_btn.w / 2,
                     astro_prev_btn.y + astro_prev_btn.h / 2, TEXT_PRIMARY, true);
            fill_rounded_rect(renderer, astro_next_btn, 8, PANEL_BG);
            draw_text(renderer, font_med, ">", astro_next_btn.x + astro_next_btn.w / 2,
                     astro_next_btn.y + astro_next_btn.h / 2, TEXT_PRIMARY, true);

            // Gear button for sensor toggles (left side)
            SDL_Rect astro_gear_btn = {310, 2, 42, 40};
            {
                Color gbg = astro_settings_open ? SETTINGS_HOVER : SETTINGS_BG;
                fill_rounded_rect(renderer, astro_gear_btn, 8, gbg);
                draw_gear_symbol(renderer, astro_gear_btn.x + astro_gear_btn.w / 2,
                                astro_gear_btn.y + astro_gear_btn.h / 2, 13, TEXT_PRIMARY, gbg);
            }

            // Sensor toggle overlay
            if (astro_settings_open) {
                int opw = 260, oph = 4 * 56 + 16;
                int opx = 310, opy = 46;
                fill_rounded_rect(renderer, {opx, opy, opw, oph}, 10, {26, 26, 38, 245});

                const char* sensor_names[] = {"Temperatur", "Feuchtigkeit", "Luftdruck", "CO\xE2\x82\x82"};
                Color sensor_colors[] = {ACCENT_TEMP, ACCENT_HUM, ACCENT_PRES, ACCENT_CO2};

                // Filter log data for mini chart preview
                std::vector<const LogPoint*> day_pts;
                for (const auto& lp : data_log) {
                    if (lp.day_offset == astro_view_day) day_pts.push_back(&lp);
                }

                for (int i = 0; i < 4; i++) {
                    int ry = opy + 8 + i * 56;
                    Color c = astro_show[i] ? sensor_colors[i] : Color{80, 80, 100, 255};
                    Color bg = astro_show[i] ? CARD_BG : Color{30, 30, 38, 255};
                    fill_rounded_rect(renderer, {opx + 8, ry, opw - 16, 50}, 8, bg);

                    // Sensor name
                    draw_text(renderer, font_med, sensor_names[i], opx + 18, ry + 6, c);

                    // Mini chart preview (right side of the row)
                    int mc_x = opx + 140, mc_y = ry + 6, mc_w = 100, mc_h = 36;
                    if (day_pts.size() >= 2) {
                        float ranges[][2] = {{10,35},{0,100},{980,1040},{0,2000}};
                        float smin = ranges[i][0], smax = ranges[i][1];
                        auto gv = [i](const LogPoint* p) -> float {
                            switch (i) { case 0: return p->temperature; case 1: return p->humidity;
                                         case 2: return p->pressure; default: return p->eco2; }
                        };
                        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                        set_color(renderer, {c.r, c.g, c.b, (uint8_t)(astro_show[i] ? 180 : 60)});
                        int prev_px = -1, prev_py = -1;
                        for (size_t j = 0; j < day_pts.size(); j++) {
                            float v = gv(day_pts[j]);
                            float n = std::clamp((v - smin) / (smax - smin), 0.0f, 1.0f);
                            int px_x = mc_x + (int)((float)day_pts[j]->minute_of_day / 1440.0f * mc_w);
                            int px_y = mc_y + mc_h - (int)(n * mc_h);
                            if (prev_px >= 0) SDL_RenderDrawLine(renderer, prev_px, prev_py, px_x, px_y);
                            prev_px = px_x; prev_py = px_y;
                        }
                        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                    }
                }
            }

            apply_night_dim(renderer, win_w, win_h);
            SDL_RenderPresent(renderer);
            if (eco_active >= 1) SDL_Delay(48);
            continue;
        }

        set_color(renderer, BG);
        SDL_RenderClear(renderer);

        // Determine air quality status
        const char* status_text = "Warte...";
        Color status_color = TEXT_DIM;
        if (display_vals.valid) {
            bool temp_ok = display_vals.temperature >= 16 && display_vals.temperature <= 20;
            bool temp_warn = display_vals.temperature >= 14 && display_vals.temperature <= 24;
            bool hum_ok = display_vals.humidity >= 40 && display_vals.humidity <= 60;
            bool hum_warn = display_vals.humidity >= 30 && display_vals.humidity <= 70;
            bool co2_ok = display_vals.eco2 < 1000;
            bool co2_warn = display_vals.eco2 < 1500;

            if (temp_ok && hum_ok && co2_ok) {
                status_text = "Alles optimal";
                status_color = {100, 220, 100, 255};
            } else if (!co2_warn) {
                status_text = "Sofort l\xC3\xBC" "ften!";
                status_color = {255, 70, 70, 255};
            } else if (!co2_ok) {
                status_text = "Bitte l\xC3\xBC" "ften";
                status_color = {255, 200, 60, 255};
            } else if (!hum_warn) {
                if (display_vals.humidity < 30) {
                    status_text = "Zu trocken";
                } else {
                    status_text = "Zu feucht - l\xC3\xBC" "ften";
                }
                status_color = {255, 70, 70, 255};
            } else if (!hum_ok) {
                if (display_vals.humidity < 40) {
                    status_text = "Etwas trocken";
                } else {
                    status_text = "Etwas feucht";
                }
                status_color = {255, 200, 60, 255};
            } else if (!temp_warn) {
                status_text = display_vals.temperature < 14 ? "Zu kalt" : "Zu warm - l\xC3\xBC" "ften";
                status_color = {255, 70, 70, 255};
            } else if (!temp_ok) {
                status_text = display_vals.temperature < 16 ? "Etwas k\xC3\xBC" "hl" : "Etwas warm";
                status_color = {255, 200, 60, 255};
            }
        }

        // Title bar (ends before settings button with margin)
        // settings_btn left edge = win_w - 346, leave 16px gap
        fill_rounded_rect(renderer, {8, 8, win_w - 446, 40}, 8, PANEL_BG);
        draw_text(renderer, font_med, "Luftqualit\xC3\xA4t", 20, 16, TEXT_PRIMARY);

        // Status indicator (dot + text)
        if (latest.valid) {
            // Small colored dot
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            set_color(renderer, status_color);
            for (int dy = -4; dy <= 4; dy++) {
                int dx = (int)sqrt(16 - dy * dy);
                SDL_RenderDrawLine(renderer, 180, 28 + dy, 180 + dx * 2, 28 + dy);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            draw_text(renderer, font_small, status_text, 194, 22, status_color);
        }

        if (demo_mode) {
            draw_text(renderer, font_small, "[DEMO]", win_w / 2, 22, {255, 200, 50, 255});
        }

        // Timestamp
        {
            time_t t = time(nullptr);
            struct tm* tm = localtime(&t);
            char ts[32];
            strftime(ts, sizeof(ts), "%H:%M:%S", tm);
            // Clock right-aligned inside title bar (title bar ends at win_w-370+8=win_w-362)
            // "HH:MM:SS" ~56px wide at 14px font, place so it ends ~10px before title bar right edge
            draw_text(renderer, font_small, ts, win_w - 516, 22, TEXT_DIM);
        }

        // Cloud sync status indicator
        {
            static uint32_t last_sync_check = 0;
            static bool sync_online = false;
            static int sync_age_s = 999;
            // Check sync_status file every 2s
            if (now - last_sync_check >= 2000) {
                last_sync_check = now;
                FILE* sf = fopen(SYNC_STATUS_PATH, "r");
                if (sf) {
                    long ts = 0;
                    char st[8] = {};
                    if (fscanf(sf, "%ld %7s", &ts, st) >= 1) {
                        sync_age_s = (int)(time(nullptr) - ts);
                        sync_online = (sync_age_s < 10 && strcmp(st, "ok") == 0);
                    }
                    fclose(sf);
                } else {
                    sync_online = false;
                    sync_age_s = 999;
                }
            }
            // Draw cloud icon + status
            Color sync_col = sync_online ? Color{80, 220, 120, 255} : Color{180, 60, 60, 200};
            const char* sync_lbl = sync_online ? "Online" : "Offline";
            // Small dot
            // Place after clock: clock at win_w-516, ~56px wide → sync at win_w-580
            int sync_x = win_w - 600;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            set_color(renderer, sync_col);
            for (int dy = -3; dy <= 3; dy++) {
                int dx = (int)sqrt(9 - dy * dy);
                SDL_RenderDrawLine(renderer, sync_x, 28 + dy, sync_x + dx * 2, 28 + dy);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            draw_text(renderer, font_small, sync_lbl, sync_x + 12, 22, sync_col);
        }

        // Power save button - shows target mode immediately
        const char* eco_label = "ECO";
        Color eco_bg = {60, 60, 80, 255};
        Color eco_hover_c = {80, 80, 100, 255};
        if (eco_target == 1) {
            eco_label = "ECO 1";
            eco_bg = {40, 100, 120, 255};
            eco_hover_c = {50, 120, 140, 255};
        } else if (eco_target == 2) {
            eco_label = "ECO 2";
            eco_bg = {40, 120, 40, 255};
            eco_hover_c = {50, 150, 50, 255};
        }
        fill_rounded_rect(renderer, power_btn, 8, power_hover ? eco_hover_c : eco_bg);
        draw_text(renderer, font_med, eco_label, power_btn.x + power_btn.w / 2, power_btn.y + power_btn.h / 2, TEXT_PRIMARY, true);

        // Settings button (gear icon)
        {
            Color sbg = settings_hover ? SETTINGS_HOVER : SETTINGS_BG;
            fill_rounded_rect(renderer, settings_btn, 8, sbg);
            draw_gear_symbol(renderer, settings_btn.x + settings_btn.w / 2,
                            settings_btn.y + settings_btn.h / 2, 13, TEXT_PRIMARY, sbg);
        }

        // Astro button
        fill_rounded_rect(renderer, astro_btn, 8, astro_hover ? ASTRO_HOVER : ASTRO_BG);
        draw_text(renderer, font_med, "ASTRO", astro_btn.x + astro_btn.w / 2, astro_btn.y + astro_btn.h / 2, TEXT_PRIMARY, true);

        // Weather button (sun+cloud symbol)
        {
            Color wbg = weather_hover ? Color{70, 80, 110, 255} : Color{50, 60, 90, 255};
            fill_rounded_rect(renderer, weather_btn, 8, wbg);
            // Draw simple sun+cloud: small sun circle + cloud arc
            int wcx = weather_btn.x + weather_btn.w / 2;
            int wcy = weather_btn.y + weather_btn.h / 2;
            // Sun (yellow circle, top-right)
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            set_color(renderer, {255, 200, 60, 255});
            for (int dy = -7; dy <= 7; dy++) {
                int dx = (int)sqrtf(49.0f - dy * dy);
                SDL_RenderDrawLine(renderer, wcx + 4 - dx, wcy - 4 + dy, wcx + 4 + dx, wcy - 4 + dy);
            }
            // Cloud (white arcs, bottom-left)
            set_color(renderer, {200, 210, 230, 255});
            for (int dy = -5; dy <= 5; dy++) {
                int dx = (int)sqrtf(25.0f - dy * dy);
                if (wcy + 2 + dy >= wcy - 2)
                    SDL_RenderDrawLine(renderer, wcx - 6 - dx, wcy + 2 + dy, wcx - 6 + dx, wcy + 2 + dy);
            }
            for (int dy = -4; dy <= 4; dy++) {
                int dx = (int)sqrtf(16.0f - dy * dy);
                if (wcy + 1 + dy >= wcy - 2)
                    SDL_RenderDrawLine(renderer, wcx + 1 - dx, wcy + 1 + dy, wcx + 1 + dx, wcy + 1 + dy);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }

        // Minimize button
        fill_rounded_rect(renderer, min_btn, 8, min_hover ? Color{80, 80, 100, 255} : Color{60, 60, 80, 255});
        // Draw a "_" line for minimize
        set_color(renderer, TEXT_PRIMARY);
        SDL_RenderDrawLine(renderer, min_btn.x + 15, min_btn.y + 28, min_btn.x + 35, min_btn.y + 28);
        SDL_RenderDrawLine(renderer, min_btn.x + 15, min_btn.y + 29, min_btn.x + 35, min_btn.y + 29);

        // Exit button
        fill_rounded_rect(renderer, exit_btn, 8, exit_hover ? EXIT_HOVER : EXIT_BG);
        draw_text(renderer, font_med, "EXIT", exit_btn.x + exit_btn.w / 2, exit_btn.y + exit_btn.h / 2, TEXT_PRIMARY, true);

        // Layout
        int card_margin = 10;
        int top_y = 60;
        int card_w = (win_w - 5 * card_margin) / 4;
        int card_h = 90;

        // Value cards with trend arrows
        if (display_vals.valid) {
            // Compute trends over 5-minute window
            float t_temp = calc_trend(history.trend_temp);
            float t_hum = calc_trend(history.trend_hum);
            float t_pres = calc_trend(history.trend_pres);

            draw_value_card(renderer, font_big, font_small,
                {card_margin, top_y, card_w, card_h},
                "Temperatur", display_vals.temperature, "\xC2\xB0""C", ACCENT_TEMP,
                "%.1f", trend_arrow(t_temp, 0.01f), trend_color(t_temp, 0.01f));

            draw_value_card(renderer, font_big, font_small,
                {card_margin * 2 + card_w, top_y, card_w, card_h},
                "Luftfeuchtigkeit", display_vals.humidity, "%RH", ACCENT_HUM,
                "%.1f", trend_arrow(t_hum, 0.05f), trend_color(t_hum, 0.05f));

            draw_value_card(renderer, font_big, font_small,
                {card_margin * 3 + card_w * 2, top_y, card_w, card_h},
                "Luftdruck", display_vals.pressure, "hPa", ACCENT_PRES,
                "%.1f", trend_arrow(t_pres, 0.005f), trend_color(t_pres, 0.005f));

            draw_value_card(renderer, font_big, font_small,
                {card_margin * 4 + card_w * 3, top_y, card_w, card_h},
                "CO\xE2\x82\x82 (est.)", display_vals.eco2, "ppm", ACCENT_CO2, "%.0f");
        } else {
            fill_rounded_rect(renderer, {card_margin, top_y, win_w - 2 * card_margin, card_h}, 10, CARD_BG);
            draw_text(renderer, font_med, "Warte auf Sensordaten...", win_w / 2, top_y + card_h / 2, TEXT_DIM, true);
        }

        // Charts (2x2) with smooth interpolated tips
        int chart_top = top_y + card_h + card_margin;
        int chart_area_h = win_h - chart_top - card_margin;
        int chart_h_each = (chart_area_h - 3 * card_margin) / 2;
        int chart_w_each = (win_w - 3 * card_margin) / 2;

        float ifrac = std::min((float)(now - last_read) / (float)read_interval, 1.0f);

        draw_chart(renderer, font_small, history.temperature,
                   {card_margin, chart_top, chart_w_each, chart_h_each},
                   ACCENT_TEMP, "Temperatur", "\xC2\xB0""C",
                   NAN, NAN, ZONES_TEMP,
                   display_vals.valid ? display_vals.temperature : NAN, ifrac,
                   HISTORY_SIZE, 3.0f);

        draw_chart(renderer, font_small, history.humidity,
                   {card_margin * 2 + chart_w_each, chart_top, chart_w_each, chart_h_each},
                   ACCENT_HUM, "Luftfeuchtigkeit", "%RH",
                   0, 100, ZONES_HUM,
                   display_vals.valid ? display_vals.humidity : NAN, ifrac);

        draw_chart(renderer, font_small, history.pressure,
                   {card_margin, chart_top + chart_h_each + card_margin, chart_w_each, chart_h_each},
                   ACCENT_PRES, "Luftdruck", "hPa",
                   NAN, NAN, ZONES_NONE,
                   display_vals.valid ? display_vals.pressure : NAN, ifrac);

        draw_co2_daily(renderer, font_small, history.eco2,
                       {card_margin * 2 + chart_w_each, chart_top + chart_h_each + card_margin, chart_w_each, chart_h_each},
                       ACCENT_CO2);

        // --- Settings overlay (drawn on top of main screen) ---
        if (settings_mode) {
            // Dim background
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
            SDL_Rect full = {0, 0, win_w, win_h};
            SDL_RenderFillRect(renderer, &full);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            // Panel dimensions (must match click handler)
            int pw = 560, ph = 600;
                    if (settings_page == SETTINGS_MAIN) {
                        // 56 header + rows*46 + buttons area
                        int n_extra = 2;  // heater toggle + night toggle always shown
                        if (g_settings.heater_filter) n_extra++;
                        if (g_settings.night_mode_auto) n_extra++;
                        ph = 56 + (5 + n_extra) * 46 + 250;
                        ph = std::min(ph, win_h - 20);  // never exceed screen
                    }
            if (settings_page == SETTINGS_SPIKE_REVIEW) ph = 540;
            else if (settings_page == SETTINGS_SPIKE_SELECT) ph = 420;
            else if (settings_page == SETTINGS_SENSOR) ph = sensor_retry_count >= 3 ? 500 : 440;
            int px = (win_w - pw) / 2, py = (win_h - ph) / 2;
            int pad = 20;
            int row_w = pw - 2 * pad;
            int rx = px + pad;

            // Spectrum page draws its own large panel - skip standard panel for it
            if (settings_page != SETTINGS_SPECTRUM) {
                // Panel background with border glow
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 60, 60, 100, 40);
                SDL_Rect glow = {px - 3, py - 3, pw + 6, ph + 6};
                SDL_RenderFillRect(renderer, &glow);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                fill_rounded_rect(renderer, {px, py, pw, ph}, 12, {26, 26, 38, 255});

                // Top bar with title
                fill_rounded_rect(renderer, {px, py, pw, 44}, 12, {35, 35, 52, 255});
                // Fix bottom corners of top bar (overlay with panel bg)
                SDL_Rect bar_fix = {px, py + 32, pw, 12};
                set_color(renderer, {35, 35, 52, 255});
                SDL_RenderFillRect(renderer, &bar_fix);

                // Separator line
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 80, 80, 120, 100);
                SDL_RenderDrawLine(renderer, px + 12, py + 44, px + pw - 12, py + 44);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

                // Close button (X) top-right
                SDL_Rect close_btn = {px + pw - 44, py + 6, 38, 32};
                fill_rounded_rect(renderer, close_btn, 6, {80, 50, 50, 255});
                draw_text(renderer, font_med, "X", close_btn.x + close_btn.w / 2,
                         close_btn.y + close_btn.h / 2, TEXT_PRIMARY, true);
            }

            if (settings_page == SETTINGS_MAIN) {
                // Title with gear icon
                draw_gear_symbol(renderer, px + 24, py + 22, 11, TEXT_DIM, {35, 35, 52, 255});
                draw_text(renderer, font_med, "Einstellungen", px + 44, py + 13, TEXT_PRIMARY);

                int base_y = py + 56;
                int rh = 46;
                // Touch-friendly arrows: 48x44
                int aw = 48, ah = 44;

                // Helper: draw a settings row with label, value centered between < > arrows
                // Layout: [label.............<  value  >]
                // Left arrow at rx+row_w - aw - 110 - aw
                // Right arrow at rx+row_w - aw
                // Value centered in the 110px gap between arrows
                auto draw_row = [&](int row, const char* label, const char* value) {
                    int ry = base_y + row * rh;
                    fill_rounded_rect(renderer, {rx, ry, row_w, ah}, 8, CARD_BG);
                    draw_text(renderer, font_med, label, rx + 14, ry + ah / 2 - 8, TEXT_PRIMARY);

                    int la_x = rx + row_w - aw - 110 - aw;
                    int ra_x = rx + row_w - aw;

                    // Left arrow button
                    fill_rounded_rect(renderer, {la_x, ry, aw, ah}, 6, PANEL_BG);
                    draw_text(renderer, font_med, "<", la_x + aw / 2, ry + ah / 2, TEXT_PRIMARY, true);

                    // Right arrow button
                    fill_rounded_rect(renderer, {ra_x, ry, aw, ah}, 6, PANEL_BG);
                    draw_text(renderer, font_med, ">", ra_x + aw / 2, ry + ah / 2, TEXT_PRIMARY, true);

                    // Value centered in the gap between arrows
                    int val_cx = la_x + aw + 55;  // center of the 110px gap
                    draw_text(renderer, font_med, value, val_cx, ry + ah / 2, ACCENT_HUM, true);
                };

                // Timezone
                int tz_idx = std::clamp((g_settings.utc_offset_min / 60) + 12, 0, 24);
                draw_row(0, "Zeitzone", TZ_NAMES[tz_idx]);

                // Sensor interval
                char rr_str[32]; snprintf(rr_str, sizeof(rr_str), "%d ms", g_settings.sensor_interval_ms);
                draw_row(1, "Sensor-Intervall", rr_str);

                // Log duration
                char ld_str[32]; snprintf(ld_str, sizeof(ld_str), "%d Tage", g_settings.log_days);
                draw_row(2, "Log-Dauer", ld_str);

                // Spike threshold
                char st_str[32]; snprintf(st_str, sizeof(st_str), "%.0f%%", g_settings.spike_threshold * 100);
                draw_row(3, "Spike-Schwelle", st_str);

                // Gas/CO2 interval
                char gi_str[32]; snprintf(gi_str, sizeof(gi_str), "%d s", g_settings.gas_interval_s);
                draw_row(4, "CO\xE2\x82\x82-Intervall", gi_str);

                // Heater filter toggle (row 5)
                {
                    int ry = base_y + 5 * rh;
                    fill_rounded_rect(renderer, {rx, ry, row_w, ah}, 8, CARD_BG);
                    draw_text(renderer, font_med, "Heater-Filter", rx + 14, ry + ah / 2 - 8, TEXT_PRIMARY);
                    // ON/OFF toggle
                    int tbw = 70, tbh = 36;
                    int tbx = rx + row_w - tbw - 8, tby = ry + (ah - tbh) / 2;
                    Color on_c = g_settings.heater_filter ? BTN_GREEN : Color{80, 60, 60, 255};
                    fill_rounded_rect(renderer, {tbx, tby, tbw, tbh}, 6, on_c);
                    draw_text(renderer, font_med, g_settings.heater_filter ? "AN" : "AUS",
                             tbx + tbw / 2, tby + tbh / 2, TEXT_PRIMARY, true);
                }

                // Blanking time (row 6, only when filter is on)
                int extra_rows = 0;
                if (g_settings.heater_filter) {
                    char bl_str[32]; snprintf(bl_str, sizeof(bl_str), "%d ms", g_settings.heater_blanking_ms);
                    draw_row(6, "Blanking-Zeit", bl_str);
                    extra_rows = 1;
                }

                // Night mode toggle (row 6+extra)
                {
                    int nrow = 6 + extra_rows;
                    int ry = base_y + nrow * rh;
                    fill_rounded_rect(renderer, {rx, ry, row_w, ah}, 8, CARD_BG);
                    draw_text(renderer, font_med, "Nachtmodus", rx + 14, ry + ah / 2 - 8, TEXT_PRIMARY);
                    int tbw = 70, tbh = 36;
                    int tbx = rx + row_w - tbw - 8, tby = ry + (ah - tbh) / 2;
                    Color on_c = g_settings.night_mode_auto ? BTN_GREEN : Color{80, 60, 60, 255};
                    fill_rounded_rect(renderer, {tbx, tby, tbw, tbh}, 6, on_c);
                    draw_text(renderer, font_med, g_settings.night_mode_auto ? "AN" : "AUS",
                             tbx + tbw / 2, tby + tbh / 2, TEXT_PRIMARY, true);
                    extra_rows += 1;
                }

                // Night mode hours (row after toggle, only when on)
                if (g_settings.night_mode_auto) {
                    int nrow = 6 + extra_rows;
                    char nh_str[32]; snprintf(nh_str, sizeof(nh_str), "%02d:00-%02d:00",
                        g_settings.night_start_h, g_settings.night_end_h);
                    draw_row(nrow, "Nacht-Zeit", nh_str);
                    extra_rows += 1;
                }

                // Spektrum button
                int btn_base = base_y + (6 + extra_rows) * rh + 10;
                fill_rounded_rect(renderer, {rx, btn_base, row_w, 50}, 8, Color{50, 60, 90, 255});
                draw_text(renderer, font_med, "FFT-Spektrum",
                         rx + row_w / 2, btn_base + 25, TEXT_PRIMARY, true);

                // Spike filter button
                fill_rounded_rect(renderer, {rx, btn_base + 60, row_w, 50}, 8, ASTRO_BG);
                draw_text(renderer, font_med, "Spike-Filter aktivieren",
                         rx + row_w / 2, btn_base + 85, TEXT_PRIMARY, true);

                // Sensor diagnostics button
                {
                    Color sbg = sensor_ok ? Color{40, 80, 60, 255} : Color{140, 60, 40, 255};
                    fill_rounded_rect(renderer, {rx, btn_base + 120, row_w, 50}, 8, sbg);
                    const char* slbl = sensor_ok ? "Sensor (OK)" : "Sensor (FEHLER)";
                    draw_text(renderer, font_med, slbl, rx + row_w / 2, btn_base + 145, TEXT_PRIMARY, true);
                }

                // Reset button
                fill_rounded_rect(renderer, {rx, btn_base + 180, row_w, 50}, 8, EXIT_BG);
                draw_text(renderer, font_med, "Auf Standard zur\xC3\xBC" "cksetzen",
                         rx + row_w / 2, btn_base + 205, TEXT_PRIMARY, true);

            } else if (settings_page == SETTINGS_SPIKE_SELECT) {
                draw_text(renderer, font_med, "Datensatz w\xC3\xA4hlen", px + 20, py + 13, TEXT_PRIMARY);

                int base_y = py + 56;
                int bh = 52;
                Color field_colors[] = {ACCENT_TEMP, ACCENT_HUM, ACCENT_PRES, ACCENT_CO2};
                for (int i = 0; i < 4; i++) {
                    SDL_Rect btn = {rx, base_y + i * (bh + 8), row_w, bh};
                    fill_rounded_rect(renderer, btn, 8, CARD_BG);
                    draw_text(renderer, font_med, FIELD_NAMES[i],
                             btn.x + btn.w / 2, btn.y + btn.h / 2, field_colors[i], true);
                }
                // "Alle analysieren" button
                SDL_Rect all_btn = {rx, base_y + 4 * (bh + 8) + 8, row_w, bh};
                fill_rounded_rect(renderer, all_btn, 8, ASTRO_BG);
                draw_text(renderer, font_med, "Alle analysieren",
                         all_btn.x + all_btn.w / 2, all_btn.y + all_btn.h / 2, TEXT_PRIMARY, true);

            } else if (settings_page == SETTINGS_SPIKE_REVIEW) {
                // Use per-spike field when reviewing all
                int cur_field = (spike_field == -1 && !spike_results.empty() && spike_current < (int)spike_results.size())
                    ? spike_results[spike_current].field : spike_field;
                Color sc = cur_field == 0 ? ACCENT_TEMP : cur_field == 1 ? ACCENT_HUM :
                           cur_field == 2 ? ACCENT_PRES : ACCENT_CO2;
                draw_text(renderer, font_med, "Spike-Filter", px + 20, py + 8, TEXT_PRIMARY);
                if (cur_field >= 0 && cur_field < 4)
                    draw_text(renderer, font_med, FIELD_NAMES[cur_field], px + 180, py + 8, sc);
                else
                    draw_text(renderer, font_med, "Alle", px + 180, py + 8, TEXT_DIM);

                if (spike_results.empty() || spike_current >= (int)spike_results.size()) {
                    draw_text(renderer, font_big, "Keine Spikes",
                             px + pw / 2, py + ph / 2 - 20, TEXT_DIM, true);
                    char info[64];
                    snprintf(info, sizeof(info), "Schwelle: %.0f%%", g_settings.spike_threshold * 100);
                    draw_text(renderer, font_med, info, px + pw / 2, py + ph / 2 + 20, TEXT_DIM, true);
                } else {
                    const SpikeInfo& sp = spike_results[spike_current];
                    char prog[64];
                    snprintf(prog, sizeof(prog), "%d / %d", spike_current + 1, (int)spike_results.size());
                    draw_text(renderer, font_med, prog, px + pw - 60, py + 13, TEXT_DIM);

                    // Chart inside panel
                    int chart_x = rx, chart_y = py + 50;
                    int chart_w = row_w, chart_h = ph - 140;
                    fill_rounded_rect(renderer, {chart_x - 4, chart_y - 4, chart_w + 8, chart_h + 8}, 8, CARD_BG);

                    int sf = sp.field;  // use per-spike field
                    auto get = [](const LogPoint& lp, int f) -> float {
                        switch (f) { case 0: return lp.temperature; case 1: return lp.humidity;
                                     case 2: return lp.pressure; default: return lp.eco2; }
                    };

                    int center = sp.log_index;
                    int start = std::max(0, center - 30);
                    int end = std::min((int)data_log.size(), center + 30);

                    if (end > start + 1) {
                        float vmin = 1e9, vmax = -1e9;
                        for (int i = start; i < end; i++) {
                            float v = get(data_log[i], sf);
                            vmin = std::min(vmin, v);
                            vmax = std::max(vmax, v);
                        }
                        float margin = (vmax - vmin) * 0.15f;
                        if (margin < 1) margin = 1;
                        vmin -= margin; vmax += margin;

                        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                        for (int i = start + 1; i < end && i < (int)data_log.size(); i++) {
                            float v0 = get(data_log[i-1], sf);
                            float v1 = get(data_log[i], sf);
                            float n0 = std::clamp((v0 - vmin) / (vmax - vmin), 0.0f, 1.0f);
                            float n1 = std::clamp((v1 - vmin) / (vmax - vmin), 0.0f, 1.0f);
                            float t0 = (float)(i - 1 - start) / (float)(end - start - 1);
                            float t1 = (float)(i - start) / (float)(end - start - 1);
                            int x0 = chart_x + (int)(t0 * chart_w);
                            int y0 = chart_y + chart_h - (int)(n0 * chart_h);
                            int x1 = chart_x + (int)(t1 * chart_w);
                            int y1 = chart_y + chart_h - (int)(n1 * chart_h);

                            if (i == center) {
                                set_color(renderer, {255, 50, 50, 255});
                                SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
                                SDL_RenderDrawLine(renderer, x0, y0+1, x1, y1+1);
                                SDL_RenderDrawLine(renderer, x0, y0-1, x1, y1-1);
                                for (int dy = -5; dy <= 5; dy++)
                                    for (int dx = -5; dx <= 5; dx++)
                                        if (dx*dx+dy*dy <= 25)
                                            SDL_RenderDrawPoint(renderer, x1+dx, y1+dy);
                            } else {
                                set_color(renderer, sc);
                                SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
                            }
                        }

                        char vlbl[64];
                        snprintf(vlbl, sizeof(vlbl), "%.1f \xe2\x86\x92 %.1f  (%.0f%%)",
                                sp.prev_value, sp.value, sp.jump_pct * 100);
                        draw_text(renderer, font_med, vlbl, chart_x + chart_w / 2,
                                 chart_y + chart_h + 6, {255, 80, 80, 255}, true);
                        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                    }

                    // Delete / Skip buttons (bigger for touch)
                    int btn_y = py + ph - 60;
                    SDL_Rect del_btn = {px + pw / 2 - 165, btn_y, 160, 52};
                    SDL_Rect skip_btn = {px + pw / 2 + 5, btn_y, 160, 52};
                    fill_rounded_rect(renderer, del_btn, 8, EXIT_BG);
                    draw_text(renderer, font_med, "L\xC3\xB6schen",
                             del_btn.x + del_btn.w / 2, del_btn.y + del_btn.h / 2, TEXT_PRIMARY, true);
                    fill_rounded_rect(renderer, skip_btn, 8, PANEL_BG);
                    draw_text(renderer, font_med, "\xC3\x9C" "berspringen",
                             skip_btn.x + skip_btn.w / 2, skip_btn.y + skip_btn.h / 2, TEXT_PRIMARY, true);
                }

            } else if (settings_page == SETTINGS_SPECTRUM) {
                // Large spectrum overlay (~80% of screen), drawn on top of dimmed bg
                int spw = win_w * 80 / 100, sph = win_h * 80 / 100;
                int spx = (win_w - spw) / 2, spy = (win_h - sph) / 2;

                // Panel background
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 60, 60, 100, 40);
                SDL_Rect sp_glow = {spx - 3, spy - 3, spw + 6, sph + 6};
                SDL_RenderFillRect(renderer, &sp_glow);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                fill_rounded_rect(renderer, {spx, spy, spw, sph}, 12, {18, 18, 28, 255});

                // Top bar
                fill_rounded_rect(renderer, {spx, spy, spw, 44}, 12, {30, 30, 46, 255});
                SDL_Rect sp_fix = {spx, spy + 32, spw, 12};
                set_color(renderer, {30, 30, 46, 255});
                SDL_RenderFillRect(renderer, &sp_fix);

                // Title
                const char* field_names[] = {"Temperatur", "Feuchtigkeit", "Luftdruck", "CO\xE2\x82\x82"};
                (void)0;  // field_units removed - Y axis now in dB
                const Color field_colors[] = {ACCENT_TEMP, ACCENT_HUM, ACCENT_PRES, ACCENT_CO2};
                char sp_title[64];
                snprintf(sp_title, sizeof(sp_title), "FFT-Spektrum: %s", field_names[spectrum_field]);
                draw_text(renderer, font_med, sp_title, spx + 20, spy + 13, TEXT_PRIMARY);

                // Close button
                SDL_Rect sp_close = {spx + spw - 44, spy + 6, 38, 32};
                fill_rounded_rect(renderer, sp_close, 6, {80, 50, 50, 255});
                draw_text(renderer, font_med, "X", sp_close.x + sp_close.w / 2,
                         sp_close.y + sp_close.h / 2, TEXT_PRIMARY, true);

                // Separator
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 80, 80, 120, 100);
                SDL_RenderDrawLine(renderer, spx + 12, spy + 44, spx + spw - 12, spy + 44);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

                // === Top control bar ===
                int dbtn_y = spy + 50;

                // Data source buttons (left side, compact)
                int dbtn_w = (spw - 300) / 4;
                for (int i = 0; i < 4; i++) {
                    SDL_Rect dbtn = {spx + 16 + i * (dbtn_w + 4), dbtn_y, dbtn_w, 36};
                    Color bg = (i == spectrum_field) ? field_colors[i] : Color{50, 50, 65, 255};
                    Color fg = (i == spectrum_field) ? Color{0, 0, 0, 255} : TEXT_DIM;
                    fill_rounded_rect(renderer, dbtn, 6, bg);
                    draw_text(renderer, font_small, field_names[i],
                             dbtn.x + dbtn.w / 2, dbtn.y + dbtn.h / 2, fg, true);
                }

                // Nyquist buttons + Hz/Period toggle (right side)
                int right_x = spx + spw - 260;
                const char* nq_labels[] = {"1 Hz", "5 Hz"};
                for (int i = 0; i < 2; i++) {
                    SDL_Rect nqb = {right_x + i * 66, dbtn_y, 60, 36};
                    Color bg = (i == spectrum_nyquist) ? Color{100, 140, 220, 255} : Color{50, 50, 65, 255};
                    Color fg = (i == spectrum_nyquist) ? Color{0, 0, 0, 255} : TEXT_DIM;
                    fill_rounded_rect(renderer, nqb, 6, bg);
                    draw_text(renderer, font_small, nq_labels[i],
                             nqb.x + nqb.w / 2, nqb.y + nqb.h / 2, fg, true);
                }
                // Hz / Period toggle
                SDL_Rect hz_btn = {right_x + 140, dbtn_y, 100, 36};
                fill_rounded_rect(renderer, hz_btn, 6,
                    spectrum_x_period ? Color{180, 130, 60, 255} : Color{50, 50, 65, 255});
                draw_text(renderer, font_small, spectrum_x_period ? "Periode" : "Frequenz",
                         hz_btn.x + hz_btn.w / 2, hz_btn.y + hz_btn.h / 2,
                         spectrum_x_period ? Color{0, 0, 0, 255} : TEXT_DIM, true);

                // === Chart area ===
                int chart_x = spx + 52, chart_y = spy + 96;
                int chart_w = spw - 72, chart_h = sph - 168;
                fill_rounded_rect(renderer, {chart_x - 4, chart_y - 4, chart_w + 8, chart_h + 8}, 8, {12, 12, 20, 255});

                // Select source data and sample rate based on nyquist mode
                const std::deque<float>* src_data = nullptr;
                std::deque<float> co2_vals;
                float sample_rate;
                if (spectrum_nyquist == 0) {
                    sample_rate = 2.0f;
                    switch (spectrum_field) {
                        case 0: src_data = &history.temperature; break;
                        case 1: src_data = &history.humidity; break;
                        case 2: src_data = &history.pressure; break;
                        case 3:
                            for (auto& cp : history.eco2) co2_vals.push_back(cp.value);
                            src_data = &co2_vals;
                            break;
                    }
                } else {
                    sample_rate = 1000.0f / std::max(50, g_settings.sensor_interval_ms);
                    switch (spectrum_field) {
                        case 0: src_data = &history.raw_temp; break;
                        case 1: src_data = &history.raw_hum; break;
                        case 2: src_data = &history.raw_pres; break;
                        case 3:
                            for (auto& cp : history.eco2) co2_vals.push_back(cp.value);
                            src_data = &co2_vals;
                            sample_rate = 2.0f;
                            break;
                    }
                }

                float display_max_freq = sample_rate / 2.0f;

                int n = src_data ? (int)src_data->size() : 0;
                if (n >= 16) {
                    int use_n = std::min(n, FFT_N);
                    Complex buf[FFT_N] = {};

                    float mean = 0;
                    for (int i = 0; i < use_n; i++) mean += (*src_data)[n - use_n + i];
                    mean /= use_n;

                    for (int i = 0; i < use_n; i++) {
                        float w = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (use_n - 1)));
                        buf[i].re = ((*src_data)[n - use_n + i] - mean) * w;
                        buf[i].im = 0;
                    }

                    fft(buf, FFT_N, false);

                    int num_bins = FFT_N / 2;
                    float mags[FFT_N / 2];
                    float mags_db[FFT_N / 2];
                    float max_mag = 0;
                    for (int i = 1; i < num_bins; i++) {
                        mags[i] = sqrtf(buf[i].re * buf[i].re + buf[i].im * buf[i].im) * 2.0f / FFT_N;
                        if (mags[i] > max_mag) max_mag = mags[i];
                    }
                    mags[0] = 0;
                    if (max_mag < 1e-9f) max_mag = 1e-9f;
                    float db_floor = -80.0f;
                    for (int i = 0; i < num_bins; i++) {
                        float val = mags[i] > 1e-12f ? 20.0f * log10f(mags[i] / max_mag) : db_floor;
                        mags_db[i] = std::max(val, db_floor);
                    }

                    float freq_res = sample_rate / FFT_N;
                    float heater_freq = 1.0f / g_settings.gas_interval_s;
                    int max_bin = std::min(num_bins, (int)(display_max_freq / freq_res) + 1);
                    if (max_bin < 3) max_bin = 3;

                    // Color-per-dB function for gradient fill modes
                    // 0=Klassik (amber line), 1=Thermal (blue→red→yellow), 2=Neon (purple→green→white)
                    auto gradient_color = [&](float db_val) -> Color {
                        float t = (db_val - db_floor) / (0.0f - db_floor);  // 0=floor, 1=peak
                        t = std::clamp(t, 0.0f, 1.0f);
                        switch (spectrum_colorscheme) {
                            case 1: {  // Thermal: black→blue→red→yellow→white
                                if (t < 0.25f) {
                                    float s = t / 0.25f;
                                    return {0, 0, (uint8_t)(180 * s), 220};
                                } else if (t < 0.5f) {
                                    float s = (t - 0.25f) / 0.25f;
                                    return {(uint8_t)(220 * s), 0, (uint8_t)(180 * (1 - s)), 220};
                                } else if (t < 0.75f) {
                                    float s = (t - 0.5f) / 0.25f;
                                    return {220, (uint8_t)(200 * s), 0, 230};
                                } else {
                                    float s = (t - 0.75f) / 0.25f;
                                    return {(uint8_t)(220 + 35 * s), (uint8_t)(200 + 55 * s), (uint8_t)(180 * s), 240};
                                }
                            }
                            case 2: {  // Neon: dark purple→cyan→green→white
                                if (t < 0.33f) {
                                    float s = t / 0.33f;
                                    return {(uint8_t)(80 * s), 0, (uint8_t)(160 * s), 200};
                                } else if (t < 0.66f) {
                                    float s = (t - 0.33f) / 0.33f;
                                    return {(uint8_t)(80 * (1 - s)), (uint8_t)(220 * s), (uint8_t)(160 + 40 * s), 220};
                                } else {
                                    float s = (t - 0.66f) / 0.34f;
                                    return {(uint8_t)(60 + 195 * s), (uint8_t)(220 + 35 * s), (uint8_t)(200 - 40 * s + 95 * s), 240};
                                }
                            }
                            default:
                                return {255, 180, 40, 200};  // Klassik amber (flat, line mode)
                        }
                    };

                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

                    // Y-axis: dB labels (left of chart, right-aligned)
                    for (int db = 0; db >= (int)db_floor; db -= 10) {
                        float norm = (float)(db - db_floor) / (0.0f - db_floor);
                        int gy = chart_y + chart_h - (int)(norm * chart_h);
                        // Grid line
                        SDL_SetRenderDrawColor(renderer, 60, 60, 80, db == 0 ? 70 : 40);
                        SDL_RenderDrawLine(renderer, chart_x, gy, chart_x + chart_w, gy);
                        // Label
                        char dbl[16];
                        snprintf(dbl, sizeof(dbl), "%d", db);
                        draw_text(renderer, font_small, dbl,
                                 chart_x - 8, gy - 7, {160, 180, 220, 255}, false);
                    }
                    // "dB" unit label at top-left
                    draw_text(renderer, font_small, "dB", chart_x - 34, chart_y - 2, {180, 200, 240, 255}, false);

                    // Draw spectrum
                    int prev_px = -1, prev_py = -1;
                    for (int i = 1; i < max_bin; i++) {
                        float norm_db = (mags_db[i] - db_floor) / (0.0f - db_floor);
                        int px_i = chart_x + (int)((float)(i - 1) / (float)(max_bin - 2) * chart_w);
                        int py_i = chart_y + chart_h - (int)(norm_db * chart_h);
                        py_i = std::clamp(py_i, chart_y, chart_y + chart_h);

                        if (spectrum_colorscheme == 0) {
                            // Klassik: amber line + translucent fill
                            SDL_SetRenderDrawColor(renderer, 255, 180, 40, 30);
                            SDL_RenderDrawLine(renderer, px_i, py_i, px_i, chart_y + chart_h);
                            if (prev_px >= 0) {
                                set_color(renderer, {255, 180, 40, 255});
                                SDL_RenderDrawLine(renderer, prev_px, prev_py, px_i, py_i);
                                SDL_RenderDrawLine(renderer, prev_px, prev_py + 1, px_i, py_i + 1);
                            }
                        } else {
                            // Gradient fill: color each vertical stripe by dB value
                            for (int y = py_i; y < chart_y + chart_h; y++) {
                                float y_db = db_floor + (0.0f - db_floor) * (float)(chart_y + chart_h - y) / chart_h;
                                Color c = gradient_color(y_db);
                                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                                SDL_RenderDrawPoint(renderer, px_i, y);
                            }
                            // White edge line on top
                            if (prev_px >= 0) {
                                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 140);
                                SDL_RenderDrawLine(renderer, prev_px, prev_py, px_i, py_i);
                            }
                        }
                        prev_px = px_i;
                        prev_py = py_i;
                    }

                    // Heater frequency marker
                    if (heater_freq < display_max_freq) {
                        int hf_x = chart_x + (int)(heater_freq / display_max_freq * chart_w);
                        SDL_SetRenderDrawColor(renderer, 255, 80, 80, 80);
                        for (int dy = chart_y; dy < chart_y + chart_h; dy += 4)
                            SDL_RenderDrawLine(renderer, hf_x, dy, hf_x, std::min(dy + 2, chart_y + chart_h));
                        char hflbl[32];
                        snprintf(hflbl, sizeof(hflbl), "Heater %.3fHz", heater_freq);
                        draw_text(renderer, font_small, hflbl, hf_x + 4, chart_y + 2, {255, 100, 100, 255});
                    }

                    // Find 5 largest peaks
                    struct Peak { int bin; float mag_db; float freq; int px; int py; };
                    Peak peaks[5] = {};
                    int peak_count = 0;
                    for (int i = 2; i < max_bin - 1; i++) {
                        if (mags_db[i] > mags_db[i-1] && mags_db[i] > mags_db[i+1] && mags_db[i] > db_floor + 6) {
                            float norm_db = (mags_db[i] - db_floor) / (0.0f - db_floor);
                            int ppx = chart_x + (int)((float)(i - 1) / (float)(max_bin - 2) * chart_w);
                            int ppy = chart_y + chart_h - (int)(norm_db * chart_h);
                            ppy = std::clamp(ppy, chart_y + 12, chart_y + chart_h);
                            Peak pk = {i, mags_db[i], i * freq_res, ppx, ppy};
                            int ins = peak_count < 5 ? peak_count : -1;
                            for (int j = 0; j < std::min(peak_count, 5); j++) {
                                if (pk.mag_db > peaks[j].mag_db) { ins = j; break; }
                            }
                            if (ins >= 0) {
                                for (int j = std::min(peak_count, 4); j > ins; j--) peaks[j] = peaks[j-1];
                                peaks[ins] = pk;
                                if (peak_count < 5) peak_count++;
                            }
                        }
                    }

                    // Label peaks, skipping those that would overlap a stronger peak's label
                    struct PeakLabel { int x, y, w; };
                    PeakLabel placed[5];
                    int placed_count = 0;
                    for (int i = 0; i < peak_count; i++) {
                        // Diamond marker always drawn
                        set_color(renderer, {255, 255, 255, 200});
                        int mx_p = peaks[i].px, my_p = peaks[i].py;
                        SDL_RenderDrawLine(renderer, mx_p, my_p - 5, mx_p + 4, my_p);
                        SDL_RenderDrawLine(renderer, mx_p + 4, my_p, mx_p, my_p + 5);
                        SDL_RenderDrawLine(renderer, mx_p, my_p + 5, mx_p - 4, my_p);
                        SDL_RenderDrawLine(renderer, mx_p - 4, my_p, mx_p, my_p - 5);

                        // Build label text
                        char plbl[32];
                        if (spectrum_x_period && peaks[i].freq > 0.01f)
                            snprintf(plbl, sizeof(plbl), "%.1fs %.0fdB", 1.0f / peaks[i].freq, peaks[i].mag_db);
                        else
                            snprintf(plbl, sizeof(plbl), "%.2fHz %.0fdB", peaks[i].freq, peaks[i].mag_db);

                        int lbl_w = (int)strlen(plbl) * 7;  // approx char width
                        int lbl_x = mx_p - lbl_w / 2;
                        int lbl_y = my_p - 16;
                        if (lbl_y < chart_y + 4) lbl_y = my_p + 10;

                        // Check overlap with already placed labels
                        bool overlap = false;
                        for (int j = 0; j < placed_count; j++) {
                            if (abs(lbl_x - placed[j].x) < (lbl_w + placed[j].w) / 2 + 8 &&
                                abs(lbl_y - placed[j].y) < 18) {
                                overlap = true;
                                break;
                            }
                        }
                        if (!overlap) {
                            draw_text(renderer, font_small, plbl, mx_p, lbl_y,
                                     {255, 255, 200, 220}, true);
                            placed[placed_count++] = {mx_p, lbl_y, lbl_w};
                        }
                    }

                    // X-axis labels (properly positioned below chart bottom)
                    int x_label_y = chart_y + chart_h + 8;
                    int n_labels = 5;
                    for (int i = 0; i <= n_labels; i++) {
                        float freq = display_max_freq * i / n_labels;
                        int lx = chart_x + (int)((float)i / n_labels * chart_w);
                        // Tick mark
                        SDL_SetRenderDrawColor(renderer, 80, 80, 120, 80);
                        SDL_RenderDrawLine(renderer, lx, chart_y + chart_h, lx, chart_y + chart_h + 4);

                        char flbl[24];
                        if (spectrum_x_period) {
                            if (freq < 0.01f)
                                snprintf(flbl, sizeof(flbl), "\xe2\x88\x9e");  // infinity
                            else
                                snprintf(flbl, sizeof(flbl), "%.1fs", 1.0f / freq);
                        } else {
                            snprintf(flbl, sizeof(flbl), "%.1f", freq);
                        }
                        draw_text(renderer, font_small, flbl, lx, x_label_y, {140, 180, 220, 255}, true);
                    }
                    // Unit label right of last tick
                    draw_text(renderer, font_small,
                             spectrum_x_period ? "s" : "Hz",
                             chart_x + chart_w + 18, x_label_y, {180, 200, 240, 255}, true);

                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                } else {
                    draw_text(renderer, font_med, "Nicht genug Daten",
                             spx + spw / 2, spy + sph / 2, TEXT_DIM, true);
                }

                // === Bottom bar: color scheme + filter slider ===
                int bot_y = spy + sph - 48;

                // Color scheme buttons (3 modes)
                const char* cs_names[] = {"Klassik", "Thermal", "Neon"};
                const Color cs_colors[] = {{220, 160, 40, 255}, {220, 60, 30, 255}, {60, 200, 180, 255}};
                int csw = 80;
                for (int i = 0; i < 3; i++) {
                    SDL_Rect cbtn = {spx + 20 + i * (csw + 8), bot_y, csw, 36};
                    Color bg = (i == spectrum_colorscheme) ? cs_colors[i] : Color{50, 50, 65, 255};
                    Color fg = (i == spectrum_colorscheme) ? Color{0, 0, 0, 255} : TEXT_DIM;
                    fill_rounded_rect(renderer, cbtn, 6, bg);
                    draw_text(renderer, font_small, cs_names[i],
                             cbtn.x + cbtn.w / 2, cbtn.y + cbtn.h / 2, fg, true);
                }

                // Filter strength slider (positioned with enough space)
                int sl_x = spx + spw - 300;
                int sl_w = 250;
                int sl_cy = bot_y + 18;

                // Track
                SDL_Rect track = {sl_x, sl_cy - 3, sl_w, 6};
                fill_rounded_rect(renderer, track, 3, {50, 50, 65, 255});

                // Filled portion
                float sl_frac = (float)(g_settings.heater_blanking_ms - 250) / (6000.0f - 250.0f);
                int fill_w = (int)(sl_frac * sl_w);
                if (fill_w > 0) {
                    SDL_Rect filled = {sl_x, sl_cy - 3, fill_w, 6};
                    Color sl_col = sl_frac < 0.5f ? Color{100, 180, 255, 255} : Color{255, 160, 60, 255};
                    fill_rounded_rect(renderer, filled, 3, sl_col);
                }

                // Thumb
                int thumb_x = sl_x + fill_w;
                SDL_Rect thumb = {thumb_x - 8, sl_cy - 10, 16, 20};
                fill_rounded_rect(renderer, thumb, 4, {220, 220, 240, 255});

                // Slider end labels + value on thumb
                draw_text(renderer, font_small, "250", sl_x - 22, sl_cy - 6, TEXT_DIM, true);
                draw_text(renderer, font_small, "6000", sl_x + sl_w + 24, sl_cy - 6, TEXT_DIM, true);
                char fs_val[16];
                snprintf(fs_val, sizeof(fs_val), "%dms", g_settings.heater_blanking_ms);
                draw_text(renderer, font_small, fs_val, thumb_x, sl_cy - 18, TEXT_PRIMARY, true);

            } else if (settings_page == SETTINGS_SENSOR) {
                // Sensor diagnostics page
                draw_text(renderer, font_med, "Sensor-Diagnose", px + 44, py + 13, TEXT_PRIMARY);

                int sy = py + 56;
                int rh = 34;

                // Status
                draw_text(renderer, font_med, "Status:", rx, sy, TEXT_DIM);
                if (sensor_ok) {
                    draw_text(renderer, font_med, "Verbunden", rx + 140, sy, {80, 220, 100, 255});
                } else {
                    draw_text(renderer, font_med, "FEHLER", rx + 140, sy, {255, 80, 60, 255});
                }
                sy += rh;

                // I2C info
                char i2c_str[64];
                snprintf(i2c_str, sizeof(i2c_str), "I2C: %s @ 0x%02X", i2c_dev.c_str(), i2c_addr);
                draw_text(renderer, font_small, i2c_str, rx, sy, TEXT_DIM);
                sy += rh - 6;

                // Modus
                draw_text(renderer, font_small, demo_mode ? "Modus: DEMO (keine echten Daten)"
                                                          : "Modus: Live-Sensor", rx, sy, TEXT_DIM);
                sy += rh - 6;

                // Retry counter
                char retry_str[64];
                snprintf(retry_str, sizeof(retry_str), "Neustarts ohne Erfolg: %d / 3", sensor_retry_count);
                Color retry_col = sensor_retry_count >= 3 ? Color{255, 100, 60, 255} : TEXT_DIM;
                draw_text(renderer, font_small, retry_str, rx, sy, retry_col);
                sy += rh;

                // Last reading (if available)
                if (display_vals.valid && !demo_mode) {
                    char rv[80];
                    snprintf(rv, sizeof(rv), "Letzte Messung: %.1f\xC2\xB0""C  %.1f%%RH  %.1f hPa",
                            display_vals.temperature, display_vals.humidity, display_vals.pressure);
                    draw_text(renderer, font_small, rv, rx, sy, ACCENT_PRES);
                    sy += rh - 6;
                }

                // Test result
                if (sensor_test_result) {
                    draw_text(renderer, font_small, sensor_test_result, rx, sy, {255, 220, 100, 255});
                    sy += rh - 6;
                }

                sy += 10;

                // Sensor test button
                fill_rounded_rect(renderer, {rx, sy, row_w, 50}, 8, Color{50, 70, 100, 255});
                draw_text(renderer, font_med, "Sensor testen",
                         rx + row_w / 2, sy + 25, TEXT_PRIMARY, true);
                sy += 60;

                // App restart button
                fill_rounded_rect(renderer, {rx, sy, row_w, 50}, 8, Color{60, 90, 50, 255});
                draw_text(renderer, font_med, "App neu starten",
                         rx + row_w / 2, sy + 25, TEXT_PRIMARY, true);
                sy += 60;

                // System reboot button (only if 3+ retries)
                if (sensor_retry_count >= 3) {
                    fill_rounded_rect(renderer, {rx, sy, row_w, 50}, 8, EXIT_BG);
                    draw_text(renderer, font_med, "System Reboot",
                             rx + row_w / 2, sy + 25, TEXT_PRIMARY, true);
                }
            }
        }

        // === Sensor warning popup (drawn on top of everything) ===
        if (sensor_warning_shown && !sensor_warning_dismissed) {
            uint32_t elapsed = SDL_GetTicks() - sensor_warning_time;
            int remaining = 60 - (int)(elapsed / 1000);
            if (remaining < 0) remaining = 0;

            // Auto action after 60s
            if (elapsed >= 60000) {
                if (sensor_retry_count >= 3) {
                    // System reboot
                    save_retry_count(sensor_retry_count + 1);
                    system("sudo reboot");
                } else {
                    // App restart
                    save_retry_count(sensor_retry_count + 1);
                    // Re-exec ourselves
                    sensor_running.store(false);
                    sensor_thread.join();
                    text_cache_destroy();
                    TTF_CloseFont(font_big);
                    TTF_CloseFont(font_med);
                    TTF_CloseFont(font_small);
                    TTF_Quit();
                    SDL_DestroyRenderer(renderer);
                    SDL_DestroyWindow(window);
                    SDL_Quit();
                    execv(argv[0], argv);
                    return 1;  // execv failed
                }
            }

            // Draw popup
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_Rect full_scr = {0, 0, win_w, win_h};
            SDL_RenderFillRect(renderer, &full_scr);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            int wpw = 460, wph = 260;
            int wpx = (win_w - wpw) / 2, wpy = (win_h - wph) / 2;
            fill_rounded_rect(renderer, {wpx, wpy, wpw, wph}, 12, {40, 30, 30, 255});

            // Warning icon (triangle with !)
            set_color(renderer, {255, 200, 60, 255});
            for (int dy = 0; dy < 30; dy++) {
                int hw = dy * 18 / 30;
                SDL_RenderDrawLine(renderer, wpx + wpw/2 - hw, wpy + 20 + dy, wpx + wpw/2 + hw, wpy + 20 + dy);
            }
            draw_text(renderer, font_med, "!", wpx + wpw/2, wpy + 34, {40, 30, 30, 255}, true);

            draw_text(renderer, font_med, "Sensor nicht erreichbar",
                     wpx + wpw/2, wpy + 65, {255, 100, 60, 255}, true);

            char warn_detail[128];
            if (sensor_retry_count >= 3) {
                snprintf(warn_detail, sizeof(warn_detail),
                    "System Reboot in %ds (Versuch %d)", remaining, sensor_retry_count + 1);
            } else {
                snprintf(warn_detail, sizeof(warn_detail),
                    "App Neustart in %ds (Versuch %d/3)", remaining, sensor_retry_count + 1);
            }
            draw_text(renderer, font_small, warn_detail, wpx + wpw/2, wpy + 95, TEXT_DIM, true);

            draw_text(renderer, font_small, i2c_dev.c_str(), wpx + wpw/2, wpy + 120, TEXT_DIM, true);

            // OK button (dismiss, run in demo mode)
            SDL_Rect warn_ok = {wpx + wpw/2 - 80, wpy + wph - 70, 160, 50};
            fill_rounded_rect(renderer, warn_ok, 8, Color{60, 80, 60, 255});
            draw_text(renderer, font_med, "OK (Demo-Modus)",
                     warn_ok.x + warn_ok.w/2, warn_ok.y + warn_ok.h/2, TEXT_PRIMARY, true);
        }

        apply_night_dim(renderer, win_w, win_h);
        SDL_RenderPresent(renderer);

        // ECO modes get extra delay on top of VSYNC
        if (eco_active >= 1) {
            SDL_Delay(48);
        }
    }

    sensor_running.store(false);
    sensor_thread.join();
    text_cache_destroy();

    TTF_CloseFont(font_big);
    TTF_CloseFont(font_med);
    TTF_CloseFont(font_small);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
