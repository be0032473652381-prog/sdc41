#include "sdc41.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "sdc41_crc.h"

#define SDC41_ADDRESS 0x62u
#define SDC41_I2C i2c0
#define SDC41_SDA_PIN 4u
#define SDC41_SCL_PIN 5u

enum {
    CMD_START_PERIODIC = 0x21b1,
    CMD_READ_MEASUREMENT = 0xec05,
    CMD_STOP_PERIODIC = 0x3f86,
    CMD_SET_OFFSET = 0x241d,
    CMD_GET_OFFSET = 0x2318,
    CMD_SET_ALTITUDE = 0x2427,
    CMD_GET_ALTITUDE = 0x2322,
    CMD_SET_ASC = 0x2416,
    CMD_GET_ASC = 0x2313,
    CMD_GET_READY = 0xe4b8,
    CMD_GET_SERIAL = 0x3682,
    CMD_SELF_TEST = 0x3639,
    CMD_SINGLE_SHOT = 0x219d
};

static sdc41_result_t write_command(uint16_t command) {
    const uint8_t bytes[2] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_write_blocking(SDC41_I2C, SDC41_ADDRESS, bytes, 2, false) == 2
               ? SDC41_OK
               : SDC41_ERR_WRITE;
}

static sdc41_result_t write_command_word(uint16_t command, uint16_t word) {
    uint8_t bytes[5] = {(uint8_t)(command >> 8), (uint8_t)command,
                        (uint8_t)(word >> 8), (uint8_t)word, 0};
    bytes[4] = sdc41_crc8(&bytes[2], 2);
    return i2c_write_blocking(SDC41_I2C, SDC41_ADDRESS, bytes, 5, false) == 5
               ? SDC41_OK
               : SDC41_ERR_WRITE;
}

static sdc41_result_t read_words(uint16_t command, uint16_t *words,
                                 size_t word_count, uint32_t delay_ms) {
    sdc41_result_t result = write_command(command);
    if (result != SDC41_OK) {
        return result;
    }
    sleep_ms(delay_ms);

    uint8_t bytes[9];
    const size_t byte_count = word_count * 3u;
    if (byte_count > sizeof(bytes) ||
        i2c_read_blocking(SDC41_I2C, SDC41_ADDRESS, bytes, byte_count, false) !=
            (int)byte_count) {
        return SDC41_ERR_READ;
    }
    for (size_t i = 0; i < word_count; ++i) {
        const size_t offset = i * 3u;
        if (sdc41_crc8(&bytes[offset], 2) != bytes[offset + 2]) {
            return SDC41_ERR_CRC;
        }
        words[i] = (uint16_t)((uint16_t)bytes[offset] << 8) | bytes[offset + 1];
    }
    return SDC41_OK;
}

void sdc41_init(void) {
    i2c_init(SDC41_I2C, 100000u);
    gpio_set_function(SDC41_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SDC41_SCL_PIN, GPIO_FUNC_I2C);
}

const char *sdc41_result_string(sdc41_result_t result) {
    switch (result) {
        case SDC41_OK: return "ok";
        case SDC41_ERR_WRITE: return "I2C command write failed";
        case SDC41_ERR_READ: return "I2C response read failed";
        case SDC41_ERR_CRC: return "response CRC check failed";
        case SDC41_ERR_VALUE: return "sensor returned an invalid value";
        default: return "unknown sensor error";
    }
}

sdc41_result_t sdc41_start_periodic(void) {
    return write_command(CMD_START_PERIODIC);
}

sdc41_result_t sdc41_stop_periodic(void) {
    return write_command(CMD_STOP_PERIODIC);
}

sdc41_result_t sdc41_measure_single_shot(void) {
    sdc41_result_t result = write_command(CMD_SINGLE_SHOT);
    if (result == SDC41_OK) {
        sleep_ms(5000);
    }
    return result;
}

sdc41_result_t sdc41_read_measurement(sdc41_measurement_t *measurement) {
    uint16_t words[3];
    sdc41_result_t result = read_words(CMD_READ_MEASUREMENT, words, 3, 1);
    if (result != SDC41_OK) {
        return result;
    }
    measurement->co2_ppm = words[0];
    measurement->temperature_milli_c =
        -45000 + (int32_t)(((int64_t)175000 * words[1]) / 65535);
    measurement->humidity_milli_percent =
        (uint32_t)(((uint64_t)100000 * words[2]) / 65535u);
    return SDC41_OK;
}

sdc41_result_t sdc41_get_ready(bool *ready) {
    uint16_t word;
    sdc41_result_t result = read_words(CMD_GET_READY, &word, 1, 1);
    if (result == SDC41_OK) {
        *ready = (word & 0x07ffu) != 0;
    }
    return result;
}

sdc41_result_t sdc41_get_serial(uint16_t words[3]) {
    return read_words(CMD_GET_SERIAL, words, 3, 1);
}

sdc41_result_t sdc41_self_test(uint16_t *result_word) {
    return read_words(CMD_SELF_TEST, result_word, 1, 10000);
}

sdc41_result_t sdc41_get_asc(bool *enabled) {
    uint16_t word;
    sdc41_result_t result = read_words(CMD_GET_ASC, &word, 1, 1);
    if (result == SDC41_OK) {
        if (word > 1u) return SDC41_ERR_VALUE;
        *enabled = word == 1u;
    }
    return result;
}

sdc41_result_t sdc41_set_asc(bool enabled) {
    return write_command_word(CMD_SET_ASC, enabled ? 1u : 0u);
}

sdc41_result_t sdc41_get_offset_raw(uint16_t *raw) {
    return read_words(CMD_GET_OFFSET, raw, 1, 1);
}

sdc41_result_t sdc41_set_offset_raw(uint16_t raw) {
    return write_command_word(CMD_SET_OFFSET, raw);
}

sdc41_result_t sdc41_get_altitude(uint16_t *metres) {
    return read_words(CMD_GET_ALTITUDE, metres, 1, 1);
}

sdc41_result_t sdc41_set_altitude(uint16_t metres) {
    return write_command_word(CMD_SET_ALTITUDE, metres);
}
