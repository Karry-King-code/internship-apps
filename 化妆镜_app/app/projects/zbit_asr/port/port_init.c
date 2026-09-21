#include "include.h"
#include "port_touch.h"

sys_cb_t sys_cb AT(.buf.bsp.sys_cb);

extern uint8_t pin_sop_sel;

//timer tick interrupt(5ms)
AT(.com_text.timer)
void usr_tmr5ms_isr(void)
{
    sys_cb.tmr5ms_cnt++;
#if !USER_KEY_KNOB2_EN
    bsp_key_scan();
#endif

    //500ms timer process
    if ((sys_cb.tmr5ms_cnt % 100) == 0) {
        sys_cb.cm_times++;
    }

    //1s timer process
    if ((sys_cb.tmr5ms_cnt % 200) == 0) {
        msg_enqueue(MSG_SYS_1S);
        sys_cb.tmr5ms_cnt = 0;
        sys_cb.lpwr_warning_cnt++;
    }
}

extern int led_tick;
extern volatile bool time_set_flag;
//timer tick interrupt(1ms)
AT(.com_text.timer)
void usr_tmr1ms_isr(void)
{
    sys_cb.tmr1ms_cnt++;

    if ((sys_cb.tmr1ms_cnt % 5) == 0) {
        usr_tmr5ms_isr();
    }

    if(time_set_flag)
    {
        led_tick++;
    }

#if LED_DISP_EN
    if ((sys_cb.tmr1ms_cnt % 50) == 0) {
        led_scan();
    }
#endif // LED_DISP_EN

    //100ms timer process
    if ((sys_cb.tmr1ms_cnt % 100) == 0) {
        sys_cb.tmr1ms_cnt = 0;
        if (sys_cb.lpwr_cnt > 0) {
            sys_cb.lpwr_cnt++;
        }
    }

#if TKEY_SCAN_SWITCH_EN
    if ((sys_cb.tmr1ms_cnt % TKEY_SCAN_TIME) == 0) {
    	tkey_cir_scan_en();
    }
#endif
}

AT(.text.bsp.sys.init)
void power_on_off_init(void)
{
#if SOFT_POWER_ON_OFF
    // //??ADkey??????????
    // GPIOAFEN &= ~BIT(POWER_ON_FALL_IO-1);
    // GPIOADE  |=  BIT(POWER_ON_FALL_IO-1);
    // GPIOADIR |=  BIT(POWER_ON_FALL_IO-1);
#endif
}

AT(.rodata.vol)
const u8 maxvol_tbl[4] = {16, 32, 50};

//?user timer????????
AT(.text.bsp.sys.init)
static void bsp_var_init(void)
{
    memset(&sys_cb, 0, sizeof(sys_cb));
    sys_cb.ms_ticks = tick_get();
    sys_cb.vol_max = maxvol_tbl[xcfg_cb.vol_max];
    if (SYS_INIT_VOLUME > sys_cb.vol_max) {
        SYS_INIT_VOLUME = sys_cb.vol_max;
    }
    if (WARNING_VOLUME > sys_cb.vol_max) {
        WARNING_VOLUME = sys_cb.vol_max;
    }

#if (PIN_SOP_SEL != 2)
    pin_sop_sel = PIN_SOP_SEL;
#endif
    // key_var_init();
    plugin_var_init();

    msg_queue_init();

    sdadc_var_init();

    dac_cb_init();
//    dev_init(is_sd_support());
}

AT(.text.bsp.sys.init)
static void bsp_io_init(void)
{
    if(!GPIO_10S_RESET){
        GPIOAPU &= ~BIT(GPIO_10S_SEL-1);
    }
    if(!MCLR_S_RESET){
        GPIOAPU &= ~BIT(11);
    }
    GPIOADE = 0;
    if(MCLR_S_RESET){
        GPIOADE |= BIT(11);
    }
    if(GPIO_10S_RESET){
        GPIOADE |= BIT(GPIO_10S_SEL-1);
    }
    GPIOBDE = 0;
    GPIOGDE = 0x3F;             //MCP FLASH
    uart0_mapping_sel();        //??UART IO??????

    /* ???? PA10 ??????????????*/
    port_touch_init();
}

AT(.text.bsp.sys.init)
void bsp_update_init(void)
{
    /// config
    if (!xcfg_init(&xcfg_cb, sizeof(xcfg_cb))) {           //??????
        printf("xcfg init error\n");
    }

    // io init
    bsp_io_init();

    // var init
    bsp_var_init();
    sys_cb.lang_id = 0;

    // peripheral init
    rtc_init();
//    param_init(sys_cb.rtc_first_pwron);

    plugin_init();
    sys_set_tmr_enable(1);

    adpll_init(SYS_CLK_SEL);
#if DAC_EN
    dac_init();
#endif
#if WARNING_UPDATE_DONE
    mp3_res_play(RES_BUF_UPDATE_DONE_MP3, RES_LEN_UPDATE_DONE_MP3);
#endif
}

/* PIN1 = PA10(电源键) + PA11(暖白LED) 同脚键合；暖白LED 高电平点亮。
 * 上电 boot 阶段（bootrom / xcfg_init 慢速读 flash）PA10 的默认上拉会把
 * PIN1 拉高，导致暖白灯上电闪一下。必须在 xcfg_init 之前就把 PIN1 钳低。
 * 后续 bsp_io_init / led_ctrl_init 会按原逻辑重新配置，本函数只负责消除
 * 上电瞬间的闪烁。 */
AT(.text.bsp.sys.init)
static void sop8_pin1_boot_clamp_low(void)
{
    /* 先中和 PA10：输入高阻、去上下拉，去掉 PIN1 上任何上拉/高驱动源 */
    GPIOAFEN &= ~BIT(10);
    GPIOADE  |=  BIT(10);
    GPIOADIR |=  BIT(10);
    GPIOAPU  &= ~BIT(10);
    GPIOAPD  &= ~BIT(10);

    /* 再把 PA11 暖白LED 做成 GPIO 输出低，把 PIN1 强拉低 */
    GPIOAFEN &= ~BIT(11);
    GPIOADE  |=  BIT(11);
    GPIOADIR &= ~BIT(11);
    GPIOACLR  =  BIT(11);
}

AT(.text.bsp.sys.init)
void bsp_sys_init(void)
{
    sop8_pin1_boot_clamp_low();   /* 上电先把 PIN1(暖白LED) 钳低，消除 boot 闪烁 */

    /// config
    if (!xcfg_init(&xcfg_cb, sizeof(xcfg_cb))) {           //??????
        printf("xcfg init error\n");
    }

    // io init
    bsp_io_init();

    // var init
    bsp_var_init();

    // power init
    pmu_init(0);

    // clock init
    adpll_init(SYS_CLK_SEL);
    set_sys_clk(SYS_CLK_SEL);
    // dbg_clk_out(9, 10);

    // peripheral init
    rtc_init();
//    param_init(sys_cb.rtc_first_pwron);
    plugin_init();
    power_on_off_init();        //???io????

#if IRRX_SW_EN
    irrx_sw_init();
#endif // IRRX_SW_EN

    led_init();
    key_init();

    /* power_on_check ??????GPIOA??????????KEY(PA10) */
    port_touch_init();

    /// enable user timer for display & dac
    sys_set_tmr_enable(1);

#if DAC_EN
    dac_init();
#endif

    bsp_change_volume(sys_cb.vol);

#if DAC_DRC_EN
    drc_v3_init((u8 *)RES_BUF_DRC_DAC_MUSIC_DRC, RES_LEN_DRC_DAC_MUSIC_DRC);
#endif

#if EX_SPIFLASH_SUPPORT
    exspiflash_init();
#endif

#if SPK_SPI_DUMP
    my_spi_init();
#endif
#if HUART_DEUMP_EN
    huart_dump_init();
#endif

#if HUART_EN
    bsp_huart_init();
#endif

#if USER_UART0_EN
    bsp_uart_init();
#endif

#if TKEY_MUL_SCAN_EN
    tkey_init();
#endif // TKEY_MUL_SCAN_EN

#if (WARNING_POWER_ON && WARNING_MP3_PLAY)
    mp3_res_play(RES_BUF_EN_POWERON_MP3, RES_LEN_EN_POWERON_MP3);
#endif

#if IRTX_HW_EN
    bsp_irtx_init(method_level);
#endif

#if I2C_SW_EN
    bsp_i2c_init();
#endif

#if SPI_AUDIO_TEST_EN
    spi_test_init();
#endif
}
