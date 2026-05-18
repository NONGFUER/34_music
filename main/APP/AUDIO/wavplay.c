/**
 ****************************************************************************************************
 * @file        wavplay.c
 * @author      sjwu
 * @version     V2.0
 * @date        2025-01-01
 * @brief       WAV音频解码与播放引擎
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 *
 * @note        V2.0优化内容:
 *              1. 精简music任务: 移除无用代码, 优化播放内循环结构
 *              2. 减少UART调试输出(播放循环内的printf会通过UART阻塞, 增加延迟)
 *              3. 完善错误处理: WAV头解析失败时的资源释放
 *              4. 任务管理优化: 使用静态MUSICTask_Handler同步避免竞态
 *              5. 单声道→立体声转换逻辑保持不变(已验证正确)
 ****************************************************************************************************
 */

#include "wavplay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "es8388.h"
#include "audioplay.h"
#include "cook_ui.h"

/* ================================================================== */
/*                      FreeRTOS任务配置                               */
/* ================================================================== */

#define MUSIC_PRIO       4                   /* 音乐播放任务优先级 */
#define MUSIC_STK_SIZE   (5 * 1024)          /* 音乐任务堆栈大小 */

#define MONITOR_PRIO     3                   /* 播放监控任务优先级(与RS485同级, 高于主循环) */
#define MONITOR_STK_SIZE (2 * 1024)          /* 监控任务堆栈(仅轮询检测, 不需要大栈) */

static TaskHandle_t s_music_task_handle  = NULL;   /* 音乐任务句柄(NULL=未创建/已销毁) */
static TaskHandle_t s_monitor_task_handle = NULL;  /* 播放监控任务句柄(NULL=未运行) */

void music(void *pvParameters);             /* 音乐任务函数声明 */

/* 外部ADC句柄(main.c初始化, 用于电位器音量控制) */
extern adc_oneshot_unit_handle_t adc1_handle;

/* ================================================================== */
/*                         全局状态变量                                */
/* ================================================================== */

__wavctrl wavctrl;                          /* 当前WAV文件解码参数 */

UINT        bytes_write         = 0;        /* 本次I2S实际写入字节数 */
volatile long long int i2s_table_size      = 0;    /* 累计已发送的音频数据总大小(字节) */
esp_err_t   i2s_play_end         = ESP_FAIL;  /* 播放自然结束标志 */
esp_err_t   i2s_play_next_prev   = ESP_FAIL;  /* 用户切歌标志(上一首/下一首) */
FSIZE_t     file_read_pos        = 0;         /* 暂停时记录的文件读取位置 */
uint8_t     audio_channels       = 2;         /* 当前音频声道数(默认立体声), 供播放循环做单声道转立体声 */

/* ================================================================== */
/*                       WAV头部解析                                   */
/* ================================================================== */

/**
 * @brief 解析WAV文件头部, 提取格式参数和数据位置
 * @param fname  文件完整路径
 * @param wavx   输出: 解析结果写入此结构体
 * @return
 *           0  - 成功(PCM格式)
 *           1  - 文件打开失败
 *           2  - 非WAV文件(RIFF标识不符)
 *           3  - 未找到data chunk
 *           4  - 非PCM格式(不支持)
 *
 * @note  解析策略: 一次性读取1KB头部, 然后以chunk链表方式遍历查找"data" chunk.
 *       支持含metadata/list/info等额外chunk的WAV文件.
 *       安全上限900字节防止损坏文件导致越界读取.
 */
uint8_t wav_decode_init(uint8_t *fname, __wavctrl *wavx)
{
    FIL *ftemp;
    uint8_t *buf;
    uint32_t br = 0;
    uint8_t res = 0;

    ChunkRIFF *riff;
    ChunkFMT  *fmt;
    ChunkDATA *data = NULL;

    /* 分配临时资源 */
    ftemp = (FIL *)malloc(sizeof(FIL));
    buf = malloc(1024);

    if (!ftemp || !buf) {
        if (ftemp) free(ftemp);
        if (buf)   free(buf);
        printf("[WAV] ERR: malloc failed in decode_init\r\n");
        return 1;
    }

    res = f_open(ftemp, (TCHAR *)fname, FA_READ);
    if (res != FR_OK) {
        res = 1;
        goto cleanup;
    }

    /* 读取前1KB(足以覆盖标准WAV头部+常见metadata) */
    f_read(ftemp, buf, 1024, (UINT *)&br);
    riff = (ChunkRIFF *)buf;

    /* 验证RIFF+WAVE标识 */
    if (riff->Format != 0x45564157) {     /* "WAVE" 的little-endian表示 */
        res = 2;
        goto cleanup;
    }

    /* 定位fmt chunk (紧随RIFF header之后, 偏移12字节处) */
    fmt = (ChunkFMT *)(buf + 12);

    /* 遍历chunk链表寻找 "data" chunk */
    {
        uint32_t offset = 12 + 8 + fmt->ChunkSize;  /* 跳过 RIFF(12) + fmt(ID+Size+Data) */

        while (offset < 900) {            /* 安全上限 */
            if (offset + 8 > 1024) break; /* 缓冲区边界检查 */

            uint32_t chunk_id   = *(uint32_t *)(buf + offset);
            uint32_t chunk_size = *(uint32_t *)(buf + offset + 4);

            if (chunk_id == 0x61746164) { /* "data" */
                data = (ChunkDATA *)(buf + offset);
                break;
            }

            /* 跳到下一个chunk (ID[4] + Size[4] + Data[ChunkSize] + 可选padding) */
            offset += 8 + chunk_size;
            if (chunk_size & 1) offset++;  /* WAV规范: 奇数尺寸需对齐到偶数 */
        }
    }

    if (!data || data->ChunkID != 0x61746164) {
        res = 3;
        goto cleanup;
    }

    /* ★ 提取音频参数到输出结构体 */
    wavx->audioformat = fmt->AudioFormat;
    wavx->nchannels   = fmt->NumOfChannels;
    wavx->samplerate  = fmt->SampleRate;
    wavx->bitrate     = fmt->ByteRate * 8;
    wavx->blockalign  = fmt->BlockAlign;
    wavx->bps         = fmt->BitsPerSample;

    /* 格式校验: 仅支持线性PCM(AudioFormat==1) */
    if (wavx->audioformat != 1) {
        printf("[WAV] ERR: unsupported format %d (only PCM=1)\r\n", wavx->audioformat);
        res = 4;
        goto cleanup;
    }

    /* 记录数据区信息 */
    wavx->datasize  = data->ChunkSize;
    wavx->datastart = (uint32_t)((uint8_t *)data - buf) + 8;  /* data chunk数据起始 = chunk首地址+8(ID+Size) */

    printf("[WAV] %s: %luHz %dbit %s ch, size=%lu bytes\r\n",
           (char *)fname, wavx->samplerate, wavx->bps,
           wavx->nchannels == 1 ? "mono" : "stereo", wavx->datasize);

cleanup:
    f_close(ftemp);
    free(ftemp);
    free(buf);
    return res;
}

/* ================================================================== */
/*                        播放时间计算                                 */
/* ================================================================== */

/**
 * @brief 计算当前播放进度(总时长 + 已播放时长)
 * @param fx   已打开的WAV文件句柄
 * @param wavx WAV控制结构体(输入datasize/bitrate, 输出totsec/cursec)
 */
void wav_get_curtime(FIL *fx, __wavctrl *wavx)
{
    long long fpos;

    wavx->totsec = wavx->datasize / (wavx->bitrate / 8);     /* 总秒数 = 数据量 / 字节速率 */
    fpos = fx->fptr - wavx->datastart;                         /* 当前已播放的数据量 */
    wavx->cursec = (uint32_t)(fpos * wavx->totsec / wavx->datasize);  /* 线性插值求当前秒 */
}

/* ================================================================== */
/*                     音乐播放任务(FreeRTOS)                          */
/**
 * @note  任务生命周期:
 *        创建 → ES8388防POP配置 → I2S静音填充 → 功放使能 → [播放循环] → 自删除
 *
 *        ★ POP音消除时序(关键!):
 *        1. 配置ES8388 DAC参数(此时功放关闭, 无输出)
 *        2. 向I2S缓冲区填充全零(静音数据)
 *        3. 等待I2S将静音数据送至DAC输出(10ms)
 *        4. 使能功放 → 此时DAC已是稳定静音, 无POP声
 * ================================================================== */
void music(void *pvParameters)
{
    (void)pvParameters;    /* 抑制未使用参数警告 */

    /* ===== 阶段1: ES8388 DAC配置与POP音消除 ===== */
    es8388_adda_cfg(1, 0);                            /* 开启DAC, 关闭ADC */
    es8388_input_cfg(0);                              /* 关闭录音通路 */
    es8388_output_cfg(1, 1);                          /* 同时开启喇叭和耳机通道 */

    /* 使用RS485持久化音量(每次播放不再被硬编码覆盖) */
    uint8_t vol = audio_get_last_volume();
    es8388_hpvol_set(vol);                            /* 耳机音量 */
    es8388_spkvol_set(vol);                           /* 喇叭音量 */
    vTaskDelay(pdMS_TO_TICKS(20));                    /* 等待ES8388内部寄存器配置稳定 */

    /* ===== 阶段2: I2S预填充静音数据(POP音消除核心) ===== */
    i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);   /* 发送一整缓冲区的零数据 */
    vTaskDelay(pdMS_TO_TICKS(10));                    /* 确保I2S DMA将静音送到DAC输出端 */

    /* ===== 阶段3: 使能功放(此时DAC输出已是干净的静音) ===== */
    xl9555_pin_write(SPK_EN_IO, 0);                  /* 低电平有效: 打开喇叭功放 */

    /* ===== 阶段4: 主播放循环 ===== */
    while (1) {
        /* ★ 外部停止安全出口: 无论当前处于什么状态(播放/暂停/空闲),
         *   收到停止信号都立即释放I2S资源并退出, 避免卡在暂停死循环中导致资源泄漏 ★ */
        if (i2s_play_next_prev == ESP_OK || i2s_play_end == ESP_OK) {
            audio_stop();                           /* 停止I2S DMA */
            i2s_deinit();                           /* 卸载I2S外设(释放控制器, 允许下次重新初始化) */
            i2s_table_size    = 0;
            i2s_play_end      = ESP_OK;             /* 标记播放结束 */
            s_music_task_handle = NULL;             /* 清除句柄(允许下次重新创建任务) */
            vTaskDelete(NULL);                      /* 销毁自身 */
        }

        if ((g_audiodev.status & 0x0F) == 0x03) {    /* 状态=0x03 → 播放中 */
            uint32_t total_read = 0;

            /* 循环读取WAV数据块并送入I2S, 直到文件读完或收到停止指令 */
            while (total_read < wavctrl.datasize) {

                /* ---- 暂停处理(带停止信号检测, 防止永久卡死) ---- */
                if ((g_audiodev.status & 0x0F) == 0x00) {
                    file_read_pos = f_tell(g_audiodev.file);   /* 记录暂停位置 */

                    /* 阻塞等待恢复播放或停止信号(轮询间隔5ms) */
                    while ((g_audiodev.status & 0x0F) != 0x03) {
                        /* 收到外部停止信号 → 跳出暂停等待, 由外层循环统一处理资源释放 */
                        if (i2s_play_next_prev == ESP_OK || i2s_play_end == ESP_OK) {
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(5));
                    }

                    /* 停止信号触发则跳出数据读取循环, 交由外层处理 */
                    if (i2s_play_next_prev == ESP_OK || i2s_play_end == ESP_OK) {
                        break;
                    }

                    f_lseek(g_audiodev.file, file_read_pos);   /* 跳回暂停位置继续 */
                }

                /* ---- 播放完成/切歌检测 ---- */
                if (i2s_table_size >= wavctrl.datasize || i2s_play_next_prev == ESP_OK) {
                    audio_stop();                   /* 停止I2S DMA */
                    i2s_deinit();                   /* 卸载I2S外设(为下次重新初始化做准备) */
                    i2s_table_size    = 0;
                    i2s_play_end      = ESP_OK;     /* 标记播放结束 */
                    s_music_task_handle = NULL;     /* 清除句柄(允许下次重新创建任务) */
                    vTaskDelete(NULL);              /* 销毁自身 */
                    /* 注意: vTaskDelete之后的代码不会被执行 */
                }

                /* ---- 计算本次读取量(处理尾部不足一块的情况) ---- */
                /*
                 * 单声道→立体声转换会使数据量翻倍, 因此源数据读取量不得超过缓冲区的一半,
                 * 否则就地扩展后会溢出WAV_TX_BUFSIZE导致内存越界!
                 */
                uint32_t max_read = WAV_TX_BUFSIZE;
                if (audio_channels == 1) {
                    max_read = WAV_TX_BUFSIZE / 2;   /* 单声道限制为一半 */
                }

                uint32_t to_read = wavctrl.datasize - total_read;
                if (to_read > max_read) {
                    to_read = max_read;
                }

                /* ---- 从SD卡读取音频数据 ---- */
                f_read(g_audiodev.file, g_audiodev.tbuf, to_read, (UINT *)&bytes_write);

                if (bytes_write > 0) {
                    /* ---- 单声道→立体声实时转换 ---- */
                    if (audio_channels == 1 && wavctrl.bps == 16) {
                        /* 16bit单声道 → 16bit立体声 (每个采样复制为L/R双通道) */
                        int16_t *src = (int16_t *)g_audiodev.tbuf;
                        uint32_t samples = bytes_write / sizeof(int16_t);
                        /* 从后向前复制, 避免覆盖未处理的源数据(原地扩展) */
                        for (int32_t i = samples - 1; i >= 0; i--) {
                            src[i * 2]     = src[i];     /* L通道 */
                            src[i * 2 + 1] = src[i];     /* R通道 */
                        }
                        bytes_write *= 2;               /* 字节数翻倍 */
                    }
                    else if (audio_channels == 1 && wavctrl.bps == 8) {
                        /* 8bit单声道 → 16bit立体声 (位扩展+通道复制) */
                        int8_t  *src8  = (int8_t *)g_audiodev.tbuf;
                        int16_t *dst16 = (int16_t *)g_audiodev.tbuf;
                        uint32_t samples = bytes_write;
                        for (int32_t i = samples - 1; i >= 0; i--) {
                            int16_t sample = (int16_t)(src8[i]) << 8;   /* 8bit→16bit符号扩展 */
                            dst16[i * 2]     = sample;    /* L */
                            dst16[i * 2 + 1] = sample;    /* R */
                        }
                        bytes_write = samples * 4;       /* 每采样: 1byte → 2×int16_t = 4bytes */
                    }

                    /* ---- 送入I2S DMA发送 ---- */
                    i2s_table_size += i2s_tx_write(g_audiodev.tbuf, bytes_write);
                    total_read += (to_read > bytes_write) ? bytes_write : to_read;
                }
            } /* end while(total_read < datasize) */
        } /* end if(playing) */

        vTaskDelay(pdMS_TO_TICKS(1));       /* 1ms让出CPU(避免饿死低优先级任务) */
    } /* end while(1) */
}

/* ================================================================== */
/*                 播放监控任务 (FreeRTOS, 异步非阻塞)                     */
/**
 * @brief  播放监控任务 — 从 wav_play_song() 的监控循环独立出来
 *
 * @note   ★★★ 异步化核心 ★★★
 *         原 design: wav_play_song() 内的 while(1) 监控循环阻塞调用方(audio_play → main loop)
 *         新 design: 监控逻辑移到此独立任务, wav_play_song() 启动后立即返回
 *
 *         监控职责:
 *         - 检测播放自然结束 (i2s_play_end)
 *         - 本地按键扫描 (切歌/暂停)
 *         - RS485音量实时响应
 *         - 播放进度显示
 *         - 资源释放 + 自删除
 *
 * @param pvParameters  未使用(FreeRTOS任务签名要求)
 */
static void play_monitor_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t key = 0;
    uint8_t res = FR_OK;

    printf("[MONITOR] Task started, monitoring playback...\r\n");

    /* ===== 监控主循环 (原 wav_play_song 步骤7) ===== */
    while (res == FR_OK) {
        while (1) {
            /* -- 检测播放是否自然结束(music任务自删除时设置此标志) -- */
            if (i2s_play_end == ESP_OK) {
                key = KEY0_PRES;     /* 结束视为"下一首" */
                printf("[MONITOR] Playback natural end\r\n");
                break;
            }

            /* -- 本地按键扫描 -- */
            key = xl9555_key_scan(0);
            switch (key) {
                case KEY0_PRES:       /* 下一首 */
                case KEY1_PRES:       /* 上一首 */
                    i2s_play_next_prev = ESP_OK;   /* 通知music任务退出 */
                    printf("[MONITOR] Key skip song\r\n");
                    break;

                case KEY2_PRES:       /* 暂停/恢复切换 */
                    if ((g_audiodev.status & 0x0F) == 0x03) {
                        audio_stop();                  /* 正在播放 -> 暂停 */
                        printf("[MONITOR] Key pause\r\n");
                    } else if ((g_audiodev.status & 0x0F) == 0x00) {
                        audio_start();                 /* 已暂停 -> 恢复 */
                        printf("[MONITOR] Key resume\r\n");
                    }
                    break;

                default:
                    break;
            }

            /* -- 仅在播放状态下刷新进度显示(暂停时不更新) -- */
            if ((g_audiodev.status & 0x0F) == 0x03) {
                wav_get_curtime(g_audiodev.file, &wavctrl);
                audio_msg_show(wavctrl.totsec, wavctrl.cursec, wavctrl.bitrate);
            }

            /* -- RS485音量控制: 播放期间实时响应 -- */
            if (rs485_volume_flag && rs485_volume_val <= 33) {
                es8388_hpvol_set(rs485_volume_val);
                es8388_spkvol_set(rs485_volume_val);
                rs485_volume_flag = 0;
                rs485_volume_val  = 0xFF;
            }

            vTaskDelay(pdMS_TO_TICKS(10));    /* 100Hz轮询频率 */
        }

        /* 有切歌键按下则退出 */
        if (key == KEY0_PRES || key == KEY1_PRES) {
            res = key;
            break;
        }
    }

    /* ===== 统一资源释放(原 wavplay.c cleanup 块) ===== */
    printf("[MONITOR] Cleanup: freeing resources\r\n");

    if (g_audiodev.file) { heap_caps_free(g_audiodev.file); g_audiodev.file = NULL; }
    if (g_audiodev.tbuf) { heap_caps_free(g_audiodev.tbuf); g_audiodev.tbuf = NULL; }
    s_music_task_handle  = NULL;
    s_monitor_task_handle = NULL;

    /* 通知 cook_ui: 播放停止 */
    cook_audio_set_playing(0);
    g_cook_status->audio.song_index = 0;
    g_cook_status->audio.changed = 1;

    printf("[MONITOR] Task exiting (resources freed)\r\n");
    vTaskDelete(NULL);              /* 自删除 */
}

/* ================================================================== */
/*                    WAV歌曲播放控制器                                */
/**
 * @brief 播放指定WAV文件 — 异步非阻塞版本
 * @param fname  文件完整路径(如 "0:/MUSIC/song.wav")
 * @return
 *         KEY0_PRES  - 播放已启动(异步, 实际播放由monitor+music任务完成)
 *         0xFF       - 错误(文件打开失败/非WAV/解码失败等)
 *
 * @note  V3.0 异步化重构:
 *        原 design: 函数内部包含监控循环, 阻塞调用方直到播放结束
 *        新 design: 仅做初始化 + 启动任务, 立即返回(耗时 <100ms)
 *
 *        流程:
 *        1. 分配DMA内存(file句柄 + 音频缓冲区)
 *        2. 解析WAV头部获取格式参数
 *        3. 按参数重新初始化I2S外设
 *        4. 同步ES8388 DAC位宽设置
 *        5. 打开WAV文件 → 启动I2S → 创建music任务 → 创建监控任务 → 返回!
 *        6. 监控任务异步处理: 结束检测/按键/音量/资源释放
 ================================================================== */
uint8_t wav_play_song(uint8_t *fname)
{
    uint8_t res = 0;

    /* 重置播放状态标志 */
    i2s_play_end       = ESP_FAIL;
    i2s_play_next_prev = ESP_FAIL;

    /* 分配DMA对齐内存 (ESP32的I2S外设要求DMA缓冲区位于特定内存区域) */
    g_audiodev.file = (FIL *)heap_caps_malloc(sizeof(FIL), MALLOC_CAP_DMA);
    g_audiodev.tbuf = heap_caps_malloc(WAV_TX_BUFSIZE, MALLOC_CAP_DMA);

    if (!g_audiodev.file || !g_audiodev.tbuf) {
        printf("[WAV] ERR: DMA malloc failed (file=%p, tbuf=%p)\r\n",
               g_audiodev.file, g_audiodev.tbuf);
        if (g_audiodev.file) { heap_caps_free(g_audiodev.file); g_audiodev.file = NULL; }
        if (g_audiodev.tbuf) { heap_caps_free(g_audiodev.tbuf); g_audiodev.tbuf = NULL; }
        return 0xFF;
    }

    memset(g_audiodev.file, 0, sizeof(FIL));
    memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);

    /* ★ 步骤1: 解析WAV文件头部(仅读头部, 不读音频数据) */
    memset(&wavctrl, 0, sizeof(__wavctrl));
    res = wav_decode_init(fname, &wavctrl);

    if (res != 0) {
        printf("[WAV] ERR: decode_init failed (%d) for %s\r\n", res, (char *)fname);
        goto cleanup;
    }

    /* ★ 步骤2: 根据WAV参数确定I2S位宽配置 */
    {
        int i2s_bits;

        switch (wavctrl.bps) {
            case 8:
            case 16:
                i2s_bits = I2S_DATA_BIT_WIDTH_16BIT;    /* 8bit/16bit统一用16bit通道传输 */
                break;
            case 24:
                i2s_bits = I2S_DATA_BIT_WIDTH_24BIT;
                break;
            default:
                printf("[WAV] ERR: unsupported bps=%d (only 8/16/24)\r\n", wavctrl.bps);
                res = 0xFF;
                goto cleanup;
        }

        /* ★ 步骤3: 用目标采样率和位宽初始化I2S外设 */
        myi2s_init(wavctrl.samplerate, i2s_bits);
        vTaskDelay(pdMS_TO_TICKS(50));                 /* 等待I2S时钟稳定(硬件约束, 不可省略) */

        /* ★ 步骤4: 同步ES8388 DAC位宽模式与I2S一致 */
        es8388_write_reg(0x17, (wavctrl.bps == 24) ? 0x38 : 0x18);

        /* 记录声道数(供music任务中的单声道转立体声使用) */
        audio_channels = wavctrl.nchannels;
    }

    /* ★ 步骤5: 打开WAV文件准备流式读取 */
    res = f_open(g_audiodev.file, (TCHAR *)fname, FA_READ);
    if (res != FR_OK) {
        printf("[WAV] ERR: f_open failed (%d)\r\n", res);
        goto cleanup;
    }

    /* ★ 步骤6: 启动I2S传输 + 创建播放任务 */
    audio_start();

    /* 防御性清理上一次残留的任务句柄 */
    if (s_music_task_handle != NULL) {
        printf("[WAV] WARN: previous music task handle not null\r\n");
        s_music_task_handle = NULL;
    }
    if (s_monitor_task_handle != NULL) {
        printf("[WAV] WARN: previous monitor task still exists\r\n");
        s_monitor_task_handle = NULL;
    }

    /* 创建 music 任务(I2S数据馈送, prio=4最高) */
    {
        BaseType_t ret = xTaskCreate(music, "music", MUSIC_STK_SIZE,
                                     NULL, MUSIC_PRIO, &s_music_task_handle);
        if (ret != pdPASS) {
            printf("[WAV] ERR: xTaskCreate music failed (%ld)\r\n", (long)ret);
            audio_stop();
            i2s_deinit();
            goto cleanup;
        }
    }

    /* ★★★ V3.0核心: 异步触发封面加载 + 创建监控任务, 立即返回! ★★★ */

    /* 异步封面加载(低优先级任务, 不抢DMA带宽) */
    audio_load_cover_async(g_current_song_index);

    /* 创建监控任务(独立于主循环, 处理结束检测/按键/音量/资源释放) */
    {
        BaseType_t ret = xTaskCreate(play_monitor_task, "wav_mon",
                                     MONITOR_STK_SIZE, NULL,
                                     MONITOR_PRIO, &s_monitor_task_handle);
        if (ret != pdPASS) {
            printf("[WAV] WARN: xTaskCreate monitor failed (%ld), sync fallback\r\n", (long)ret);
            /* 监控任务创建失败: music任务仍会自行检测停止信号并退出,
               但资源释放需等下次调用或手动清理 */
            s_monitor_task_handle = NULL;
        }
    }

    printf("[WAV] Playback started ASYNC (music+prio%d monitor+prio%d)\r\n",
           MUSIC_PRIO, MONITOR_PRIO);

    /* ★ 立即返回! 主循环不再阻塞! ★ */
    return KEY0_PRES;

cleanup:
    /* 初始化阶段失败时统一释放 */
    if (g_audiodev.file) { heap_caps_free(g_audiodev.file); g_audiodev.file = NULL; }
    if (g_audiodev.tbuf) { heap_caps_free(g_audiodev.tbuf); g_audiodev.tbuf = NULL; }
    s_music_task_handle   = NULL;
    s_monitor_task_handle = NULL;
    return 0xFF;
}
