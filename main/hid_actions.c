/*
 * HID action helper implementations.
 */

#include "hid_actions.h"

#include <math.h>
#include <stdbool.h>

#include "esp_hidd_prf_api.h"
#include "hid_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define HID_TOUCH_INTERVAL_MS 16
#define HID_TAP_HOLD_MS 50
#define HID_LONG_PRESS_MIN_MS 20
#define HID_KEY_HOLD_MS 60
#define HID_CONSUMER_HOLD_MS 80

#define HID_PI 3.1415926f

static inline int16_t hid_clamp_coord(int32_t coord)
{
    if (coord < HID_ABS_MIN_COORD)
    {
        return HID_ABS_MIN_COORD;
    }
    if (coord > HID_ABS_MAX_COORD)
    {
        return HID_ABS_MAX_COORD;
    }
    return (int16_t)coord;
}

static uint16_t hid_map_normalized(float value)
{
    if (value < 0.0f)
    {
        value = 0.0f;
    }
    else if (value > 1.0f)
    {
        value = 1.0f;
    }

    int32_t scaled = (int32_t)(value * HID_ABS_MAX_COORD + 0.5f);
    return (uint16_t)hid_clamp_coord(scaled);
}

static void hid_touch_update(uint16_t conn_id, bool touch_down, float norm_x, float norm_y)
{
    uint16_t mapped_x = hid_map_normalized(norm_x);
    uint16_t mapped_y = hid_map_normalized(norm_y);

    esp_hidd_send_touch_value(conn_id, touch_down, mapped_x, mapped_y);
}

void hid_touch_tap(uint16_t conn_id, float norm_x, float norm_y)
{
    hid_touch_update(conn_id, true, norm_x, norm_y);
    vTaskDelay(pdMS_TO_TICKS(HID_TAP_HOLD_MS));
    hid_touch_update(conn_id, false, norm_x, norm_y);
}

void hid_touch_long_press(uint16_t conn_id, float norm_x, float norm_y, uint32_t press_ms)
{
    if (press_ms < HID_LONG_PRESS_MIN_MS)
    {
        press_ms = HID_LONG_PRESS_MIN_MS;
    }

    hid_touch_update(conn_id, true, norm_x, norm_y);
    vTaskDelay(pdMS_TO_TICKS(press_ms));
    hid_touch_update(conn_id, false, norm_x, norm_y);
}

void hid_touch_swipe(uint16_t conn_id, float start_x, float start_y, float end_x, float end_y, uint32_t duration_ms)
{
    if (duration_ms == 0)
    {
        duration_ms = 600; // 榛樿鏀炬參婊戝姩閫熷害
    }

    if (duration_ms < HID_TOUCH_INTERVAL_MS * 4)
    {
        duration_ms = HID_TOUCH_INTERVAL_MS * 4;
    }

    const uint32_t interval_ms = HID_TOUCH_INTERVAL_MS;
    uint32_t steps = duration_ms / interval_ms;
    if (steps < 5)
    {
        steps = 5;
    }

    hid_touch_update(conn_id, true, start_x, start_y);

    float dx = end_x - start_x;
    float dy = end_y - start_y;
    float path_len = sqrtf(dx * dx + dy * dy);

    float perp_x = -dy;
    float perp_y = dx;
    float perp_len = sqrtf(perp_x * perp_x + perp_y * perp_y);
    if (perp_len > 0.0001f)
    {
        perp_x /= perp_len;
        perp_y /= perp_len;
    }
    else
    {
        perp_x = 0.0f;
        perp_y = 0.1f;
    }

    float arc_offset = fmaxf(0.02f, path_len * 0.25f);

    for (uint32_t i = 1; i <= steps; ++i)
    {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));

        float t = (float)i / (float)steps;
        float eased = 0.5f - 0.5f * cosf(t * HID_PI); // ease-in-out to simulate acceleration
        float along_x = start_x + dx * eased;
        float along_y = start_y + dy * eased;

        float arc = sinf(eased * HID_PI); // create slight arc offset
        float current_x = along_x + perp_x * arc * arc_offset;
        float current_y = along_y + perp_y * arc * arc_offset;

        hid_touch_update(conn_id, true, current_x, current_y);
    }

    hid_touch_update(conn_id, false, end_x, end_y);
}

static void hid_consumer_click(uint16_t conn_id, uint16_t usage)
{
    esp_hidd_send_consumer_value(conn_id, usage, true);
    vTaskDelay(pdMS_TO_TICKS(HID_CONSUMER_HOLD_MS));
    esp_hidd_send_consumer_value(conn_id, usage, false);
}

void hid_touch_multi_tap(uint16_t conn_id, uint32_t count, const float *xs, const float *ys)
{
    if (count == 0 || !xs || !ys)
    {
        return;
    }

    if (count > 5)
    {
        count = 5;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        hid_touch_tap(conn_id, xs[i], ys[i]);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void hid_touch_multi_long_press(uint16_t conn_id, uint32_t count, const float *xs, const float *ys, uint32_t press_ms)
{
    if (count == 0 || !xs || !ys)
    {
        return;
    }

    if (count > 5)
    {
        count = 5;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        hid_touch_long_press(conn_id, xs[i], ys[i], press_ms);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
void hid_press_volume_up(uint16_t conn_id)
{
    hid_consumer_click(conn_id, HID_CONSUMER_VOLUME_UP);
}

void hid_press_volume_down(uint16_t conn_id)
{
    hid_consumer_click(conn_id, HID_CONSUMER_VOLUME_DOWN);
}

void hid_press_home(uint16_t conn_id)
{
    hid_consumer_click(conn_id, HID_CONSUMER_AC_HOME);
}

void hid_press_back(uint16_t conn_id)
{
    hid_consumer_click(conn_id, HID_CONSUMER_AC_BACK);
}

void hid_press_power(uint16_t conn_id)
{
    hid_consumer_click(conn_id, HID_CONSUMER_POWER);
}



