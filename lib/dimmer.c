#include "dimmer.h"

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

void dim_init(dimmer_t *d, uint8_t last_on, bool on, uint16_t fade_ms)
{
    *d = (dimmer_t){0};
    d->last_on = (last_on >= 1U && last_on <= 100U) ? last_on : 100U;
    d->fade_default_ms = fade_ms <= DIM_FADE_MAX_MS ? fade_ms : DIM_FADE_DEFAULT_MS;
    if (on) {
        d->state = DIM_ON;
        d->target = d->last_on;
        d->fade_from = 0.0f;
        d->fade_ms = d->fade_default_ms;      /* power-loss recovery fades back up */
    }
}

static void start_fade(dimmer_t *d, uint8_t target, uint32_t fade_ms)
{
    d->target = target;
    d->fade_from = d->level;
    d->fade_ms = fade_ms;
    d->fade_t = 0;
    d->state = target ? DIM_ON : DIM_OFF;
    if (target) {
        d->last_on = target;
    }
}

int dim_set_level(dimmer_t *d, int level, int fade_ms)
{
    if (level < 0 || level > 100 || fade_ms > (int)DIM_FADE_MAX_MS) {
        return DIM_E_RANGE;
    }
    if (d->state == DIM_FAULT) {
        return DIM_E_FAULT;
    }
    start_fade(d, (uint8_t)level, fade_ms < 0 ? d->fade_default_ms : (uint32_t)fade_ms);
    return DIM_OK;
}

int dim_set_fade(dimmer_t *d, int fade_ms)
{
    if (fade_ms < 0 || fade_ms > (int)DIM_FADE_MAX_MS) {
        return DIM_E_RANGE;
    }
    d->fade_default_ms = (uint16_t)fade_ms;
    return DIM_OK;
}

int dim_on(dimmer_t *d) { return dim_set_level(d, d->last_on, -1); }
int dim_off(dimmer_t *d) { return d->state == DIM_FAULT ? DIM_E_FAULT : dim_set_level(d, 0, -1); }
int dim_toggle(dimmer_t *d) { return d->state == DIM_ON ? dim_off(d) : dim_on(d); }

int dim_reset(dimmer_t *d)
{
    if (d->state != DIM_FAULT) {
        return DIM_OK;
    }
    /* Overcurrent can't be measured once the output is off, so the reset is allowed and the dimmer
     * stays off; if the overload is still there when the user turns it on, it trips again.
     * Temperature is still measurable, so an overtemperature reset waits until the heatsink cools. */
    if (d->last_in.temp_c >= DIM_RESET_BELOW_C) {
        return DIM_E_CAUSE;
    }
    /* back to a safe state: off, nothing re-energised until the user turns it on */
    d->faults = 0;
    d->state = DIM_OFF;
    d->target = 0;
    d->level = 0.0f;
    d->fade_ms = 0;
    d->overload_ms = 0;
    return DIM_OK;
}

static void trip(dimmer_t *d, uint8_t fault)
{
    d->faults |= fault;
    d->state = DIM_FAULT;
    d->target = 0;
    d->level = 0.0f;
    d->fade_ms = 0;
}

uint16_t dim_tick(dimmer_t *d, const dim_inputs_t *in)
{
    d->last_in = *in;

    /* 1. protections, checked before anything else every tick */
    if (in->peak_a > DIM_SHORT_PEAK_A) {
        trip(d, DIM_F_SHORT);
    }
    if (in->current_a > DIM_OVERLOAD_A) {
        d->overload_ms = (uint16_t)(d->overload_ms + DIM_TICK_MS);
        if (d->overload_ms >= DIM_OVERLOAD_MS) {
            trip(d, DIM_F_OVERLOAD);
        }
    } else {
        d->overload_ms = 0;
    }
    if (in->temp_c >= DIM_TRIP_C) {
        trip(d, DIM_F_OVERTEMP);
    }

    /* 2. mains supervision: ride through a brownout, resume at the same level */
    d->no_zc_ms = in->zero_cross ? 0 : (uint16_t)(d->no_zc_ms + DIM_TICK_MS);
    bool mains_lost = d->no_zc_ms >= DIM_MAINS_LOSS_MS;

    /* 3. fade (linear in perceived level) */
    if (d->fade_ms == 0 || d->fade_t >= d->fade_ms) {
        d->level = d->target;
        d->fade_ms = 0;
    } else {
        d->fade_t += DIM_TICK_MS;
        float a = d->fade_t >= d->fade_ms ? 1.0f : (float)d->fade_t / (float)d->fade_ms;
        d->level = d->fade_from + (d->target - d->fade_from) * a;
    }

    /* 4. thermal derating */
    float max = dim_max_level(in->temp_c);
    float applied = d->level > max ? max : d->level;

    d->flags = (uint8_t)((mains_lost ? DIM_FLAG_MAINS_LOST : 0U) | (d->level > max ? DIM_FLAG_DERATED : 0U) |
                         (d->fade_ms ? DIM_FLAG_FADING : 0U));
    d->duty = (d->state == DIM_FAULT || mains_lost) ? 0U : dim_gamma_permille(applied);
    return d->duty;
}

uint16_t dim_gamma_permille(float level)
{
    float l = clampf(level, 0.0f, 100.0f);
    float y = l > 8.0f ? ((l + 16.0f) / 116.0f) * ((l + 16.0f) / 116.0f) * ((l + 16.0f) / 116.0f) : l / 903.3f;
    return (uint16_t)(y * 1000.0f + 0.5f);
}

float dim_max_level(float temp_c)
{
    if (temp_c <= DIM_DERATE_START_C) {
        return 100.0f;
    }
    if (temp_c >= DIM_DERATE_END_C) {
        return DIM_DERATE_MIN_LEVEL;
    }
    float a = (temp_c - DIM_DERATE_START_C) / (DIM_DERATE_END_C - DIM_DERATE_START_C);
    return 100.0f - a * (100.0f - DIM_DERATE_MIN_LEVEL);
}
