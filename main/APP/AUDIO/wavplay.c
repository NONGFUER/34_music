/**
 ****************************************************************************************************
 * @file        wavplay.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       wav解码 代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 ESP32-P4 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "wavplay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "es8388.h"
#include "audioplay.h"

/******************************************************************************************************/
/*FreeRTOS配置*/

/* MUSIC 任务 配置
 * 包括: 任务句柄 任务优先级 堆栈大小 创建任务
 */
#define MUSIC_PRIO      4                   /* 任务优先级 */
#define MUSIC_STK_SIZE  5*1024              /* 任务堆栈大小 */
TaskHandle_t            MUSICTask_Handler;  /* 任务句柄 */
void music(void *pvParameters);             /* 任务函数 */

static portMUX_TYPE my_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* 外部ADC句柄（main.c中初始化，用于电位器音量控制） */
extern adc_oneshot_unit_handle_t adc1_handle;

/******************************************************************************************************/

__wavctrl wavctrl;                          /* WAV音频文件解码参数结构体 */
UINT bytes_write = 0;                       /* 写一次I2S大小 */
volatile long long int i2s_table_size = 0;  /* 积累每次发送音频数据总大小 */
esp_err_t i2s_play_end = ESP_FAIL;          /* 播放结束标志位 */
esp_err_t i2s_play_next_prev = ESP_FAIL;    /* 下一首或者上一首标志位 */
FSIZE_t file_read_pos = 0;                  /* 记录当前WAV读取位置 */
uint8_t audio_channels = 2;                 /* 当前音频声道数(默认立体声),供播放循环做单声道转立体声 */

/**
 * @brief       WAV解析初始化
 * @param       fname : 文件路径+文件名
 * @param       wavx  : 信息存放结构体指针
 * @retval      0,打开文件成功
 *              1,打开文件失败
 *              2,非WAV文件
 *              3,DATA区域未找到
 */
uint8_t wav_decode_init(uint8_t *fname, __wavctrl *wavx)
{
    FIL *ftemp;
    uint8_t *buf; 
    uint32_t br = 0;
    uint8_t res = 0;

    ChunkRIFF *riff;
    ChunkFMT *fmt;
    ChunkFACT *fact;
    ChunkDATA *data;
    
    ftemp = (FIL*)malloc(sizeof(FIL));
    buf = malloc(1024);
    
    if (ftemp && buf)
    {
        res = f_open(ftemp, (TCHAR*)fname, FA_READ);            /* 打开文件 */
        
        if (res == FR_OK)
        {
            f_read(ftemp, buf, 1024, (UINT *)&br);              /* 读取WAV头数据(增大到1024以支持带metadata的文件) */
            riff = (ChunkRIFF *)buf;
            
            if (riff->Format == 0x45564157)                     /* 是WAV文件 */
            {
                fmt = (ChunkFMT *)(buf + 12);
                
                /* 健壮的chunk链表遍历: 从fmt chunk之后逐个查找data chunk */
                uint32_t offset = 12 + 8 + fmt->ChunkSize;      /* 跳过RIFF(12) + fmt(ID+Size+Data) */
                data = NULL;
                
                while (offset < 900)  /* 安全上限,防止损坏文件导致越界 */
                {
                    if (offset + 8 > 1024) break;  /* 缓冲区边界检查 */
                    
                    uint32_t *p_chunk_id   = (uint32_t *)(buf + offset);
                    uint32_t *p_chunk_size = (uint32_t *)(buf + offset + 4);
                    
                    if (*p_chunk_id == 0x61746164)             /* 找到 "data" chunk */
                    {
                        data = (ChunkDATA *)(buf + offset);
                        break;
                    }
                    /* 跳过此chunk: 8字节(ID+Size) + ChunkSize数据 */
                    offset += 8 + (*p_chunk_size);
                    if (*p_chunk_size & 1) offset++;           /* 奇数对齐(WAV规范) */
                }
                
                if (data != NULL && data->ChunkID == 0x61746164)  /* 解析成功 */
                {
                    wavx->audioformat = fmt->AudioFormat;       /* 音频格式 */
                    wavx->nchannels = fmt->NumOfChannels;       /* 通道数 */
                    wavx->samplerate = fmt->SampleRate;         /* 采样率 */
                    wavx->bitrate = fmt->ByteRate * 8;
                    wavx->blockalign = fmt->BlockAlign;
                    wavx->bps = fmt->BitsPerSample;
                    
                    /* 仅支持PCM格式(AudioFormat=1), 其他格式会严重失真 */
                    if (wavx->audioformat != 1)
                    {
                        printf("Err: unsupported format %d(only PCM=1)\r\n", wavx->audioformat);
                        res = 4;    /* 非PCM格式 */
                    }
                    else
                    {
                        wavx->datasize = data->ChunkSize;
                        wavx->datastart = offset + 8;           /* data chunk起始位置 = offset + 跳过"data"+Size(8字节) */
                     
                    printf("wavx->audioformat:%d\r\n", wavx->audioformat);
                    printf("wavx->nchannels:%d\r\n", wavx->nchannels);
                    printf("wavx->samplerate:%ld\r\n", wavx->samplerate);
                    printf("wavx->bitrate:%ld\r\n", wavx->bitrate);
                    printf("wavx->blockalign:%d\r\n", wavx->blockalign);
                    printf("wavx->bps:%d\r\n", wavx->bps);
                    printf("wavx->datasize:%ld\r\n", wavx->datasize);
                    printf("wavx->datastart:%ld\r\n", wavx->datastart);
                    }  /* end else (PCM format OK) */
                }
                else
                {
                    res = 3;
                }
            }
            else
            {
                res = 2;
            }
        }
        else
        {
            res = 1;
        }
    }
    
    f_close(ftemp);
    free(ftemp);
    free(buf); 
    
    return res;
}

/**
 * @brief       获取当前播放时间
 * @param       fx    : 文件指针
 * @param       wavx  : wavx播放控制器
 * @retval      无
 */
void wav_get_curtime(FIL *fx, __wavctrl *wavx)
{
    long long fpos = 0;

    wavx->totsec = wavx->datasize / (wavx->bitrate / 8);    /* 歌曲总长度(单位:秒) */
    fpos = fx->fptr-wavx->datastart;                        /* 得到当前文件播放到的地方 */
    wavx->cursec = fpos * wavx->totsec / wavx->datasize;    /* 当前播放到第多少秒了? */
}

/**
 * @brief       music任务
 * @param       pvParameters : 传入参数(未用到)
 * @retval      无
 */
void music(void *pvParameters)
{
    pvParameters = pvParameters;

    /* ES8388初始化配置，有效降低启动时发出沙沙声
     * ★ 关键时序优化：先配置ES8388内部参数 → 填充静音数据到I2S缓冲区 → 最后才使能功放
     *   这样可以避免功放打开瞬间放大DAC的随机/不稳定输出，消除POP音和杂音 */
    es8388_adda_cfg(1,0);                           /* 打开DAC，关闭ADC */
    es8388_input_cfg(0);                            /* 录音关闭 */
    es8388_output_cfg(1,1);                         /* 同时打开喇叭(线路)和耳机通道 */
    es8388_hpvol_set(20);                           /* 设置耳机音量(有效范围0~33) */
    es8388_spkvol_set(20);                          /* 喇叭/线路音量，供外部功放使用 */
    vTaskDelay(pdMS_TO_TICKS(20));                  /* 等待ES8388内部配置稳定 */
    i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);  /* 先发送一段无声音的数据，填充I2S缓冲区 */
    vTaskDelay(pdMS_TO_TICKS(10));                  /* 等待I2S将静音数据播放出去，确保DAC输出稳定为0 */
    xl9555_pin_write(SPK_EN_IO,0);                  /* ★ 最后才打开喇叭功放使能（此时输出已是静音） */

    while(1)
    {
        if ((g_audiodev.status & 0x0F) == 0x03)     /* 打开了音频 */
        {
            /* 使用while循环替代for, 正确处理尾部不足WAV_TX_BUFSIZE的数据 */
            uint32_t total_read = 0;
            while (total_read < wavctrl.datasize)
            {
                if ((g_audiodev.status & 0x0F) == 0x00)             /* 暂停播放 */
                {
                    file_read_pos = f_tell(g_audiodev.file);        /* 记录暂停位置 */

                    while(1)
                    {
                        if ((g_audiodev.status & 0x0F) == 0x03)     /* 重新打开了 */
                        {
                            break;
                        }

                        vTaskDelay(pdMS_TO_TICKS(5));               /* 死等 */
                    }

                    f_lseek(g_audiodev.file, file_read_pos);        /* 跳过到之前停止的位置 */
                }

                /* 判断是否播放完成 */
                if (i2s_table_size >= wavctrl.datasize || i2s_play_next_prev == ESP_OK)
                {
                    audio_stop();                   /* 先停止播放 */
                    i2s_deinit();                   /* 卸载I2S */
                    i2s_table_size = 0;             /* 总大小清零 */
                    i2s_play_end = ESP_OK;          /* 已播放完成标志位 */
                    vTaskDelete(NULL);              /* 删除当前任务 */
                    vTaskDelay(pdMS_TO_TICKS(5));   /* 适当延时（为了删除这个任务） */
                    break;                          /* 防止延时5ms未能删除音频任务 */
                }

                /* 计算本次应读取的字节数(处理尾部剩余数据)
                 * 单声道转立体声会使数据翻倍,因此读取量不能超过缓冲区一半,
                 * 否则转换后会溢出缓冲区导致蓝屏! */
                uint32_t max_read = WAV_TX_BUFSIZE;
                if (audio_channels == 1) max_read = WAV_TX_BUFSIZE / 2;   /* 单声道限制为一半 */

                uint32_t to_read = wavctrl.datasize - total_read;
                if (to_read > max_read) to_read = max_read;              /* 使用修正后的上限 */

                f_read(g_audiodev.file, g_audiodev.tbuf, to_read, (UINT*)&bytes_write);

                if (bytes_write > 0)
                {
                    /* 单声道转立体声: 将单声道数据复制到左右声道 */
                    if (audio_channels == 1 && wavctrl.bps == 16)
                    {
                        int16_t *src = (int16_t *)g_audiodev.tbuf;
                        uint32_t samples = bytes_write / sizeof(int16_t);
                        /* 从后往前复制,避免覆盖未处理数据 */
                        for (int32_t i = samples - 1; i >= 0; i--)
                        {
                            src[i * 2]     = src[i];     /* L通道 = 原始采样 */
                            src[i * 2 + 1] = src[i];     /* R通道 = 复制采样 */
                        }
                        bytes_write *= 2;                /* 字节数翻倍(单声道->立体声) */
                    }
                    else if (audio_channels == 1 && wavctrl.bps == 8)
                    {
                        /* 8bit单声道转立体声16bit */
                        int8_t *src8 = (int8_t *)g_audiodev.tbuf;
                        int16_t *dst16 = (int16_t *)g_audiodev.tbuf;
                        uint32_t samples = bytes_write;
                        for (int32_t i = samples - 1; i >= 0; i--)
                        {
                            int16_t sample = src8[i] << 8;    /* 8bit扩展到16bit */
                            dst16[i * 2]     = sample;        /* L */
                            dst16[i * 2 + 1] = sample;        /* R */
                        }
                        bytes_write = samples * 4;           /* 每个采样变成2个16bit=4字节 */
                    }

                    i2s_table_size += i2s_tx_write(g_audiodev.tbuf, bytes_write);
                    total_read += (to_read > bytes_write) ? bytes_write : to_read;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    vTaskDelete(NULL);
}

/**
 * @brief       播放某个wav文件
 * @param       fname : 文件路径+文件名
 * @retval      KEY0_PRES : 下一首
 *              KEY1_PRES : 上一首
 *              KEY2_PRES : 停止/启动
 *              其他,非WAV文件
 */
uint8_t wav_play_song(uint8_t *fname)
{
    uint8_t key = 0;
    uint8_t res = 0;
    
    i2s_play_end = ESP_FAIL;
    i2s_play_next_prev = ESP_FAIL;
    g_audiodev.file = (FIL*)heap_caps_malloc(sizeof(FIL),MALLOC_CAP_DMA);
    g_audiodev.tbuf = heap_caps_malloc(WAV_TX_BUFSIZE, MALLOC_CAP_DMA);       /* 音频数据 */

    /* ★ 先解析WAV文件获取采样率和位宽参数 */
    memset(&wavctrl,0,sizeof(__wavctrl));       /* 对WAV结构体相关参数清零 */
    res = wav_decode_init(fname, &wavctrl);     /* 对wav音频文件解码 */

    if (g_audiodev.file || g_audiodev.tbuf)
    {
        memset(g_audiodev.file,0,sizeof(FIL));      /* 文件指针清零 */
        memset(g_audiodev.tbuf,0,WAV_TX_BUFSIZE);   /* buf清零 */

        if (res == 0)                               /* 解码成功 */
        {
            /* ★ 根据解析结果确定I2S位宽参数 */
            int i2s_bits;
            if (wavctrl.bps == 16 || wavctrl.bps == 8)
                i2s_bits = I2S_DATA_BIT_WIDTH_16BIT;   /* 8bit/16bit都扩展为16bit播放 */
            else if (wavctrl.bps == 24)
                i2s_bits = I2S_DATA_BIT_WIDTH_24BIT;
            else
            {
                printf("Err: unsupported bps:%d(only 8/16/24)\r\n", wavctrl.bps);
                res = 0xFF;
            }

            if (res != 0xFF)
            {
                myi2s_init(wavctrl.samplerate, i2s_bits);              /* ★ 用目标参数直接初始化I2S(避免reconfig不可靠) */
                vTaskDelay(pdMS_TO_TICKS(50));

                /* 配置ES8388 DAC与I2S同步 */
                if (wavctrl.bps == 16 || wavctrl.bps == 8)
                    es8388_write_reg(0x17, 0x18);                      /* DAC 16bit模式 */
                else if (wavctrl.bps == 24)
                    es8388_write_reg(0x17, 0x38);                      /* DAC 24bit模式 */

                audio_channels = wavctrl.nchannels;                     /* 记录声道数供播放循环使用 */
                printf("audio_channels:%d\r\n", audio_channels);
            }

            res = f_open(g_audiodev.file, (TCHAR*)fname, FA_READ);      /* 打开WAV音频文件 */

            if (res == FR_OK)
            {
                audio_start();  /* 开启I2S */
                /* 打开成功后，才创建音频任务 */
                if (MUSICTask_Handler == NULL && res == FR_OK)
                {
                    taskENTER_CRITICAL(&my_spinlock);
                    xTaskCreate(music, "music", 4096, &MUSICTask_Handler, 5, NULL);
                    taskEXIT_CRITICAL(&my_spinlock);
                }

                while (res == FR_OK)
                { 
                    while (1)
                    {
                        /* 播放结束，下一首 */
                        if (i2s_play_end == ESP_OK)
                        {
                            res = KEY0_PRES;
                            break;
                        }

                        key = xl9555_key_scan(0);

                        switch (key)
                        {
                            /* 下一首/上一首 */
                            case KEY0_PRES:
                            case KEY1_PRES:
                                i2s_play_next_prev = ESP_OK;
                                break;
                            /* 暂停/开启 */
                            case KEY2_PRES:
                                if ((g_audiodev.status & 0x0F) == 0x03)
                                {
                                    audio_stop();
                                }
                                else if ((g_audiodev.status & 0x0F) == 0x00)
                                {
                                    audio_start();
                                }
                                break;
                        }

                        if ((g_audiodev.status & 0x0F) == 0x03)                 /* 暂停不刷新时间 */
                        {
                            wav_get_curtime(g_audiodev.file, &wavctrl);         /* 得到总时间和当前播放的时间 */
                            audio_msg_show(wavctrl.totsec, wavctrl.cursec, wavctrl.bitrate);
                        }

                        /* ★ RS485音量控制: 播放期间实时响应 */
                        if (rs485_volume_flag && rs485_volume_val <= 33)
                        {
                            es8388_hpvol_set(rs485_volume_val);
                            es8388_spkvol_set(rs485_volume_val);
                            printf("[MUSIC] Volume -> %d/33\r\n", rs485_volume_val);
                            rs485_volume_flag = 0;
                            rs485_volume_val = 0xFF;
                        }

                        vTaskDelay(pdMS_TO_TICKS(10));
                    }

                    if (key == KEY1_PRES || key == KEY0_PRES)                   /* 退出切换歌曲 */
                    {
                        res = key;
                        break;
                    }
                }
            }
            else
            {
                res = 0xFF;
            }
        }
        else
        {
            res = 0xFF;
        }
    }
    else
    {
        res = 0xFF;
    }

    heap_caps_free(g_audiodev.file);
    heap_caps_free(g_audiodev.tbuf);
    g_audiodev.tbuf = NULL;
    g_audiodev.file = NULL;
    MUSICTask_Handler = NULL;
    return res;
}
