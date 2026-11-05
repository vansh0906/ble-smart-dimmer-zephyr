/*
 * Power stage interface: what the firmware drives (the output duty) and what it measures.
 *
 * On a real dimmer this would be a phase-cut or PWM driver plus ADC channels for mains voltage,
 * load current and an NTC on the heatsink. Here it is implemented by a model of 120 V / 60 Hz mains,
 * an LED-driver load, a 12-bit ADC and a heatsink (power_stage_sim.c), so the same firmware can be
 * tested on the simulated nRF52.
 */
#ifndef POWER_STAGE_H
#define POWER_STAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "dimmer.h"
#include "meter.h"

void ps_init(void);
void ps_set_duty(uint16_t permille);
/* Advance the hardware by one 10 ms tick: fills the dimmer inputs and feeds ADC samples to the meter. */
void ps_tick(uint32_t now_ms, dim_inputs_t *in, meter_t *m);

/* Test hooks (fault injection), only used by the test build */
void ps_inject_extra_current(float amps);
void ps_inject_short(bool on);
void ps_inject_temperature(float celsius);
void ps_inject_mains_off(uint32_t now_ms, uint32_t ms);
void ps_clear_injections(void);
float ps_true_power_w(void);       /* model's exact power, averaged over the last 100 ms */
double ps_true_energy_wh(void);
float ps_temperature(void);

#endif
