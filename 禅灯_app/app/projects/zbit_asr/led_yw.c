#include "include.h"
#include "led_ctrl.h"
#include "port_touch.h"

/*===========================================================================
 * 硬件 IO 与 PWM 配置（SOP8 产品板）
 *   PIN5 / 暖白(YLED): 键合 PA4/PA5/PA6 → 仅 PA4 出 PWM0，PA5/PA6 高阻
 *   PIN4 / 正白(WLED): 键合 PA7/PA8/PA9 → 仅 PA7 出 PWM1，PA8/PA9 高阻
 *
 * 注意：同脚多 pad 只能一路驱动。若 PA7+PA8 同时映射 PWM，两路波形会在
 * 同一物理脚上互抢，容易出现类似「呼吸」的闪烁/双灯同亮。
 *
 * LED_WHITE_GPIO_FORCE=1：正白改 GPIO 推挽验证硬件；确认后改回 0。
 *===========================================================================*/
#define LED_YELLOW           IO_PA4
#define LED_WHITE            IO_PA7

#define YELLOW_NUM           0
#define WHITE_NUM            1

#define TIMER_FREC           25000
#define LED_WHITE_GPIO_FORCE 0        /* 1=正白 GPIO 直驱验证；0=PWM */

extern void timer_pwm_init(u8 io1, u8 io2, u8 io3, u8 io4, u32 fre, u32 duty);
extern void set_timer_pwm_duty(u8 num, u32 duty);

/*
 * SDK 的 set_timer_pwm_duty(0) 会写成「满占空比 + 反相」。
 * 关断改为：duty=0 且清反相位，避免关不断导致双灯同亮。
 *
 * 长按亮→暗闪：优先按下面两点缓解（与呼吸共用写入口）
 * 1) 周期前段再改 DUTY，减轻中途改比较值导致的「多亮一截」
 * 2) 值未变则不写寄存器，避免每步重清另一路 TMR2CON
 */
static u32 s_pwm_raw_last[2] = {0, 0};
static u8  s_pwm_raw_valid   = 0; /* bit0=PWM0, bit1=PWM1 已缓存 */

/* 等到 PWM 周期前约 1/8，再改比较值；超时则直接写，避免卡死 */
static void led_pwm_wait_period_head(void)
{
    u32 pr = TMR2PR;
    u32 lim;
    u32 spins = 0;

    if (pr < 16) {
        return;
    }
    lim = pr >> 3;
    while ((TMR2CNT > lim) && (spins < 8000)) {
        spins++;
    }
}

static void led_pwm_channel_off(u8 num)
{
    if (num <= 1) {
        if ((s_pwm_raw_valid & (1u << num)) && (s_pwm_raw_last[num] == 0)) {
            return;
        }
        led_pwm_wait_period_head();
    }

    if (num == 0) {
        TMR2DUTY0 = 0;
        TMR2CON &= ~BIT(17);
        s_pwm_raw_last[0] = 0;
        s_pwm_raw_valid  |= BIT(0);
    } else if (num == 1) {
        TMR2DUTY1 = 0;
        TMR2CON &= ~BIT(19);
        s_pwm_raw_last[1] = 0;
        s_pwm_raw_valid  |= BIT(1);
    } else if (num == 2) {
        TMR2DUTY2 = 0;
        TMR2CON &= ~BIT(21);
    } else if (num == 3) {
        TMR2DUTY3 = 0;
        TMR2CON &= ~BIT(23);
    }
}

static void led_pwm_channel_on(u8 num, u32 duty)
{
    if (duty == 0) {
        led_pwm_channel_off(num);
        return;
    }
    set_timer_pwm_duty(num, duty);
    if (num <= 1) {
        /* 百分比接口：缓存失效，避免与 raw 路径误判相同 */
        s_pwm_raw_valid &= (u8)~(1u << num);
    }
}

/* 呼吸灯细粒度：直接写比较寄存器（避免 0~100% 只有 100 级的阶梯感） */
static void led_pwm_channel_raw(u8 num, u32 duty_raw)
{
    if (num > 1) {
        return;
    }
    if (duty_raw == 0) {
        led_pwm_channel_off(num);
        return;
    }
    if (duty_raw > TMR2PR) {
        duty_raw = TMR2PR;
    }
    if ((s_pwm_raw_valid & (1u << num)) && (s_pwm_raw_last[num] == duty_raw)) {
        return;
    }

    led_pwm_wait_period_head();

    if (num == 0) {
        TMR2DUTY0 = duty_raw;
        TMR2CON &= ~BIT(17);
    } else {
        TMR2DUTY1 = duty_raw;
        TMR2CON &= ~BIT(19);
    }
    s_pwm_raw_last[num] = duty_raw;
    s_pwm_raw_valid    |= (u8)(1u << num);
}

/* SOP8 同脚键合的未用 pad：数字输入高阻，禁止再挂 PWM/推挽 */
static void sop8_led_bond_hiz(void)
{
    /* PIN5：PA5/PA6 与 PA4 同键合 */
    GPIOAFEN &= ~(BIT(5) | BIT(6));
    GPIOADE  |=  (BIT(5) | BIT(6));
    GPIOADIR |=  (BIT(5) | BIT(6));
    GPIOAPU  &= ~(BIT(5) | BIT(6));
    GPIOAPD  &= ~(BIT(5) | BIT(6));

    /* PIN4：PA8/PA9 与 PA7 同键合（切勿再给 PA8 映射 PWM） */
    GPIOAFEN &= ~(BIT(8) | BIT(9));
    GPIOADE  |=  (BIT(8) | BIT(9));
    GPIOADIR |=  (BIT(8) | BIT(9));
    GPIOAPU  &= ~(BIT(8) | BIT(9));
    GPIOAPD  &= ~(BIT(8) | BIT(9));
}

#if LED_WHITE_GPIO_FORCE
static void white_gpio_set(u8 on)
{
    GPIOAFEN &= ~BIT(LED_WHITE - 1);
    GPIOADE  |=  BIT(LED_WHITE - 1);
    GPIOADIR &= ~BIT(LED_WHITE - 1);
    GPIOADRV |=  BIT(LED_WHITE - 1);

    if (on) {
        GPIOASET = BIT(LED_WHITE - 1);
    } else {
        GPIOACLR = BIT(LED_WHITE - 1);
    }
}
#endif

static void led_pwm_set_yellow(u32 duty)
{
    led_pwm_channel_on(YELLOW_NUM, duty);
}

static void led_pwm_set_white(u32 duty)
{
#if LED_WHITE_GPIO_FORCE
    white_gpio_set(duty > 0);
#else
    led_pwm_channel_on(WHITE_NUM, duty);
#endif
}

/*===========================================================================
 * 亮度档位 (6档线性: 1=最暗 ~ 6=最亮，等间距 PWM 占空比)
 * 注: 高电平点亮模式, 数值越大越亮
 *===========================================================================*/
typedef enum {
    BRIGHTNESS_OFF     = 0,
    BRIGHTNESS_LEVEL_1,
    BRIGHTNESS_LEVEL_2,
    BRIGHTNESS_LEVEL_3,
    BRIGHTNESS_LEVEL_4,
    BRIGHTNESS_LEVEL_5,
    BRIGHTNESS_LEVEL_6,
    BRIGHTNESS_MAX
} led_brightness_e;

/* 6 级线性: 17/33/50/67/83/100 (100*i/6 四舍五入) */
static const int brightness_level_arr[BRIGHTNESS_MAX] = {0, 17, 33, 50, 67, 83, 100};

/*
 * 呼吸灯 / 长按调光：512 级连续亮度，谷底微亮
 * 单程 (20→512)：492×5ms ≈ 2.46s（目标 2~2.5s）
 * 长按：到边界按住停住；任意松手后，下次长按换向
 * 语音仍按 6 档跳变
 */
#define BREATH_STEP_MS           5
#define BREATH_MAX               512
#define BREATH_MIN               20     /* ≈4%，不完全灭 */
#define LONG_PRESS_STEP_MS       5      /* 与呼吸同节奏 */

/*
 * 混合档：每路占空比 = 当前亮度 × 下表比例 / 100
 * 依据实测（黄/白/双色电流）校准：同档双色总电流 ≈ 单色黄/白
 *   1档 /2 已≈单色；3~6档需抬到约 70%
 */
static const u8 mix_duty_pct[BRIGHTNESS_MAX] = {
    0,
    50,   /* 1档 */
    64,   /* 2档 */
    72,   /* 3档 */
    73,   /* 4档 */
    73,   /* 5档 */
    70,   /* 6档 */
};

/* dim: BREATH_MIN~BREATH_MAX → PWM 比较值 */
static u32 led_dim_to_raw(int dim_val)
{
    u32 raw;

    if (dim_val < BREATH_MIN) {
        dim_val = BREATH_MIN;
    } else if (dim_val > BREATH_MAX) {
        dim_val = BREATH_MAX;
    }
    raw = (u32)(TMR2PR + 1) * (u32)dim_val / BREATH_MAX;
    if (raw == 0) {
        raw = 1;
    }
    return raw;
}

/*===========================================================================
 * 颜色模式 (触摸5态 + 语音变颜色4态)
 *===========================================================================*/
typedef enum {
    COLOR_NONE         = 0,
    COLOR_YELLOW,       // 暖白
    COLOR_WHITE,        // 正白
    COLOR_YELLOW_WHITE, // 暖白+正白混合
} led_color_index_e;

typedef enum {
    LED_MODE_WARM      = 0,   // 暖白静态
    LED_MODE_COLD,            // 正白静态
    LED_MODE_MIX,             // 暖白+正白混合静态
    LED_MODE_BREATH,          // 呼吸灯（固定暖白）
} led_color_mode_e;

/*===========================================================================
 * LED 控制状态结构
 *===========================================================================*/
typedef struct {
    bool              on;              // LED 是否亮
    led_color_index_e color_index;     // 当前静态颜色
    led_brightness_e  brightness_level;// 亮度档位（语音跳档用）
    int               brightness_val;  // 连续亮度 BREATH_MIN~BREATH_MAX
} led_handle_t;

static led_handle_t   led_handle = {0};

static void led_sync_level_from_dim(led_handle_t *handle)
{
    int pct;
    int i;
    int best;
    int best_d;
    int d;

    if (handle == NULL) {
        return;
    }
    pct = (int)((u32)handle->brightness_val * 100UL / BREATH_MAX);
    best = BRIGHTNESS_LEVEL_1;
    best_d = brightness_level_arr[BRIGHTNESS_LEVEL_1] - pct;
    if (best_d < 0) {
        best_d = -best_d;
    }
    for (i = BRIGHTNESS_LEVEL_2; i <= BRIGHTNESS_LEVEL_6; i++) {
        d = brightness_level_arr[i] - pct;
        if (d < 0) {
            d = -d;
        }
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    handle->brightness_level = (led_brightness_e)best;
}

static void led_set_dim_from_level(led_handle_t *handle, led_brightness_e level)
{
    int val;

    if (handle == NULL || level <= BRIGHTNESS_OFF || level >= BRIGHTNESS_MAX) {
        return;
    }
    handle->brightness_level = level;
    val = brightness_level_arr[level] * BREATH_MAX / 100;
    if (val < BREATH_MIN) {
        val = BREATH_MIN;
    } else if (val > BREATH_MAX) {
        val = BREATH_MAX;
    }
    handle->brightness_val = val;
}

/*
 * 混合比例：在 6 档校准点之间按连续亮度线性插值。
 * 避免长按过档时直接跳 mix_duty_pct（尤其 6档70%↔5档73%）造成比单色更闪。
 */
static u8 led_mix_pct_from_dim(int dim_val)
{
    int pct;
    int i;
    int p0, p1;
    int m0, m1;
    int span;
    int mix;

    if (dim_val < BREATH_MIN) {
        dim_val = BREATH_MIN;
    } else if (dim_val > BREATH_MAX) {
        dim_val = BREATH_MAX;
    }
    pct = (int)((u32)dim_val * 100UL / BREATH_MAX);

    if (pct <= brightness_level_arr[BRIGHTNESS_LEVEL_1]) {
        return mix_duty_pct[BRIGHTNESS_LEVEL_1];
    }
    if (pct >= brightness_level_arr[BRIGHTNESS_LEVEL_6]) {
        return mix_duty_pct[BRIGHTNESS_LEVEL_6];
    }

    for (i = BRIGHTNESS_LEVEL_1; i < BRIGHTNESS_LEVEL_6; i++) {
        p0 = brightness_level_arr[i];
        p1 = brightness_level_arr[i + 1];
        if (pct >= p0 && pct <= p1) {
            m0 = (int)mix_duty_pct[i];
            m1 = (int)mix_duty_pct[i + 1];
            span = p1 - p0;
            if (span <= 0) {
                return (u8)m0;
            }
            mix = m0 + (m1 - m0) * (pct - p0) / span;
            if (mix < 0) {
                mix = 0;
            } else if (mix > 100) {
                mix = 100;
            }
            return (u8)mix;
        }
    }
    return mix_duty_pct[BRIGHTNESS_LEVEL_6];
}

static u32 led_mix_raw_from_dim(led_handle_t *handle, u32 raw)
{
    u32 mix;
    u8  pct;

    if (handle == NULL) {
        return 0;
    }
    led_sync_level_from_dim(handle); /* 档位标签仍同步，供语音/打印 */
    pct = led_mix_pct_from_dim(handle->brightness_val);
    mix = raw * pct / 100;
    if (raw > 0 && mix == 0) {
        mix = 1;
    }
    return mix;
}

/* 颜色模式状态（触摸/语音共用） */
static led_color_mode_e  g_color_mode       = LED_MODE_WARM;

/* 呼吸灯状态：固定暖白；谷底保留微亮 */
static bool              g_in_breath        = false;
static int               g_breath_val       = BREATH_MIN;  // BREATH_MIN~BREATH_MAX
static int               g_breath_step      = 1;   // +1 渐亮, -1 渐暗
static uint8_t           g_breath_timer     = 0;   // BREATH_STEP_MS 计数

/* 长按调光：连续亮度；任意松手后下次长按换向 */
static int               g_bright_dir           = 1;   // +1 向亮, -1 向暗
static uint16_t          g_longpress_timer      = 0;   // LONG_PRESS_STEP_MS 计数
static bool              g_reverse_next_long    = false; // 松手 → 下次长按换向

/* 语音使能标志（上电/从L3M唤醒后默认 false，首次触摸点亮后置 true） */
static bool              g_voice_enabled    = false;

/* 待机进 L3M 超时：30 分钟
 * 适用：
 *   1) 首次上电未操作，灯灭即开始计时
 *   2) 触摸/语音关灯后，灯灭再开始计时
 */
#define LED_SLEEP_TIMEOUT_MS    (30UL * 60 * 1000)

static volatile uint32_t g_sleep_cnt        = 0;
static volatile bool     g_need_sleep       = false;

/* PWM 初始化完成标志：led_ctrl_init() 结束前为 false，防止 ISR 提前操作 PWM */
static volatile bool     g_led_pwm_ready    = false;

/* 旧版定时器相关（保留 led_tick 供 port_init.c 外部 extern，暂不使用） */
int  led_tick     = 0;
volatile bool time_set_flag = false;

/*===========================================================================
 * 内部辅助函数
 *===========================================================================*/

/* 将所有 PWM 通道直接设为 0 */
static inline void pwm_all_off(void)
{
    if (!g_led_pwm_ready) {
        return;
    }
    led_pwm_set_yellow(0);
    led_pwm_set_white(0);
}

/* 按当前 handle 连续亮度刷新 PWM（呼吸灯模式时跳过，由 breath_tick 管理） */
static int led_update(led_handle_t *handle)
{
    u32 raw;

    if (handle == NULL) {
        return 1;
    }
    if (g_in_breath) {
        return 0;   // 呼吸灯期间 breath_tick 独立管理 PWM
    }

    if (!handle->on) {
        pwm_all_off();
        return 0;
    }

    if (handle->brightness_val < BREATH_MIN) {
        handle->brightness_val = BREATH_MIN;
    } else if (handle->brightness_val > BREATH_MAX) {
        handle->brightness_val = BREATH_MAX;
    }

    raw = led_dim_to_raw(handle->brightness_val);

    switch (handle->color_index) {
    case COLOR_YELLOW_WHITE: {
        u32 mix = led_mix_raw_from_dim(handle, raw);
        led_pwm_channel_raw(YELLOW_NUM, mix);
        led_pwm_channel_raw(WHITE_NUM, mix);
        break;
    }
    case COLOR_YELLOW:
        /* 先关正白，再开暖白，避免关断残留导致双灯同亮 */
        led_pwm_channel_raw(WHITE_NUM, 0);
        led_pwm_channel_raw(YELLOW_NUM, raw);
        break;
    case COLOR_WHITE:
        led_pwm_channel_raw(YELLOW_NUM, 0);
        led_pwm_channel_raw(WHITE_NUM, raw);
        break;
    default:
        led_pwm_channel_raw(WHITE_NUM, 0);
        led_pwm_channel_raw(YELLOW_NUM, raw);
        break;
    }
    return 0;
}

static inline void led_yw_on(led_handle_t *handle)
{
    if (!handle) return;
    handle->on = true;
    led_update(handle);
}

static inline void led_yw_off(led_handle_t *handle)
{
    if (!handle) return;
    handle->on = false;
    led_update(handle);
}

/* 进入呼吸灯：固定暖白，从微亮开始渐亮 */
static void enter_breath_mode(void)
{
    g_in_breath            = true;
    led_handle.color_index = COLOR_YELLOW;  /* 呼吸固定暖白 */
    g_breath_val           = BREATH_MIN;
    g_breath_step          = 1;
    g_breath_timer         = 0;
    /* 先关掉正白，避免残留 */
    led_pwm_set_white(0);
}

/* 退出呼吸灯模式，恢复指定静态颜色 */
static void exit_breath_mode(led_color_index_e color)
{
    g_in_breath = false;
    led_handle.color_index = color;
    led_update(&led_handle);
}

/*===========================================================================
 * 初始化
 *===========================================================================*/
void led_ctrl_init(void)
{
    g_led_pwm_ready = false;

    led_handle.on               = false;               // 上电后 LED 灭，等待触摸开灯
    led_handle.color_index      = COLOR_YELLOW;        // 默认暖白（首次触摸时使用）
    led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_4);

    g_color_mode    = LED_MODE_WARM;
    g_in_breath     = false;
    g_voice_enabled = false;
    g_need_sleep    = false;
    g_sleep_cnt     = 0;
    g_bright_dir           = 1;   /* 默认先向最亮 */
    g_longpress_timer      = 0;
    g_reverse_next_long    = false;

    /* SOP8：仅 PWM0=PA4 暖白、PWM1=PA7 正白；同脚 PA5/6/8/9 高阻 */
    GLOBAL_INT_DISABLE();
    sop8_led_bond_hiz();
#if LED_WHITE_GPIO_FORCE
    timer_pwm_init(LED_YELLOW, 0, 0, 0, TIMER_FREC, 0);
    white_gpio_set(0);
#else
    timer_pwm_init(LED_YELLOW, LED_WHITE, 0, 0, TIMER_FREC, 0);
#endif
    GPIOADRV |= BIT(LED_YELLOW - 1);
#if !LED_WHITE_GPIO_FORCE
    GPIOADRV |= BIT(LED_WHITE - 1);
#endif

    g_led_pwm_ready = true;
    GLOBAL_INT_RESTORE();

    /* 再次确保同脚未用 pad 高阻（timer_pwm_init 可能改过 GPIO 功能位） */
    sop8_led_bond_hiz();
    pwm_all_off();
    my_printf("led_init: Y=PA4 W=PA7 only (bond HiZ)\n");
}

bool led_pwm_is_ready(void)
{
    return g_led_pwm_ready;
}

/*===========================================================================
 * 语音控制接口（KWS 词条 ID → 动作）
 *
 * KWS ID 映射（与 libkws 词条一致）：
 *   0: 开灯    1: 关灯    2: 瓜灯(→关灯)  3: 光灯(→关灯)
 *   4: 变颜色  5: 亮一点  6: 暗一点
 *   7: 调到最亮  8: 调到最暗
 *===========================================================================*/
int led_ctrl(uint16_t cmd)
{
    if (!g_voice_enabled) {
        /* 未触摸开灯前：语音识别可打印，但不控制 LED */
        return -1;
    }

    /* 灯灭待机时：只有「开灯」算有效操作并清零计时；
     * 其它词条（含误触发的关灯/调光）不清零，否则永远进不了 L3M */
    if (led_handle.on || cmd == 0) {
        g_sleep_cnt  = 0;
        g_need_sleep = false;
    }

    switch (cmd) {
    case 0:  /* 开灯：关灯后再开一律暖白（与触摸从灭到亮一致） */
        if (!led_handle.on) {
            g_in_breath = false;
            g_color_mode = LED_MODE_WARM;
            led_handle.color_index = COLOR_YELLOW;
            led_yw_on(&led_handle);
            my_printf("voice: warm on duty=%d\n", led_handle.brightness_val);
        }
        break;

    case 1:  /* 关灯 */
    case 2:  /* 瓜灯 → 关灯 */
    case 3:  /* 光灯 → 关灯 */
        g_in_breath = false;
        g_color_mode = LED_MODE_WARM;  /* 下次开灯从暖白开始 */
        led_yw_off(&led_handle);
        break;

    case 4:  /* 变颜色：4种模式循环（WARM→COLD→MIX→BREATH→WARM） */
        if (!led_handle.on) break;
        switch (g_color_mode) {
        case LED_MODE_WARM:
            g_color_mode = LED_MODE_COLD;
            exit_breath_mode(COLOR_WHITE);
            my_printf("voice: cold(PIN4)\n");
            break;
        case LED_MODE_COLD:
            g_color_mode = LED_MODE_MIX;
            exit_breath_mode(COLOR_YELLOW_WHITE);
            my_printf("voice: mix\n");
            break;
        case LED_MODE_MIX:
            g_color_mode = LED_MODE_BREATH;
            enter_breath_mode();
            my_printf("voice: breath warm\n");
            break;
        case LED_MODE_BREATH:
            g_color_mode = LED_MODE_WARM;
            exit_breath_mode(COLOR_YELLOW);
            my_printf("voice: warm(PIN5)\n");
            break;
        default:
            g_color_mode = LED_MODE_WARM;
            exit_breath_mode(COLOR_YELLOW);
            break;
        }
        break;

    case 5:  /* 亮一点 */
        if (!led_handle.on || g_in_breath) break;
        if (led_handle.brightness_level < BRIGHTNESS_LEVEL_6) {
            led_set_dim_from_level(&led_handle,
                                   (led_brightness_e)(led_handle.brightness_level + 1));
        }
        led_update(&led_handle);
        my_printf("voice: brighter lvl=%d\n", (int)led_handle.brightness_level);
        break;

    case 6:  /* 暗一点 */
        if (!led_handle.on || g_in_breath) break;
        if (led_handle.brightness_level > BRIGHTNESS_LEVEL_1) {
            led_set_dim_from_level(&led_handle,
                                   (led_brightness_e)(led_handle.brightness_level - 1));
        }
        led_update(&led_handle);
        my_printf("voice: darker lvl=%d\n", (int)led_handle.brightness_level);
        break;

    case 7:  /* 调到最亮 */
        if (!led_handle.on || g_in_breath) break;
        led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_6);
        led_update(&led_handle);
        my_printf("voice: max lvl=6\n");
        break;

    case 8:  /* 调到最暗（最暗档，不关灯） */
        if (!led_handle.on || g_in_breath) break;
        led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_1);
        led_update(&led_handle);
        my_printf("voice: min lvl=1\n");
        break;

    default:
        break;
    }

    return 0;
}

/*===========================================================================
 * 触摸单击：5态循环
 *   OFF → WARM → COLD → MIX → BREATH → OFF
 *   第一次点亮同时使能语音功能
 *===========================================================================*/
void led_touch_click(void)
{
    if (!g_led_pwm_ready) {
        return;
    }

    /* 任何触摸动作重置睡眠计时器 */
    g_sleep_cnt  = 0;
    g_need_sleep = false;

    if (!led_handle.on) {
        /* 关机 → 暖白（第1次点亮） */
        g_color_mode           = LED_MODE_WARM;
        g_in_breath            = false;
        led_handle.color_index = COLOR_YELLOW;
        led_yw_on(&led_handle);
        g_voice_enabled = true;  /* 首次触摸开灯后语音才使能 */
        g_bright_dir         = 1;     /* 默认先向最亮 */
        g_reverse_next_long  = false;
        my_printf("touch: warm(PIN5) on\n");
    } else {
        switch (g_color_mode) {
        case LED_MODE_WARM:    /* 暖白 → 正白 */
            g_color_mode = LED_MODE_COLD;
            exit_breath_mode(COLOR_WHITE);
            my_printf("touch: cold(PIN4) lvl=%d\n", (int)led_handle.brightness_level);
            break;
        case LED_MODE_COLD:    /* 正白 → 混合 */
            g_color_mode = LED_MODE_MIX;
            exit_breath_mode(COLOR_YELLOW_WHITE);
            my_printf("touch: mix(PIN4+5)\n");
            break;
        case LED_MODE_MIX:     /* 混合 → 呼吸灯 */
            g_color_mode = LED_MODE_BREATH;
            enter_breath_mode();
            my_printf("touch: breath warm\n");
            break;
        case LED_MODE_BREATH:  /* 呼吸灯 → 关机 */
            g_color_mode = LED_MODE_WARM;  /* 下次触摸/开灯从暖白开始 */
            g_in_breath  = false;
            led_yw_off(&led_handle);
            my_printf("touch: off (next on=warm)\n");
            break;
        default:
            g_color_mode           = LED_MODE_WARM;
            g_in_breath            = false;
            led_handle.color_index = COLOR_YELLOW;
            led_yw_on(&led_handle);
            g_voice_enabled = true;
            break;
        }
        /* 短按切色不改调光方向 / 边界换向标记 */
    }
}

/*===========================================================================
 * 长按调光
 * 1) 从当前亮度按当前方向连续调（约 2.5s 暗↔亮）
 * 2) 中途松手：亮度停住；下次长按换向
 * 3) 到最亮/最暗：停在边界，按住不往回走；松手后下次长按换向
 *===========================================================================*/
void led_touch_long_press_start(void)
{
    bool need_rev;

    g_longpress_timer = 0;

    /* 上次松手换向标记，或已在当前方向尽头 → 本次长按换向 */
    need_rev = g_reverse_next_long;
    g_reverse_next_long = false;
    if (!need_rev) {
        if ((g_bright_dir > 0 && led_handle.brightness_val >= BREATH_MAX) ||
            (g_bright_dir < 0 && led_handle.brightness_val <= BREATH_MIN)) {
            need_rev = true;
        }
    }
    if (need_rev) {
        g_bright_dir = -g_bright_dir;
    }

    my_printf("longpress: start dim=%d dir=%d\n",
              led_handle.brightness_val, g_bright_dir);
}

void led_touch_long_press_tick(void)
{
    int next;

    if (!g_led_pwm_ready || !led_handle.on || g_in_breath) {
        return;
    }

    g_longpress_timer++;
    if (g_longpress_timer < LONG_PRESS_STEP_MS) {
        return;
    }
    g_longpress_timer = 0;

    g_sleep_cnt  = 0;
    g_need_sleep = false;

    next = led_handle.brightness_val + g_bright_dir;
    if (next > BREATH_MAX) {
        next = BREATH_MAX;          /* 停在最亮，本会话不反向 */
    } else if (next < BREATH_MIN) {
        next = BREATH_MIN;          /* 停在最暗，本会话不反向 */
    }
    /* 已在边界：不再空刷 PWM，避免原地反复写寄存器 */
    if (next == led_handle.brightness_val) {
        return;
    }
    led_handle.brightness_val = next;

    led_sync_level_from_dim(&led_handle);
    led_update(&led_handle);
}

void led_touch_long_press_stop(void)
{
    g_longpress_timer = 0;
    led_sync_level_from_dim(&led_handle);
    /* 中途或边界松手：下次长按一律换向 */
    g_reverse_next_long = true;
    my_printf("longpress: stop lvl=%d dim=%d dir=%d next_rev=1\n",
              (int)led_handle.brightness_level, led_handle.brightness_val,
              g_bright_dir);
}

/*===========================================================================
 * 呼吸灯 tick（主循环约 1ms 补跑）
 * 固定暖白；BREATH_MIN↔BREATH_MAX；单程约 2.46s（5ms/级）
 *===========================================================================*/
void led_breath_tick(void)
{
    u32 raw;

    if (!g_led_pwm_ready || !g_in_breath) {
        return;
    }

    g_breath_timer++;
    if (g_breath_timer < BREATH_STEP_MS) {
        return;
    }
    g_breath_timer = 0;

    g_breath_val += g_breath_step;
    if (g_breath_val >= BREATH_MAX) {
        g_breath_val  = BREATH_MAX;
        g_breath_step = -1;
    } else if (g_breath_val <= BREATH_MIN) {
        g_breath_val  = BREATH_MIN;
        g_breath_step = 1;
    }

    raw = (u32)(TMR2PR + 1) * (u32)g_breath_val / BREATH_MAX;
    if (raw == 0) {
        raw = 1;
    }

    led_pwm_channel_raw(YELLOW_NUM, raw);
    led_pwm_channel_raw(WHITE_NUM, 0);
}

/*===========================================================================
 * 睡眠计时 tick（每 1ms 调用）
 * 条件：LED 灭（含首次上电未触摸、以及关灯后待机）
 *===========================================================================*/
void led_sleep_tick(void)
{
    if (!led_handle.on) {
        g_sleep_cnt++;
        if (g_sleep_cnt >= LED_SLEEP_TIMEOUT_MS) {
            if (!g_need_sleep) {
                my_printf("sleep: timeout %lums -> L3M (voice=%d)\n",
                          (unsigned long)LED_SLEEP_TIMEOUT_MS,
                          g_voice_enabled ? 1 : 0);
            }
            g_need_sleep = true;
        } else if ((g_sleep_cnt % 30000UL) == 0) {
            /* 每 30s 打一次进度，方便确认计时在跑 */
            my_printf("sleep: cnt=%lu / %lu (voice=%d)\n",
                      (unsigned long)g_sleep_cnt,
                      (unsigned long)LED_SLEEP_TIMEOUT_MS,
                      g_voice_enabled ? 1 : 0);
        }
    } else {
        /* 灯亮：不计时 */
        g_sleep_cnt  = 0;
        g_need_sleep = false;
    }
}

/*===========================================================================
 * 语音使能控制
 *===========================================================================*/
void led_voice_enable(bool en)
{
    g_voice_enabled = en;
    if (!en) {
        g_sleep_cnt  = 0;
        g_need_sleep = false;
    }
}

bool led_voice_is_enabled(void)
{
    return g_voice_enabled;
}

/*===========================================================================
 * 睡眠标志查询（由 bsp_asr.c 在每帧回调开始处轮询）
 *===========================================================================*/
bool led_need_sleep(void)
{
    return g_need_sleep;
}

/* 主循环轮询：按 tick_get() 补跑触摸/呼吸/睡眠逻辑（禁止在 ISR 中调用） */
void led_app_process(void)
{
    static u32 last_tick;
    u32 n;

    if (!g_led_pwm_ready) {
        return;
    }

    if (last_tick == 0) {
        last_tick = tick_get();
        return;
    }

    /* 每 1ms 补跑一轮，最多连续补 50 轮防止卡死 */
    for (n = 0; n < 50 && tick_check_expire(last_tick, 1); n++) {
        last_tick += 1;
        port_touch_scan();
        led_breath_tick();
        led_sleep_tick();
    }
}
