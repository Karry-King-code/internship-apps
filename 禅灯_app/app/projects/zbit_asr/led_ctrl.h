#ifndef LED_CTRL_H
#define LED_CTRL_H

#include <stdint.h>
#include <stdbool.h>

#define LED_LOW_LEVEL_LIGHT     0
#define LED_HIGH_LEVEL_LIGHT    1

#define LED_LIGHT_MODE          LED_HIGH_LEVEL_LIGHT

/* 基础接口 */
void led_ctrl_init(void);
int  led_ctrl(uint16_t cmd);

/* 触摸按键接口 */
void led_touch_click(void);             // 单击：5态循环
void led_touch_long_press_start(void);  // 长按开始：若上次边界松手则换向
void led_touch_long_press_tick(void);   // 长按：连续调光（到边界停住）
void led_touch_long_press_stop(void);   // 长按松手：停住；边界则标记下次换向

/* 呼吸灯接口（主循环约 1ms 补跑） */
void led_breath_tick(void);             // 仅在呼吸灯模式时有效，其余时无操作

/* 30分钟睡眠计时接口（主循环约 1ms 补跑） */
void led_sleep_tick(void);              // 灯灭即计时（含首次上电），超时后置睡眠标志

/* 语音使能控制 */
void led_voice_enable(bool en);
bool led_voice_is_enabled(void);

/* 睡眠标志查询（由 bsp_asr.c 轮询） */
bool led_need_sleep(void);

/* PWM/LED 是否已完成初始化 */
bool led_pwm_is_ready(void);

/* 主循环调用（不可在 1ms ISR 中调用，避免 comm 区栈溢出） */
void led_app_process(void);

#endif /* LED_CTRL_H */
