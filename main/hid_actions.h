/*
 * HID action helpers for normalized touch and key events.
 */

#ifndef HID_ACTIONS_H
#define HID_ACTIONS_H

#include <stdint.h>

#define HID_ABS_MIN_COORD 0
#define HID_ABS_MAX_COORD 32767

void hid_touch_tap(uint16_t conn_id, float norm_x, float norm_y);
void hid_touch_long_press(uint16_t conn_id, float norm_x, float norm_y, uint32_t press_ms);
void hid_touch_swipe(uint16_t conn_id, float start_x, float start_y, float end_x, float end_y, uint32_t duration_ms);
void hid_touch_multi_tap(uint16_t conn_id, uint32_t count, const float *xs, const float *ys);
void hid_touch_multi_long_press(uint16_t conn_id, uint32_t count, const float *xs, const float *ys, uint32_t press_ms);

void hid_press_volume_up(uint16_t conn_id);
void hid_press_volume_down(uint16_t conn_id);
void hid_press_home(uint16_t conn_id);
void hid_press_back(uint16_t conn_id);
void hid_press_power(uint16_t conn_id);

#endif /* HID_ACTIONS_H */
