/*
 * BLE smart dimmer firmware (Zephyr RTOS).
 *
 * Event-driven: a 10 ms kernel timer releases the control thread (measure, protect, fade, output);
 * BLE writes act on the dimmer directly in the Bluetooth RX context and report errors synchronously;
 * notifications and flash saves run as work items. Settings (last level, on/off, fade time) are kept
 * in flash with the settings subsystem (NVS), so the dimmer comes back the same after a power cut.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#include "app.h"
#include "power_stage.h"

K_MUTEX_DEFINE(app_lock);
dimmer_t dim;
meter_t meter;

static K_SEM_DEFINE(tick_sem, 0, 1);
static uint8_t saved_level = 100, saved_on, restored;
static uint16_t saved_fade = DIM_FADE_DEFAULT_MS;
static struct dim_status last_status;
static uint32_t inject_ms;
static uint8_t inject_kind;

/* ------------------------------------------------------------------ settings */
static int dim_settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    if (!strcmp(key, "lvl") && len == 1) {
        restored = 1;
        return read_cb(cb_arg, &saved_level, 1) == 1 ? 0 : -EINVAL;
    }
    if (!strcmp(key, "on") && len == 1) {
        return read_cb(cb_arg, &saved_on, 1) == 1 ? 0 : -EINVAL;
    }
    if (!strcmp(key, "fade") && len == 2) {
        return read_cb(cb_arg, &saved_fade, 2) == 2 ? 0 : -EINVAL;
    }
    return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(dim, "dim", NULL, dim_settings_set, NULL, NULL);

static void save_work_fn(struct k_work *w)
{
    uint8_t lvl, on;
    uint16_t fade;
    ARG_UNUSED(w);
    k_mutex_lock(&app_lock, K_FOREVER);
    lvl = dim.last_on;
    on = dim.state == DIM_ON;
    fade = dim.fade_default_ms;
    k_mutex_unlock(&app_lock);
    if (lvl != saved_level || on != saved_on || fade != saved_fade) {
        settings_save_one("dim/lvl", &lvl, 1);
        settings_save_one("dim/on", &on, 1);
        settings_save_one("dim/fade", &fade, 2);
        saved_level = lvl;
        saved_on = on;
        saved_fade = fade;
        printk("SAVE t=%u lvl=%u on=%u fade=%u\n", k_uptime_get_32(), lvl, on, fade);
    }
}
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_fn);

/* ------------------------------------------------------------------ status */
void app_fill_status(struct dim_status *s)
{
    s->state = (uint8_t)dim.state;
    s->target = dim.target;
    s->level = (uint8_t)(dim.level + 0.5f);
    s->faults = dim.faults;
    s->flags = dim.flags;
    s->temp_c = (int8_t)dim.last_in.temp_c;
    s->fade_ms = dim.fade_default_ms;
}

void app_fill_meter(struct dim_meter *m)
{
    m->power_dw = (uint16_t)(meter.power_w > 0.0f ? meter.power_w * 10.0f + 0.5f : 0.0f);
    m->energy_mwh = (uint32_t)(meter.energy_wh * 1000.0);
    m->vrms_dv = (uint16_t)(meter.vrms * 10.0f + 0.5f);
    m->irms_ma = (uint16_t)(meter.irms * 1000.0f + 0.5f);
    m->pf_pct = (uint8_t)(meter.pf * 100.0f + 0.5f);
}

void app_changed(void)
{
    k_work_reschedule(&save_work, K_MSEC(500));        /* debounce flash writes */
}

/* ------------------------------------------------------------------ test hooks */
void app_inject(uint8_t op, int32_t arg)
{
    uint32_t now = k_uptime_get_32();
    switch (op) {
    case 1: ps_inject_extra_current((float)arg / 10.0f); break;
    case 2: ps_inject_short(arg != 0); break;
    case 3: ps_inject_temperature((float)arg); break;
    case 4: ps_inject_mains_off(now, (uint32_t)arg); break;
    default: ps_clear_injections(); break;
    }
    inject_ms = now;
    inject_kind = op;
    printk("INJECT t=%u op=%u arg=%d\n", now, op, arg);
}

void app_true_meter(uint16_t *power_dw, uint32_t *energy_mwh)
{
    float p = ps_true_power_w();
    *power_dw = (uint16_t)(p > 0.0f ? p * 10.0f + 0.5f : 0.0f);
    *energy_mwh = (uint32_t)(ps_true_energy_wh() * 1000.0);
}

/* ------------------------------------------------------------------ control loop */
static void tick_timer_fn(struct k_timer *t)
{
    ARG_UNUSED(t);
    k_sem_give(&tick_sem);
}
static K_TIMER_DEFINE(tick_timer, tick_timer_fn, NULL);

int main(void)
{
    dim_inputs_t in;
    uint32_t ticks = 0;
    uint8_t prev_faults = 0, prev_flags = 0;

    if (ble_init() != 0) {                    /* Bluetooth first, then settings (Zephyr's required order) */
        printk("BLE init failed\n");
        return 0;
    }
    settings_subsys_init();
    settings_load();
    dim_init(&dim, saved_level, saved_on, saved_fade);
    printk("BOOT restored=%u lvl=%u on=%u fade=%u\n", restored, saved_level, saved_on, saved_fade);
    ps_init();
    ble_advertise();
    k_timer_start(&tick_timer, K_MSEC(DIM_TICK_MS), K_MSEC(DIM_TICK_MS));

    for (;;) {
        k_sem_take(&tick_sem, K_FOREVER);
        uint32_t now = k_uptime_get_32();
        struct dim_status st;

        k_mutex_lock(&app_lock, K_FOREVER);
        ps_tick(now, &in, &meter);
        uint16_t duty = dim_tick(&dim, &in);
        ps_set_duty(duty);
        ticks++;
        if (ticks % 10U == 0U) {
            meter_window(&meter, 0.1f);
        }
        if (dim.faults != prev_faults && dim.faults) {
            printk("TRIP t=%u faults=0x%02x after=%u ms (I=%d mA, T=%d C)\n", now, dim.faults, now - inject_ms,
                   (int)(in.current_a * 1000.0f), (int)in.temp_c);
        }
        if ((dim.flags ^ prev_flags) & DIM_FLAG_MAINS_LOST) {
            printk("MAINS t=%u %s after=%u ms\n", now, (dim.flags & DIM_FLAG_MAINS_LOST) ? "lost" : "back",
                   now - inject_ms);
        }
        prev_faults = dim.faults;
        prev_flags = dim.flags;
        app_fill_status(&st);
        k_mutex_unlock(&app_lock);

        bool changed = memcmp(&st, &last_status, sizeof st) != 0;
        bool fading = st.flags & DIM_FLAG_FADING;
        if (changed && (!fading || ticks % 5U == 0U)) {      /* during a fade: every 50 ms */
            last_status = st;
            ble_notify_status();
        }
        if (ticks % 10U == 0U) {
            printk("LOG t=%u st=%u tgt=%u lvl=%u duty=%u f=0x%02x fl=0x%02x I=%d T=%d P=%d\n", now, st.state,
                   st.target, st.level, duty, st.faults, st.flags, (int)(in.current_a * 1000.0f), (int)in.temp_c,
                   (int)(meter.power_w * 10.0f));
        }
        if (ticks % 100U == 0U) {
            ble_notify_meter();
        }
    }
    return 0;
}
