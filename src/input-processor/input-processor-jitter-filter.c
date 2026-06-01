#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/event_manager.h>
// target_include_directories(app PRIVATE include) が必要
#include <drivers/input_processor.h>

int my_abs(int x)
{
    return x < 0 ? -x : x;
}

LOG_MODULE_REGISTER(zmk_input_processor_jitter_filter, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_input_processor_jitter_filter

struct ip_jitter_filter_config
{
    int32_t threshold;
    int32_t timeout_ms;
    int32_t ignore_after_key_ms;
    bool reset_on_key;
    bool reset_on_key_before_threshold;
};

struct ip_jitter_filter_data
{
    int32_t accum_x;
    int32_t accum_y;
    bool is_moving;
    int64_t last_timestamp;
    int64_t last_key_timestamp;
};

// ==========================================
// Input Processor handler
// ==========================================
static int jitter_filter_handle(const struct device *dev, struct input_event *evt,
                                uint32_t param1, uint32_t param2,
                                struct zmk_input_processor_state *state)
{
    const struct ip_jitter_filter_config *cfg = dev->config;
    struct ip_jitter_filter_data *data = dev->data;

    int64_t now = k_uptime_get();

    if (evt->type != INPUT_EV_REL || (evt->code != INPUT_REL_X && evt->code != INPUT_REL_Y))
    {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (cfg->ignore_after_key_ms > 0 && (now - data->last_key_timestamp <= cfg->ignore_after_key_ms))
    {
        return ZMK_INPUT_PROC_STOP;
    }

    if (now - data->last_timestamp > cfg->timeout_ms)
    {
        data->accum_x = 0;
        data->accum_y = 0;
        data->is_moving = false;
    }

    data->last_timestamp = now;

    if (data->is_moving)
    {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (evt->code == INPUT_REL_X)
    {
        data->accum_x += evt->value;
    }
    else if (evt->code == INPUT_REL_Y)
    {
        data->accum_y += evt->value;
    }

    int32_t total_dist = my_abs(data->accum_x) + my_abs(data->accum_y);

    // しきい値を超えたら移動開始、次のイベントから通し、このイベントまでのすべてのイベントは消滅させる
    if (total_dist >= cfg->threshold)
    {
        data->is_moving = true;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api jitter_filter_api = {
    .handle_event = jitter_filter_handle,
};

// ==========================================
// initialization and device definition
// ==========================================
static int jitter_filter_init(const struct device *dev)
{
    struct ip_jitter_filter_data *data = dev->data;
    data->accum_x = 0;
    data->accum_y = 0;
    data->is_moving = false;
    data->last_timestamp = 0;
    data->last_key_timestamp = 0;

    return 0;
}

#define IP_JITTER_FILTER_INIT(n)                                                                   \
    static const struct ip_jitter_filter_config ip_jitter_config_##n = {                           \
        .threshold = DT_INST_PROP_OR(n, threshold, 50),                                            \
        .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 1000),                                        \
        .ignore_after_key_ms = DT_INST_PROP_OR(n, ignore_after_key_ms, 0),                         \
        .reset_on_key = DT_INST_PROP_OR(n, reset_on_key, false),                                   \
        .reset_on_key_before_threshold = DT_INST_PROP_OR(n, reset_on_key_before_threshold, false), \
    };                                                                                             \
    static struct ip_jitter_filter_data ip_jitter_data_##n;                                        \
    DEVICE_DT_INST_DEFINE(n, &jitter_filter_init, NULL, &ip_jitter_data_##n,                       \
                          &ip_jitter_config_##n, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                          &jitter_filter_api);

DT_INST_FOREACH_STATUS_OKAY(IP_JITTER_FILTER_INIT)

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#define JITTER_DEVICE_ADDR(idx) DEVICE_DT_GET(DT_DRV_INST(idx)),

static const struct device *const filter_instances[] = {DT_INST_FOREACH_STATUS_OKAY(JITTER_DEVICE_ADDR)};

#define NUM_INSTANCES DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

#else
static struct device *const filter_instances[] = {NULL};
#define NUM_INSTANCES 0
#endif

static int jitter_filter_position_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL)
    {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int64_t now = k_uptime_get();

    // 静的に作られた全インスタンス（左右）のタイピング時刻を一斉更新＆リセット
    for (uint8_t i = 0; i < NUM_INSTANCES; i++)
    {
        const struct device *dev = filter_instances[i];
        if (dev == NULL || !device_is_ready(dev))
        {
            continue;
        }

        const struct ip_jitter_filter_config *cfg = dev->config;
        struct ip_jitter_filter_data *data = dev->data;

        data->last_key_timestamp = now;

        if (cfg->reset_on_key)
        {
            data->accum_x = 0;
            data->accum_y = 0;
            data->is_moving = false;
        }
        else if (cfg->reset_on_key_before_threshold && !data->is_moving)
        {
            data->accum_x = 0;
            data->accum_y = 0;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(jitter_filter_position_listener, jitter_filter_position_listener);
ZMK_SUBSCRIPTION(jitter_filter_position_listener, zmk_position_state_changed);
