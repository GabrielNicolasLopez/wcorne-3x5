#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#include "active_time.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Persistent ACTIVE time, including the idle timeout grace period.
 * Save every five minutes and synchronously before idle/sleep.
 * A sudden power cut can lose at most the current checkpoint interval.
 */
static struct k_spinlock active_time_lock;
static int64_t active_ms;
static int64_t active_since;
static bool session_active = true;
static bool usb_powered;
static bool counter_ready;
static int64_t saved_ms;
K_MUTEX_DEFINE(counter_save_lock);

int64_t corne_active_time_total(void) {
    k_spinlock_key_t key = k_spin_lock(&active_time_lock);
    int64_t elapsed = active_ms;
    if (session_active && !usb_powered) {
        elapsed += k_uptime_get() - active_since;
    }
    k_spin_unlock(&active_time_lock, key);
    return elapsed;
}

static void save_active_time(void) {
    k_mutex_lock(&counter_save_lock, K_FOREVER);
    if (counter_ready) {
        int64_t total = corne_active_time_total();
        if (total != saved_ms) {
            int err = settings_save_one("corne_time/active_ms", &total, sizeof(total));
            if (err == 0) {
                saved_ms = total;
            } else {
                LOG_ERR("Active time save failed: %d", err);
            }
        }
    }
    k_mutex_unlock(&counter_save_lock);
}

static void checkpoint(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(counter_checkpoint, checkpoint);

static void checkpoint(struct k_work *work) {
    ARG_UNUSED(work);
    save_active_time();
    k_work_reschedule(&counter_checkpoint, K_MINUTES(5));
}

static int counter_settings_set(const char *name, size_t len,
                                settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "active_ms") != 0) {
        return -ENOENT;
    }
    int64_t restored;
    if (len != sizeof(restored)) {
        return -EINVAL;
    }
    int rc = read_cb(cb_arg, &restored, sizeof(restored));
    if (rc < 0) {
        return rc;
    }
    if (rc != sizeof(restored) || restored < 0) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&active_time_lock);
    active_ms = restored;
    active_since = k_uptime_get();
    k_spin_unlock(&active_time_lock, key);
    saved_ms = restored;
    return 0;
}

static int counter_settings_commit(void) {
    /* USB events may precede settings loading. Reconcile after restoring flash
     * so booting while plugged in cannot resurrect the previous cycle's total.
     */
    bool powered = zmk_usb_is_powered();
    k_spinlock_key_t key = k_spin_lock(&active_time_lock);
    usb_powered = powered;
    session_active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    if (powered) {
        active_ms = 0;
    }
    active_since = k_uptime_get();
    k_spin_unlock(&active_time_lock, key);
    counter_ready = true;
    k_work_reschedule(&counter_checkpoint, powered ? K_NO_WAIT : K_MINUTES(5));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(corne_time, "corne_time", NULL, counter_settings_set,
                               counter_settings_commit, NULL);


static int activity_changed(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    k_spinlock_key_t key = k_spin_lock(&active_time_lock);
    int64_t now = k_uptime_get();
    if (session_active && !usb_powered) {
        active_ms += now - active_since;
    }
    session_active = ev->state == ZMK_ACTIVITY_ACTIVE;
    active_since = now;
    k_spin_unlock(&active_time_lock, key);
    if (ev->state != ZMK_ACTIVITY_ACTIVE) {
        /* Do not defer: ZMK powers off immediately after the sleep event returns. */
        save_active_time();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_active_time, activity_changed);
ZMK_SUBSCRIPTION(corne_active_time, zmk_activity_state_changed);

static int usb_changed(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);
    bool powered = ev->conn_state != ZMK_USB_CONN_NONE;
    k_spinlock_key_t key = k_spin_lock(&active_time_lock);
    bool changed = powered != usb_powered;
    if (changed) {
        /* Reset on attachment, remain at zero while powered, and start a new
         * interval at detachment. HID/suspend/resume changes are not new cycles.
         */
        active_ms = 0;
        active_since = k_uptime_get();
        usb_powered = powered;
    }
    k_spin_unlock(&active_time_lock, key);
    if (changed) {
        save_active_time();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_active_time_usb, usb_changed);
ZMK_SUBSCRIPTION(corne_active_time_usb, zmk_usb_conn_state_changed);

