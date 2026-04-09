#include "bme688.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <cstring>
#include <cstdio>
#include <cmath>

// BME688 registers
#define BME688_CHIP_ID       0xD0
#define BME688_RESET         0xE0
#define BME688_CTRL_HUM      0x72
#define BME688_CTRL_MEAS     0x74
#define BME688_CTRL_GAS_1    0x71
#define BME688_CTRL_GAS_0    0x70
#define BME688_GAS_WAIT_0    0x64
#define BME688_RES_HEAT_0    0x5A
#define BME688_STATUS        0x1D
#define BME688_MEAS_STATUS   0x1D

#define BME688_DATA_ADDR     0x1D
#define BME688_CHIP_ID_VAL   0x61

BME688::BME688(const std::string& i2c_device, uint8_t address)
    : addr_(address), device_(i2c_device) {}

BME688::~BME688() {
    if (fd_ >= 0) close(fd_);
}

bool BME688::write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return ::write(fd_, buf, 2) == 2;
}

uint8_t BME688::read_reg(uint8_t reg) {
    uint8_t val = 0;
    ::write(fd_, &reg, 1);
    ::read(fd_, &val, 1);
    return val;
}

void BME688::read_regs(uint8_t reg, uint8_t* buf, int len) {
    ::write(fd_, &reg, 1);
    ::read(fd_, buf, len);
}

bool BME688::init() {
    fd_ = open(device_.c_str(), O_RDWR);
    if (fd_ < 0) {
        perror("Failed to open I2C device");
        return false;
    }

    if (ioctl(fd_, I2C_SLAVE, addr_) < 0) {
        perror("Failed to set I2C address");
        close(fd_);
        fd_ = -1;
        return false;
    }

    // Soft reset
    write_reg(BME688_RESET, 0xB6);
    usleep(10000);

    uint8_t chip_id = read_reg(BME688_CHIP_ID);
    if (chip_id != BME688_CHIP_ID_VAL) {
        fprintf(stderr, "BME688 not found (chip_id=0x%02X, expected 0x%02X)\n", chip_id, BME688_CHIP_ID_VAL);
        close(fd_);
        fd_ = -1;
        return false;
    }

    load_calibration();

    // Configure humidity oversampling x1
    write_reg(BME688_CTRL_HUM, 0x01);

    // Configure gas heater: 320°C for 150ms (needs stable temp for accurate reading)
    write_reg(BME688_RES_HEAT_0, calc_heater_res(320));
    write_reg(BME688_GAS_WAIT_0, calc_heater_dur(150));

    // Enable gas, set heater profile 0
    write_reg(BME688_CTRL_GAS_1, 0x10);

    return true;
}

void BME688::load_calibration() {
    uint8_t coeff1[25], coeff2[16];
    read_regs(0x89, coeff1, 25);
    read_regs(0xE1, coeff2, 16);

    // Temperature
    par_t1_ = (uint16_t)(coeff2[9] << 8 | coeff2[8]);
    par_t2_ = (int16_t)(coeff1[2] << 8 | coeff1[1]);
    par_t3_ = (int8_t)coeff1[3];

    // Pressure
    par_p1_ = (uint16_t)(coeff1[6] << 8 | coeff1[5]);
    par_p2_ = (int16_t)(coeff1[8] << 8 | coeff1[7]);
    par_p3_ = (int8_t)coeff1[9];
    par_p4_ = (int16_t)(coeff1[12] << 8 | coeff1[11]);
    par_p5_ = (int16_t)(coeff1[14] << 8 | coeff1[13]);
    par_p6_ = (int8_t)coeff1[16];
    par_p7_ = (int8_t)coeff1[15];
    par_p8_ = (int16_t)(coeff1[20] << 8 | coeff1[19]);
    par_p9_ = (int16_t)(coeff1[22] << 8 | coeff1[21]);
    par_p10_ = coeff1[23];

    // Humidity
    par_h1_ = (uint16_t)((coeff2[2] << 4) | (coeff2[1] & 0x0F));
    par_h2_ = (uint16_t)((coeff2[0] << 4) | (coeff2[1] >> 4));
    par_h3_ = (int8_t)coeff2[3];
    par_h4_ = (int8_t)coeff2[4];
    par_h5_ = (int8_t)coeff2[5];
    par_h6_ = coeff2[6];
    par_h7_ = (int8_t)coeff2[7];

    // Gas
    par_g1_ = (int8_t)coeff2[12];
    par_g2_ = (int16_t)(coeff2[11] << 8 | coeff2[10]);
    par_g3_ = (int8_t)coeff2[13];

    // Additional calibration
    res_heat_range_ = (read_reg(0x02) >> 4) & 0x03;
    res_heat_val_ = (int8_t)read_reg(0x00);
    range_sw_err_ = ((int8_t)(read_reg(0x04) & 0xF0)) >> 4;
}

float BME688::calc_temperature(uint32_t adc) {
    float var1 = ((((float)adc / 16384.0f) - ((float)par_t1_ / 1024.0f)) * (float)par_t2_);
    float var2 = (((((float)adc / 131072.0f) - ((float)par_t1_ / 8192.0f)) *
                   (((float)adc / 131072.0f) - ((float)par_t1_ / 8192.0f))) * ((float)par_t3_ * 16.0f));
    t_fine_ = var1 + var2;
    return t_fine_ / 5120.0f;
}

float BME688::calc_pressure(uint32_t adc) {
    float var1 = (t_fine_ / 2.0f) - 64000.0f;
    float var2 = var1 * var1 * ((float)par_p6_ / 131072.0f);
    var2 = var2 + (var1 * (float)par_p5_ * 2.0f);
    var2 = (var2 / 4.0f) + ((float)par_p4_ * 65536.0f);
    var1 = ((((float)par_p3_ * var1 * var1) / 16384.0f) + ((float)par_p2_ * var1)) / 524288.0f;
    var1 = (1.0f + (var1 / 32768.0f)) * (float)par_p1_;
    float press = 1048576.0f - (float)adc;
    if (var1 != 0) {
        press = ((press - (var2 / 4096.0f)) * 6250.0f) / var1;
        var1 = ((float)par_p9_ * press * press) / 2147483648.0f;
        var2 = press * ((float)par_p8_ / 32768.0f);
        float var3 = (press / 256.0f) * (press / 256.0f) * (press / 256.0f) * (par_p10_ / 131072.0f);
        press = press + (var1 + var2 + var3 + ((float)par_p7_ * 128.0f)) / 16.0f;
    } else {
        press = 0;
    }
    return press / 100.0f; // hPa
}

float BME688::calc_humidity(uint16_t adc) {
    float temp_comp = t_fine_ / 5120.0f;
    float var1 = (float)adc - (((float)par_h1_ * 16.0f) + (((float)par_h3_ / 2.0f) * temp_comp));
    float var2 = var1 * (((float)par_h2_ / 262144.0f) *
                         (1.0f + (((float)par_h4_ / 16384.0f) * temp_comp) +
                          (((float)par_h5_ / 1048576.0f) * temp_comp * temp_comp)));
    float var3 = (float)par_h6_ / 16384.0f;
    float var4 = (float)par_h7_ / 2097152.0f;
    float hum = var2 + ((var3 + (var4 * temp_comp)) * var2 * var2);
    if (hum > 100.0f) hum = 100.0f;
    if (hum < 0.0f) hum = 0.0f;
    return hum;
}

float BME688::calc_gas(uint16_t adc, uint8_t range) {
    static const float lookup1[16] = {
        1, 1, 1, 1, 1, 0.99, 1, 0.992,
        1, 1, 0.998, 0.995, 1, 0.99, 1, 1
    };
    static const float lookup2[16] = {
        8000000, 4000000, 2000000, 1000000, 499500.4995, 248262.1648,
        125000, 63004.03226, 31281.28128, 15625, 7812.5, 3906.25,
        1953.125, 976.5625, 488.28125, 244.140625
    };

    float var1 = (1340.0f + (5.0f * range_sw_err_)) * lookup1[range];
    float gas_res = var1 * lookup2[range] / ((float)adc - 512.0f + var1);
    return gas_res / 1000.0f; // kOhm
}

uint8_t BME688::calc_heater_res(uint16_t target_temp) {
    float var1 = ((float)par_g1_ / 16.0f) + 49.0f;
    float var2 = (((float)par_g2_ / 32768.0f) * 0.0005f) + 0.00235f;
    float var3 = (float)par_g3_ / 1024.0f;
    float var4 = var1 * (1.0f + (var2 * (float)target_temp));
    float var5 = var4 + (var3 * 25.0f); // ambient = 25
    float res_heat = (uint8_t)(3.4f * ((var5 * (4.0f / (4.0f + (float)res_heat_range_)) *
                                         (1.0f / (1.0f + ((float)res_heat_val_ * 0.002f)))) - 25));
    return (uint8_t)res_heat;
}

uint8_t BME688::calc_heater_dur(uint16_t dur_ms) {
    uint8_t factor = 0;
    uint8_t dur_val;
    if (dur_ms >= 0xFC0) {
        dur_val = 0xFF;
    } else {
        while (dur_ms > 0x3F) {
            dur_ms /= 4;
            factor++;
        }
        dur_val = (uint8_t)(dur_ms + (factor * 64));
    }
    return dur_val;
}

float BME688::calc_iaq(float gas_res, float humidity) {
    sample_count_++;

    // Adaptive baseline tracking:
    // Gas resistance is highest in clean air, drops with pollution.
    // We track the best (highest) value seen as our baseline.
    if (sample_count_ <= 5) {
        // Warmup: initialize baseline with first readings
        gas_baseline_ = gas_res;
        gas_baseline_slow_ = gas_res;
        return 25.0f; // During warmup, assume good air
    }

    // Update baseline: slowly adapt upward (clean air), fast adapt if new max
    if (gas_res > gas_baseline_) {
        gas_baseline_ = gas_res;
    } else {
        // Slow decay: baseline drifts down ~0.1% per sample to track sensor aging
        gas_baseline_ = gas_baseline_ * 0.999f + gas_res * 0.001f;
    }
    // Slow moving average for long-term reference
    gas_baseline_slow_ = gas_baseline_slow_ * 0.995f + gas_res * 0.005f;

    // Use the higher of tracked baseline and slow average
    float baseline = std::max(gas_baseline_, gas_baseline_slow_);
    if (baseline < 1.0f) baseline = 1.0f;

    // Gas contribution to IAQ (75% weight, per BSEC documentation)
    // ratio: 1.0 = at baseline (clean), <1.0 = polluted
    float gas_ratio = gas_res / baseline;
    float gas_score;
    if (gas_ratio >= 1.0f) {
        gas_score = 0.0f; // At or above baseline = perfect
    } else {
        // Logarithmic scaling: small drops = small IAQ change, big drops = big change
        gas_score = -logf(gas_ratio) * 100.0f;
    }

    // Humidity contribution (25% weight)
    // Optimal humidity ~40%RH, deviation increases IAQ score
    float hum_offset = fabsf(humidity - hum_baseline_);
    float hum_score = hum_offset * 1.0f; // 1 IAQ point per %RH deviation

    // Combined IAQ (0 = excellent, 500 = terrible)
    float iaq = gas_score * 0.75f + hum_score * 0.25f;
    if (iaq < 0.0f) iaq = 0.0f;
    if (iaq > 500.0f) iaq = 500.0f;

    return iaq;
}

BME688Data BME688::read_fast() {
    BME688Data data = {};
    data.valid = false;

    if (fd_ < 0) {
        init();
        return data;
    }

    // Disable gas measurement for fast read
    write_reg(BME688_CTRL_GAS_1, 0x00);

    // Trigger forced mode: temp x2, press x16, forced mode
    write_reg(BME688_CTRL_MEAS, (0x02 << 5) | (0x05 << 2) | 0x01);

    // Without gas: measurement takes ~30ms
    usleep(35000);

    uint8_t status = read_reg(BME688_MEAS_STATUS);
    if (!(status & 0x80)) {
        usleep(15000);
    }

    uint8_t raw[10];
    read_regs(0x1D, raw, 10);

    uint32_t adc_pres = ((uint32_t)raw[2] << 12) | ((uint32_t)raw[3] << 4) | ((uint32_t)raw[4] >> 4);
    uint32_t adc_temp = ((uint32_t)raw[5] << 12) | ((uint32_t)raw[6] << 4) | ((uint32_t)raw[7] >> 4);
    uint16_t adc_hum = (uint16_t)(raw[8] << 8 | raw[9]);

    data.temperature = calc_temperature(adc_temp);
    data.pressure = calc_pressure(adc_pres);
    data.humidity = calc_humidity(adc_hum);

    if (std::isnan(data.temperature) || std::isinf(data.temperature) ||
        data.temperature < -40 || data.temperature > 85 ||
        std::isnan(data.pressure) || data.pressure < 300 || data.pressure > 1100 ||
        std::isnan(data.humidity) || data.humidity < 0 || data.humidity > 100) {
        return data;
    }

    // Keep last known gas/iaq/eco2 values (not measured in fast mode)
    data.gas_resistance = 0;
    data.iaq = 0;
    data.eco2 = 400;
    data.valid = true;
    return data;
}

BME688Data BME688::read() {
    BME688Data data = {};
    data.valid = false;

    if (fd_ < 0) {
        // Try to reconnect
        init();
        return data;
    }

    // Re-configure gas heater (read_fast() disables gas entirely)
    // 320°C for 150ms — needs enough time to stabilize from cold
    write_reg(BME688_RES_HEAT_0, calc_heater_res(320));
    write_reg(BME688_GAS_WAIT_0, calc_heater_dur(150));
    write_reg(BME688_CTRL_GAS_1, 0x10);  // run_gas=1, profile 0

    // Trigger forced mode: temp x2, press x16, forced mode
    write_reg(BME688_CTRL_MEAS, (0x02 << 5) | (0x05 << 2) | 0x01);

    // Wait for measurement (~40ms T/H/P + 150ms heater + margin)
    usleep(300000);

    // Check if new data is available
    uint8_t status = read_reg(BME688_MEAS_STATUS);
    if (!(status & 0x80)) {
        // Heater might need more time from cold start
        usleep(150000);
        status = read_reg(BME688_MEAS_STATUS);
    }

    // Read raw data
    uint8_t raw[15];
    read_regs(0x1D, raw, 15);

    uint32_t adc_pres = ((uint32_t)raw[2] << 12) | ((uint32_t)raw[3] << 4) | ((uint32_t)raw[4] >> 4);
    uint32_t adc_temp = ((uint32_t)raw[5] << 12) | ((uint32_t)raw[6] << 4) | ((uint32_t)raw[7] >> 4);
    uint16_t adc_hum = (uint16_t)(raw[8] << 8 | raw[9]);
    uint16_t adc_gas = (uint16_t)((raw[13] << 2) | (raw[14] >> 6));
    uint8_t gas_range = raw[14] & 0x0F;
    bool gas_valid = (raw[14] & 0x20) != 0;
    bool heat_stab = (raw[14] & 0x10) != 0;

    data.temperature = calc_temperature(adc_temp);
    data.pressure = calc_pressure(adc_pres);
    data.humidity = calc_humidity(adc_hum);

    // Sanity check: discard garbage readings
    if (std::isnan(data.temperature) || std::isinf(data.temperature) ||
        data.temperature < -40 || data.temperature > 85 ||
        std::isnan(data.pressure) || data.pressure < 300 || data.pressure > 1100 ||
        std::isnan(data.humidity) || data.humidity < 0 || data.humidity > 100) {
        return data; // valid stays false
    }

    if (gas_valid && heat_stab) {
        data.gas_resistance = calc_gas(adc_gas, gas_range);
        if (std::isnan(data.gas_resistance) || data.gas_resistance <= 0) {
            data.gas_resistance = 0;
            data.iaq = 0;
            data.eco2 = 400;
        } else {
            data.iaq = calc_iaq(data.gas_resistance, data.humidity);
            data.eco2 = 400.0f * expf(0.012766f * data.iaq);
            if (data.eco2 > 10000.0f) data.eco2 = 10000.0f;
        }
    } else {
        data.gas_resistance = 0;
        data.iaq = 0;
        data.eco2 = 400;
    }

    data.valid = true;
    return data;
}
