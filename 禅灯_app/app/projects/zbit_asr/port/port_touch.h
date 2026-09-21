#ifndef PORT_TOUCH_H
#define PORT_TOUCH_H

/*
 * QM301 单键触摸 IC（OUT → MCU PA10，经 1k 串阻）
 *
 * 硬件要求（规格书）：
 *   STG = GND  → 同步输出模式（类似轻触键，输出电平跟随手指，支持短按/长按）
 *   STG = VDD  → 电平切换模式（自锁，每次触摸翻转 OUT，不支持长按调光）
 *
 * SLH 决定有效电平（与 config.h 唤醒沿需一致）：
 *   SLH = GND  → 无触摸 OUT=低，触摸 OUT=高  → TOUCH_QM301_ACTIVE_HIGH = 1，唤醒用上升沿
 *   SLH = VDD  → 无触摸 OUT=高，触摸 OUT=低  → TOUCH_QM301_ACTIVE_HIGH = 0，唤醒用下降沿
 */
#define TOUCH_QM301_ACTIVE_HIGH     0   /* 当前硬件：SLH=VDD，触摸=低，与 POWER_ON_FALL_IO 一致 */

/* QM301 输出响应时间 TR ≤ 60ms，消抖须大于此值 */
#define TOUCH_DEBOUNCE_MS           65U
#define TOUCH_LONG_MS               800U
/* QM301 规格单次有效触摸约 ≤6s（硬件）；软件不做截断 */
#define TOUCH_QM301_MAX_MS          6000U

void port_touch_init(void);
void port_touch_scan(void);  /* 每 1ms 调用，由 led_app_process() 调度 */

#endif /* PORT_TOUCH_H */
