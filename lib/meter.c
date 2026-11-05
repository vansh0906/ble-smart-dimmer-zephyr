#include <math.h>
#include "meter.h"

#define V_FS 200.0f
#define I_FS 25.0f
#define ADC_MAX 2047

void meter_reset(meter_t *m)
{
    m->sum_v2 = m->sum_i2 = m->sum_p = 0.0;
    m->n = 0;
}

void meter_add(meter_t *m, float v, float i)
{
    m->sum_v2 += (double)v * (double)v;
    m->sum_i2 += (double)i * (double)i;
    m->sum_p += (double)v * (double)i;
    m->n++;
}

void meter_window(meter_t *m, float window_s)
{
    if (m->n == 0) {
        m->vrms = m->irms = m->power_w = m->pf = 0.0f;
        return;
    }
    m->vrms = (float)sqrt(m->sum_v2 / m->n);
    m->irms = (float)sqrt(m->sum_i2 / m->n);
    m->power_w = (float)(m->sum_p / m->n);
    float va = m->vrms * m->irms;
    m->pf = va > 1e-3f ? m->power_w / va : 0.0f;
    if (m->power_w > 0.0f) {
        m->energy_wh += (double)m->power_w * (double)window_s / 3600.0;
    }
    meter_reset(m);
}

static int16_t quant(float x, float fs)
{
    float c = x / fs * ADC_MAX;
    c = c > ADC_MAX ? ADC_MAX : (c < -ADC_MAX - 1 ? -ADC_MAX - 1 : c);
    return (int16_t)lrintf(c);
}

int16_t meter_adc_v(float v) { return quant(v, V_FS); }
int16_t meter_adc_i(float i) { return quant(i, I_FS); }
float meter_counts_to_v(int16_t c) { return (float)c * V_FS / ADC_MAX; }
float meter_counts_to_i(int16_t c) { return (float)c * I_FS / ADC_MAX; }
