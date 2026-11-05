#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include "dimmer.h"
#include "meter.h"

/* Shared between the control loop (10 ms timer) and BLE callbacks; guarded by app_lock. */
extern struct k_mutex app_lock;
extern dimmer_t dim;
extern meter_t meter;

struct __packed dim_status {
    uint8_t state;
    uint8_t target;
    uint8_t level;          /* actual perceived level, rounded */
    uint8_t faults;
    uint8_t flags;
    int8_t temp_c;
    uint16_t fade_ms;       /* default fade */
};

struct __packed dim_meter {
    uint16_t power_dw;      /* 0.1 W */
    uint32_t energy_mwh;
    uint16_t vrms_dv;       /* 0.1 V */
    uint16_t irms_ma;
    uint8_t pf_pct;
};

void app_fill_status(struct dim_status *s);
void app_fill_meter(struct dim_meter *m);
void app_changed(void);                       /* status changed: notify + schedule a settings save */
void app_inject(uint8_t op, int32_t arg);     /* test hooks */
void app_true_meter(uint16_t *power_dw, uint32_t *energy_mwh);

/* ble_svc.c */
int ble_init(void);
void ble_advertise(void);
void ble_notify_status(void);
void ble_notify_meter(void);

#endif
