#include "include.h"

//正常启动Main函数
int main(void)
{
    u32 rst_reason;
    rst_reason = sys_rst_init();
    printf("Hello Dragon asr project: %08x\n", rst_reason);
    printf("VERSIONID: %x\n", VERSIONID);
    printf("build-time: %s-%s\n", __DATE__, __TIME__);
    sys_rst_dump(rst_reason);
    sys_cb.wakeup_reason = lowpwr_get_wakeup_source();

    sys_ram_info_dump();
    bsp_sys_init();

#if SYS_CODE_ERASE
    erase_code();
#endif
    printf("DEVICEID: 0x%x\n", DEVICEID);

//    GPIOAFEN &= ~BIT(7); // PA0 作为gpio使用
//    GPIOADE |= BIT(7);   // pa0 作为数字IO
//    GPIOADIR &= ~BIT(7);
//
//    GPIOA |= BIT(7); // H
//    GPIOA &= ~BIT(7); // L
    func_run();
    return 0;
}




//升级完成
void update_complete(int mode)
{
    bsp_update_init();
    printf("update complete: %d\n", mode);
    if (mode == 0) {
        WDT_DIS();
        while (1);
    }
    WDT_RST();
}
