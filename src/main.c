#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "sdc41.h"
#include "sdc41_crc.h"

#define CONSOLE_UART uart1
#define CONSOLE_TX_PIN 8u
#define CONSOLE_RX_PIN 9u
#define LINE_CAPACITY 96u
#define AUTO_REFRESH_INTERVAL_MS 5000u
#define SAMPLE_POLL_INTERVAL_MS 1000u
#define COUNTDOWN_REFRESH_INTERVAL_MS 1000u
#define CO2_FILTER_SAMPLE_COUNT 7u
#define SDC41_SAMPLE_PERIOD_SECONDS 5u
#define SDC41_WARMUP_SECONDS 60u
#define DISPLAY_VALUE_COLUMN 17u
#define DISPLAY_PROMPT_ROW 17u
#define DISPLAY_COMMAND_LIST_FIRST_ROW 19u
#define DISPLAY_OUTPUT_FIRST_ROW 31u
#define DISPLAY_OUTPUT_LAST_ROW 38u

typedef enum { MODE_PERIODIC, MODE_SINGLE } measurement_mode_t;
typedef enum { SENSOR_ACTIVE, SENSOR_WARMING_UP, SENSOR_OFF } sensor_state_t;
typedef void (*command_handler_t)(const char *args);

typedef struct {
    const char *name;
    const char *display;
    const char *example;
    command_handler_t handler;
} command_entry_t;

static measurement_mode_t mode = MODE_PERIODIC;
static sensor_state_t sensor_state = SENSOR_ACTIVE;
static absolute_time_t warmup_deadline;
static bool start_periodic_after_warmup;
static sdc41_measurement_t last_measurement;
static bool have_measurement;
static sdc41_result_t last_measurement_result = SDC41_OK;
static bool data_ready_latched;
static sdc41_result_t last_ready_result = SDC41_OK;
static absolute_time_t last_raw_sample_time;

typedef struct {
    uint16_t samples[CO2_FILTER_SAMPLE_COUNT];
    uint8_t count;
    uint8_t next;
    bool batch_just_completed;
    bool ema_seeded;
    int32_t ema_milli_ppm;
} co2_filter_t;

static co2_filter_t co2_filter;

typedef struct {
    uint16_t serial_words[3];
    bool asc;
    uint16_t offset_raw;
    uint16_t altitude;
    sdc41_result_t serial_result;
    sdc41_result_t asc_result;
    sdc41_result_t offset_result;
    sdc41_result_t altitude_result;
} menu_config_cache_t;

static menu_config_cache_t menu_config;

typedef struct {
    sdc41_result_t measurement_result;
    bool have_measurement;
    sdc41_measurement_t measurement;
    menu_config_cache_t config;
    measurement_mode_t mode;
    sdc41_result_t ready_result;
    bool ready;
    uint8_t filter_count;
    bool filtered_co2_available;
    int32_t filtered_co2_milli_ppm;
} menu_snapshot_t;

static menu_snapshot_t last_printed_menu;
static bool have_last_printed_menu;
static bool command_output_active;
static unsigned command_output_row = DISPLAY_OUTPUT_FIRST_ROW;

static void show_command_prompt(void);
static void draw_command_list(void);

static void console_write(const char *text) {
    uart_puts(CONSOLE_UART, text);
    if (command_output_active) {
        for (const char *cursor = text; *cursor != '\0'; ++cursor) {
            if (*cursor == '\n' && command_output_row < DISPLAY_OUTPUT_LAST_ROW) {
                ++command_output_row;
            }
        }
    }
}

static void console_printf(const char *format, ...) {
    char buffer[192];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    console_write(buffer);
}

static void format_fixed_milli(char *buffer, size_t size, int32_t value,
                               const char *unit) {
    const char *sign = value < 0 ? "-" : "";
    uint32_t magnitude =
        value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    (void)snprintf(buffer, size, "%s%lu.%03lu%s", sign,
                   (unsigned long)(magnitude / 1000u),
                   (unsigned long)(magnitude % 1000u), unit);
}

static void draw_fixed_layout(const char *status, bool thermal_waiting) {
    static const char waiting[] = "thermal stabilisation waiting";
    const char *field_value = thermal_waiting ? waiting : "";
    uint64_t serial = ((uint64_t)menu_config.serial_words[0] << 32) |
                      ((uint64_t)menu_config.serial_words[1] << 16) |
                      menu_config.serial_words[2];

    console_printf("\x1b[8;38;80t\x1b[2J\x1b[%u;r\x1b[H"
                   "luftfugl sdc41 — SCD41 test harness\r\n",
                   DISPLAY_OUTPUT_FIRST_ROW);
    if (menu_config.serial_result == SDC41_OK)
        console_printf("- serial      = %llu\r\n", (unsigned long long)serial);
    else
        console_printf("- serial      = error (%s)\r\n",
                       sdc41_result_string(menu_config.serial_result));
    console_printf("- i2c address = 0x%02X\r\n\r\n%s\r\n", SDC41_ADDRESS,
                   status);
    console_printf("- co2         = %s\r\n", field_value);
    console_printf("- co2 raw     = %s\r\n", field_value);
    console_printf("- filter      = %s\r\n", field_value);
    console_printf("- temperature = %s\r\n", field_value);
    console_printf("- humidity    = %s\r\n", field_value);
    console_printf("- asc         = %s\r\n", field_value);
    console_printf("- offset      = %s\r\n", field_value);
    console_printf("- altitude    = %s\r\n", field_value);
    console_printf("- mode        = %s\r\n", field_value);
    console_printf("- data ready  = %s\r\n\r\nEnter Command > \r\n\r\n",
                   field_value);
    draw_command_list();
    show_command_prompt();
    command_output_active = false;
    command_output_row = DISPLAY_OUTPUT_FIRST_ROW;
}

static uint32_t warmup_seconds_remaining(void) {
    int64_t remaining_us =
        absolute_time_diff_us(get_absolute_time(), warmup_deadline);
    if (remaining_us <= 0) return 0;
    return (uint32_t)((remaining_us + 999999) / 1000000);
}

static void update_warmup_status(void) {
    console_printf("\x1b" "7\x1b[5;1H\x1b[KSDC41 is Warming up (%lu sec ... "
                   "counting down to 0)\x1b" "8",
                   (unsigned long)warmup_seconds_remaining());
}

static void draw_warmup_layout(void) {
    char status[80];
    (void)snprintf(status, sizeof(status),
                   "SDC41 is Warming up (%lu sec ... counting down to 0)",
                   (unsigned long)warmup_seconds_remaining());
    draw_fixed_layout(status, true);
}

static void draw_off_layout(void) {
    draw_fixed_layout("SDC41 = OFF", true);
}

static void update_fixed_field(unsigned row, const char *value) {
    console_printf("\x1b" "7\x1b[%u;%uH\x1b[K%s\x1b" "8", row,
                   DISPLAY_VALUE_COLUMN, value);
}

static void show_command_prompt(void) {
    console_printf("\x1b[%u;1H\x1b[KEnter Command > ", DISPLAY_PROMPT_ROW);
}

static void begin_command_output(void) {
    console_printf("\x1b[%u;1H\x1b[K", command_output_row);
    command_output_active = true;
}

static void end_command_output(void) {
    command_output_active = false;
    show_command_prompt();
}

static void print_sensor_error(const char *operation, sdc41_result_t result) {
    console_printf("error: %s: %s\r\n", operation, sdc41_result_string(result));
}

static void print_fixed_milli(int32_t value, const char *unit) {
    const char *sign = value < 0 ? "-" : "";
    uint32_t magnitude = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    console_printf("%s%lu.%03lu%s", sign, (unsigned long)(magnitude / 1000u),
                   (unsigned long)(magnitude % 1000u), unit);
}

static uint16_t median_of_filter_samples(void) {
    uint16_t sorted[CO2_FILTER_SAMPLE_COUNT];
    memcpy(sorted, co2_filter.samples, sizeof(sorted));
    for (size_t i = 1; i < CO2_FILTER_SAMPLE_COUNT; ++i) {
        uint16_t value = sorted[i];
        size_t position = i;
        while (position > 0 && sorted[position - 1] > value) {
            sorted[position] = sorted[position - 1];
            --position;
        }
        sorted[position] = value;
    }
    return sorted[CO2_FILTER_SAMPLE_COUNT / 2u];
}

static void filter_push(uint16_t raw_co2_ppm) {
    co2_filter.batch_just_completed = false;
    co2_filter.samples[co2_filter.next] = raw_co2_ppm;
    co2_filter.next =
        (uint8_t)((co2_filter.next + 1u) % CO2_FILTER_SAMPLE_COUNT);
    ++co2_filter.count;
    if (co2_filter.count == CO2_FILTER_SAMPLE_COUNT) {
        int32_t median_milli_ppm = (int32_t)median_of_filter_samples() * 1000;
        if (!co2_filter.ema_seeded) {
            co2_filter.ema_milli_ppm = median_milli_ppm;
            co2_filter.ema_seeded = true;
        } else {
            co2_filter.ema_milli_ppm =
                (3 * median_milli_ppm + 7 * co2_filter.ema_milli_ppm + 5) /
                10;
        }
        co2_filter.count = 0;
        co2_filter.next = 0;
        co2_filter.batch_just_completed = true;
    }
}

static uint8_t filter_display_count(void) {
    return co2_filter.batch_just_completed ? CO2_FILTER_SAMPLE_COUNT
                                           : co2_filter.count;
}

static uint16_t filtered_co2_ppm(void) {
    return (uint16_t)((co2_filter.ema_milli_ppm + 500) / 1000);
}

static uint32_t seconds_since_last_raw_sample(void) {
    int64_t elapsed_us =
        absolute_time_diff_us(last_raw_sample_time, get_absolute_time());
    if (elapsed_us <= 0) return 0;
    return (uint32_t)(elapsed_us / 1000000);
}

static uint32_t countdown_clamped(uint32_t period_seconds,
                                  uint32_t elapsed_seconds) {
    return elapsed_seconds < period_seconds ? period_seconds - elapsed_seconds
                                            : 0;
}

static uint32_t raw_countdown_seconds(void) {
    return countdown_clamped(SDC41_SAMPLE_PERIOD_SECONDS,
                             seconds_since_last_raw_sample());
}

static uint32_t filtered_countdown_seconds(void) {
    uint32_t samples_remaining = CO2_FILTER_SAMPLE_COUNT - co2_filter.count;
    return countdown_clamped(samples_remaining * SDC41_SAMPLE_PERIOD_SECONDS,
                             seconds_since_last_raw_sample());
}

static const char *co2_zone_label(uint16_t co2_ppm) {
    if (co2_ppm < 600u) return "'Excellent < 600 ppm'";
    if (co2_ppm < 800u) return "'Good 600–800 ppm'";
    if (co2_ppm < 1000u) return "'Moderate 800–1,000 ppm'";
    if (co2_ppm <= 2000u) return "'Poor 1,000–2,000 ppm'";
    return "'Hazardous > 2,000 ppm'";
}

static void format_co2_value(char *buffer, size_t size,
                             const menu_snapshot_t *snapshot) {
    if (!snapshot->filtered_co2_available)
        (void)snprintf(buffer, size, "pending - %lu seconds",
                       (unsigned long)filtered_countdown_seconds());
    else {
        uint16_t co2_ppm =
            (uint16_t)((snapshot->filtered_co2_milli_ppm + 500) / 1000);
        (void)snprintf(buffer, size,
                       "%u ppm - %lu seconds ->>> co2 zone: %s", co2_ppm,
                       (unsigned long)filtered_countdown_seconds(),
                       co2_zone_label(co2_ppm));
    }
}

static void format_raw_co2_value(char *buffer, size_t size,
                                 const menu_snapshot_t *snapshot) {
    if (snapshot->measurement_result != SDC41_OK)
        (void)snprintf(buffer, size, "error (%s) - %lu seconds",
                       sdc41_result_string(snapshot->measurement_result),
                       (unsigned long)raw_countdown_seconds());
    else if (!snapshot->have_measurement)
        (void)snprintf(buffer, size, "pending - %lu seconds",
                       (unsigned long)raw_countdown_seconds());
    else
        (void)snprintf(buffer, size, "%u ppm - %lu seconds",
                       snapshot->measurement.co2_ppm,
                       (unsigned long)raw_countdown_seconds());
}

static void refresh_countdown_fields(void) {
    if (sensor_state != SENSOR_ACTIVE || !have_last_printed_menu) return;
    char value[80];
    format_co2_value(value, sizeof(value), &last_printed_menu);
    update_fixed_field(6, value);
    format_raw_co2_value(value, sizeof(value), &last_printed_menu);
    update_fixed_field(7, value);
}

static void accept_measurement(const sdc41_measurement_t *measurement) {
    last_measurement = *measurement;
    have_measurement = true;
    last_measurement_result = SDC41_OK;
    data_ready_latched = true;
    last_raw_sample_time = get_absolute_time();
    if (sensor_state == SENSOR_ACTIVE) filter_push(measurement->co2_ppm);
}

static void poll_periodic_sample(void) {
    if (sensor_state == SENSOR_OFF) return;
    bool ready = false;
    last_ready_result = sdc41_get_ready(&ready);
    if (last_ready_result != SDC41_OK) return;
    if (!ready) return;

    sdc41_measurement_t measurement;
    last_measurement_result = sdc41_read_measurement(&measurement);
    if (last_measurement_result == SDC41_OK) accept_measurement(&measurement);
}

static void print_measurement(const sdc41_measurement_t *measurement) {
    if (!co2_filter.ema_seeded)
        console_printf("co2: pending, co2 raw: %u ppm, temperature: ",
                       measurement->co2_ppm);
    else
        console_printf("co2: %u ppm, co2 raw: %u ppm, temperature: ",
                       filtered_co2_ppm(), measurement->co2_ppm);
    print_fixed_milli(measurement->temperature_milli_c, " degrees C");
    console_write(", humidity: ");
    print_fixed_milli((int32_t)measurement->humidity_milli_percent, " %RH\r\n");
}

static sdc41_result_t enter_idle(bool *restart_periodic) {
    *restart_periodic = mode == MODE_PERIODIC;
    if (*restart_periodic) {
        sdc41_result_t result = sdc41_stop_periodic();
        if (result == SDC41_OK) sleep_ms(500);
        return result;
    }
    return SDC41_OK;
}

static void leave_idle(bool restart_periodic) {
    if (restart_periodic) {
        sdc41_result_t result = sdc41_start_periodic();
        if (result != SDC41_OK) {
            print_sensor_error("could not restore periodic measurement", result);
        }
    }
}

static bool begin_idle_operation(const char *operation, bool *restart_periodic) {
    sdc41_result_t result = enter_idle(restart_periodic);
    if (result != SDC41_OK) {
        print_sensor_error(operation, result);
        return false;
    }
    return true;
}

static bool parse_long(const char *text, long minimum, long maximum, long *value) {
    char *end;
    if (*text == '\0' || isspace((unsigned char)*text)) return false;
    long parsed = strtol(text, &end, 10);
    if (*end != '\0' || parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

static bool parse_offset(const char *text, uint16_t *raw) {
    char *end;
    if (*text == '\0' || isspace((unsigned char)*text)) return false;
    double value = strtod(text, &end);
    if (*end != '\0' || value < 0.0 || value > 20.0) return false;
    double encoded = value * 65536.0 / 175.0;
    if (encoded > 65535.0) encoded = 65535.0;
    *raw = (uint16_t)(encoded + 0.5);
    return true;
}

static void command_co2(const char *args) {
    if (*args != '\0') {
        console_write("rejected: co2 takes no arguments; example: co2\r\n");
        return;
    }
    sdc41_result_t result = last_measurement_result;
    if (mode == MODE_SINGLE) {
        console_write("single-shot measurement: waiting 5000 ms\r\n");
        result = sdc41_measure_single_shot();
        if (result == SDC41_OK) {
            sdc41_measurement_t measurement;
            result = sdc41_read_measurement(&measurement);
            if (result == SDC41_OK) accept_measurement(&measurement);
        }
    }
    if (result == SDC41_OK && !have_measurement) {
        console_write("co2: no measurement available yet; try again after 5 seconds\r\n");
        return;
    }
    if (result != SDC41_OK) print_sensor_error("could not read measurement", result);
    else print_measurement(&last_measurement);
}

static void command_ready(const char *args) {
    if (*args != '\0') {
        console_write("rejected: ready takes no arguments; example: ready\r\n");
        return;
    }
    bool ready = data_ready_latched;
    if (last_ready_result != SDC41_OK)
        print_sensor_error("could not read data-ready status", last_ready_result);
    else {
        console_printf("data ready: %s\r\n", ready ? "yes" : "no");
        if (ready) data_ready_latched = false;
    }
}

static void command_serial(const char *args) {
    if (*args != '\0') {
        console_write("rejected: serial takes no arguments; example: serial\r\n");
        return;
    }
    bool restart;
    if (!begin_idle_operation("could not stop periodic measurement", &restart)) return;
    uint16_t words[3];
    sdc41_result_t result = sdc41_get_serial(words);
    leave_idle(restart);
    if (result != SDC41_OK) print_sensor_error("could not read serial number", result);
    else console_printf("serial: %04X%04X%04X\r\n", words[0], words[1], words[2]);
}

static void command_selftest(const char *args) {
    if (*args != '\0') {
        console_write("rejected: selftest takes no arguments; example: selftest\r\n");
        return;
    }
    bool restart;
    if (!begin_idle_operation("could not stop periodic measurement", &restart)) return;
    console_write("self-test: running, this takes up to 10 seconds\r\n");
    uint16_t result_word;
    sdc41_result_t result = sdc41_self_test(&result_word);
    leave_idle(restart);
    if (result != SDC41_OK) print_sensor_error("self-test failed to execute", result);
    else console_printf("self-test: %s%s\r\n", result_word == 0 ? "pass" : "fail",
                        result_word == 0 ? "" : " (sensor reported a malfunction)");
}

static void command_asc(const char *args) {
    bool setting = *args != '\0';
    bool requested = false;
    if (setting && strcmp(args, "on") != 0 && strcmp(args, "off") != 0) {
        console_write("rejected: asc must be 'asc', 'asc on', or 'asc off'; example: asc off\r\n");
        return;
    }
    if (setting) requested = strcmp(args, "on") == 0;
    bool restart;
    if (!begin_idle_operation("could not stop periodic measurement", &restart)) return;
    bool enabled = false;
    sdc41_result_t result = setting ? sdc41_set_asc(requested) : sdc41_get_asc(&enabled);
    leave_idle(restart);
    if (result != SDC41_OK) print_sensor_error(setting ? "could not set ASC" : "could not read ASC", result);
    else {
        if (setting) {
            menu_config.asc = requested;
            menu_config.asc_result = SDC41_OK;
        }
        console_printf("asc: %s%s\r\n", setting ? "set " : "", (setting ? requested : enabled) ? "on" : "off");
    }
}

static void command_offset(const char *args) {
    bool setting = *args != '\0';
    uint16_t raw = 0;
    if (setting && !parse_offset(args, &raw)) {
        console_write("rejected: offset must be a number between 0 and 20 degrees; example: offset 4.5\r\n");
        return;
    }
    bool restart;
    if (!begin_idle_operation("could not stop periodic measurement", &restart)) return;
    sdc41_result_t result = setting ? sdc41_set_offset_raw(raw) : sdc41_get_offset_raw(&raw);
    leave_idle(restart);
    if (result != SDC41_OK) {
        print_sensor_error(setting ? "could not set temperature offset" : "could not read temperature offset", result);
    } else {
        int32_t milli = (int32_t)(((uint64_t)raw * 175000u) / 65536u);
        if (setting) {
            menu_config.offset_raw = raw;
            menu_config.offset_result = SDC41_OK;
        }
        console_write(setting ? "offset: set " : "offset: ");
        print_fixed_milli(milli, " degrees C\r\n");
    }
}

static void command_altitude(const char *args) {
    bool setting = *args != '\0';
    long parsed = 0;
    if (setting && !parse_long(args, 0, 3000, &parsed)) {
        console_write("rejected: altitude must be a whole number from 0 to 3000 metres; example: altitude 12\r\n");
        return;
    }
    uint16_t metres = (uint16_t)parsed;
    bool restart;
    if (!begin_idle_operation("could not stop periodic measurement", &restart)) return;
    sdc41_result_t result = setting ? sdc41_set_altitude(metres) : sdc41_get_altitude(&metres);
    leave_idle(restart);
    if (result != SDC41_OK) print_sensor_error(setting ? "could not set altitude" : "could not read altitude", result);
    else {
        if (setting) {
            menu_config.altitude = metres;
            menu_config.altitude_result = SDC41_OK;
        }
        console_printf("altitude: %s%u m\r\n", setting ? "set " : "", metres);
    }
}

static void command_mode(const char *args) {
    if (*args == '\0') {
        console_printf("mode: %s\r\n", mode == MODE_PERIODIC ? "periodic" : "single");
        return;
    }
    if (strcmp(args, "periodic") != 0 && strcmp(args, "single") != 0) {
        console_write("rejected: mode must be 'mode', 'mode periodic', or 'mode single'; example: mode single\r\n");
        return;
    }
    measurement_mode_t requested = strcmp(args, "periodic") == 0 ? MODE_PERIODIC : MODE_SINGLE;
    if (requested == mode) {
        console_printf("mode: already %s\r\n", args);
        return;
    }
    sdc41_result_t result = requested == MODE_SINGLE ? sdc41_stop_periodic() : sdc41_start_periodic();
    if (result != SDC41_OK) print_sensor_error("could not switch measurement mode", result);
    else {
        mode = requested;
        console_printf("mode: set %s\r\n", args);
    }
}

static void command_status(const char *args) {
    if (*args != '\0') {
        console_write("rejected: status takes no arguments; example: status\r\n");
        return;
    }
    bool ready = data_ready_latched;
    sdc41_result_t ready_result = last_ready_result;
    bool restart;
    if (!begin_idle_operation("could not stop periodic measurement", &restart)) return;
    bool asc;
    sdc41_result_t asc_result = sdc41_get_asc(&asc);
    leave_idle(restart);
    console_printf("status: mode=%s, asc=", mode == MODE_PERIODIC ? "periodic" : "single");
    if (asc_result == SDC41_OK) console_write(asc ? "on" : "off");
    else console_printf("error (%s)", sdc41_result_string(asc_result));
    console_write(", last reading=");
    if (have_measurement) {
        console_printf("%u ppm/", last_measurement.co2_ppm);
        print_fixed_milli(last_measurement.temperature_milli_c, " C/");
        print_fixed_milli((int32_t)last_measurement.humidity_milli_percent, " %RH");
    } else console_write("none");
    if (ready_result == SDC41_OK) {
        console_printf(", data ready=%s\r\n", ready ? "yes" : "no");
        if (ready) data_ready_latched = false;
    }
    else console_printf(", data ready=error (%s)\r\n", sdc41_result_string(ready_result));
}

static void format_menu_error(char *buffer, size_t size,
                              sdc41_result_t result) {
    (void)snprintf(buffer, size, "error (%s)", sdc41_result_string(result));
}

static void refresh_menu_config(void) {
    bool restart;
    sdc41_result_t idle_result = enter_idle(&restart);
    if (idle_result == SDC41_OK) {
        menu_config.serial_result = sdc41_get_serial(menu_config.serial_words);
        menu_config.asc_result = sdc41_get_asc(&menu_config.asc);
        menu_config.offset_result =
            sdc41_get_offset_raw(&menu_config.offset_raw);
        menu_config.altitude_result =
            sdc41_get_altitude(&menu_config.altitude);
        leave_idle(restart);
    } else {
        menu_config.serial_result = idle_result;
        menu_config.asc_result = idle_result;
        menu_config.offset_result = idle_result;
        menu_config.altitude_result = idle_result;
    }
}

static bool measurement_field_changed(const menu_snapshot_t *current,
                                      const menu_snapshot_t *previous,
                                      int32_t current_value,
                                      int32_t previous_value) {
    return current->measurement_result != previous->measurement_result ||
           current->have_measurement != previous->have_measurement ||
           (current->measurement_result == SDC41_OK &&
            current->have_measurement && current_value != previous_value);
}

static bool result_value_changed(sdc41_result_t current_result,
                                 sdc41_result_t previous_result,
                                 uint64_t current_value,
                                 uint64_t previous_value) {
    return current_result != previous_result ||
           (current_result == SDC41_OK && current_value != previous_value);
}

static void print_menu(bool refresh_config, bool changed_only) {
    if (sensor_state == SENSOR_WARMING_UP) {
        draw_warmup_layout();
        return;
    }
    if (sensor_state == SENSOR_OFF) {
        draw_off_layout();
        return;
    }

    bool ready = data_ready_latched;
    sdc41_result_t ready_result = last_ready_result;
    sdc41_result_t measurement_result = last_measurement_result;

    if (refresh_config) refresh_menu_config();

    menu_snapshot_t current = {
        .measurement_result = measurement_result,
        .have_measurement = have_measurement,
        .measurement = last_measurement,
        .config = menu_config,
        .mode = mode,
        .ready_result = ready_result,
        .ready = ready,
        .filter_count = filter_display_count(),
        .filtered_co2_available = co2_filter.ema_seeded,
        .filtered_co2_milli_ppm = co2_filter.ema_milli_ppm,
    };
    bool all = !changed_only || !have_last_printed_menu;
    bool filter_ready = current.filter_count == CO2_FILTER_SAMPLE_COUNT;
    bool co2_changed =
        all || current.filtered_co2_available !=
                   last_printed_menu.filtered_co2_available ||
        (current.filtered_co2_available &&
         current.filtered_co2_milli_ppm !=
             last_printed_menu.filtered_co2_milli_ppm);
    bool raw_co2_changed =
        all || measurement_field_changed(&current, &last_printed_menu,
                                         current.measurement.co2_ppm,
                                         last_printed_menu.measurement.co2_ppm);
    bool filter_changed =
        all || current.filter_count != last_printed_menu.filter_count;
    bool temperature_changed =
        all || measurement_field_changed(
                   &current, &last_printed_menu,
                   current.measurement.temperature_milli_c,
                   last_printed_menu.measurement.temperature_milli_c);
    bool humidity_changed =
        all || measurement_field_changed(
                   &current, &last_printed_menu,
                   (int32_t)current.measurement.humidity_milli_percent,
                   (int32_t)last_printed_menu.measurement.humidity_milli_percent);
    bool ready_changed =
        all || result_value_changed(ready_result,
                                    last_printed_menu.ready_result, ready,
                                    last_printed_menu.ready);

    if (all) draw_fixed_layout("SDC41 active", false);

    char value[80];

    if (co2_changed) {
        format_co2_value(value, sizeof(value), &current);
        update_fixed_field(6, value);
    }
    if (raw_co2_changed) {
        format_raw_co2_value(value, sizeof(value), &current);
        update_fixed_field(7, value);
    }
    if (filter_changed) {
        static const char *const progress[CO2_FILTER_SAMPLE_COUNT] = {
            "100%", "86%", "71%", "57%", "43%", "29%", "14%",
        };
        (void)snprintf(value, sizeof(value), "%s",
                       filter_ready ? "ready" : progress[current.filter_count]);
        update_fixed_field(8, value);
    }
    if (temperature_changed) {
        if (measurement_result != SDC41_OK)
            format_menu_error(value, sizeof(value), measurement_result);
        else if (!have_measurement)
            (void)snprintf(value, sizeof(value), "pending");
        else
            format_fixed_milli(value, sizeof(value),
                               last_measurement.temperature_milli_c, " C");
        update_fixed_field(9, value);
    }
    if (humidity_changed) {
        if (measurement_result != SDC41_OK)
            format_menu_error(value, sizeof(value), measurement_result);
        else if (!have_measurement)
            (void)snprintf(value, sizeof(value), "pending");
        else
            format_fixed_milli(
                value, sizeof(value),
                (int32_t)last_measurement.humidity_milli_percent, " %RH");
        update_fixed_field(10, value);
    }

    uint64_t serial = ((uint64_t)menu_config.serial_words[0] << 32) |
                      ((uint64_t)menu_config.serial_words[1] << 16) |
                      menu_config.serial_words[2];
    uint64_t previous_serial =
        ((uint64_t)last_printed_menu.config.serial_words[0] << 32) |
        ((uint64_t)last_printed_menu.config.serial_words[1] << 16) |
        last_printed_menu.config.serial_words[2];
    if (all || result_value_changed(menu_config.serial_result,
                                    last_printed_menu.config.serial_result,
                                    serial, previous_serial)) {
        if (menu_config.serial_result == SDC41_OK)
            (void)snprintf(value, sizeof(value), "%llu",
                           (unsigned long long)serial);
        else
            format_menu_error(value, sizeof(value), menu_config.serial_result);
        update_fixed_field(2, value);
    }
    if (all || result_value_changed(menu_config.asc_result,
                                    last_printed_menu.config.asc_result,
                                    menu_config.asc,
                                    last_printed_menu.config.asc)) {
        if (menu_config.asc_result == SDC41_OK)
            (void)snprintf(value, sizeof(value), "%s",
                           menu_config.asc ? "on" : "off");
        else
            format_menu_error(value, sizeof(value), menu_config.asc_result);
        update_fixed_field(11, value);
    }
    if (all || result_value_changed(menu_config.offset_result,
                                    last_printed_menu.config.offset_result,
                                    menu_config.offset_raw,
                                    last_printed_menu.config.offset_raw)) {
        if (menu_config.offset_result == SDC41_OK) {
            int32_t offset_milli = (int32_t)(
                ((uint64_t)menu_config.offset_raw * 175000u) / 65536u);
            format_fixed_milli(value, sizeof(value), offset_milli, " C");
        } else {
            format_menu_error(value, sizeof(value), menu_config.offset_result);
        }
        update_fixed_field(12, value);
    }
    if (all || result_value_changed(menu_config.altitude_result,
                                    last_printed_menu.config.altitude_result,
                                    menu_config.altitude,
                                    last_printed_menu.config.altitude)) {
        if (menu_config.altitude_result == SDC41_OK)
            (void)snprintf(value, sizeof(value), "%u m",
                           menu_config.altitude);
        else
            format_menu_error(value, sizeof(value), menu_config.altitude_result);
        update_fixed_field(13, value);
    }
    if (all || mode != last_printed_menu.mode) {
        (void)snprintf(value, sizeof(value), "%s",
                       mode == MODE_PERIODIC ? "periodic" : "single");
        update_fixed_field(14, value);
    }
    if (ready_changed) {
        if (ready_result == SDC41_OK)
            (void)snprintf(value, sizeof(value), "%s", ready ? "yes" : "no");
        else
            format_menu_error(value, sizeof(value), ready_result);
        update_fixed_field(15, value);
        if (ready_result == SDC41_OK && ready) data_ready_latched = false;
    }
    if (all) {
        (void)snprintf(value, sizeof(value), "0x%02X", SDC41_ADDRESS);
        update_fixed_field(3, value);
    }

    last_printed_menu = current;
    have_last_printed_menu = true;
}

static void reset_measurement_pipeline(void) {
    memset(&co2_filter, 0, sizeof(co2_filter));
    memset(&last_measurement, 0, sizeof(last_measurement));
    have_measurement = false;
    last_measurement_result = SDC41_OK;
    data_ready_latched = false;
    last_ready_result = SDC41_OK;
    last_raw_sample_time = get_absolute_time();
    have_last_printed_menu = false;
}

static void begin_sensor_warmup(void) {
    sensor_state = SENSOR_WARMING_UP;
    warmup_deadline = make_timeout_time_ms(SDC41_WARMUP_SECONDS * 1000u);
    reset_measurement_pipeline();
}

static void complete_sensor_warmup(void) {
    reset_measurement_pipeline();
    if (start_periodic_after_warmup) {
        sdc41_result_t result = sdc41_start_periodic();
        start_periodic_after_warmup = false;
        if (result != SDC41_OK) {
            sensor_state = SENSOR_OFF;
            draw_off_layout();
            begin_command_output();
            print_sensor_error("could not start periodic measurement", result);
            end_command_output();
            return;
        }
        mode = MODE_PERIODIC;
    }
    sensor_state = SENSOR_ACTIVE;
    print_menu(false, false);
}

static void command_sdc41(const char *args) {
    if (strcmp(args, "off") == 0) {
        if (sensor_state == SENSOR_OFF) {
            console_write("rejected: sdc41 already off\r\n");
            return;
        }
        bool restart_periodic;
        sdc41_result_t result = enter_idle(&restart_periodic);
        if (result == SDC41_OK) result = sdc41_power_down();
        if (result != SDC41_OK) {
            print_sensor_error("could not power down SDC41", result);
            return;
        }
        (void)restart_periodic;
        start_periodic_after_warmup = false;
        sensor_state = SENSOR_OFF;
        reset_measurement_pipeline();
        draw_off_layout();
        return;
    }
    if (strcmp(args, "on") == 0) {
        if (sensor_state != SENSOR_OFF) {
            console_write("rejected: sdc41 already on\r\n");
            return;
        }
        sdc41_wake_up();
        start_periodic_after_warmup = true;
        begin_sensor_warmup();
        draw_warmup_layout();
        return;
    }
    console_write("rejected: sdc41 requires on or off; example: sdc41 off\r\n");
}

static void command_menu(const char *args) {
    if (*args != '\0') {
        console_write("rejected: menu takes no arguments; example: menu\r\n");
        return;
    }
    print_menu(true, false);
}

static const char help_text[] =
    "commands:\r\n"
    "  co2                 example: co2\r\n"
    "  ready               example: ready\r\n"
    "  serial              example: serial\r\n"
    "  selftest            example: selftest\r\n"
    "  asc [on|off]        example: asc off\r\n"
    "  offset [degrees]    example: offset 4.5\r\n"
    "  altitude [metres]   example: altitude 12\r\n"
    "  mode [periodic|single]  example: mode single\r\n"
    "  status              example: status\r\n"
    "  sdc41 <on|off>      examples: sdc41 on, sdc41 off\r\n"
    "  menu                example: menu\r\n"
    "  help [command]      example: help offset\r\n"
    "note: ambient pressure can be set by the sensor protocol but cannot be read back; this harness has no pressure command.\r\n";

static void command_help(const char *args) {
    if (*args == '\0') {
        console_write(help_text);
        return;
    }
    struct detail { const char *name; const char *text; };
    static const struct detail details[] = {
        {"co2", "co2: print the latest periodic reading, or take a 5-second single shot. Example: co2\r\n"},
        {"ready", "ready: print whether unread measurement data is ready. Example: ready\r\n"},
        {"serial", "serial: print the sensor's 48-bit serial number. Example: serial\r\n"},
        {"selftest", "selftest: stop measurement and run the approximately 10-second sensor self-test. Example: selftest\r\n"},
        {"asc", "asc [on|off]: read or set automatic self-calibration. Example: asc off\r\n"},
        {"offset", "offset [degrees]: read or set temperature offset from 0 to 20 degrees C. Example: offset 4.5\r\n"},
        {"altitude", "altitude [metres]: read or set altitude from 0 to 3000 m. Example: altitude 12\r\n"},
        {"mode", "mode [periodic|single]: read or select measurement mode. Example: mode single\r\n"},
        {"status", "status: print mode, ASC, last reading, and data-ready state. Example: status\r\n"},
        {"sdc41", "sdc41 <on|off>: enter sensor power-down or wake it and begin the 60-second warm-up. Examples: sdc41 on, sdc41 off\r\n"},
        {"menu", "menu: print every readable parameter as key = value lines and refresh cached configuration; changed fields auto-refresh every 5 seconds while the console is idle. Selftest is deliberately excluded. Example: menu\r\n"},
        {"help", "help [command]: list commands or show one command in detail. Example: help offset\r\n"},
    };
    for (size_t i = 0; i < sizeof(details) / sizeof(details[0]); ++i) {
        if (strcmp(args, details[i].name) == 0) {
            console_write(details[i].text);
            return;
        }
    }
    if (strcmp(args, "pressure") == 0) {
        console_write("pressure: not implemented because SCD41 ambient pressure has no read-back command.\r\n");
    } else {
        console_printf("rejected: unknown help topic '%s'; example: help offset\r\n", args);
    }
}

static const command_entry_t command_table[] = {
    {"co2", "co2", "co2", command_co2},
    {"ready", "ready", "ready", command_ready},
    {"serial", "serial", "serial", command_serial},
    {"selftest", "selftest", "selftest", command_selftest},
    {"asc", "asc [on|off]", "asc off", command_asc},
    {"offset", "offset [degrees]", "offset 4.5", command_offset},
    {"altitude", "altitude [metres]", "altitude 12", command_altitude},
    {"mode", "mode [periodic|single]", "mode single", command_mode},
    {"status", "status", "status", command_status},
    {"sdc41", "sdc41 <on|off>", "sdc41 off", command_sdc41},
    {"menu", "menu", "menu", command_menu},
    {"help", "help [command]", "help offset", command_help},
};

_Static_assert(sizeof(command_table) / sizeof(command_table[0]) ==
                   DISPLAY_OUTPUT_FIRST_ROW - DISPLAY_COMMAND_LIST_FIRST_ROW,
               "command list rows must end immediately before output");

static void draw_command_list(void) {
    for (size_t i = 0; i < sizeof(command_table) / sizeof(command_table[0]);
         ++i) {
        console_printf("  %-24s example: %s\r\n", command_table[i].display,
                       command_table[i].example);
    }
}

static void execute_line(char *line) {
    while (isspace((unsigned char)*line)) ++line;
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';
    if (*line == '\0') return;
    char *args = line;
    while (*args != '\0' && !isspace((unsigned char)*args)) ++args;
    if (*args != '\0') {
        *args++ = '\0';
        while (isspace((unsigned char)*args)) ++args;
    }

    for (size_t i = 0; i < sizeof(command_table) / sizeof(command_table[0]);
         ++i) {
        if (strcmp(line, command_table[i].name) == 0) {
            command_table[i].handler(args);
            return;
        }
    }
    console_printf("rejected: unknown command '%s'; type help for examples\r\n",
                   line);
}

static void console_init(void) {
    uart_init(CONSOLE_UART, 115200u);
    gpio_set_function(CONSOLE_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(CONSOLE_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(CONSOLE_UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(CONSOLE_UART, false, false);
    uart_set_fifo_enabled(CONSOLE_UART, true);
}

int main(void) {
    console_init();
    sdc41_init();

    for (int remaining = 1000; remaining >= 0; remaining -= 100) {
        console_printf("booting: SCD41 power-up, %d ms remaining\r\n", remaining);
        if (remaining != 0) sleep_ms(100);
    }
    if (!sdc41_crc_self_check()) {
        console_write("crc self-check: FAILED; CRC(0xBEEF) did not equal 0x92; sensor commands disabled\r\n");
        while (true) tight_loop_contents();
    }
    console_write("crc self-check: passed; CRC(0xBEEF) = 0x92\r\n");
    sdc41_result_t result = sdc41_start_periodic();
    if (result == SDC41_OK) {
        console_write("mode: periodic measurement started\r\n");
        start_periodic_after_warmup = false;
        begin_sensor_warmup();
        refresh_menu_config();
        draw_warmup_layout();
    }
    else {
        print_sensor_error("could not start periodic measurement", result);
        print_menu(true, false);
    }

    char line[LINE_CAPACITY];
    size_t length = 0;
    bool ignored_lf = false;
    absolute_time_t refresh_deadline =
        make_timeout_time_ms(AUTO_REFRESH_INTERVAL_MS);
    absolute_time_t sample_deadline =
        make_timeout_time_ms(SAMPLE_POLL_INTERVAL_MS);
    absolute_time_t countdown_deadline =
        make_timeout_time_ms(COUNTDOWN_REFRESH_INTERVAL_MS);
    while (true) {
        bool did_work = false;
        if (uart_is_readable(CONSOLE_UART)) {
            did_work = true;
            char ch = (char)uart_getc(CONSOLE_UART);
            if (ch == '\r' || ch == '\n') {
                if (ch == '\n' && ignored_lf) {
                    ignored_lf = false;
                    refresh_deadline =
                        make_timeout_time_ms(AUTO_REFRESH_INTERVAL_MS);
                    continue;
                }
                ignored_lf = ch == '\r';
                line[length] = '\0';
                begin_command_output();
                execute_line(line);
                end_command_output();
                length = 0;
            } else {
                ignored_lf = false;
                if (ch == '\b' || ch == 0x7f) {
                    if (length > 0) {
                        --length;
                        console_write("\b \b");
                    }
                } else if (isprint((unsigned char)ch)) {
                    if (length + 1 < sizeof(line)) {
                        line[length++] = ch;
                        uart_putc_raw(CONSOLE_UART, ch);
                    } else {
                        begin_command_output();
                        console_write("rejected: command is too long; maximum is 95 characters\r\n");
                        end_command_output();
                        length = 0;
                    }
                }
            }
            refresh_deadline = make_timeout_time_ms(AUTO_REFRESH_INTERVAL_MS);
        }
        if (time_reached(sample_deadline)) {
            poll_periodic_sample();
            sample_deadline = make_timeout_time_ms(SAMPLE_POLL_INTERVAL_MS);
            did_work = true;
        }
        if (sensor_state == SENSOR_WARMING_UP &&
            time_reached(warmup_deadline)) {
            complete_sensor_warmup();
            refresh_deadline = make_timeout_time_ms(AUTO_REFRESH_INTERVAL_MS);
            did_work = true;
        }
        if (time_reached(countdown_deadline)) {
            if (sensor_state == SENSOR_WARMING_UP)
                update_warmup_status();
            else
                refresh_countdown_fields();
            countdown_deadline =
                make_timeout_time_ms(COUNTDOWN_REFRESH_INTERVAL_MS);
            did_work = true;
        }
        if (sensor_state == SENSOR_ACTIVE && length == 0 &&
            time_reached(refresh_deadline)) {
            print_menu(false, true);
            refresh_deadline = make_timeout_time_ms(AUTO_REFRESH_INTERVAL_MS);
            did_work = true;
        }
        if (!did_work) tight_loop_contents();
    }
}
