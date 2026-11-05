/* Host unit tests for the dimmer core and the energy meter (no Zephyr needed). */
#include <math.h>
#include <stdio.h>
#include "dimmer.h"
#include "meter.h"

static int run, failed;
#define CHECK(cond, name) do { run++; if (!(cond)) { failed++; printf("FAIL %s (line %d)\n", name, __LINE__); } \
                               else { printf("PASS %s\n", name); } } while (0)

static const dim_inputs_t NORMAL = {.current_a = 2.0f, .peak_a = 2.9f, .temp_c = 30.0f, .zero_cross = true};

static void ticks(dimmer_t *d, const dim_inputs_t *in, int n)
{
    for (int i = 0; i < n; i++) {
        dim_tick(d, in);
    }
}

int main(void)
{
    dimmer_t d;

    /* gamma curve */
    int mono = 1;
    for (int l = 1; l <= 100; l++) {
        mono &= dim_gamma_permille((float)l) >= dim_gamma_permille((float)(l - 1));
    }
    CHECK(dim_gamma_permille(0) == 0 && dim_gamma_permille(100) == 1000 && mono, "gamma 0..1000, monotonic");
    CHECK(dim_gamma_permille(50) > 150 && dim_gamma_permille(50) < 220, "gamma: 50 % perceived is ~18 % duty");

    /* commands and ranges */
    dim_init(&d, 0, false, 300);
    CHECK(d.state == DIM_OFF && d.last_on == 100, "init: off, last-on defaults to 100");
    CHECK(dim_set_level(&d, 101, 0) == DIM_E_RANGE && dim_set_level(&d, -1, 0) == DIM_E_RANGE &&
          dim_set_fade(&d, 10001) == DIM_E_RANGE && d.target == 0, "out-of-range level/fade rejected");
    dim_set_level(&d, 60, 0);
    ticks(&d, &NORMAL, 1);
    CHECK(d.state == DIM_ON && d.level == 60.0f && d.duty == dim_gamma_permille(60), "level 60 with no fade");
    dim_off(&d);
    ticks(&d, &NORMAL, 40);
    dim_on(&d);
    ticks(&d, &NORMAL, 40);
    CHECK(d.target == 60 && d.state == DIM_ON, "on restores the last level");
    dim_toggle(&d);
    CHECK(d.state == DIM_OFF && d.target == 0, "toggle from on goes off");

    /* fade timing */
    dim_init(&d, 100, false, 300);
    dim_set_level(&d, 100, 1000);
    ticks(&d, &NORMAL, 50);
    float half = d.level;
    ticks(&d, &NORMAL, 49);
    int fading = d.flags & DIM_FLAG_FADING;
    ticks(&d, &NORMAL, 2);
    CHECK(fabsf(half - 50.0f) < 1.0f && fading && d.level == 100.0f && !(d.flags & DIM_FLAG_FADING),
          "1000 ms fade: 50 % at 500 ms, done at 1000 ms");

    /* overload: 6.5 A for 190 ms does not trip, 200 ms does */
    dim_init(&d, 100, false, 0);
    dim_set_level(&d, 100, 0);
    dim_inputs_t ov = NORMAL;
    ov.current_a = 6.5f;
    ticks(&d, &ov, 19);
    int not_yet = d.state == DIM_ON;
    ticks(&d, &ov, 1);
    CHECK(not_yet && d.state == DIM_FAULT && (d.faults & DIM_F_OVERLOAD) && d.duty == 0, "overload trips at 200 ms");
    dim_init(&d, 100, false, 0);
    dim_set_level(&d, 100, 0);
    for (int i = 0; i < 100; i++) {
        dim_tick(&d, i % 15 == 14 ? &NORMAL : &ov);         /* dips below the limit restart the timer */
    }
    CHECK(d.state == DIM_ON, "overload timer restarts when current drops");

    /* short: instantaneous peak trips in the same tick */
    dim_init(&d, 100, false, 0);
    dim_set_level(&d, 100, 0);
    dim_inputs_t sh = NORMAL;
    sh.peak_a = 28.0f;
    uint16_t duty = dim_tick(&d, &sh);
    CHECK(duty == 0 && d.state == DIM_FAULT && (d.faults & DIM_F_SHORT), "short trips in the same tick");
    CHECK(dim_set_level(&d, 50, 0) == DIM_E_FAULT && dim_on(&d) == DIM_E_FAULT, "commands refused while in fault");
    CHECK(dim_reset(&d) == DIM_OK && d.state == DIM_OFF && d.faults == 0 && d.target == 0,
          "reset after a short: off, nothing re-energised");

    /* thermal derating and trip */
    CHECK(dim_max_level(80.0f) == 100.0f && fabsf(dim_max_level(87.5f) - 62.5f) < 0.01f &&
          dim_max_level(95.0f) == 25.0f, "derating: 100 % at 80 C, 62.5 % at 87.5 C, 25 % at 95 C");
    dim_init(&d, 100, false, 0);
    dim_set_level(&d, 100, 0);
    dim_inputs_t hot = NORMAL;
    hot.temp_c = 90.0f;
    ticks(&d, &hot, 1);
    CHECK((d.flags & DIM_FLAG_DERATED) && d.duty == dim_gamma_permille(dim_max_level(90.0f)), "derated output at 90 C");
    hot.temp_c = 100.0f;
    ticks(&d, &hot, 1);
    hot.temp_c = 85.0f;
    ticks(&d, &hot, 1);
    int refused = dim_reset(&d) == DIM_E_CAUSE;
    hot.temp_c = 75.0f;
    ticks(&d, &hot, 1);
    CHECK((d.faults & DIM_F_OVERTEMP) && refused && dim_reset(&d) == DIM_OK, "overtemp trips at 100 C, reset only below 80 C");

    /* mains loss: ride through, resume at the same level */
    dim_init(&d, 100, false, 0);
    dim_set_level(&d, 70, 0);
    ticks(&d, &NORMAL, 5);
    dim_inputs_t nomains = NORMAL;
    nomains.zero_cross = false;
    ticks(&d, &nomains, 2);
    int early = !(d.flags & DIM_FLAG_MAINS_LOST);
    ticks(&d, &nomains, 1);
    int lost = (d.flags & DIM_FLAG_MAINS_LOST) && d.duty == 0;
    ticks(&d, &NORMAL, 1);
    CHECK(early && lost && d.duty == dim_gamma_permille(70) && d.state == DIM_ON && d.faults == 0,
          "mains loss: off after 25 ms, back at the same level, no fault");

    /* power-loss recovery: restore on + level, fade back up */
    dim_init(&d, 35, true, 800);
    ticks(&d, &NORMAL, 40);
    float mid = d.level;
    ticks(&d, &NORMAL, 41);
    CHECK(d.state == DIM_ON && mid > 10.0f && mid < 25.0f && d.level == 35.0f, "restore after power cycle fades to 35 %");

    /* meter: 120 V, 4 A, PF 0.95 -> 456 W */
    meter_t m = {0};
    float phi = acosf(0.95f);
    for (int k = 0; k < 600; k++) {
        float th = 6.2831853f * 60.0f * (float)k / 6000.0f;
        meter_add(&m, 169.7056f * sinf(th), 5.656854f * sinf(th - phi));
    }
    meter_window(&m, 0.1f);
    CHECK(fabsf(m.power_w - 456.0f) < 0.5f && fabsf(m.pf - 0.95f) < 0.002f && fabsf(m.vrms - 120.0f) < 0.05f,
          "meter: 456 W, PF 0.95, 120 V RMS");
    CHECK(fabs(m.energy_wh - 456.0 * 0.1 / 3600.0) < 1e-4, "meter: energy integrates power over the window");
    float worst = 0.0f;
    for (float v = -199.0f; v <= 199.0f; v += 0.37f) {
        float e = fabsf(meter_counts_to_v(meter_adc_v(v)) - v);
        worst = e > worst ? e : worst;
    }
    CHECK(worst < 0.06f, "12-bit ADC: voltage error under half an LSB");

    printf("%d/%d unit tests pass\n", run - failed, run);
    return failed ? 1 : 0;
}
