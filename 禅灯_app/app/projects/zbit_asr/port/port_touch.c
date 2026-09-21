#include "include.h"
#include "led_ctrl.h"
#include "port_touch.h"

/*===========================================================================
 * QM301 OUT → PA10
 * 短按松手切态；长按 ≥800ms 连续调光，松手即停；到边界松手后下次长按换向。
 *===========================================================================*/
#define TOUCH_IO            IO_PA10
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

void port_touch_scan(void)
{
    if (!led_pwm_is_ready()) {
        return;
    }

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
            s_touch_state = TOUCH_IDLE;
            s_touch_cnt   = 0;
            s_long_active = false;
            led_touch_click();
        } else if (s_touch_cnt >= TOUCH_LONG_MS) {
            s_touch_state = TOUCH_LONG_PRESS;
            s_touch_cnt   = 0;
            if (!led_voice_is_enabled()) {
                /* 未开灯前：长按也开灯（方便调试） */
                led_touch_click();
                s_long_active = false;
            } else {
                s_long_active = true;
                led_touch_long_press_start();
            }
        }
        break;

    case TOUCH_LONG_PRESS:
        if (!touch_is_pressed()) {
            led_touch_long_press_stop();
            s_long_active = false;
            s_touch_state = TOUCH_IDLE;
            s_touch_cnt   = 0;
        } else if (s_long_active) {
            led_touch_long_press_tick();
        }
        break;

    default:
        s_touch_state = TOUCH_IDLE;
        break;
    }
}
