
#define DT_DRV_COMPAT zmk_behavior_orbit

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>
#include <zmk/events/keycode_state_changed.h>

#include <dt-bindings/zmk/keys.h>

LOG_MODULE_REGISTER(behavior_orbit, CONFIG_ZMK_LOG_LEVEL);

enum orbit_mode {
    ORBIT_MODE_IDLE,
    ORBIT_MODE_WINDOW,
    ORBIT_MODE_VOLUME,
};

struct orbit_data {
    struct k_work_delayable timeout_work;

    struct sensor_value remainder[ZMK_KEYMAP_SENSORS_LEN]
                                 [ZMK_KEYMAP_LAYERS_LEN];

    int triggers[ZMK_KEYMAP_SENSORS_LEN]
                [ZMK_KEYMAP_LAYERS_LEN];

    enum orbit_mode mode;

    bool alt_held;
};

struct orbit_config {
    uint32_t timeout_ms;
};

/* Send Alt press */
static int orbit_press_alt(struct orbit_data *data, int64_t timestamp) {
    if (data->alt_held) {
        return 0;
    }

    int ret = raise_zmk_keycode_state_changed_from_encoded(
        LEFT_ALT, true, timestamp);

    if (ret == 0) {
        data->alt_held = true;
    }

    return ret;
}

/* Send Alt release */
static void orbit_release_alt(struct orbit_data *data) {
    if (!data->alt_held) {
        return;
    }

    raise_zmk_keycode_state_changed_from_encoded(
        LEFT_ALT, false, k_uptime_get());

    data->alt_held = false;
}

/* Send a complete key tap */
static int orbit_send_key(uint32_t key, int64_t timestamp) {
    int ret = raise_zmk_keycode_state_changed_from_encoded(
        key, true, timestamp);

    if (ret != 0) {
        return ret;
    }

    return raise_zmk_keycode_state_changed_from_encoded(
        key, false, timestamp);
}

/* Reset session after inactivity */
static void orbit_timeout(struct k_work *work) {
    struct k_work_delayable *dwork =
        k_work_delayable_from_work(work);

    struct orbit_data *data =
        CONTAINER_OF(dwork, struct orbit_data, timeout_work);

    orbit_release_alt(data);

    data->mode = ORBIT_MODE_IDLE;

    LOG_DBG("Session ended");
}

/* Receive encoder sensor data */
static int orbit_accept_data(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event,
    const struct zmk_sensor_config *sensor_config,
    size_t channel_data_size,
    const struct zmk_sensor_channel_data *channel_data) {

    const struct device *dev =
        zmk_behavior_get_binding(binding->behavior_dev);

    struct orbit_data *data = dev->data;

    if (channel_data_size == 0 || sensor_config == NULL) {
        return -EINVAL;
    }

    int sensor_index =
        ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    if (sensor_index >= ZMK_KEYMAP_SENSORS_LEN ||
        event.layer >= ZMK_KEYMAP_LAYERS_LEN) {
        return -EINVAL;
    }

    const struct sensor_value value = channel_data[0].value;

    int triggers;

    /* Compatibility with older encoder sensor format */
    if (value.val1 == 0) {
        triggers = value.val2;
    } else {
        struct sensor_value remainder =
            data->remainder[sensor_index][event.layer];

        remainder.val1 += value.val1;
        remainder.val2 += value.val2;

        if (remainder.val2 >= 1000000 ||
            remainder.val2 <= -1000000) {

            remainder.val1 += remainder.val2 / 1000000;
            remainder.val2 %= 1000000;
        }

        if (sensor_config->triggers_per_rotation == 0) {
            return -EINVAL;
        }

        int trigger_degrees =
            360 / sensor_config->triggers_per_rotation;

        if (trigger_degrees <= 0) {
            return -EINVAL;
        }

        triggers = remainder.val1 / trigger_degrees;

        remainder.val1 %= trigger_degrees;

        data->remainder[sensor_index][event.layer] = remainder;
    }

    data->triggers[sensor_index][event.layer] = triggers;

    return 0;
}

/* Process encoder rotation */
static int orbit_process(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event,
    enum behavior_sensor_binding_process_mode mode) {

    const struct device *dev =
        zmk_behavior_get_binding(binding->behavior_dev);

    struct orbit_data *data = dev->data;

    const struct orbit_config *cfg = dev->config;

    int sensor_index =
        ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    if (sensor_index >= ZMK_KEYMAP_SENSORS_LEN ||
        event.layer >= ZMK_KEYMAP_LAYERS_LEN) {
        return -EINVAL;
    }

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        data->triggers[sensor_index][event.layer] = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int triggers = data->triggers[sensor_index][event.layer];

    data->triggers[sensor_index][event.layer] = 0;

    if (triggers == 0) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    /* Positive = clockwise, negative = counter-clockwise */
    int direction = (triggers > 0) ? 1 : -1;

    int steps = (triggers > 0) ? triggers : -triggers;

    /* Select mode only on the first movement */
    if (data->mode == ORBIT_MODE_IDLE) {

        if (direction > 0) {
            data->mode = ORBIT_MODE_WINDOW;
            LOG_DBG("Alt+Tab mode started");
        } else {
            data->mode = ORBIT_MODE_VOLUME;
            LOG_DBG("Volume mode started");
        }
    }

    /* Restart inactivity timeout */
    k_work_reschedule(
        &data->timeout_work,
        K_MSEC(cfg->timeout_ms));

    if (data->mode == ORBIT_MODE_WINDOW) {

        /* Keep Alt pressed */
        int ret = orbit_press_alt(data, event.timestamp);

        if (ret != 0) {
            return ret;
        }

        /* CW = Tab, CCW = Shift + Tab */
        uint32_t key = (direction > 0) ? TAB : LS(TAB);

        for (int i = 0; i < steps; i++) {
            ret = orbit_send_key(key, event.timestamp);

            if (ret != 0) {
                return ret;
            }
        }

    } else if (data->mode == ORBIT_MODE_VOLUME) {

        /* Clockwise = Volume Up, Counter-clockwise = Volume Down */
        uint32_t key = (direction > 0)
                           ? C_VOLUME_UP
                           : C_VOLUME_DOWN;

        for (int i = 0; i < steps; i++) {
            int ret = orbit_send_key(key, event.timestamp);

            if (ret != 0) {
                return ret;
            }
        }
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

/* Initialize behavior */
static int orbit_init(const struct device *dev) {
    struct orbit_data *data = dev->data;

    data->mode = ORBIT_MODE_IDLE;
    data->alt_held = false;

    k_work_init_delayable(
        &data->timeout_work,
        orbit_timeout);

    return 0;
}

static const struct behavior_driver_api orbit_driver_api = {
    .sensor_binding_accept_data = orbit_accept_data,
    .sensor_binding_process = orbit_process,
};

#define ORBIT_INST(n)                                               \
    static struct orbit_data orbit_data_##n;                        \
    static const struct orbit_config orbit_config_##n = {           \
        .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 1000),         \
    };                                                              \
    BEHAVIOR_DT_INST_DEFINE(                                        \
        n,                                                          \
        orbit_init,                                                 \
        NULL,                                                       \
        &orbit_data_##n,                                            \
        &orbit_config_##n,                                          \
        POST_KERNEL,                                                \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
        &orbit_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ORBIT_INST)
