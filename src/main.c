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

typedef enum { MODE_PERIODIC, MODE_SINGLE } measurement_mode_t;

static measurement_mode_t mode = MODE_PERIODIC;
static sdc41_measurement_t last_measurement;
static bool have_measurement;

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
} menu_snapshot_t;

static menu_snapshot_t last_printed_menu;
static bool have_last_printed_menu;

static void console_write(const char *text) {
    uart_puts(CONSOLE_UART, text);
}

static void console_printf(const char *format, ...) {
    char buffer[192];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    console_write(buffer);
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

static void print_measurement(const sdc41_measurement_t *measurement) {
    console_printf("co2: %u ppm, temperature: ", measurement->co2_ppm);
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
    sdc41_result_t result;
    if (mode == MODE_SINGLE) {
        console_write("single-shot measurement: waiting 5000 ms\r\n");
        result = sdc41_measure_single_shot();
        if (result == SDC41_OK) result = sdc41_read_measurement(&last_measurement);
        if (result == SDC41_OK) have_measurement = true;
    } else {
        bool ready = false;
        result = sdc41_get_ready(&ready);
        if (result == SDC41_OK && ready) {
            result = sdc41_read_measurement(&last_measurement);
            if (result == SDC41_OK) have_measurement = true;
        }
        if (result == SDC41_OK && !have_measurement) {
            console_write("co2: no periodic measurement available yet; try again after 5 seconds\r\n");
            return;
        }
    }
    if (result != SDC41_OK) print_sensor_error("could not read measurement", result);
    else print_measurement(&last_measurement);
}

static void command_ready(const char *args) {
    if (*args != '\0') {
        console_write("rejected: ready takes no arguments; example: ready\r\n");
        return;
    }
    bool ready;
    sdc41_result_t result = sdc41_get_ready(&ready);
    if (result != SDC41_OK) print_sensor_error("could not read data-ready status", result);
    else console_printf("data ready: %s\r\n", ready ? "yes" : "no");
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
    bool ready;
    sdc41_result_t ready_result = sdc41_get_ready(&ready);
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
    if (ready_result == SDC41_OK) console_printf(", data ready=%s\r\n", ready ? "yes" : "no");
    else console_printf(", data ready=error (%s)\r\n", sdc41_result_string(ready_result));
}

static void print_menu_value_error(sdc41_result_t result) {
    console_printf("error (%s)\r\n", sdc41_result_string(result));
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
    bool ready = false;
    console_write("checkpoint B\r\n");
    sdc41_result_t ready_result = sdc41_get_ready(&ready);
    console_write("checkpoint C\r\n");
    sdc41_result_t measurement_result = SDC41_OK;
    if (ready_result == SDC41_OK && ready) {
        measurement_result = sdc41_read_measurement(&last_measurement);
        if (measurement_result == SDC41_OK) have_measurement = true;
    }

    if (refresh_config) refresh_menu_config();

    menu_snapshot_t current = {
        .measurement_result = measurement_result,
        .have_measurement = have_measurement,
        .measurement = last_measurement,
        .config = menu_config,
        .mode = mode,
        .ready_result = ready_result,
        .ready = ready,
    };
    bool all = !changed_only || !have_last_printed_menu;
    bool co2_changed =
        all || measurement_field_changed(&current, &last_printed_menu,
                                         current.measurement.co2_ppm,
                                         last_printed_menu.measurement.co2_ppm);
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

    if (co2_changed) {
        console_write("co2 = ");
        if (measurement_result != SDC41_OK)
            print_menu_value_error(measurement_result);
        else if (!have_measurement)
            console_write("pending\r\n");
        else
            console_printf("%u ppm\r\n", last_measurement.co2_ppm);
    }
    if (temperature_changed) {
        console_write("temperature = ");
        if (measurement_result != SDC41_OK)
            print_menu_value_error(measurement_result);
        else if (!have_measurement)
            console_write("pending\r\n");
        else
            print_fixed_milli(last_measurement.temperature_milli_c, " C\r\n");
    }
    if (humidity_changed) {
        console_write("humidity = ");
        if (measurement_result != SDC41_OK)
            print_menu_value_error(measurement_result);
        else if (!have_measurement)
            console_write("pending\r\n");
        else
            print_fixed_milli((int32_t)last_measurement.humidity_milli_percent,
                              " %RH\r\n");
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
        console_write("serial = ");
        if (menu_config.serial_result == SDC41_OK)
            console_printf("%llu\r\n", (unsigned long long)serial);
        else
            print_menu_value_error(menu_config.serial_result);
    }
    if (all || result_value_changed(menu_config.asc_result,
                                    last_printed_menu.config.asc_result,
                                    menu_config.asc,
                                    last_printed_menu.config.asc)) {
        console_write("asc = ");
        if (menu_config.asc_result == SDC41_OK)
            console_printf("%s\r\n", menu_config.asc ? "on" : "off");
        else
            print_menu_value_error(menu_config.asc_result);
    }
    if (all || result_value_changed(menu_config.offset_result,
                                    last_printed_menu.config.offset_result,
                                    menu_config.offset_raw,
                                    last_printed_menu.config.offset_raw)) {
        console_write("offset = ");
        if (menu_config.offset_result == SDC41_OK) {
            int32_t offset_milli = (int32_t)(
                ((uint64_t)menu_config.offset_raw * 175000u) / 65536u);
            print_fixed_milli(offset_milli, " C\r\n");
        } else {
            print_menu_value_error(menu_config.offset_result);
        }
    }
    if (all || result_value_changed(menu_config.altitude_result,
                                    last_printed_menu.config.altitude_result,
                                    menu_config.altitude,
                                    last_printed_menu.config.altitude)) {
        console_write("altitude = ");
        if (menu_config.altitude_result == SDC41_OK)
            console_printf("%u m\r\n", menu_config.altitude);
        else
            print_menu_value_error(menu_config.altitude_result);
    }
    if (all || mode != last_printed_menu.mode)
        console_printf("mode = %s\r\n",
                       mode == MODE_PERIODIC ? "periodic" : "single");
    if (all || result_value_changed(ready_result,
                                    last_printed_menu.ready_result, ready,
                                    last_printed_menu.ready)) {
        console_write("data ready = ");
        if (ready_result == SDC41_OK)
            console_printf("%s\r\n", ready ? "yes" : "no");
        else
            print_menu_value_error(ready_result);
    }

    last_printed_menu = current;
    have_last_printed_menu = true;
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
        {"menu", "menu: print every readable parameter as key = value lines and refresh cached configuration; changed fields auto-refresh every 3 seconds while the console is idle. Selftest is deliberately excluded. Example: menu\r\n"},
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

    if (strcmp(line, "co2") == 0) command_co2(args);
    else if (strcmp(line, "ready") == 0) command_ready(args);
    else if (strcmp(line, "serial") == 0) command_serial(args);
    else if (strcmp(line, "selftest") == 0) command_selftest(args);
    else if (strcmp(line, "asc") == 0) command_asc(args);
    else if (strcmp(line, "offset") == 0) command_offset(args);
    else if (strcmp(line, "altitude") == 0) command_altitude(args);
    else if (strcmp(line, "mode") == 0) command_mode(args);
    else if (strcmp(line, "status") == 0) command_status(args);
    else if (strcmp(line, "menu") == 0) command_menu(args);
    else if (strcmp(line, "help") == 0) command_help(args);
    else console_printf("rejected: unknown command '%s'; type help for examples\r\n", line);
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
        console_write("checkpoint A\r\n");
    }
    else print_sensor_error("could not start periodic measurement", result);
    print_menu(true, false);
    console_write("console ready; type help\r\n");

    char line[LINE_CAPACITY];
    size_t length = 0;
    bool ignored_lf = false;
    absolute_time_t refresh_deadline = make_timeout_time_ms(3000);
    while (true) {
        if (uart_is_readable(CONSOLE_UART)) {
            char ch = (char)uart_getc(CONSOLE_UART);
            if (ch == '\r' || ch == '\n') {
                if (ch == '\n' && ignored_lf) {
                    ignored_lf = false;
                    refresh_deadline = make_timeout_time_ms(3000);
                    continue;
                }
                ignored_lf = ch == '\r';
                console_write("\r\n");
                line[length] = '\0';
                execute_line(line);
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
                        console_write("\r\nrejected: command is too long; maximum is 95 characters\r\n");
                        length = 0;
                    }
                }
            }
            refresh_deadline = make_timeout_time_ms(3000);
        } else if (length == 0 && time_reached(refresh_deadline)) {
            print_menu(false, true);
            refresh_deadline = make_timeout_time_ms(3000);
        } else {
            tight_loop_contents();
        }
    }
}
