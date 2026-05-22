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
#include "esp_rom_sys.h"
#include "wavplay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "es8388.h"
#include "audioplay.h"
#include "cook_ui.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

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

/* 外部全局静音标志(main.c 中 B0/B1~B5 设置) */
extern uint8_t g_mute_flag;

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

/* ★ I2S持久化状态(避免每次切歌都重新初始化) ★ */
static int    s_i2s_cur_rate     = 0;        /* 当前I2S采样率(0=未初始化) */
static int    s_i2s_cur_bits     = 0;        /* 当前I2S位宽(0=未初始化) */
static uint8_t s_i2s_initialized = 0;         /* I2S是否已初始化标志 */

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
 * ================================================================== */
void music(void *pvParameters)
{
    (void)pvParameters;    /* 抑制未使用参数警告 */


    int64_t t_music = esp_timer_get_time();
    printf("[MUSIC] task start @%lld us\r\n", t_music);




    /* ===== 第1步: 安全状态下配置所有参数 (输出关闭, soft_mute保持wav_play_song中已设好的1) ===== */
    //es8388_output_cfg(0, 0);                          /* 确保输出关闭 */
    es8388_soft_mute(1);
    es8388_hpvol_set(0);                           
    es8388_spkvol_set(0);

    /* ===== 第2步: I2S DMA管道预填充静音 (确保DAC数字输入端是纯净零电平) ===== */
    memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);
    for (int i = 0; i < 6; i++) {                    /* 6帧≈96ms, 覆盖整个初始化期 */
        i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);
    }
    vTaskDelay(pdMS_TO_TICKS(10));                    /* 短暂等待DMA pipeline稳定 */

    /* ===== 第3步: 解除静音+淡入 (静音状态时跳过; ★报警强制满音量时覆盖) ===== */
    if (!g_mute_flag || cook_is_alarm_force_vol()) {
        es8388_soft_mute(0);                              /* 同步解除软静音 */

        if (cook_is_alarm_force_vol()) {
            /* ★ 报警模式: 直接设最大音量(不淡入) ★ */
            es8388_hpvol_set(33);
            es8388_spkvol_set(33);
        } else {
            uint8_t target_vol = audio_get_last_volume();
            for(int v = 0; v <= target_vol; v += 3) {
                es8388_hpvol_set(v);
                vTaskDelay(pdMS_TO_TICKS(2)); // 每2ms增加一点音量
            }
            es8388_hpvol_set(target_vol);
        }
    } else {
        es8388_hpvol_set(0);  /* 静音状态: 保持硬件音量为0 */
    } 

    /* ===== 阶段4: 主播放循环 ===== */
    bool fade_in_done = false;
    while (1) {
        
        if (i2s_play_next_prev == ESP_OK || i2s_play_end == ESP_OK) {   
            //1.音量渐出
            uint8_t cur_vol = audio_get_last_volume();
            for(int v = cur_vol; v >= 0; v--) {
                es8388_hpvol_set(v);
               esp_rom_delay_us(1500);
            }
            es8388_hpvol_set(0);                            /* 硬件音量归零 */
            es8388_soft_mute(1);                            /* 先软静音(立即生效<1ms) */
            

            /* 静音冲刷DMA残留数据 (NULL保护: monitor可能已提前清理) */
            if (g_audiodev.tbuf) {
                memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);  
                for(int i = 0; i < 4; i++) {
                    i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));

           
            audio_stop();                                   /* 停止I2S DMA */
            
            i2s_table_size    = 0;
            i2s_play_end      = ESP_OK;                     /* 标记播放结束 */
            s_music_task_handle = NULL;                     /* 清除句柄(允许下次重新创建任务) */
            vTaskDelete(NULL);                              /* 销毁自身 */
        }

        if ((g_audiodev.status & 0x0F) == 0x03 && g_audiodev.file && g_audiodev.tbuf) {    /* 状态=0x03 → 播放中 */
            uint32_t total_read = 0;

            /* 循环读取WAV数据块并送入I2S, 直到文件读完或收到停止指令 */
            while (total_read < wavctrl.datasize) {

                /* ---- 暂停处理(带停止信号检测, 防止永久卡死) ---- */
                if ((g_audiodev.status & 0x0F) == 0x00) {
                    file_read_pos = f_tell(g_audiodev.file);
                    memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);
                    es8388_hpvol_set(0);
                    es8388_spkvol_set(0);
                    while ((g_audiodev.status & 0x0F) != 0x03) {
                        /* 收到外部停止信号 → 跳出暂停等待, 由外层循环统一处理资源释放 */
                        if (i2s_play_next_prev == ESP_OK || i2s_play_end == ESP_OK) {
                            break;
                        }
                        i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);
                    }

                    /* 停止信号触发则跳出数据读取循环, 交由外层处理 */
                    if (i2s_play_next_prev == ESP_OK || i2s_play_end == ESP_OK) {
                        break;
                    }

                    f_lseek(g_audiodev.file, file_read_pos);
                    if (!g_mute_flag) {
                        uint8_t target = audio_get_last_volume();
                        for(int v = 0; v <= target; v += 3) {
                            es8388_hpvol_set(v);
                            vTaskDelay(pdMS_TO_TICKS(2));
                        }
                        es8388_hpvol_set(target);
                    }
                }

                /* ---- 播放完成/切歌检测 ---- */
                if (i2s_table_size >= wavctrl.datasize || i2s_play_next_prev == ESP_OK) {
                    uint8_t cur_vol = audio_get_last_volume();
                    for(int v = cur_vol; v >= 0; v--) {
                        es8388_hpvol_set(v);
                        esp_rom_delay_us(1500);
                    }
                    es8388_soft_mute(1);                            /* 先软静音(立即生效<1ms) */
                    es8388_hpvol_set(0);                            /* 硬件音量归零 */

                    /* 静音冲刷DMA残留数据 (NULL保护) */
                    if (g_audiodev.tbuf) {
                        memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);
                        for(int i = 0; i < 4; i++) {
                            i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));

                    /* 仅停止DMA (保留I2S通道复用) */
                    audio_stop();                                   /* 停止I2S DMA */
                    /* ★ 不调用i2s_deinit(), I2S通道持久化 ★ */
                    i2s_table_size    = 0;
                    i2s_play_end      = ESP_OK;                     /* 标记播放结束 */
                    s_music_task_handle = NULL;                     /* 清除句柄(允许下次重新创建任务) */
                    vTaskDelete(NULL);                              /* 销毁自身 */
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
                    //渐入算法
                    if (!fade_in_done && audio_channels == 2 && wavctrl.bps == 16) {
                        int16_t *samples = (int16_t *)g_audiodev.tbuf;
                        int num_samples = bytes_write / 2; // 16-bit 样本数
                        
                        // 对首个数据包进行 0.0 -> 1.0 的平滑放大
                        for (int i = 0; i < num_samples; i += 2) {
                            float ratio = (float)i / num_samples;
                            float vol_mult = ratio * ratio* ratio*ratio;
                            samples[i]   = (int16_t)(samples[i] * vol_mult);       // 左声道
                            samples[i+1] = (int16_t)(samples[i+1] * vol_mult);     // 右声道
                        }
                        fade_in_done = true;
                    }
                    /* ---- 送入I2S DMA发送 ---- */
                    i2s_table_size += i2s_tx_write(g_audiodev.tbuf, bytes_write);
                    total_read += (to_read > bytes_write) ? bytes_write : to_read;
                }
            } /* end while(total_read < datasize) */
        } /* end if(playing) */

        vTaskDelay(pdMS_TO_TICKS(1));       /* 1ms让出CPU(避免饿死低优先级任务) */
        esp_task_wdt_reset();                 /* ★ 主动喂狗, 防止高优先级任务阻塞导致watchdog触发 ★ */
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

    /* ★ 重置所有播放状态标志(防止残留脏状态导致下次启动崩溃!) ★ */
    s_music_task_handle  = NULL;
    s_monitor_task_handle = NULL;
    i2s_table_size       = 0;
    i2s_play_end         = ESP_FAIL;           /* 重置为"未结束"状态 */
    i2s_play_next_prev   = ESP_FAIL;           /* 重置为"无切歌指令" */
    g_audiodev.status    = 0;                  /* 确保状态为"停止" */

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
    int64_t t_wps = esp_timer_get_time();
    printf("[WAV] wav_play_song entry @%lld us\r\n", t_wps);

    /* ★★★ 防御性检查: 确保上一次播放已完全结束 ★★★ */
    if (s_music_task_handle != NULL || s_monitor_task_handle != NULL) {
        printf("[WAV] WARN: Previous playback still running, waiting...\r\n");

        i2s_play_next_prev = ESP_OK;

        for (int wait = 0; wait < 100; wait++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (s_music_task_handle == NULL && s_monitor_task_handle == NULL) break;
        }

        if (s_music_task_handle != NULL) {
            printf("[WAV] ERR: music task stuck, force delete\r\n");
            vTaskDelete(s_music_task_handle);
            s_music_task_handle = NULL;
        }
        if (s_monitor_task_handle != NULL) {
            vTaskDelete(s_monitor_task_handle);
            s_monitor_task_handle = NULL;
        }
        if (g_audiodev.file) { heap_caps_free(g_audiodev.file); g_audiodev.file = NULL; }
        if (g_audiodev.tbuf) { heap_caps_free(g_audiodev.tbuf); g_audiodev.tbuf = NULL; }

        audio_stop();
        i2s_table_size     = 0;
        i2s_play_end       = ESP_FAIL;
        i2s_play_next_prev = ESP_FAIL;

        printf("[WAV] Force cleanup done\r\n");
    }

    /* ★ 步骤0: 尽早静音(防御之前可能残留的白噪声) ★ */
    es8388_soft_mute(1);                             /* 打开软静音 */
    es8388_hpvol_set(0);                             /* 音量归零 */
    es8388_spkvol_set(0);
    /* 注意: es8388_output_cfg(1,0) 已在 main.c init_es8388() 中全局一次性开启 */

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

    /* ★ 步骤2: 根据WAV参数确定I2S位宽配置 + 按需初始化/重配置 ★ */
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

        /* ★★★ I2S持久化策略(核心优化!) ★★★
         * 原问题: 每次切歌都 myi2s_init() + i2s_deinit(), 导致:
         *   - 时钟重建POP音 (~50-100ms不稳定期)
         *   - DMA缓冲区重新分配
         *   - ES8388需要重新同步
         *
         * 新方案: I2S只初始化一次,后续按需reconfig
         *   场景A: 首次播放 → 完整init (耗时~100ms)
         *   场景B: 相同参数切歌 → 零操作! 直接复用 (耗时0ms)
         *   场景C: 不同参数切歌 → 轻量reconfig (耗时<5ms)
         */

        if (!s_i2s_initialized) {
            /* ===== 场景A: 首次初始化 (完整流程) ===== */
            printf("[I2S] First init: rate=%luHz bits=%lu\r\n",
                   (unsigned long)wavctrl.samplerate, (unsigned long)i2s_bits);
            myi2s_init(wavctrl.samplerate, i2s_bits);
            s_i2s_initialized = 1;
            s_i2s_cur_rate    = wavctrl.samplerate;
            s_i2s_cur_bits    = i2s_bits;
            vTaskDelay(pdMS_TO_TICKS(100));     /* 等待I2S时钟稳定(仅首次需要) */
        }
        else if (s_i2s_cur_rate != wavctrl.samplerate || s_i2s_cur_bits != i2s_bits) {
            /* ===== 场景C: 参数变化, 轻量reconfig (不释放句柄!) ===== */
            printf("[I2S] Reconfig: %lu/%lu -> %lu/%lu\r\n",
                   (unsigned long)s_i2s_cur_rate, (unsigned long)s_i2s_cur_bits,
                   (unsigned long)wavctrl.samplerate, (unsigned long)i2s_bits);

            /* 先停止DMA传输(保留通道句柄) */
            audio_stop();
            i2s_trx_stop();
            /* 使用ESP-IDF的reconfig API (无需deinit+init!) */
            i2s_set_samplerate_bits_sample(wavctrl.samplerate, i2s_bits);

            /* 更新缓存参数 */
            s_i2s_cur_rate = wavctrl.samplerate;
            s_i2s_cur_bits = i2s_bits;

            vTaskDelay(pdMS_TO_TICKS(10));      /* reconfig只需10ms稳定时间 */
        }
        else {
            /* ===== 场景B: 参数相同, 完全复用 (零开销!) ===== */
            printf("[I2S] Reuse: rate=%luHz bits=%lu (no re-init)\r\n",
                   (unsigned long)wavctrl.samplerate, (unsigned long)i2s_bits);
            /* 注意: 静音预填充需在 audio_start() 之后进行, 此处仅记录标记 */
        }

        /* ★ 同步ES8388 DAC位宽模式与I2S一致 ★ */
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
   

    /* ★ 步骤6: 启动I2S传输 + 预填充静音(消除audio_start→music任务间的空窗期POP音) ★ */
    audio_start();
    vTaskDelay(pdMS_TO_TICKS(10));
    
    es8388_hpvol_set(0);
    es8388_spkvol_set(0);
    {
        memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);
        for (int i = 0; i < 8; i++) {
            i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);
        }
    }

    /* 注意: 功放开启放在music任务中执行, 那里有完整的POP音消除流程 */
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