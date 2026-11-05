/*
 * Power stage model: 120 V RMS / 60 Hz mains, an LED-driver load (4 A RMS at full output, power
 * factor 0.95), a 12-bit ADC sampling at 6 kHz with 0.2 % current-gain error and +/-1 LSB noise,
 * and a first-order heatsink thermal model. Runs inside the firmware image in simulated time.
 */
#include <math.h>
#include "power_stage.h"

#define FS_HZ           6000U
#define SAMPLES_TICK    (FS_HZ * DIM_TICK_MS / 1000U)   /* 60 */
#define CYCLE_SAMPLES   (FS_HZ / 60U)                    /* 100 = one mains cycle */
#define V_RMS           120.0f
#define I_FULL_RMS      4.0f
#define PF              0.95f
#define TWO_PI          6.28318530718f

static uint16_t duty;
static uint64_t n_sample;
static float extra_a, temp_c = 25.0f, ambient_c = 25.0f;
static bool short_on;
static uint32_t mains_off_until;
static float i_ring[CYCLE_SAMPLES];
static uint32_t ring_pos;
static double true_p_acc, true_energy_wh;
static uint32_t true_p_n;
static float true_p_last;
static uint32_t lcg = 12345;

static float noise_lsb(void)
{
    lcg = lcg * 1103515245U + 12345U;
    return (float)((int)((lcg >> 16) % 3U) - 1);        /* -1, 0 or +1 LSB */
}

void ps_init(void) { duty = 0; }
void ps_set_duty(uint16_t permille) { duty = permille > 1000U ? 1000U : permille; }

void ps_tick(uint32_t now_ms, dim_inputs_t *in, meter_t *m)
{
    const float phi = acosf(PF);
    const float w = TWO_PI * 60.0f / FS_HZ;
    bool mains = now_ms >= mains_off_until;
    /* load and fault currents only flow while the output conducts */
    float conduct = (float)duty / 1000.0f;
    float i_rms_load = (short_on && duty) ? 20.0f : I_FULL_RMS * conduct;
    float extra = extra_a * conduct;
    float prev_v = 0.0f;
    bool zc = false;
    double p_tick = 0.0;
    float peak = 0.0f;

    for (uint32_t k = 0; k < SAMPLES_TICK; k++, n_sample++) {
        float th = w * (float)(n_sample % CYCLE_SAMPLES);
        float v = mains ? V_RMS * 1.41421356f * sinf(th) : 0.0f;
        float i = mains ? 1.41421356f * (i_rms_load * sinf(th - phi) + extra * sinf(th)) : 0.0f;
        if (k && ((prev_v < 0.0f && v >= 0.0f) || (prev_v > 0.0f && v <= 0.0f)) && mains) {
            zc = true;
        }
        prev_v = v;
        p_tick += (double)v * (double)i;
        /* what the ADC sees */
        int16_t cv = meter_adc_v(v), ci = meter_adc_i(i * 1.002f);
        float mv = meter_counts_to_v((int16_t)(cv + noise_lsb()));
        float mi = meter_counts_to_i((int16_t)(ci + noise_lsb()));
        meter_add(m, mv, mi);
        i_ring[ring_pos] = mi;
        peak = fabsf(mi) > peak ? fabsf(mi) : peak;
        ring_pos = (ring_pos + 1U) % CYCLE_SAMPLES;
    }
    p_tick /= SAMPLES_TICK;
    true_p_acc += p_tick;
    true_p_n++;
    true_energy_wh += p_tick > 0.0 ? p_tick * DIM_TICK_MS / 3.6e6 : 0.0;
    if (true_p_n == 10U) {
        true_p_last = (float)(true_p_acc / true_p_n);
        true_p_acc = 0.0;
        true_p_n = 0;
    }

    /* heatsink: +20 C over ambient at full load, 20 s time constant */
    float p = p_tick > 0.0 ? (float)p_tick : 0.0f;
    float t_target = ambient_c + p * (20.0f / (V_RMS * I_FULL_RMS * PF));
    temp_c += (t_target - temp_c) * ((float)DIM_TICK_MS / 20000.0f);

    double s = 0.0;                                       /* RMS current over the last mains cycle */
    for (uint32_t k = 0; k < CYCLE_SAMPLES; k++) {
        s += (double)i_ring[k] * (double)i_ring[k];
    }
    in->current_a = (float)sqrt(s / CYCLE_SAMPLES);
    in->peak_a = peak;
    in->temp_c = temp_c;
    in->zero_cross = zc;
}

void ps_inject_extra_current(float amps) { extra_a = amps; }
void ps_inject_short(bool on) { short_on = on; }
void ps_inject_temperature(float c) { temp_c = c; ambient_c = c; }
void ps_inject_mains_off(uint32_t now_ms, uint32_t ms) { mains_off_until = now_ms + ms; }
void ps_clear_injections(void)
{
    extra_a = 0.0f;
    short_on = false;
    ambient_c = 25.0f;
    temp_c = 25.0f;
    mains_off_until = 0;
}
float ps_true_power_w(void) { return true_p_last; }
double ps_true_energy_wh(void) { return true_energy_wh; }
float ps_temperature(void) { return temp_c; }
