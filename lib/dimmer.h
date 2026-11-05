/*
 * Dimmer control core: hardware-independent, so it runs both in the Zephyr firmware and in the
 * host unit tests.
 *
 * Every 10 ms tick the firmware passes in what the power stage measured (load current, heatsink
 * temperature, whether mains zero crossings were seen) and gets back the output duty to apply.
 * Commands (on, off, level, fade, reset) come from BLE writes between ticks.
 */
#ifndef DIMMER_H
#define DIMMER_H

#include <stdbool.h>
#include <stdint.h>

#define DIM_TICK_MS             10U
#define DIM_FADE_MAX_MS         10000U
#define DIM_FADE_DEFAULT_MS     300U

/* Protection limits for a 120 V, 600 W rated dimmer (5 A) */
#define DIM_SHORT_PEAK_A        21.2f    /* instantaneous peak (15 A RMS): trips in the same tick */
#define DIM_OVERLOAD_A          6.25f    /* 125 % of rated current ... */
#define DIM_OVERLOAD_MS         200U     /* ... for this long */
#define DIM_DERATE_START_C      80.0f    /* full output up to here */
#define DIM_DERATE_END_C        95.0f    /* 25 % output here */
#define DIM_DERATE_MIN_LEVEL    25.0f
#define DIM_TRIP_C              100.0f
#define DIM_RESET_BELOW_C       80.0f
#define DIM_MAINS_LOSS_MS       25U      /* three missing half cycles at 60 Hz */

typedef enum { DIM_OFF = 0, DIM_ON = 1, DIM_FAULT = 2 } dim_state_t;

/* latched faults (cleared only by a reset command once the cause is gone) */
#define DIM_F_SHORT             0x01U
#define DIM_F_OVERLOAD          0x02U
#define DIM_F_OVERTEMP          0x04U
/* status flags */
#define DIM_FLAG_MAINS_LOST     0x10U
#define DIM_FLAG_DERATED        0x20U
#define DIM_FLAG_FADING         0x40U

/* result codes for commands */
#define DIM_OK                  0
#define DIM_E_RANGE             (-1)
#define DIM_E_FAULT             (-2)     /* command refused: dimmer is in a fault */
#define DIM_E_CAUSE             (-3)     /* reset refused: heatsink still too hot */

typedef struct {
    float current_a;            /* RMS load current over the last mains cycle */
    float peak_a;               /* largest instantaneous current during the tick */
    float temp_c;               /* heatsink temperature */
    bool zero_cross;            /* at least one mains zero crossing during the tick */
} dim_inputs_t;

typedef struct {
    dim_state_t state;
    uint8_t faults;
    uint8_t flags;
    uint8_t target;             /* requested level 0..100 (0 = off) */
    uint8_t last_on;            /* level used by "on" */
    uint16_t fade_default_ms;
    float level;                /* actual perceived level 0..100, follows target over the fade */
    float fade_from;
    uint32_t fade_ms;
    uint32_t fade_t;
    uint16_t overload_ms;
    uint16_t no_zc_ms;
    uint16_t duty;              /* last output, per mille */
    dim_inputs_t last_in;
} dimmer_t;

void dim_init(dimmer_t *d, uint8_t last_on, bool on, uint16_t fade_ms);
int dim_set_level(dimmer_t *d, int level, int fade_ms);   /* fade_ms < 0: default fade */
int dim_set_fade(dimmer_t *d, int fade_ms);
int dim_on(dimmer_t *d);
int dim_off(dimmer_t *d);
int dim_toggle(dimmer_t *d);
int dim_reset(dimmer_t *d);
uint16_t dim_tick(dimmer_t *d, const dim_inputs_t *in);

uint16_t dim_gamma_permille(float level);   /* perceived level (CIE 1931 lightness) -> duty */
float dim_max_level(float temp_c);          /* thermal derating */

#endif
