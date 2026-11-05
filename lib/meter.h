/* Energy metering from sampled mains voltage and load current. Hardware-independent. */
#ifndef METER_H
#define METER_H

#include <stdint.h>

typedef struct {
    double sum_v2, sum_i2, sum_p;
    uint32_t n;
    float vrms, irms, power_w, pf;
    double energy_wh;
} meter_t;

void meter_reset(meter_t *m);
void meter_add(meter_t *m, float v, float i);
/* close a measurement window of window_s seconds: computes RMS, real power, power factor, energy */
void meter_window(meter_t *m, float window_s);

/* 12-bit ADC front end: +/-200 V and +/-25 A full scale */
int16_t meter_adc_v(float v);
int16_t meter_adc_i(float i);
float meter_counts_to_v(int16_t c);
float meter_counts_to_i(int16_t c);

#endif
