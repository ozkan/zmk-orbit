
#define DT_DRV_COMPAT zmk_behavior_orbit

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

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
    ORBIT_MODE_CLOCKWISE,
    ORBIT_MODE_COUNTER_CLOCKWISE,
};

struct orbit_data {
    struct k_work_delayable timeout_work;

    struct sensor_value remainder[ZMK_KEYMAP_SENSORS_LEN]
                                 [ZMK_KEYMAP_LAYERS_LEN];

    int triggers[ZMK_KEYMAP_SENSORS_LEN]
                [ZMK_KEYMAP_LAYERS_LEN];

    enum orbit_mode mode;

    const uint32_t *held_mods;
    size_t held_mods_len;
};

struct orbit_config {
    uint32_t timeout_ms;

    const uint32_t *clockwise_keys;
    const uint32_t *counter_clockwise_keys;

    const uint32_t *clockwise_mods;
    size_t clockwise_mods_len;

    const uint32_t *counter_clockwise_mods;
    size_t counter_clockwise_mods_len;
};

/* Press all modifiers selected for the current session */
static int orbit_press_mods(
    struct orbit_data *data,
    const uint32_t *mods,
    size_t mods_len,
    int64_t timestamp) {

    data->held_mods = mods;
    data->held_mods_len = 0;

    for (size_t i = 0; i < mods_len; i++) {
        int ret = raise_zmk_keycode_state_changed_from_encoded(
            mods[i], true, timestamp);

        if (ret != 0) {
            /* Release modifiers already pressed */
            for (size_t j = data->held_mods_len; j > 0; j--) {
                raise_zmk_keycode_state_changed_from_encoded(
                    mods[j - 1], false, timestamp);
            }

            data->held_mods = NULL;
            data->held_mods_len = 0;

            return ret;
        }

        data->held_mods_len++;
    }

    return 0;
}

/* Release all modifiers held by the current session */
static void orbit_release_mods(struct orbit_data *data) {
    for (size_t i = data->held_mods_len; i > 0; i--) {
        raise_zmk_keycode_state_changed_from_encoded(
            data->held_mods[i - 1], false, k_uptime_get());
    }

    data->held_mods = NULL;
    data->held_mods_len = 0;
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

    orbit_release_mods(data);

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

    /*
     * Select the session mode on the first movement.
     * The first movement also executes its corresponding key.
     */
    if (data->mode == ORBIT_MODE_IDLE) {

        int ret;

        if (direction > 0) {
            data->mode = ORBIT_MODE_CLOCKWISE;

            ret = orbit_press_mods(
                data,
                cfg->clockwise_mods,
                cfg->clockwise_mods_len,
                event.timestamp);

            LOG_DBG("Clockwise mode started");

        } else {
            data->mode = ORBIT_MODE_COUNTER_CLOCKWISE;

            ret = orbit_press_mods(
                data,
                cfg->counter_clockwise_mods,
                cfg->counter_clockwise_mods_len,
                event.timestamp);

            LOG_DBG("Counter-clockwise mode started");
        }

        if (ret != 0) {
            data->mode = ORBIT_MODE_IDLE;
            return ret;
        }
    }

    /* Restart inactivity timeout */
    k_work_reschedule(
        &data->timeout_work,
        K_MSEC(cfg->timeout_ms));

    /*
     * Select the key according to the active mode
     * and current rotation direction.
     *
     * Index 0 = clockwise
     * Index 1 = counter-clockwise
     */
    const uint32_t *keys =
        (data->mode == ORBIT_MODE_CLOCKWISE)
            ? cfg->clockwise_keys
            : cfg->counter_clockwise_keys;

    uint32_t key = keys[(direction > 0) ? 0 : 1];

    for (int i = 0; i < steps; i++) {
        int ret = orbit_send_key(key, event.timestamp);

        if (ret != 0) {
            return ret;
        }
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

/* Initialize behavior */
static int orbit_init(const struct device *dev) {
    struct orbit_data *data = dev->data;

    data->mode = ORBIT_MODE_IDLE;
    data->held_mods = NULL;
    data->held_mods_len = 0;

    k_work_init_delayable(
        &data->timeout_work,
        orbit_timeout);

    return 0;
}

static const struct behavior_driver_api orbit_driver_api = {
    .sensor_binding_accept_data = orbit_accept_data,
    .sensor_binding_process = orbit_process,
};

#define ORBIT_INST(n)                                                   \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, clockwise) == 2,                   \
                 "clockwise must contain exactly two keycodes");       \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, counter_clockwise) == 2,           \
                 "counter-clockwise must contain exactly two keycodes");\
    static const uint32_t orbit_clockwise_keys_##n[] =                  \
        DT_INST_PROP(n, clockwise);                                     \
    static const uint32_t orbit_counter_clockwise_keys_##n[] =          \
        DT_INST_PROP(n, counter_clockwise);                             \
    static const uint32_t orbit_clockwise_mods_##n[] =                  \
        DT_INST_PROP_OR(n, clockwise_keep_mods, {0});                   \
    static const uint32_t orbit_counter_clockwise_mods_##n[] =          \
        DT_INST_PROP_OR(n, counter_clockwise_keep_mods, {0});           \
    static struct orbit_data orbit_data_##n;                            \
    static const struct orbit_config orbit_config_##n = {               \
        .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 1000),              \
        .clockwise_keys = orbit_clockwise_keys_##n,                     \
        .counter_clockwise_keys = orbit_counter_clockwise_keys_##n,     \
        .clockwise_mods = orbit_clockwise_mods_##n,                     \
        .clockwise_mods_len =                                           \
            DT_INST_PROP_LEN_OR(n, clockwise_keep_mods, 0),             \
        .counter_clockwise_mods = orbit_counter_clockwise_mods_##n,     \
        .counter_clockwise_mods_len =                                   \
            DT_INST_PROP_LEN_OR(n, counter_clockwise_keep_mods, 0),     \
    };                                                                  \
    BEHAVIOR_DT_INST_DEFINE(                                            \
        n,                                                              \
        orbit_init,                                                     \
        NULL,                                                           \
        &orbit_data_##n,                                                \
        &orbit_config_##n,                                              \
        POST_KERNEL,                                                    \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                            \
        &orbit_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ORBIT_INST)
