#ifndef SDC41_H
#define SDC41_H

#include <stdbool.h>
#include <stdint.h>

#define SDC41_ADDRESS 0x62u

typedef enum {
    SDC41_OK = 0,
    SDC41_ERR_WRITE,
    SDC41_ERR_READ,
    SDC41_ERR_CRC,
    SDC41_ERR_VALUE
} sdc41_result_t;

typedef struct {
    uint16_t co2_ppm;
    int32_t temperature_milli_c;
    uint32_t humidity_milli_percent;
} sdc41_measurement_t;

void sdc41_init(void);
const char *sdc41_result_string(sdc41_result_t result);
sdc41_result_t sdc41_start_periodic(void);
sdc41_result_t sdc41_stop_periodic(void);
sdc41_result_t sdc41_power_down(void);
void sdc41_wake_up(void);
sdc41_result_t sdc41_measure_single_shot(void);
sdc41_result_t sdc41_read_measurement(sdc41_measurement_t *measurement);
sdc41_result_t sdc41_get_ready(bool *ready);
sdc41_result_t sdc41_get_serial(uint16_t words[3]);
sdc41_result_t sdc41_self_test(uint16_t *result);
sdc41_result_t sdc41_get_asc(bool *enabled);
sdc41_result_t sdc41_set_asc(bool enabled);
sdc41_result_t sdc41_get_offset_raw(uint16_t *raw);
sdc41_result_t sdc41_set_offset_raw(uint16_t raw);
sdc41_result_t sdc41_get_altitude(uint16_t *metres);
sdc41_result_t sdc41_set_altitude(uint16_t metres);

#endif
