# 实习作品：AB16x 离线语音设备固件（化妆镜 / 禅灯）

中科蓝讯 **AB16x（RISC-V RV32）** 离线语音产品的两套设备固件（实习项目源码归档）：

| 工程 | 产品 | 识别方案 |
|---|---|---|
| `化妆镜_app/` | 语音化妆镜 | KWS 词库 28 词（libkws_AB169_611_wd28），WS2812 灯效 |
| `禅灯_app/` | 禅意灯 | KWS 词库 9 词（libkws_AB169_301_wd9） |

## 工程结构（两套同构）

```
app/
├── platform/bsp/          # 板级驱动：SDADC 采音、DAC 播报、PWM、时钟
├── platform/modules/
│   ├── zbit_kws/          # 离线关键词识别（KWS）算法库 + 词表/阈值配置
│   └── ws/                # TDNN 声学模型与音频处理
└── projects/zbit_asr/     # CodeBlocks 工程（app-kws.cbp）
```

## 编译与烧录

- IDE：CodeBlocks 打开 `app/projects/zbit_asr/app-kws.cbp`
- 编译链：riscv32-elf-gcc（march=rv32imac）
- 烧录：AB16X Downloader，烧写 `Output/bin/app.prd`（4 线串口，1.5M 波特率）

---

**EN**: Two AB16x (Bluetrum RISC-V) offline voice device firmwares from internship: voice makeup mirror (28-word KWS, WS2812 effects) and zen lamp (9-word KWS). CodeBlocks + riscv32-elf-gcc, flashed via AB16X Downloader.

## 声明

实习项目源码，仅作个人能力展示与技术归档。
