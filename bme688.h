#pragma once
#include <cstdint>
#include <string>

struct BME688Data {
    float temperature;      // °C
    float humidity;         // %RH
    float pressure;         // hPa
    float gas_resistance;   // kOhm
    float iaq;              // 0-500 Indoor Air Quality
    float eco2;             // ppm (estimated CO2)
    bool valid;
};

class BME688 {
public:
    BME688(const std::string& i2c_device = "/dev/i2c-1", uint8_t address = 0x77);
    ~BME688();

    bool init();
    BME688Data read();
    BME688Data read_fast();  // temp/hum/press only, no gas heater (~10ms)

private:
    int fd_ = -1;
    uint8_t addr_;
    std::string device_;

    // Calibration data
    uint16_t par_t1_;
    int16_t par_t2_;
    int8_t par_t3_;
    uint16_t par_p1_;
    int16_t par_p2_;
    int8_t par_p3_;
    int16_t par_p4_;
    int16_t par_p5_;
    int8_t par_p6_;
    int8_t par_p7_;
    int16_t par_p8_;
    int16_t par_p9_;
    uint8_t par_p10_;
    uint16_t par_h1_;
    uint16_t par_h2_;
    int8_t par_h3_;
    int8_t par_h4_;
    int8_t par_h5_;
    uint8_t par_h6_;
    int8_t par_h7_;
    int8_t par_g1_;
    int16_t par_g2_;
    int8_t par_g3_;
    uint8_t res_heat_range_;
    int8_t res_heat_val_;
    int8_t range_sw_err_;

    float t_fine_;

    // IAQ baseline tracking
    float gas_baseline_ = 0;       // adaptive baseline (best gas resistance seen)
    float gas_baseline_slow_ = 0;  // slow-moving average for drift compensation
    int sample_count_ = 0;
    float hum_baseline_ = 40.0f;   // optimal humidity reference

    float calc_iaq(float gas_res, float humidity);

    bool write_reg(uint8_t reg, uint8_t val);
    uint8_t read_reg(uint8_t reg);
    void read_regs(uint8_t reg, uint8_t* buf, int len);
    void load_calibration();

    float calc_temperature(uint32_t adc);
    float calc_pressure(uint32_t adc);
    float calc_humidity(uint16_t adc);
    float calc_gas(uint16_t adc, uint8_t range);
    uint8_t calc_heater_res(uint16_t target_temp);
    uint8_t calc_heater_dur(uint16_t dur_ms);
};
