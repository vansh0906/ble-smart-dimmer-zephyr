/*
 * BLE peripheral: advertising and the Dimmer GATT service.
 *
 *   Level   (read, write*, notify not used)  uint8 0..100, 0 = off, uses the default fade
 *   Fade    (read, write*)                   uint16 ms, 0..10000
 *   Command (write*)                         1 on, 2 off, 3 toggle, 4 reset fault
 *   Status  (read, notify)                   struct dim_status
 *   Meter   (read, notify every 1 s)         struct dim_meter
 *   Test    (write*, read; test builds only) fault injection, true power from the load model
 *   (* = needs an encrypted link: LE Secure Connections pairing)
 *
 * Writes are validated here and answered with an ATT error the phone can show:
 *   0x13 value not allowed, 0x0D bad length, 0x80 dimmer in fault, 0x81 fault cause still present.
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "app.h"

#define DIM_UUID(n) BT_UUID_128_ENCODE(0x6d0c0000 + (n), 0x4c5a, 0x4d1e, 0x9a3b, 0x1f2e3d4c5b6a)
static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(DIM_UUID(1));
static const struct bt_uuid_128 level_uuid = BT_UUID_INIT_128(DIM_UUID(2));
static const struct bt_uuid_128 fade_uuid = BT_UUID_INIT_128(DIM_UUID(3));
static const struct bt_uuid_128 cmd_uuid = BT_UUID_INIT_128(DIM_UUID(4));
static const struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(DIM_UUID(5));
static const struct bt_uuid_128 meter_uuid = BT_UUID_INIT_128(DIM_UUID(6));
#if defined(CONFIG_APP_TEST_HOOKS)
static const struct bt_uuid_128 test_uuid = BT_UUID_INIT_128(DIM_UUID(0xF0));
#endif

#define ERR_FAULT 0x80
#define ERR_CAUSE 0x81

static ssize_t map_err(int r)
{
    switch (r) {
    case DIM_E_RANGE: return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    case DIM_E_FAULT: return BT_GATT_ERR(ERR_FAULT);
    case DIM_E_CAUSE: return BT_GATT_ERR(ERR_CAUSE);
    default: return 0;
    }
}

static ssize_t read_level(struct bt_conn *c, const struct bt_gatt_attr *a, void *buf, uint16_t len, uint16_t off)
{
    uint8_t v = dim.target;
    return bt_gatt_attr_read(c, a, buf, len, off, &v, 1);
}

static ssize_t write_level(struct bt_conn *c, const struct bt_gatt_attr *a, const void *buf, uint16_t len,
                           uint16_t off, uint8_t flags)
{
    ARG_UNUSED(c); ARG_UNUSED(a); ARG_UNUSED(flags);
    if (off || len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    k_mutex_lock(&app_lock, K_FOREVER);
    int r = dim_set_level(&dim, ((const uint8_t *)buf)[0], -1);
    k_mutex_unlock(&app_lock);
    if (r) {
        return map_err(r);
    }
    app_changed();
    return len;
}

static ssize_t read_fade(struct bt_conn *c, const struct bt_gatt_attr *a, void *buf, uint16_t len, uint16_t off)
{
    uint16_t v = sys_cpu_to_le16(dim.fade_default_ms);
    return bt_gatt_attr_read(c, a, buf, len, off, &v, 2);
}

static ssize_t write_fade(struct bt_conn *c, const struct bt_gatt_attr *a, const void *buf, uint16_t len,
                          uint16_t off, uint8_t flags)
{
    ARG_UNUSED(c); ARG_UNUSED(a); ARG_UNUSED(flags);
    if (off || len != 2) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    k_mutex_lock(&app_lock, K_FOREVER);
    int r = dim_set_fade(&dim, sys_get_le16(buf));
    k_mutex_unlock(&app_lock);
    if (r) {
        return map_err(r);
    }
    app_changed();
    return len;
}

static ssize_t write_cmd(struct bt_conn *c, const struct bt_gatt_attr *a, const void *buf, uint16_t len,
                         uint16_t off, uint8_t flags)
{
    ARG_UNUSED(c); ARG_UNUSED(a); ARG_UNUSED(flags);
    if (off || len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    int r;
    k_mutex_lock(&app_lock, K_FOREVER);
    switch (((const uint8_t *)buf)[0]) {
    case 1: r = dim_on(&dim); break;
    case 2: r = dim_off(&dim); break;
    case 3: r = dim_toggle(&dim); break;
    case 4: r = dim_reset(&dim); break;
    default: r = DIM_E_RANGE; break;
    }
    k_mutex_unlock(&app_lock);
    if (r) {
        return map_err(r);
    }
    app_changed();
    return len;
}

static ssize_t read_status(struct bt_conn *c, const struct bt_gatt_attr *a, void *buf, uint16_t len, uint16_t off)
{
    struct dim_status s;
    k_mutex_lock(&app_lock, K_FOREVER);
    app_fill_status(&s);
    k_mutex_unlock(&app_lock);
    return bt_gatt_attr_read(c, a, buf, len, off, &s, sizeof s);
}

static ssize_t read_meter(struct bt_conn *c, const struct bt_gatt_attr *a, void *buf, uint16_t len, uint16_t off)
{
    struct dim_meter m;
    k_mutex_lock(&app_lock, K_FOREVER);
    app_fill_meter(&m);
    k_mutex_unlock(&app_lock);
    return bt_gatt_attr_read(c, a, buf, len, off, &m, sizeof m);
}

#if defined(CONFIG_APP_TEST_HOOKS)
static ssize_t write_test(struct bt_conn *c, const struct bt_gatt_attr *a, const void *buf, uint16_t len,
                          uint16_t off, uint8_t flags)
{
    ARG_UNUSED(c); ARG_UNUSED(a); ARG_UNUSED(flags);
    if (off || len != 5) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    const uint8_t *b = buf;
    k_mutex_lock(&app_lock, K_FOREVER);
    app_inject(b[0], (int32_t)sys_get_le32(b + 1));
    k_mutex_unlock(&app_lock);
    return len;
}

static ssize_t read_test(struct bt_conn *c, const struct bt_gatt_attr *a, void *buf, uint16_t len, uint16_t off)
{
    uint8_t v[6];
    uint16_t p;
    uint32_t e;
    k_mutex_lock(&app_lock, K_FOREVER);
    app_true_meter(&p, &e);
    k_mutex_unlock(&app_lock);
    sys_put_le16(p, v);
    sys_put_le32(e, v + 2);
    return bt_gatt_attr_read(c, a, buf, len, off, v, sizeof v);
}
#endif

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    printk("CCC t=%u value=%u\n", k_uptime_get_32(), value);
}

BT_GATT_SERVICE_DEFINE(dim_svc,
    BT_GATT_PRIMARY_SERVICE(&svc_uuid),
    BT_GATT_CHARACTERISTIC(&level_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT, read_level, write_level, NULL),
    BT_GATT_CHARACTERISTIC(&fade_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT, read_fade, write_fade, NULL),
    BT_GATT_CHARACTERISTIC(&cmd_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_cmd, NULL),
    BT_GATT_CHARACTERISTIC(&status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                           read_status, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&meter_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                           read_meter, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
#if defined(CONFIG_APP_TEST_HOOKS)
    BT_GATT_CHARACTERISTIC(&test_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT, read_test, write_test, NULL),
#endif
);

#define ATTR_STATUS (&dim_svc.attrs[8])
#define ATTR_METER (&dim_svc.attrs[11])

void ble_notify_status(void)
{
    struct dim_status s;
    k_mutex_lock(&app_lock, K_FOREVER);
    app_fill_status(&s);
    k_mutex_unlock(&app_lock);
    (void)bt_gatt_notify(NULL, ATTR_STATUS, &s, sizeof s);
}

void ble_notify_meter(void)
{
    struct dim_meter m;
    k_mutex_lock(&app_lock, K_FOREVER);
    app_fill_meter(&m);
    k_mutex_unlock(&app_lock);
    (void)bt_gatt_notify(NULL, ATTR_METER, &m, sizeof m);
}

/* ------------------------------------------------------------------ advertising + connections */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, DIM_UUID(1)),
};
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void adv_work_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    printk("ADV t=%u start err=%d\n", k_uptime_get_32(), err);
}
static K_WORK_DEFINE(adv_work, adv_work_fn);

static void connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);
    printk("CONNECTED t=%u err=0x%02x\n", k_uptime_get_32(), err);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    printk("DISCONNECTED t=%u reason=0x%02x\n", k_uptime_get_32(), reason);
}

static void recycled(void)
{
    k_work_submit(&adv_work);                 /* advertise again so the phone can come back */
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    ARG_UNUSED(conn);
    printk("SECURITY t=%u level=%d err=%d\n", k_uptime_get_32(), level, err);
}

BT_CONN_CB_DEFINE(conn_cbs) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
    .security_changed = security_changed,
};

int ble_init(void)
{
    return bt_enable(NULL);
}

void ble_advertise(void)
{
    k_work_submit(&adv_work);
}
