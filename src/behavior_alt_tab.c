
#define DT_DRV_COMPAT zmk_behavior_alt_tab
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/sensors.h>
#include <zmk/keys.h>

#include <dt-bindings/zmk/keys.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_REGISTER(behavior_alt_tab, CONFIG_ZMK_LOG_LEVEL);

#define ALT_KEY ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTALT)

struct alt_tab_data {
    struct k_work_delayable timeout_work;

    struct sensor_value remainder;

    int triggers;

    bool alt_held;
};

struct alt_tab_config {
    uint32_t timeout_ms;
};

static int alt_tab_press_alt(struct alt_tab_data *data, int64_t timestamp) {
    if (data->alt_held) {
        return 0;
    }

    int ret = raise_zmk_keycode_state_changed_from_encoded(
        ALT_KEY, true, timestamp);

    if (ret == 0) {
        data->alt_held = true;
    }

    return ret;
}

static void alt_tab_release_alt(struct alt_tab_data *data) {
    if (!data->alt_held) {
        return;
    }

    raise_zmk_keycode_state_changed_from_encoded(
        ALT_KEY, false, k_uptime_get());

    data->alt_held = false;
}

static void alt_tab_timeout(struct k_work *work) {
    struct k_work_delayable *dwork =
        k_work_delayable_from_work(work);

    struct alt_tab_data *data =
        CONTAINER_OF(dwork, struct alt_tab_data, timeout_work);

    alt_tab_release_alt(data);
}

static int alt_tab_send_key(uint32_t key, int64_t timestamp) {
    int ret = raise_zmk_keycode_state_changed_from_encoded(
        key, true, timestamp);

    if (ret != 0) {
        return ret;
    }

    return raise_zmk_keycode_state_changed_from_encoded(
        key, false, timestamp);
}

static int alt_tab_accept_data(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event,
    const struct zmk_sensor_config *sensor_config,
    size_t channel_data_size,
    const struct zmk_sensor_channel_data *channel_data) {

    const struct device *dev =
        zmk_behavior_get_binding(binding->behavior_dev);

    struct alt_tab_data *data = dev->data;

    if (channel_data_size == 0 || sensor_config == NULL) {
        return -EINVAL;
    }

    const struct sensor_value value = channel_data[0].value;

    if (value.val1 == 0) {
        data->triggers = value.val2;
        return 0;
    }

    data->remainder.val1 += value.val1;
    data->remainder.val2 += value.val2;

    if ((data->remainder.val2 < 0 ? -data->remainder.val2 : data->remainder.val2) >= 1000000) {
        data->remainder.val1 +=
            data->remainder.val2 / 1000000;

        data->remainder.val2 %= 1000000;
    }

    int trigger_degrees =
        360 / sensor_config->triggers_per_rotation;

    if (trigger_degrees <= 0) {
        return -EINVAL;
    }

    data->triggers =
        data->remainder.val1 / trigger_degrees;

    data->remainder.val1 %= trigger_degrees;

    return 0;
}

static int alt_tab_process(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event,
    enum behavior_sensor_binding_process_mode mode) {

    const struct device *dev =
        zmk_behavior_get_binding(binding->behavior_dev);

    struct alt_tab_data *data = dev->data;

    const struct alt_tab_config *cfg = dev->config;

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        data->triggers = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int triggers = data->triggers;
    data->triggers = 0;

    if (triggers == 0) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int direction = (triggers > 0) ? 1 : -1;

    if (triggers < 0) {
        triggers = -triggers;
    }

    int ret = alt_tab_press_alt(data, event.timestamp);

    if (ret != 0) {
        return ret;
    }

    for (int i = 0; i < triggers; i++) {

        uint32_t key = (direction > 0) ? TAB : LS(TAB);

        ret = alt_tab_send_key(key, event.timestamp);

        if (ret != 0) {
            return ret;
        }
    }

    k_work_reschedule(
        &data->timeout_work,
        K_MSEC(cfg->timeout_ms));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int alt_tab_init(const struct device *dev) {
    struct alt_tab_data *data = dev->data;

    k_work_init_delayable(
        &data->timeout_work,
        alt_tab_timeout);

    return 0;
}

static const struct behavior_driver_api alt_tab_driver_api = {
    .sensor_binding_accept_data = alt_tab_accept_data,
    .sensor_binding_process = alt_tab_process,
};

#define ALT_TAB_INST(n)                                             \
    static struct alt_tab_data alt_tab_data_##n;                    \
    static const struct alt_tab_config alt_tab_config_##n = {       \
        .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 1000),         \
    };                                                              \
    BEHAVIOR_DT_INST_DEFINE(                                        \
        n,                                                          \
        alt_tab_init,                                               \
        NULL,                                                       \
        &alt_tab_data_##n,                                          \
        &alt_tab_config_##n,                                        \
        POST_KERNEL,                                                \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
        &alt_tab_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ALT_TAB_INST)
