#include "include.h"
#include "led_ctrl.h"
#include "port_touch.h"

/*===========================================================================
 * QM301 OUT → PA4
 * 短按松手切态；长按 ≥800ms 连续调光，松手即停；到边界松手后下次长按换向。
 * 双击：300ms内两次短按触发双击事件
 *===========================================================================*/
#define TOUCH_IO            IO_PA4
#define TOUCH_IO_BIT        (TOUCH_IO - 1)

typedef enum {
    TOUCH_IDLE = 0,
    TOUCH_DEBOUNCE,
    TOUCH_PRESSED,
    TOUCH_LONG_PRESS,
} touch_state_t;

static gpio_t        s_touch_gpio;
static touch_state_t s_touch_state = TOUCH_IDLE;
static uint16_t      s_touch_cnt   = 0;
static bool          s_long_active = false;

/* 双击检测相关 */
static uint32_t      s_last_release_time = 0;    /* 上次松手时间 */
static bool          s_waiting_for_double = false; /* 是否在等待第二次点击 */
static bool          s_click_processed = false;   /* 当前点击是否已被处理 */

static inline bool touch_raw_level(void)
{
    if (s_touch_gpio.sfr == NULL) {
        return (GPIOA & BIT(TOUCH_IO_BIT)) != 0;
    }
    return (s_touch_gpio.sfr[GPIOx] & BIT(s_touch_gpio.num)) != 0;
}

static inline bool touch_is_pressed(void)
{
#if TOUCH_QM301_ACTIVE_HIGH
    return touch_raw_level();
#else
    return !touch_raw_level();
#endif
}

void port_touch_init(void)
{
    bsp_gpio_cfg_init(&s_touch_gpio, TOUCH_IO);
    if (s_touch_gpio.sfr != NULL) {
        s_touch_gpio.sfr[GPIOxDE]  |= BIT(s_touch_gpio.num);
        s_touch_gpio.sfr[GPIOxFEN] &= ~BIT(s_touch_gpio.num);
        s_touch_gpio.sfr[GPIOxDIR] |= BIT(s_touch_gpio.num);
#if TOUCH_QM301_ACTIVE_HIGH
        s_touch_gpio.sfr[GPIOxPU]  &= ~BIT(s_touch_gpio.num);
        s_touch_gpio.sfr[GPIOxPD]  |=  BIT(s_touch_gpio.num);
#else
        s_touch_gpio.sfr[GPIOxPU]  |=  BIT(s_touch_gpio.num);
        s_touch_gpio.sfr[GPIOxPD]  &= ~BIT(s_touch_gpio.num);
#endif
    } else {
        GPIOAFEN &= ~BIT(TOUCH_IO_BIT);
        GPIOADE  |=  BIT(TOUCH_IO_BIT);
        GPIOADIR |=  BIT(TOUCH_IO_BIT);
#if TOUCH_QM301_ACTIVE_HIGH
        GPIOAPU  &= ~BIT(TOUCH_IO_BIT);
        GPIOAPD  |=  BIT(TOUCH_IO_BIT);
#else
        GPIOAPU  |=  BIT(TOUCH_IO_BIT);
        GPIOAPD  &= ~BIT(TOUCH_IO_BIT);
#endif
    }
}

/**
 * @brief 处理短按事件（单击或双击的一部分）
 */
static void handle_short_press(uint32_t current_time)
{
    /* 检查是否在等待第二次点击 */
    if (s_waiting_for_double) {
        /* 检查是否在双击超时内 */
        if ((current_time - s_last_release_time) < DOUBLE_CLICK_TIMEOUT_MS) {
            /* 第二次点击 -> 双击 */
            s_waiting_for_double = false;
            s_click_processed = true;
            led_touch_double_click();
            my_printf("touch: double click\n");
            return;
        } else {
            /* 超时，之前的等待作废，这次作为新点击 */
            s_waiting_for_double = false;
        }
    }

    /* 第一次点击：启动等待双击超时 */
    s_last_release_time = current_time;
    s_waiting_for_double = true;
    s_click_processed = false;
    my_printf("touch: first click, waiting for double...\n");
}

/**
 * @brief 检查双击超时，超时则触发单击
 */
static void check_double_timeout(uint32_t current_time)
{
    if (!s_waiting_for_double || s_click_processed) {
        return;
    }

    if ((current_time - s_last_release_time) >= DOUBLE_CLICK_TIMEOUT_MS) {
        /* 超时，触发单击 */
        s_waiting_for_double = false;
        s_click_processed = true;

        if (led_handle_is_on()) {
            /* 开灯状态：切换颜色 */
            led_touch_click();
            my_printf("touch: single click (color switch)\n");
        } else {
            /* 关灯状态：单击开灯（需要语音使能） */
            led_touch_single_click_on();
            my_printf("touch: single click on (led off)\n");
        }
    }
}

void port_touch_scan(void)
{
    uint32_t current_time;

    if (!led_pwm_is_ready()) {
        return;
    }

    current_time = tick_get();

    /* 检查双击超时（每次scan都检查） */
    check_double_timeout(current_time);

    switch (s_touch_state) {

    case TOUCH_IDLE:
        if (touch_is_pressed()) {
            s_touch_state = TOUCH_DEBOUNCE;
            s_touch_cnt   = 0;
        }
        break;

    case TOUCH_DEBOUNCE:
        s_touch_cnt++;
        if (!touch_is_pressed()) {
            s_touch_state = TOUCH_IDLE;
            s_touch_cnt   = 0;
        } else if (s_touch_cnt >= TOUCH_DEBOUNCE_MS) {
            s_touch_state = TOUCH_PRESSED;
            s_touch_cnt   = 0;
        }
        break;

    case TOUCH_PRESSED:
        s_touch_cnt++;
        if (!touch_is_pressed()) {
            /* 短按松手 */
            s_touch_state = TOUCH_IDLE;
            s_touch_cnt   = 0;

            if (!s_long_active) {
                /* 处理短按（单击或双击的一部分） */
                handle_short_press(current_time);
            }
            s_long_active = false;
        } else if (s_touch_cnt >= TOUCH_LONG_MS) {
            /* 达到长按阈值 */
            s_touch_state = TOUCH_LONG_PRESS;
            s_touch_cnt   = 0;
            s_long_active = true;

            /* 长按时取消双击等待 */
            s_waiting_for_double = false;
            s_click_processed = true;

            /* 只有开灯后才处理长按调光 */
            if (led_handle_is_on()) {
                led_touch_long_press_start();
            } else {
                my_printf("touch: long press ignored (led off)\n");
            }
        }
        break;

    case TOUCH_LONG_PRESS:
        if (!touch_is_pressed()) {
            /* 长按松手 */
            s_long_active = false;
            s_touch_state = TOUCH_IDLE;
            s_touch_cnt   = 0;
            if (led_handle_is_on()) {
                led_touch_long_press_stop();
            }
        } else if (s_long_active && led_handle_is_on()) {
            led_touch_long_press_tick();
        }
        break;

    default:
        s_touch_state = TOUCH_IDLE;
        break;
    }
}
