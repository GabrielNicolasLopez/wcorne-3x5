#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include "active_time.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* A separate, read-only service leaves ZMK's split protocol unchanged.
 * The existing encrypted split connection carries the little-endian ms total.
 */
#define TIME_SERVICE_UUID BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0xc9b67a20, 0xb618, 0x48d1, 0xa5c7, 0x0bd71b139a80))
#define TIME_VALUE_UUID BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0xc9b67a21, 0xb618, 0x48d1, 0xa5c7, 0x0bd71b139a80))

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static ssize_t read_active_time(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    uint8_t value[8];
    sys_put_le64((uint64_t)corne_active_time_total(), value);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

/* Static services are sorted by name. Append ours after split_svc so adding
 * telemetry does not move the existing battery/key/trackpad attribute handles.
 */
BT_GATT_SERVICE_DEFINE(zz_corne_time_service,
    BT_GATT_PRIMARY_SERVICE(TIME_SERVICE_UUID),
    BT_GATT_CHARACTERISTIC(TIME_VALUE_UUID, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, read_active_time, NULL, NULL));

#else

static struct bt_gatt_read_params read_params;
static struct bt_gatt_discover_params discover_params;
static atomic_t value_handle;
/* Read parameters outlive the polling function, including the UUID pointer. */
static struct bt_uuid_128 time_value_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xc9b67a21, 0xb618, 0x48d1, 0xa5c7, 0x0bd71b139a80));
static atomic_t read_pending;
static struct k_spinlock remote_lock;
static int64_t remote_ms;
static int64_t received_at;
static bool received;

static void poll_time(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(time_poll, poll_time);

static uint8_t time_discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               struct bt_gatt_discover_params *params) {
    if (attr) {
        const struct bt_gatt_chrc *chrc = attr->user_data;
        atomic_set(&value_handle, chrc->value_handle);
        LOG_DBG("Right active-time handle: %u", chrc->value_handle);
    } else {
        LOG_WRN("Right active-time characteristic not found");
    }
    atomic_clear(&read_pending);
    k_work_reschedule(&time_poll, attr ? K_MSEC(100) : K_SECONDS(30));
    return BT_GATT_ITER_STOP;
}

static void time_disconnected(struct bt_conn *conn, uint8_t reason) {
    struct bt_conn_info info;
    if (!bt_conn_get_info(conn, &info) && info.role == BT_CONN_ROLE_CENTRAL) {
        /* Handles belong to one connection/database; discover again on reconnect.
         * Outstanding request storage stays reserved until its completion callback.
         */
        atomic_clear(&value_handle);
    }
}

BT_CONN_CB_DEFINE(time_connection_callbacks) = {.disconnected = time_disconnected};

bool corne_right_active_time(int64_t *total_ms, bool *fresh) {
    k_spinlock_key_t key = k_spin_lock(&remote_lock);
    bool known = received;
    *total_ms = remote_ms;
    *fresh = received && (k_uptime_get() - received_at < 65000);
    k_spin_unlock(&remote_lock, key);
    return known;
}

static uint8_t time_read_cb(struct bt_conn *conn, uint8_t err,
                            struct bt_gatt_read_params *params,
                            const void *data, uint16_t length) {
    if (err) {
        LOG_WRN("Right active-time read failed: ATT 0x%02x", err);
        atomic_clear(&value_handle);
    } else if (!data || length != 8) {
        LOG_WRN("Right active-time value missing or invalid (%u bytes)", length);
    } else {
        uint64_t value = sys_get_le64(data);
        if (value <= INT64_MAX) {
            k_spinlock_key_t key = k_spin_lock(&remote_lock);
            remote_ms = (int64_t)value;
            received_at = k_uptime_get();
            received = true;
            k_spin_unlock(&remote_lock, key);
            LOG_DBG("Right active-time received: %llu ms", (unsigned long long)value);
        }
    }
    atomic_clear(&read_pending);
    return BT_GATT_ITER_STOP;
}

static void read_from_split(struct bt_conn *conn, void *data) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    LOG_DBG("Right active-time link: state=%u security=%u", (unsigned)info.state,
            (unsigned)bt_conn_get_security(conn));
    if (info.state != BT_CONN_STATE_CONNECTED || bt_conn_get_security(conn) < BT_SECURITY_L2) {
        return;
    }
    /* This keyboard has exactly one peripheral. Never read from its PC connection. */
    if (!atomic_cas(&read_pending, 0, 1)) {
        LOG_WRN("Right active-time request still pending");
        return;
    }
    int err;
    uint16_t handle = atomic_get(&value_handle);
    if (!handle) {
        discover_params = (struct bt_gatt_discover_params){
            .uuid = &time_value_uuid.uuid,
            .func = time_discovered,
            .start_handle = 1,
            .end_handle = 0xffff,
            .type = BT_GATT_DISCOVER_CHARACTERISTIC,
        };
        err = bt_gatt_discover(conn, &discover_params);
    } else {
        /* Use the same single-handle read path as ZMK's battery fetcher. */
        read_params = (struct bt_gatt_read_params){
            .func = time_read_cb,
            .handle_count = 1,
            .single = {.handle = handle, .offset = 0},
        };
        err = bt_gatt_read(conn, &read_params);
    }
    if (err) {
        LOG_WRN("Right active-time request failed: %d", err);
        atomic_clear(&read_pending);
    }
}

static void poll_time(struct k_work *work) {
    LOG_DBG("Polling right active-time");
    k_work_reschedule(&time_poll, K_SECONDS(30));
    bt_conn_foreach(BT_CONN_TYPE_LE, read_from_split, NULL);
}

static int time_poll_init(void) {
    /* Let initial split discovery/subscriptions finish before optional telemetry. */
    k_work_reschedule(&time_poll, K_SECONDS(15));
    return 0;
}

SYS_INIT(time_poll_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
