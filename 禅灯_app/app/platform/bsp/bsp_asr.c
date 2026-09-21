#include "include.h"
#include "zbit_kws/kws_task.h"
#include "xcfg.h"
#include "sha256.h"
#include "zbitkws.h"
#include "led_ctrl.h"

#include "../modules/zbit_kws/libkws_AB169_301_180k_V3.1.0_cn_wd9_108_7b319ab_20260705_221039.c"

#if ASR_RECOG_EN

#define TRACE_EN                    0

#if TRACE_EN
#define TRACE(...)                  printf(__VA_ARGS__)
#else
#define TRACE(...)
#endif

#define KWS_SAMPLES                 512         //KWS一帧的样点数，固定值

#define VAD_DMA_DEFAULT_SIZE        1280
#define ASR_PCM_LEN                 (1280*2)
#define ASR_PCM_BUFF_NUM            (5)
#define ASR_PRE_READ                (3)
#define VAD_START_THRESHOLD_VALUE   (1500000)
#define VAD_STOP_THRESHOLD_VALUE    (2000000)

uint8_t cgf_asr_recog_en = ASR_RECOG_EN;
void dump_putbuf(u8 *buf,  int buf_len, u8 file_idx);

struct asr_buf_t {
    volatile s16 wptr;
    volatile s16 rptr;
} asr_buf AT(.ws_asr.data);

typedef struct {
    uint8_t *rptr;
    uint8_t *wptr;
    uint8_t *start;
    uint8_t *end;
    uint16_t total;
    uint16_t trig;
} ring_buf_t;

typedef enum {
    ASR_128 = 0,
    ASR_256,
    ASR_512,
    ASR_1024,
} ASR_LEN;

typedef enum {
    VAD_IDLE = 0,
    VAD_START,
    VAD_CONTINUE,
} VAD_STA;

typedef enum {
    VAD_NORMAL = 0,
    VAD_SLEEP,
    VAD_W4_WAKE,
} VAD_SLEEP_STA;

typedef struct {
    volatile u8 sta;
    u32 samples;                //VAD的样点数
    volatile u32 vad_reboot_cnt;

    //DMA有关的
    volatile u16 offset;        //VADDMACON0的高16位，转成u16的样点数再计算
    u16* speech_start;          //语音在adcdma上对应的起始地址
    u32 dma_len;                //DMA总长度
    u8* dma_ptr;                //DMA数据的起始地址
    u8* dma_ptr_t;
    u32 dma_addr;
    u32 dma_addr_end;

} vad_cb_t;

void asr_kick_start(void);
void asr_vad_callback(int16_t *ptr, u16 samples, u8 voice_flag);
void asr_huart_init(void);
bool asr_huart_putcs(const void *buf, uint len);
void sys_enter_sleep_vad(void);
void vad_irq_init(void);
void spk_ains3_init(ains3_cb_t *ains3_cb);
void spk_ains3_plus_process(s16 *buf);
u32 asr_alg_process(short *ptr);
void asr_alg_stop(void);
void asr_alg_start(void);
void asr_alg_init(void);
int vad_process(int16_t *ptr);
int vad_init(u32 start_threshold_value, u32 stop_threshold_value);
void ring_buf_init(ring_buf_t *pbuf, void *start, uint16_t size, uint16_t trig);
bool ring_buf_put(ring_buf_t *pbuf, uint8_t *ptr, uint16_t len);
bool ring_buf_get(ring_buf_t *pbuf, void (*callback)(uint8_t *, uint16_t), uint16_t len);
void adpcm_encode_process(void);

#if ASR_AINS_EN
static ains3_cb_t ains3_cb AT(.ains3_buf.sta);
#endif
#if ASR_VAD_EN
volatile int vad_state AT(.ws_asr.vad);
u8 ws_sysclk AT(.ws_asr.vad);
#endif
#if FUNC_REC_EN
u8 mpa_is_encoding(void);
int mpa_encode_frame(void);
extern u8 mp3enc_flag;
extern u8 adpcm_enc_flag;
#endif

volatile u8 asr_kick_kws_flag AT(.buf.asrkick);
// static u8 asr_pcm_cache[ASR_PCM_LEN] AT(.ws_asr.test);
// static ring_buf_t asr_pcm AT(.ws_asr.test);

asr_cfg_t asr_cfg = {
    .huart_dump_en      = 0,                //是否通过HUART dump音频数据，使用bluetrum_voice_record工具接收
};

AT(.asr_text.asr_deal)
void asr_pcm_callback(u8 *ptr, u16 len)
{
    asr_alg_process((int16_t *)ptr);
}

AT(.vad_text.kws.proc)
void asr_kws_process(void)
{
    if(asr_kick_kws_flag){
        // ring_buf_get(&asr_pcm, asr_pcm_callback, 640 * 2);
        asr_kick_kws_flag = 0;
    }
}

AT(.com_text.thread.asr)
void asr_kick_start(void)
{
    asr_kick_kws_flag = 1;
}



//extern void kws_load_input(int16_t *audio, int32_t audio_len);

//AT(.com_rodata.bat)
//const char mic_pow_str[] = "%d\n";
///SDADC DMA中断起的AUPCM线程

bool gpio_set = 0;



#define KWS_WAKE_UP_SEC        15
static int32_t kws_wake_tick = 0;

static void kws_sleep(void)
{
    if (kws_wake_tick > 0)
    {
        kws_wake_tick--;
        if (kws_wake_tick == 0)
        {
//            uart_com_write_pkt(0x11);
        }
    }
}

static void kws_refresh(void)
{
    kws_wake_tick = 15*1000/96;
}

static bool auth_succ_flag = false;

AT(.com_text.vad.proc)
void asr_sdadc_process(u8 *ptr, u32 samples, int ch_mode)
{
    if (!auth_succ_flag)
    {
        return;
    }

#if HUART_DEUMP_EN
    dump_putbuf(ptr, samples, 0);
    dump_putbuf(&ptr[samples], samples, 0);
#endif

    kws_sleep();

    kws_load_input((int16_t*)ptr, 128*3);

    int16_t top=-1,prob;
    kws_classify(&top,&prob);
    if((top>=0) && (prob >= WW_TRG_MODE[top]))
    {
        my_printf("Cmd:%d\nOutput: %s (%d)\n", top, kw_string2[top], prob);

        /* 语音控制 LED（voice_enabled 判断在 led_ctrl 内部处理） */
        led_ctrl(top);
    }
}

int8_t raw_buffer[128*16*2] __attribute__((section(".kws.ram_buf"))) = {0};
int32_t raw_buffer_size = 128*16*2;
int8_t med_buffer[512*4] __attribute__((section(".kws.ram_buf"))) = {0};
int32_t med_buffer_size = 512*4;
///ASR启动
void asr_start(void)
{
    int32_t buf_size, buf_size2;
    getBufferSize(&buf_size, &buf_size2);
    printf("\r\nbuf_size:%d buf_size2:%d\r\n", buf_size,  buf_size2);

    int ret = kws_init(raw_buffer, raw_buffer_size, med_buffer, med_buffer_size);
    printf("\r\nkws_init ret:%d\r\n",  ret);
    if (ret == 0)
    {
        auth_succ_flag = true;
    }

    printf("kws version:%s\n", kws_get_version());

    /* 初始化 LED 控制（PWM 硬件就绪，LED 灭，语音使能=false，等待触摸开机） */
    led_ctrl_init();
}

///ASR停止
void asr_stop(void)
{

}

///ASR初始化
void asr_init(asr_cfg_t* cfg)
{

}

u32 asr_get_sdadc_dma_addr(void)
{
    return 0;
}

u32 asr_get_sdadc_dma_len(void)
{
    return 0;
}

void bsp_asr_restart(void)
{

}

void bsp_asr_start(void)
{
    asr_start();
}

void bsp_asr_stop(void)
{

}

void bsp_asr_init(void)
{

}
#endif // ASR_RECOG_EN
