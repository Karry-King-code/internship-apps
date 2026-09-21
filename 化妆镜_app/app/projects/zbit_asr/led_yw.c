#include "include.h"
#include "led_ctrl.h"
#include "port_touch.h"

/*===========================================================================
 * 硬件 IO 与 PWM 配置（SOP8 产品板）
 *   PIN4 / 正白(WLED): 键合 PA7/PA8/PA9 → 仅 PA7 出 PWM1，PA8/PA9 高阻
 *   PIN1 / 暖白(YLED): 键合 PA10/PA11 → 仅 PA11 出 PWM0，PA10 高阻
 *   PIN6 /  红灯(RED): 键合 PA1/PA2/PA3->仅PA1出PWM2，PA2/PA3 高阻
 * 注意：同脚多 pad 只能一路驱动。若 PA7+PA8 同时映射 PWM，两路波形会在
 * 同一物理脚上互抢，容易出现类似「呼吸」的闪烁/双灯同亮。
 *
 * LED_WHITE_GPIO_FORCE=1：正白改 GPIO 推挽验证硬件；确认后改回 0。
 *===========================================================================*/
 #define LED_FLASH_TIMES     240

#define LED_YELLOW           IO_PA7        //IO_PA11
#define LED_WHITE            IO_PA11         //IO_PA7
#define LED_RED              IO_PA1

#define YELLOW_NUM           0
#define WHITE_NUM            1
#define RED_NUM              2

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
    if (duty_raw == 0) {
        led_pwm_channel_off(num);
        return;
    }
    if (duty_raw > TMR2PR) {
        duty_raw = TMR2PR;
    }
  /* 黄白通道使用缓存优化 */
  if (num <= 1) {
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
     return;
  }
  if (num == 2) {
        led_pwm_wait_period_head();
        TMR2DUTY2 = duty_raw;
        TMR2CON &= ~BIT(21);  /* 清除反相使能 */
  }
}

/* SOP8 同脚键合的未用 pad：数字输入高阻，禁止再挂 PWM/推挽 */
static void sop8_led_bond_hiz(void)
{
    /* PIN1：PA11 与 PA10 同键合 */
    GPIOAFEN &= ~(BIT(10));
    GPIOADE  |=  (BIT(10));
    GPIOADIR |=  (BIT(10));
    GPIOAPU  &= ~(BIT(10));
    GPIOAPD  &= ~(BIT(10));

    /* PIN4：PA8/PA9 与 PA7 同键合（切勿再给 PA8 映射 PWM） */
    GPIOAFEN &= ~(BIT(8) | BIT(9));
    GPIOADE  |=  (BIT(8) | BIT(9));
    GPIOADIR |=  (BIT(8) | BIT(9));
    GPIOAPU  &= ~(BIT(8) | BIT(9));
    GPIOAPD  &= ~(BIT(8) | BIT(9));

    /* PIN6：PA2/PA3 与 PA1 同键合（切勿再给 PA2 映射 PWM） */
    GPIOAFEN &= ~(BIT(2) | BIT(3));
    GPIOADE  |=  (BIT(2) | BIT(3));
    GPIOADIR |=  (BIT(2) | BIT(3));
    GPIOAPU  &= ~(BIT(2) | BIT(3));
    GPIOAPD  &= ~(BIT(2) | BIT(3));
}

/* 上电瞬间把三路灯脚钳为 GPIO 推挽输出低电平，
 * 避免 timer_pwm_init 把脚切成 PWM 复用输出前电平不定导致的灯闪 */
static void sop8_led_gpio_low(void)
{
    GPIOAFEN &= ~BIT(LED_YELLOW - 1);
    GPIOADE  |=  BIT(LED_YELLOW - 1);
    GPIOADIR &= ~BIT(LED_YELLOW - 1);
    GPIOACLR  =  BIT(LED_YELLOW - 1);

    GPIOAFEN &= ~BIT(LED_WHITE - 1);
    GPIOADE  |=  BIT(LED_WHITE - 1);
    GPIOADIR &= ~BIT(LED_WHITE - 1);
    GPIOACLR  =  BIT(LED_WHITE - 1);

    GPIOAFEN &= ~BIT(LED_RED - 1);
    GPIOADE  |=  BIT(LED_RED - 1);
    GPIOADIR &= ~BIT(LED_RED - 1);
    GPIOACLR  =  BIT(LED_RED - 1);
}

/* PIN6 = PA1/PA2/PA3 同脚键合，现作为 MIC 供电（硬件已改）。
 * 把 PA1 配成 GPIO 推挽输出高给 MIC 供电；PA2/PA3 保持高阻输入（同脚由 PA1 驱动）。
 * 注意：PA1 不能再复用为红灯 PWM，否则会与 MIC 供电互抢。 */
static void mic_power_on(void)
{
    GPIOAFEN &= ~BIT(LED_RED - 1);   /* PA1 关复用 → GPIO */
    GPIOADE  |=  BIT(LED_RED - 1);   /* 数字使能 */
    GPIOADIR &= ~BIT(LED_RED - 1);   /* 方向 = 输出 */
    GPIOADRV |=  BIT(LED_RED - 1);   /* 高驱动能力 */
    GPIOAPU  &= ~BIT(LED_RED - 1);   /* 去上拉 */
    GPIOAPD  &= ~BIT(LED_RED - 1);   /* 去下拉 */
    GPIOASET  =  BIT(LED_RED - 1);   /* 输出高 → MIC 供电 */
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
 * 颜色模式 (触摸4态)
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
typedef enum {
    LED_TIMER_DEFAULT = 0,
    LED_TIMER_10MIN,
    LED_TIMER_30MIN,
    LED_TIMER_60S,          /* 睡眠模式: 60s 自动关灯 */
} led_timer_status;

static led_timer_status led_status = LED_TIMER_DEFAULT;
int  led_tick     = 0;
volatile bool time_set_flag = false;
static volatile uint32_t  sleepdsms_cnt      = 0;    //睡眠模式、定时模式计时
/*===========================================================================
 * 内部辅助函数
 *===========================================================================*/

/* 将所有 PWM 通道直接设为 0（PA1 现为 MIC 供电，非 PWM，不再关它） */
static inline void pwm_all_off(void)
{
    if (!g_led_pwm_ready) {
        return;
    }
    led_pwm_set_yellow(0);
    led_pwm_set_white(0);
}

static int duty_half(int val)
{
    return val / 2;
}

static void pwm_apply_color(led_handle_t *handle, int duty)
{
    int off = brightness_level_arr[BRIGHTNESS_OFF];

    if (!handle->on || duty <= 0) {
        led_pwm_channel_raw(YELLOW_NUM, off);
        led_pwm_channel_raw(WHITE_NUM, off);
        return;
    }

    if (handle->color_index == COLOR_YELLOW_WHITE) {
        int half = duty_half(duty);
        led_pwm_channel_raw(YELLOW_NUM, half);
        led_pwm_channel_raw(WHITE_NUM, half);
    } else if (handle->color_index == COLOR_YELLOW) {
        led_pwm_channel_raw(YELLOW_NUM, duty);
        led_pwm_channel_raw(WHITE_NUM, off);
    } else if (handle->color_index == COLOR_WHITE) {
        led_pwm_channel_raw(YELLOW_NUM, off);
        led_pwm_channel_raw(WHITE_NUM, duty);
    } else {
        led_pwm_channel_raw(YELLOW_NUM, off);
        led_pwm_channel_raw(WHITE_NUM, off);
    }
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

// 我添加的代码  /***************************************************/***************************************************
static int led_set_color(led_handle_t *handle, led_color_index_e color)
{
    if ((handle == NULL) || (!handle->on)) {
        return 1;
    }
    handle->color_index = color;
    return led_update(handle);
}

static void led_limit_flash(led_handle_t *handle, int flash_num)
{
    int i;
    int raw;
    bool saved_on = handle->on;   /* 保存原 on 状态，闪烁后恢复，避免睡眠/定时把灯状态清成 off */
//    if (!handle->on) {
//        return;
//    }
    handle->on = true;
    raw = led_dim_to_raw(handle->brightness_val);
    for (i = 0; i < flash_num; i++) {
       delay_ms(LED_FLASH_TIMES);
       pwm_apply_color(handle, 0);
       delay_ms(LED_FLASH_TIMES);
       pwm_apply_color(handle, raw);
    }
    delay_ms(LED_FLASH_TIMES);
    handle->on = saved_on;       /* 恢复原 on 状态（睡眠/定时模式下灯应保持亮） */
}

static void led_clear_timer(void)
{
    led_status = LED_TIMER_DEFAULT;
    sleepdsms_cnt = 0;
}

/*===========================================================================
 * 语音唤醒状态管理
 * 双击开灯后：语音使能，但必须说唤醒词后才能执行其他命令
 * 唤醒后10秒无任何词则退出唤醒状态
 *===========================================================================*/
#define WAKEUP_TIMEOUT_MS       10000U      /* 唤醒超时10秒 */

static bool g_wakeup_active = false;        /* 是否处于唤醒状态（可说命令词） */
static uint32_t g_wakeup_timer = 0;         /* 唤醒计时器 */

/**
 * @brief 检查是否处于唤醒状态
 */
bool led_is_wakeup_active(void)
{
    return g_wakeup_active;
}

/**
 * @brief 刷新唤醒计时器（识别到指令时调用）
 */
void led_refresh_wakeup_timer(void)
{
    g_wakeup_timer = 0;
    g_wakeup_active = true;
    my_printf("wakeup: timer refreshed\n");
}

/**
 * @brief 唤醒超时检测（1ms调用）
 */
void led_wakeup_tick(void)
{
    if (!g_wakeup_active) {
        return;
    }

    g_wakeup_timer++;
    if (g_wakeup_timer >= WAKEUP_TIMEOUT_MS) {
        g_wakeup_active = false;
        g_wakeup_timer = 0;
        my_printf("wakeup: timeout, exit wakeup state\n");
    }
}

// 我添加的代码 END  /***********************************************/***************************************************

/*===========================================================================
 * 初始化
 *===========================================================================*/
void led_ctrl_init(void)
{
    g_led_pwm_ready = false;

    led_handle.on               = false;
    led_handle.color_index      = COLOR_WHITE;
    led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_6);

    g_color_mode    = LED_MODE_COLD;
    g_in_breath     = false;
    g_voice_enabled = false;
    g_need_sleep    = false;
    g_sleep_cnt     = 0;
    sleepdsms_cnt   = 0;
    g_bright_dir           = 1;
    g_longpress_timer      = 0;
    g_reverse_next_long    = false;
    led_tick = 0;
    led_clear_timer();

    /* SOP8：PWM0=PA11 暖白、PWM1=PA7 正白；PA1(PIN6) 已改为 MIC 供电；同脚 PA10/8/9/2/3 高阻 */
    GLOBAL_INT_DISABLE();
    sop8_led_bond_hiz();
    sop8_led_gpio_low();   /* 先钳低两路灯脚，消除切换 PWM 瞬间的闪 */

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

    /* PA1(PIN6) 供 MIC 电源：GPIO 推挽输出高，不再复用红灯 PWM */
    mic_power_on();

    /* 8. PWM 初始化后立刻把两路占空比关死（直接写寄存器，不经过 g_led_pwm_ready 门控：
     *    此刻 ready 仍为 false，pwm_all_off 会直接返回，起不到关闭作用） */
    led_pwm_channel_off(YELLOW_NUM);
    led_pwm_channel_off(WHITE_NUM);

    g_led_pwm_ready = true;
    GLOBAL_INT_RESTORE();

    /* 9. 再次确保同脚未用 pad 高阻 */
    sop8_led_bond_hiz();
    pwm_all_off();
    my_printf("led_init: PWM0=PA11, PWM1=PA7, PA1=MIC_PWR\n");
}

bool led_pwm_is_ready(void)
{
    return g_led_pwm_ready;
}

/*===========================================================================
 * 语音控制接口（KWS 词条 ID → 动作）
 *
 * KWS ID 映射：
 *   0: 你好小镜 (唤醒词)
 *   1: 小镜小镜 (唤醒词)
 *   2-26: 其他命令词
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

    /* 睡眠(60s)/定时(30min)倒计时内：其他命令词(除23/24睡眠、25/26定时命令外)
     * 取消自动关灯定时, 命令本身照常执行——重新说定时命令才重新计时。
     * 唤醒词(0/1)不会流到此函数,天然不影响定时 */
//    if ((led_status == LED_TIMER_60S) || (led_status == LED_TIMER_30MIN)) {                          //
//        if ((cmd != 23) && (cmd != 24) && (cmd != 25) && (cmd != 26)) {
//            led_clear_timer();
//        }
//    }

    /* ========== 唤醒词处理 ========== */
if (cmd == 0 || cmd == 1) {
    led_handle_t  saved= {0};
    // 保存当前颜色
    saved.color_index = led_handle.color_index;
    saved.on = led_handle.on;
    // 闪烁提示（暖白+正白闪两下；PA1 现为 MIC 供电，不再做红灯指示）
    led_handle.color_index = COLOR_YELLOW_WHITE;
    led_limit_flash(&led_handle, 2);

    // 恢复颜色
    led_handle.color_index = saved.color_index;
    led_handle.on = saved.on;


    // 进入唤醒状态，重置计时器（从0开始计时）
    g_wakeup_active = true;
    g_wakeup_timer = 0;

    //delay_ms(LED_FLASH_TIMES);
    //计时清除
    if ((led_status == LED_TIMER_60S)|| (led_status == LED_TIMER_30MIN) )
    {
        led_clear_timer();
    }
    // 恢复灯光状态
    if (led_handle.on) {
        led_yw_on(&led_handle);
    }else{
           led_pwm_set_yellow(0);
           led_pwm_set_white(0);
    }

    my_printf("wakeup: active, timer reset\n");
    return 0;
}

    /* ========== 非唤醒词：检查是否在唤醒状态 ========== */
    if (!g_wakeup_active) {
        /* 未唤醒：忽略所有非唤醒词命令 */
        my_printf("voice: cmd %d ignored (not wakeup, say '小镜小镜' or '你好小镜')\n", cmd);
        return -1;
    }

    /* 唤醒状态下：执行命令，刷新计时器 */
    g_wakeup_timer = 0;  /* 刷新10秒计时 */

    switch (cmd) {
    case 2:   //我回来了
    case 3:  /* 打开灯光：关灯后再开一律暖白（与触摸从灭到亮一致） */
        if (!led_handle.on) {
            g_in_breath = false;
           // g_color_mode = LED_MODE_WARM;
           //led_handle.color_index = COLOR_YELLOW;
            led_yw_on(&led_handle);
            my_printf("voice: warm on duty=%d\n", led_handle.brightness_val);
        }
        break;

    case 4:  /* 我出去了 */
    case 5:  /* 关闭灯光 */
        g_in_breath = false;
        //g_color_mode = LED_MODE_WARM;  /* 下次开灯从暖白开始 */
        led_yw_off(&led_handle);
        break;

    case 6:   /* 白光灯 */
    case 7:   /* 白色灯光 */
    case 8:   /* 打开白光 */
        if (!led_handle.on) {
            led_yw_on(&led_handle);          /* 灭灯: 先开灯(恢复上次亮度)再切色 */
        }
        led_set_color(&led_handle, COLOR_WHITE);   /* 切正白,亮度不变 */
        break;

    case 9:   /* 黄光灯 */
    case 10:  /* 黄色灯光 */
    case 11:  /* 打开黄光 */
        if (!led_handle.on) {
            led_yw_on(&led_handle);          /* 灭灯: 先开灯(恢复上次亮度)再切色 */
        }
        led_set_color(&led_handle, COLOR_YELLOW);   /* 切暖白,亮度不变 */
        break;

    case 12:  /* 中性灯 */
    case 13:  /* 中性光 */
    case 14:  /* 打开中性光 */
        if (!led_handle.on) {
            led_yw_on(&led_handle);          /* 灭灯: 先开灯(恢复上次亮度)再切色 */
        }
        g_color_mode = LED_MODE_MIX;
        led_set_color(&led_handle, COLOR_YELLOW_WHITE); /* 切混合,亮度不变 */
        break;

//    case 4:  /* 变颜色：4种模式循环（WARM→COLD→MIX→BREATH→WARM） */
//        if (!led_handle.on) break;
//        switch (g_color_mode) {
//        case LED_MODE_WARM:
//            g_color_mode = LED_MODE_COLD;
//            exit_breath_mode(COLOR_WHITE);
//            my_printf("voice: cold(PIN4)\n");
//            break;
//        case LED_MODE_COLD:
//            g_color_mode = LED_MODE_MIX;
//            exit_breath_mode(COLOR_YELLOW_WHITE);
//            my_printf("voice: mix\n");
//            break;
//        case LED_MODE_MIX:
//            g_color_mode = LED_MODE_BREATH;
//            enter_breath_mode();
//            my_printf("voice: breath warm\n");
//            break;
//        case LED_MODE_BREATH:
//            g_color_mode = LED_MODE_WARM;
//            exit_breath_mode(COLOR_YELLOW);
//            my_printf("voice: warm(PIN5)\n");
//            break;
//        default:
//            g_color_mode = LED_MODE_WARM;
//            exit_breath_mode(COLOR_YELLOW);
//            break;
//        }
//        break;

    case 17:  /* 亮一点 */
    case 18:  //调亮一点
        if (!led_handle.on || g_in_breath) break;
        if (led_handle.brightness_level < BRIGHTNESS_LEVEL_6) {
            led_set_dim_from_level(&led_handle,
                                   (led_brightness_e)(led_handle.brightness_level + 1));
        }
        led_update(&led_handle);
        my_printf("voice: brighter lvl=%d\n", (int)led_handle.brightness_level);
        break;

    case 15:  /* 暗一点 */
    case 16:  //调暗一点
        if (!led_handle.on || g_in_breath) break;
        if (led_handle.brightness_level > BRIGHTNESS_LEVEL_1) {
            led_set_dim_from_level(&led_handle,
                                   (led_brightness_e)(led_handle.brightness_level - 1));
        }
        led_update(&led_handle);
        my_printf("voice: darker lvl=%d\n", (int)led_handle.brightness_level);
        break;

    case 19:  /* 最大亮度 */
    case 20:    //灯光最亮
        if (!led_handle.on || g_in_breath) break;
        led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_6);
        led_update(&led_handle);
        my_printf("voice: max lvl=6\n");
        break;

    case 21:  /* 最小亮度（最暗档，不关灯） */
    case 22:   // 灯光最暗
        if (!led_handle.on || g_in_breath) break;
        led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_1);
        led_update(&led_handle);
        my_printf("voice: min lvl=1\n");
        break;

    case 23:  /* 睡觉模式 */
    case 24:  /* 睡眠模式 */
        /* 睡眠模式: 绿光(同"打开黄光"=COLOR_YELLOW/PA4) + 20%亮度, 60s后自动关灯。
         * 仅唤醒状态下可执行(命令门控在 bsp_asr.c);
         * 睡眠倒计时内听到其他命令词 → 取消定时且命令照常执行, 需重说本命令重新计时 */
        if (!led_handle.on) {
            led_yw_on(&led_handle);              /* 灭灯时先开灯(恢复上次状态) */
        }
        led_set_color(&led_handle, COLOR_YELLOW);
        led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_1);
        led_status = LED_TIMER_60S;
        sleepdsms_cnt = 0;
        led_limit_flash(&led_handle,1);    /* 闪烁1下: 提示已听进去并进入睡眠 */
        break;

    case 25:  /* 定时模式 */
    case 26:  /* 开始定时 */
        /* 定时模式: 保持当前灯光状态不变, 30分钟后自动关灯。
         * 仅亮灯状态可执行(灭灯无反应);
         * 倒计时内其他命令词 → 取消定时且命令照常执行, 重说本命令重新计30分钟 */
        if (!led_handle.on) {
            break;                       /* 灭灯: 无反应 */
        }
        led_status = LED_TIMER_30MIN;
        sleepdsms_cnt = 0;
        led_limit_flash(&led_handle,1);    /* 闪烁1下: 提示已听进去并进入定时 */
        break;

    default:
        break;
    }

    return 0;
}

/*===========================================================================
 * 触摸双击处理：开灯/关灯切换（独立控制语音使能）
 *===========================================================================*/
void led_touch_double_click(void)
{
    if (!g_led_pwm_ready) {
        return;
    }

    /* 任何触摸动作重置睡眠计时器 */
    g_sleep_cnt  = 0;
    g_need_sleep = false;

    if (!led_handle.on) {
        /* 双击开灯：正白最大亮度，使能语音,进入待唤醒状态  */
        g_color_mode           = LED_MODE_COLD;
        g_in_breath            = false;
        led_handle.color_index = COLOR_WHITE;
        led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_6);
        led_yw_on(&led_handle);
        g_voice_enabled = true;
        g_wakeup_active = false;        /* 开灯后进入待唤醒状态 */
        g_wakeup_timer = 0;
        g_bright_dir         = 1;
        g_reverse_next_long  = false;
        my_printf("double click: on (cold, max brightness, voice enabled ,waiting wakeup)\n");
    } else {
        /* 双击关灯：完全关闭，禁用语音 */
        g_in_breath = false;
        led_yw_off(&led_handle);
        g_voice_enabled = false;
        g_wakeup_active = false;        /* 关灯退出唤醒 */
        g_wakeup_timer = 0;
        my_printf("double click: off (voice disabled, wakeup exit)\n");
    }
}

/*===========================================================================
 * 触摸单击开灯（关灯状态下单击开灯）
 *===========================================================================*/
void led_touch_single_click_on(void)
{
    if (!g_led_pwm_ready) {
        return;
    }

    /* 如果灯已经亮，不处理 */
    if (led_handle.on) {
        return;
    }

    /* 双击关灯后单击不能开灯（语音禁用状态） */
    if (!g_voice_enabled) {
        my_printf("touch: single click ignored (double click to on)\n");
        return;
    }

    /* 任何触摸动作重置睡眠计时器 */
    g_sleep_cnt  = 0;
    g_need_sleep = false;

    /* 单击开灯：正白最大亮度 */
    g_color_mode           = LED_MODE_COLD;
    g_in_breath            = false;
    led_handle.color_index = COLOR_WHITE;
    led_set_dim_from_level(&led_handle, BRIGHTNESS_LEVEL_6);
    led_yw_on(&led_handle);
    g_voice_enabled = true;  /* 确保语音使能 */
    g_bright_dir         = 1;
    g_reverse_next_long  = false;
    my_printf("single click: on (cold, max brightness)\n");
}

/*===========================================================================
 * 触摸单击处理（开灯状态下切换颜色）
 *===========================================================================*/
void led_touch_click(void)
{
    if (!g_led_pwm_ready) {
        return;
    }

    /* 任何触摸动作重置睡眠计时器 */
    g_sleep_cnt  = 0;
    g_need_sleep = false;

    /* 如果灯没亮，不处理 */
    if (!led_handle.on) {
        return;
    }

    /* 开灯状态下，单击切换颜色 */
    switch (g_color_mode) {
    case LED_MODE_COLD:    /* 正白 → 暖白 */
        g_color_mode = LED_MODE_WARM;
        exit_breath_mode(COLOR_YELLOW);
        my_printf("touch: warm(PIN4) lvl=%d\n", (int)led_handle.brightness_level);
        break;
    case LED_MODE_WARM:    /* 暖白 → 混合 */
        g_color_mode = LED_MODE_MIX;
        exit_breath_mode(COLOR_YELLOW_WHITE);
        my_printf("touch: mix(PIN1+4)\n");
        break;
    case LED_MODE_MIX:     /* 混合 → 关机（单击关灯，语音保持使能） */
        g_color_mode = LED_MODE_COLD;
        g_in_breath  = false;
        led_yw_off(&led_handle);
        /* 注意：语音保持使能，单击可以再开灯 */
        my_printf("touch: off (single click to on, voice enabled)\n");
        break;
    default:
        g_color_mode           = LED_MODE_WARM;
        g_in_breath            = false;
        led_handle.color_index = COLOR_YELLOW;
        led_yw_on(&led_handle);
        break;
    }
}

/*===========================================================================
 * 查询LED是否亮着（供port_touch使用）
 *===========================================================================*/
bool led_handle_is_on(void)
{
    return led_handle.on;
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
 * 睡眠模式、定时模式计时 tick（每 1ms 调用）
 * 条件：LED 亮（黄光LED亮、亮度20%，60s后灭灯 或 当前亮灯状态30分钟关灯）
 *===========================================================================*/
void led_sleepdsms_tick(void)
{
    if (led_handle.on) {
        sleepdsms_cnt++;
      if (led_status != LED_TIMER_DEFAULT) {
        if (LED_TIMER_30MIN == led_status) {
            if (sleepdsms_cnt >= 30UL * 60 * 1000) {
                led_status = LED_TIMER_DEFAULT;
                led_yw_off(&led_handle);
                sleepdsms_cnt = 0;
            }
        } else if (LED_TIMER_60S == led_status) {
            /* 睡眠模式: 60s 到点自动关灯 */
            if (sleepdsms_cnt >= 1UL * 60 * 1000) {
                led_status = LED_TIMER_DEFAULT;
                led_yw_off(&led_handle);
                sleepdsms_cnt = 0;
            }
        }
      }
    } else {
        /* 灯灭：不计时 */
        sleepdsms_cnt = 0;
        return;
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
        //led_breath_tick();
        led_wakeup_tick();      /* 唤醒超时检测 */
        //led_sleep_tick();
        led_sleepdsms_tick();
    }
}

//void led_loop(void)
//{
//    static led_timer_status led_status_last = LED_TIMER_DEFAULT;
//
//
//    if (!led_handle.on) {
//        if (led_status != LED_TIMER_DEFAULT) {
//            led_status = LED_TIMER_DEFAULT;
//            led_status_last = led_status;
//            led_tick = 0;
//        }
//        return;
//    }
//
//    if (led_status != led_status_last) {
//        led_status_last = led_status;
//        led_tick = 0;
//    }
//
//    if (led_status != LED_TIMER_DEFAULT) {
//        if (LED_TIMER_10MIN == led_status) {
//            if (led_tick >= 10 * 60 * 1000) {
//                led_status = LED_TIMER_DEFAULT;
//                time_set_flag = false;
//                led_yw_off(&led_handle);
//                led_tick = 0;
//            }
//        } else if (LED_TIMER_30MIN == led_status) {
//            if (led_tick >= 30 * 60 * 1000) {
//                led_status = LED_TIMER_DEFAULT;
//                time_set_flag = false;
//                led_yw_off(&led_handle);
//                led_tick = 0;
//            }
//        } else if (LED_TIMER_60S == led_status) {
//            /* 睡眠模式: 60s 到点自动关灯 */
//            if (led_tick >= 60 * 1000) {
//                led_status = LED_TIMER_DEFAULT;
//                time_set_flag = false;
//                led_yw_off(&led_handle);
//                led_tick = 0;
//            }
//        }
//    }
//
//}
