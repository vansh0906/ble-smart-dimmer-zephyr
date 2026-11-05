/*
 * Test central ("the phone app"): a second simulated nRF52 that finds the dimmer over BLE, pairs,
 * and runs one scenario per -testid. Every check is made over the air through the GATT service.
 * Results are printed as "METRIC name=value" lines for tools/run.py.
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>

#include "babblekit/testcase.h"
#include "bs_tracing.h"
#include "bstests.h"

#define DIM_UUID(n) BT_UUID_128_ENCODE(0x6d0c0000 + (n), 0x4c5a, 0x4d1e, 0x9a3b, 0x1f2e3d4c5b6a)
static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(DIM_UUID(1));
static const struct bt_uuid_128 chr_uuid[] = {
    BT_UUID_INIT_128(DIM_UUID(2)), BT_UUID_INIT_128(DIM_UUID(3)), BT_UUID_INIT_128(DIM_UUID(4)),
    BT_UUID_INIT_128(DIM_UUID(5)), BT_UUID_INIT_128(DIM_UUID(6)), BT_UUID_INIT_128(DIM_UUID(0xF0)),
};
enum { H_LEVEL, H_FADE, H_CMD, H_STATUS, H_METER, H_TEST, H_N };
static uint16_t handle[H_N];
static int chars_found;

struct __packed dim_status { uint8_t state, target, level, faults, flags; int8_t temp_c; uint16_t fade_ms; };
struct __packed dim_meter { uint16_t power_dw; uint32_t energy_mwh; uint16_t vrms_dv, irms_ma; uint8_t pf_pct; };

#define ST_OFF 0
#define ST_ON 1
#define ST_FAULT 2
#define FL_MAINS 0x10
#define FL_DERATED 0x20
#define FL_FADING 0x40

static struct bt_conn *conn;
static K_SEM_DEFINE(sem_conn, 0, 1);
static K_SEM_DEFINE(sem_sec, 0, 1);
static K_SEM_DEFINE(sem_disc, 0, 1);
static K_SEM_DEFINE(sem_op, 0, 1);
static volatile int op_err;
static uint8_t read_buf[32];
static volatile int read_len;
static volatile int sec_level;

static volatile struct dim_status cur;
static volatile uint32_t status_count;
#define MAX_LOG 600
static struct { uint32_t t; struct dim_status s; } slog[MAX_LOG];
static volatile int slog_n;
static volatile struct dim_meter cur_meter;

#define METRIC(fmt, ...) printk("METRIC " fmt "\n", ##__VA_ARGS__)

/* ------------------------------------------------------------------ connection */
static bool ad_has_uuid(struct bt_data *d, void *user)
{
    bool *found = user;
    if (d->type == BT_DATA_UUID128_ALL && d->data_len == 16 && !memcmp(d->data, svc_uuid.val, 16)) {
        *found = true;
        return false;
    }
    return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
    bool found = false;
    ARG_UNUSED(rssi);
    if (conn || (type != BT_GAP_ADV_TYPE_ADV_IND)) {
        return;
    }
    bt_data_parse(ad, ad_has_uuid, &found);
    if (!found) {
        return;
    }
    bt_le_scan_stop();
    static const struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(6, 6, 0, 100);   /* 7.5 ms, 1 s timeout */
    if (bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, &p, &conn)) {
        TEST_FAIL("create connection failed");
    }
}

static void connected(struct bt_conn *c, uint8_t err)
{
    if (err) {
        TEST_FAIL("connection failed 0x%02x", err);
    }
    ARG_UNUSED(c);
    k_sem_give(&sem_conn);
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
    printk("central: disconnected 0x%02x\n", reason);
    if (c == conn) {
        bt_conn_unref(conn);
        conn = NULL;
    }
}

static void security_changed(struct bt_conn *c, bt_security_t level, enum bt_security_err err)
{
    ARG_UNUSED(c);
    sec_level = err ? -err : level;
    k_sem_give(&sem_sec);
}

BT_CONN_CB_DEFINE(cbs) = { .connected = connected, .disconnected = disconnected, .security_changed = security_changed };

/* ------------------------------------------------------------------ GATT helpers */
static uint16_t svc_start, svc_end;

static uint8_t disc_cb(struct bt_conn *c, const struct bt_gatt_attr *attr, struct bt_gatt_discover_params *p)
{
    ARG_UNUSED(c);
    if (!attr) {
        k_sem_give(&sem_disc);
        return BT_GATT_ITER_STOP;
    }
    if (p->type == BT_GATT_DISCOVER_PRIMARY) {
        struct bt_gatt_service_val *sv = attr->user_data;
        svc_start = attr->handle;
        svc_end = sv->end_handle;
        k_sem_give(&sem_disc);
        return BT_GATT_ITER_STOP;
    }
    struct bt_gatt_chrc *ch = attr->user_data;
    for (int i = 0; i < H_N; i++) {
        if (!bt_uuid_cmp(ch->uuid, &chr_uuid[i].uuid)) {
            handle[i] = ch->value_handle;
            chars_found++;
        }
    }
    return BT_GATT_ITER_CONTINUE;
}

static void discover(void)
{
    static struct bt_gatt_discover_params p;
    p.uuid = &svc_uuid.uuid;
    p.func = disc_cb;
    p.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    p.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    p.type = BT_GATT_DISCOVER_PRIMARY;
    if (bt_gatt_discover(conn, &p) || k_sem_take(&sem_disc, K_SECONDS(5))) {
        TEST_FAIL("service discovery failed");
    }
    p.uuid = NULL;
    p.start_handle = svc_start + 1;
    p.end_handle = svc_end;
    p.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    chars_found = 0;
    if (bt_gatt_discover(conn, &p) || k_sem_take(&sem_disc, K_SECONDS(5))) {
        TEST_FAIL("characteristic discovery failed");
    }
}

static void write_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_write_params *p)
{
    ARG_UNUSED(c); ARG_UNUSED(p);
    op_err = err;
    k_sem_give(&sem_op);
}

static int gwrite(int h, const void *data, uint16_t len)
{
    static struct bt_gatt_write_params p;
    p.func = write_cb;
    p.handle = handle[h];
    p.offset = 0;
    p.data = data;
    p.length = len;
    if (bt_gatt_write(conn, &p) || k_sem_take(&sem_op, K_SECONDS(3))) {
        TEST_FAIL("write to handle %u timed out", handle[h]);
    }
    return op_err;
}

static uint8_t read_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_read_params *p, const void *data, uint16_t len)
{
    ARG_UNUSED(c); ARG_UNUSED(p);
    if (data && !err) {
        read_len = len < sizeof read_buf ? len : sizeof read_buf;
        memcpy(read_buf, data, read_len);
    }
    op_err = err;
    k_sem_give(&sem_op);                      /* STOP: no further callback for this read */
    return BT_GATT_ITER_STOP;
}

static int gread(int h, void *out, int maxlen)
{
    static struct bt_gatt_read_params p;
    p.func = read_cb;
    p.handle_count = 1;
    p.single.handle = handle[h];
    p.single.offset = 0;
    read_len = 0;
    op_err = 0;
    if (bt_gatt_read(conn, &p) || k_sem_take(&sem_op, K_SECONDS(3))) {
        TEST_FAIL("read of handle %u timed out", handle[h]);
    }
    if (op_err) {
        return -op_err;
    }
    memcpy(out, read_buf, read_len < maxlen ? read_len : maxlen);
    return read_len;
}

static uint8_t notify_cb(struct bt_conn *c, struct bt_gatt_subscribe_params *p, const void *data, uint16_t len)
{
    ARG_UNUSED(c);
    if (!data) {
        return BT_GATT_ITER_STOP;
    }
    if (p->value_handle == handle[H_STATUS] && len == sizeof(struct dim_status)) {
        memcpy((void *)&cur, data, len);
        status_count++;
        if (slog_n < MAX_LOG) {
            slog[slog_n].t = k_uptime_get_32();
            memcpy(&slog[slog_n].s, data, len);
            slog_n++;
        }
    } else if (p->value_handle == handle[H_METER] && len == sizeof(struct dim_meter)) {
        memcpy((void *)&cur_meter, data, len);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void subscribe(int h)
{
    static struct bt_gatt_subscribe_params sp[2];
    struct bt_gatt_subscribe_params *p = &sp[h == H_STATUS ? 0 : 1];
    p->notify = notify_cb;
    p->value_handle = handle[h];
    p->ccc_handle = handle[h] + 1;          /* the CCC descriptor follows the value */
    p->value = BT_GATT_CCC_NOTIFY;
    if (bt_gatt_subscribe(conn, p)) {
        TEST_FAIL("subscribe failed");
    }
    k_sleep(K_MSEC(50));
}

static void connect_and_discover(void)
{
    if (bt_enable(NULL) && !bt_is_ready()) {
        TEST_FAIL("bt_enable failed");
    }
    if (bt_le_scan_start(BT_LE_SCAN_PASSIVE, device_found)) {
        TEST_FAIL("scan start failed");
    }
    if (k_sem_take(&sem_conn, K_SECONDS(10))) {
        TEST_FAIL("dimmer not found / not connected");
    }
    discover();
}

static void pair(void)
{
    if (bt_conn_set_security(conn, BT_SECURITY_L2) || k_sem_take(&sem_sec, K_SECONDS(10)) || sec_level < 2) {
        TEST_FAIL("pairing failed (%d)", sec_level);
    }
}

static void read_status(struct dim_status *s)
{
    if (gread(H_STATUS, s, sizeof *s) != (int)sizeof *s) {
        TEST_FAIL("status read failed");
    }
}

static int set_level(int lvl) { uint8_t v = (uint8_t)lvl; return gwrite(H_LEVEL, &v, 1); }
static int set_fade(int ms) { uint8_t v[2]; sys_put_le16((uint16_t)ms, v); return gwrite(H_FADE, v, 2); }
static int cmd(int c) { uint8_t v = (uint8_t)c; return gwrite(H_CMD, &v, 1); }
static int inject(int op, int32_t arg) { uint8_t v[5] = {(uint8_t)op}; sys_put_le32((uint32_t)arg, v + 1); return gwrite(H_TEST, v, 5); }

static void expect_ok(int err, const char *what)
{
    if (err) {
        TEST_FAIL("%s: ATT error 0x%02x", what, err);
    }
}

/* wait until a status notification matches */
typedef bool (*pred_t)(const struct dim_status *s);
static int wait_status(pred_t pred, uint32_t timeout_ms)
{
    uint32_t t0 = k_uptime_get_32();
    while (k_uptime_get_32() - t0 < timeout_ms) {
        struct dim_status s;
        memcpy(&s, (const void *)&cur, sizeof s);
        if (pred(&s)) {
            return (int)(k_uptime_get_32() - t0);
        }
        k_sleep(K_MSEC(2));
    }
    return -1;
}
static bool is_fault(const struct dim_status *s) { return s->state == ST_FAULT; }
static bool is_full_on(const struct dim_status *s) { return s->level == 100 && !(s->flags & FL_FADING); }
static bool is_20(const struct dim_status *s) { return s->level == 20 && !(s->flags & FL_FADING); }
static bool mains_lost(const struct dim_status *s) { return s->flags & FL_MAINS; }
static bool mains_ok(const struct dim_status *s) { return !(s->flags & FL_MAINS); }
static bool derated(const struct dim_status *s) { return s->flags & FL_DERATED; }

/* ------------------------------------------------------------------ scenarios */
static void t_basic(void)
{
    struct dim_status s;
    TEST_START("basic");
    connect_and_discover();
    METRIC("chars_found=%d", chars_found);
    if (chars_found != H_N) {
        TEST_FAIL("found %d of %d characteristics", chars_found, H_N);
    }
    read_status(&s);
    int e = set_level(50);
    METRIC("unencrypted_write_err=0x%02x", e);
    read_status(&s);
    METRIC("unencrypted_write_changed=%d", s.target != 0);
    if (!(e == BT_ATT_ERR_AUTHENTICATION || e == BT_ATT_ERR_ENCRYPTION_KEY_SIZE || e == 0x0f) || s.target) {
        TEST_FAIL("unencrypted write was not rejected (0x%02x)", e);
    }
    pair();
    METRIC("security_level=%d", sec_level);
    subscribe(H_STATUS);
    subscribe(H_METER);

    /* input validation: out-of-range values rejected, nothing changes */
    uint8_t two[2] = {50, 0};
    int v1 = set_level(101), v2 = gwrite(H_LEVEL, two, 2), v3 = set_fade(10001), v4 = cmd(9);
    read_status(&s);
    METRIC("reject_level101=0x%02x reject_badlen=0x%02x reject_fade=0x%02x reject_cmd=0x%02x unchanged=%d", v1, v2,
           v3, v4, s.target == 0 && s.fade_ms == 300);
    if (v1 != BT_ATT_ERR_VALUE_NOT_ALLOWED || v2 != BT_ATT_ERR_INVALID_ATTRIBUTE_LEN ||
        v3 != BT_ATT_ERR_VALUE_NOT_ALLOWED || v4 != BT_ATT_ERR_VALUE_NOT_ALLOWED || s.target != 0) {
        TEST_FAIL("input validation");
    }

    /* metering: measured power vs. the load model's true power at four levels */
    expect_ok(set_fade(0), "fade 0");
    int levels[] = {100, 75, 50, 25};
    float worst = 0.0f;
    for (int i = 0; i < 4; i++) {
        expect_ok(set_level(levels[i]), "level");
        k_sleep(K_MSEC(1500));
        struct dim_meter m;
        uint8_t tv[6];
        gread(H_METER, &m, sizeof m);
        gread(H_TEST, tv, sizeof tv);
        float meas = m.power_dw / 10.0f, truth = sys_get_le16(tv) / 10.0f;
        float err = truth > 0 ? 100.0f * (meas - truth) / truth : 100.0f;
        if (err < 0) {
            err = -err;
        }
        worst = err > worst ? err : worst;
        METRIC("meter_level%d_w=%d.%d true_w=%d.%d vrms=%d.%d irms_ma=%d pf=%d", levels[i], m.power_dw / 10,
               m.power_dw % 10, sys_get_le16(tv) / 10, sys_get_le16(tv) % 10, m.vrms_dv / 10, m.vrms_dv % 10,
               m.irms_ma, m.pf_pct);
    }
    METRIC("meter_worst_err_permille=%d", (int)(worst * 10.0f));
    METRIC("meter_notified_w=%d", cur_meter.power_dw / 10);

    /* on/off/toggle keep the last level */
    expect_ok(cmd(2), "off");
    k_sleep(K_MSEC(100));
    read_status(&s);
    int off_ok = s.state == ST_OFF && s.target == 0;
    expect_ok(cmd(1), "on");
    k_sleep(K_MSEC(100));
    read_status(&s);
    int on_ok = s.state == ST_ON && s.target == 25;
    expect_ok(cmd(3), "toggle");
    k_sleep(K_MSEC(100));
    read_status(&s);
    int tog_ok = s.state == ST_OFF;
    METRIC("off_ok=%d on_restores_last=%d toggle_ok=%d", off_ok, on_ok, tog_ok);
    if (!off_ok || !on_ok || !tog_ok) {
        TEST_FAIL("on/off/toggle");
    }
    TEST_PASS("basic");
}

static void t_fade(void)
{
    TEST_START("fade");
    connect_and_discover();
    pair();
    subscribe(H_STATUS);
    expect_ok(set_fade(1000), "fade 1000");
    slog_n = 0;
    expect_ok(set_level(100), "level 100");
    uint32_t t0 = k_uptime_get_32();
    int up = wait_status(is_full_on, 3000);
    int n_up = slog_n, mono = 1;
    for (int i = 1; i < n_up; i++) {
        mono &= slog[i].s.level >= slog[i - 1].s.level;
    }
    uint32_t t_end = n_up ? slog[n_up - 1].t : 0;
    METRIC("fade_up_ms=%d fade_up_notifs=%d fade_up_monotonic=%d", (int)(t_end - t0), n_up, mono);
    for (int i = 0; i < n_up; i++) {
        printk("FADE up t=%d level=%d\n", (int)(slog[i].t - t0), slog[i].s.level);
    }
    expect_ok(set_fade(3000), "fade 3000");
    slog_n = 0;
    expect_ok(set_level(20), "level 20");
    t0 = k_uptime_get_32();
    int down = wait_status(is_20, 6000);
    int n_dn = slog_n;
    mono = 1;
    for (int i = 1; i < n_dn; i++) {
        mono &= slog[i].s.level <= slog[i - 1].s.level;
    }
    t_end = n_dn ? slog[n_dn - 1].t : 0;
    METRIC("fade_down_ms=%d fade_down_notifs=%d fade_down_monotonic=%d", (int)(t_end - t0), n_dn, mono);
    for (int i = 0; i < n_dn; i++) {
        printk("FADE down t=%d level=%d\n", (int)(slog[i].t - t0), slog[i].s.level);
    }
    if (up < 0 || down < 0) {
        TEST_FAIL("fade did not complete");
    }
    TEST_PASS("fade");
}

static void t_faults(void)
{
    struct dim_status s;
    TEST_START("faults");
    connect_and_discover();
    pair();
    subscribe(H_STATUS);
    expect_ok(set_fade(0), "fade 0");
    expect_ok(set_level(100), "level 100");
    k_sleep(K_MSEC(500));

    /* overload: +3 A on a 4 A load at full output */
    expect_ok(inject(1, 30), "inject overload");
    int ov = wait_status(is_fault, 1000);
    read_status(&s);
    int ov_flag = s.faults & 0x02;
    int rej = set_level(50);                  /* refused while in fault */
    int rst = cmd(4);                         /* reset allowed: output stays off */
    read_status(&s);
    int safe_off = s.state == ST_OFF && s.target == 0;
    expect_ok(set_level(100), "turn on with the overload still there");
    int retrip = wait_status(is_fault, 1000); /* must trip again */
    expect_ok(inject(0, 0), "clear");
    expect_ok(cmd(4), "reset");
    expect_ok(set_level(100), "on after the overload is gone");
    k_sleep(K_MSEC(500));
    read_status(&s);
    int healthy = s.state == ST_ON && s.faults == 0;
    METRIC("overload_seen=%d overload_flag=%d cmd_in_fault=0x%02x reset=0x%02x safe_off_after_reset=%d "
           "retrip_if_still_overloaded=%d on_after_clear=%d", ov >= 0, ov_flag != 0, rej, rst, safe_off, retrip >= 0,
           healthy);
    if (ov < 0 || !ov_flag || rej != 0x80 || rst || !safe_off || retrip < 0 || !healthy) {
        TEST_FAIL("overload handling");
    }

    /* short circuit */
    expect_ok(set_level(100), "level 100");
    k_sleep(K_MSEC(300));
    expect_ok(inject(2, 1), "inject short");
    int sh = wait_status(is_fault, 500);
    read_status(&s);
    int sh_flag = s.faults & 0x01;
    expect_ok(inject(0, 0), "clear");
    k_sleep(K_MSEC(100));
    int sh_rst = cmd(4);
    METRIC("short_seen=%d short_flag=%d short_reset=0x%02x", sh >= 0, sh_flag != 0, sh_rst);
    if (sh < 0 || !sh_flag || sh_rst) {
        TEST_FAIL("short handling");
    }

    /* overtemperature: derate at 90 C, trip at 101 C, reset only once cooled */
    expect_ok(set_level(100), "level 100");
    k_sleep(K_MSEC(300));
    expect_ok(inject(3, 90), "temp 90");
    int dr = wait_status(derated, 500);
    k_sleep(K_MSEC(1200));
    struct dim_meter m;
    gread(H_METER, &m, sizeof m);
    expect_ok(inject(3, 101), "temp 101");
    int ot = wait_status(is_fault, 500);
    read_status(&s);
    int ot_flag = s.faults & 0x04;
    int ot_rst_hot = cmd(4);
    expect_ok(inject(3, 70), "temp 70");
    k_sleep(K_MSEC(100));
    int ot_rst = cmd(4);
    METRIC("derated_seen=%d derated_power_w=%d overtemp_seen=%d overtemp_flag=%d reset_hot=0x%02x reset_cool=0x%02x",
           dr >= 0, m.power_dw / 10, ot >= 0, ot_flag != 0, ot_rst_hot, ot_rst);
    if (dr < 0 || ot < 0 || !ot_flag || ot_rst_hot != 0x81 || ot_rst) {
        TEST_FAIL("overtemperature handling");
    }
    TEST_PASS("faults");
}

static void t_brownout(void)
{
    struct dim_status s;
    TEST_START("brownout");
    connect_and_discover();
    pair();
    subscribe(H_STATUS);
    expect_ok(set_fade(0), "fade 0");
    expect_ok(set_level(60), "level 60");
    k_sleep(K_MSEC(300));
    expect_ok(inject(4, 200), "mains off 200 ms");
    int lost = wait_status(mains_lost, 500);
    int back = wait_status(mains_ok, 1000);
    k_sleep(K_MSEC(100));
    read_status(&s);
    int resumed = s.state == ST_ON && s.target == 60 && s.level == 60 && s.faults == 0;
    METRIC("mains_lost_seen=%d mains_back_seen=%d resumed_same_level=%d", lost >= 0, back >= 0, resumed);
    if (lost < 0 || back < 0 || !resumed) {
        TEST_FAIL("brownout ride-through");
    }
    TEST_PASS("brownout");
}

static void t_linkloss_a(void)
{
    TEST_START("linkloss_a");
    connect_and_discover();
    pair();
    expect_ok(set_fade(0), "fade 0");
    expect_ok(set_level(70), "level 70");
    k_sleep(K_MSEC(500));
    TEST_PASS("linkloss_a: phone leaves without disconnecting");
    bs_trace_silent_exit(0);                  /* radio gone: the dimmer only sees a supervision timeout */
}

static void t_linkloss_b(void)
{
    struct dim_status s;
    TEST_START("linkloss_b");
    k_sleep(K_MSEC(3000));                    /* a second phone shows up later */
    connect_and_discover();
    METRIC("reconnected_at_ms=%u", k_uptime_get_32());
    read_status(&s);
    int kept = s.state == ST_ON && s.target == 70 && s.level == 70;
    METRIC("level_kept_after_link_loss=%d level=%d", kept, s.level);
    if (!kept) {
        TEST_FAIL("level changed after link loss");
    }
    TEST_PASS("linkloss_b");
}

static void t_persist_set(void)
{
    TEST_START("persist_set");
    connect_and_discover();
    pair();
    expect_ok(set_fade(800), "fade 800");
    expect_ok(set_level(35), "level 35");
    k_sleep(K_MSEC(1500));                    /* settings are saved 500 ms after the last change */
    TEST_PASS("persist_set");
}

static void t_persist_check(void)
{
    struct dim_status s;
    TEST_START("persist_check");
    connect_and_discover();
    k_sleep(K_MSEC(1500));                    /* let the power-up fade finish */
    read_status(&s);
    int ok = s.state == ST_ON && s.target == 35 && s.level == 35 && s.fade_ms == 800;
    METRIC("restored_after_power_cycle=%d state=%d target=%d fade=%d", ok, s.state, s.target, s.fade_ms);
    if (!ok) {
        TEST_FAIL("settings not restored");
    }
    TEST_PASS("persist_check");
}

static void t_stress(void)
{
    struct dim_status s;
    TEST_START("stress");
    connect_and_discover();
    pair();
    expect_ok(set_fade(0), "fade 0");
    srand(7);
    int target = 0, last_on = 100, on = 0, bad = 0, errs = 0;
    const int n = 300;
    uint32_t t0 = k_uptime_get_32();
    for (int i = 0; i < n; i++) {
        int op = rand() % 4, e = 0;
        if (op == 0) {
            int lvl = rand() % 101;
            e = set_level(lvl);
            target = lvl;
            on = lvl > 0;
            if (lvl) {
                last_on = lvl;
            }
        } else if (op == 1) {
            e = cmd(1);
            target = last_on;
            on = 1;
        } else if (op == 2) {
            e = cmd(2);
            target = 0;
            on = 0;
        } else {
            e = cmd(3);
            if (on) {
                target = 0;
                on = 0;
            } else {
                target = last_on;
                on = 1;
            }
        }
        errs += e != 0;
        read_status(&s);
        bad += s.target != target || s.state != (on ? ST_ON : ST_OFF);
    }
    uint32_t dt = k_uptime_get_32() - t0;
    METRIC("stress_ops=%d mismatches=%d att_errors=%d avg_op_ms=%d.%d", n, bad, errs, (int)(dt / n),
           (int)((dt * 10U / n) % 10U));
    if (bad || errs) {
        TEST_FAIL("stress: %d mismatches, %d errors", bad, errs);
    }
    TEST_PASS("stress");
}

static const struct bst_test_instance tests[] = {
    {.test_id = "basic", .test_main_f = t_basic},
    {.test_id = "fade", .test_main_f = t_fade},
    {.test_id = "faults", .test_main_f = t_faults},
    {.test_id = "brownout", .test_main_f = t_brownout},
    {.test_id = "linkloss_a", .test_main_f = t_linkloss_a},
    {.test_id = "linkloss_b", .test_main_f = t_linkloss_b},
    {.test_id = "persist_set", .test_main_f = t_persist_set},
    {.test_id = "persist_check", .test_main_f = t_persist_check},
    {.test_id = "stress", .test_main_f = t_stress},
    BSTEST_END_MARKER};

static struct bst_test_list *install(struct bst_test_list *list)
{
    return bst_add_tests(list, tests);
}

bst_test_install_t test_installers[] = {install, NULL};

int main(void)
{
    bst_main();
    return 0;
}
