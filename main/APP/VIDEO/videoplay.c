/**
 ****************************************************************************************************
 * @file        videoplay.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       视频播放器 应用代码
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

#include "videoplay.h"
#include "exfuns.h"
#include "led.h"
#include "mytimer.h"
#include "audioplay.h"

const char *videoplay_tag = "video_play";
extern uint16_t g_frame;                /* 播放帧率 */
extern volatile uint8_t g_frameup;      /* 视频播放时隙控制变量,当等于1的时候,可以更新下一帧视频 */

/**
 * @brief       获取指定路径下有效视频文件的数量
 * @param       path: 指定路径
 * @retval      有效视频文件的数量
 */
static uint16_t video_get_tnum(char *path)
{
    uint8_t res;
    uint16_t rval = 0;
    FF_DIR tdir;            /* 临时目录 */
    FILINFO *tfileinfo;     /* 临时文件信息 */
    
    tfileinfo = (FILINFO *)malloc(sizeof(FILINFO));             /* 申请内存 */
    res = (uint8_t)f_opendir(&tdir, (const TCHAR *)path);       /* 打开目录 */
    
    if ((res == 0) && tfileinfo)
    {
        while (1)                                               /* 查询总的有效文件数 */
        {
            res = (uint8_t)f_readdir(&tdir, tfileinfo);         /* 读取目录下的一个文件 */
            
            if ((res != 0) || (tfileinfo->fname[0] == 0))       /* 错误或到末尾，退出 */
            {
                break;
            }
            
            res = exfuns_file_type(tfileinfo->fname);
            if ((res & 0xF0) == 0x60)                           /* 是视频文件 */
            {
                rval++;
            }
        }
    }
    
    free(tfileinfo);        /* 释放内存 */
    
    return rval;
}

/**
 * @brief       显示视频基本信息
 * @param       name : 视频名字
 * @param       index: 当前索引
 * @param       total: 总文件数
 * @retval      无
 */
static void video_bmsg_show(uint8_t *name, uint16_t index, uint16_t total)
{  
    uint8_t *buf = malloc(100);     /* 申请100字节内存 */
    
    sprintf((char *)buf, "文件名:%s", name);
    text_show_string(10, 10, lcddev.width - 10, 16, (char *)buf, 16, 0, RED);   /* 显示文件名 */
    sprintf((char *)buf, "索引:%d/%d", index, total);
    text_show_string(10, 30, lcddev.width - 10, 16, (char *)buf, 16, 0, RED);   /* 显示索引 */
    
    free(buf);                      /* 释放内存 */
}

/**
 * @brief       显示当前视频文件的相关信息
 * @param       aviinfo: avi控制结构体
 * @retval      无
 */
static void video_info_show(AVI_INFO *aviinfo)
{
    uint8_t *buf = malloc(100);     /* 申请100字节内存 */
    
    sprintf((char *)buf, "声道数:%d,采样率:%ld", aviinfo->Channels, aviinfo->SampleRate * 10);
    text_show_string(10, 50, lcddev.width - 10, 16, (char *)buf, 16, 0, RED);
    
    sprintf((char *)buf, "帧率:%ld帧", 1000 / (aviinfo->SecPerFrame / 1000));
    text_show_string(10, 70, lcddev.width - 10, 16, (char *)buf, 16, 0, RED);
    
    free(buf);                      /* 释放内存 */
}

/**
 * @brief       显示当前播放时间
 * @param       favi   : 当前播放的视频文件
 * @param       aviinfo: avi控制结构体
 * @retval      无
 */
void video_time_show(FIL *favi, AVI_INFO *aviinfo)
{
    static uint32_t oldsec;     /* 上一次的播放时间 */
    
    uint8_t *buf;
    
    uint32_t totsec = 0;        /* video文件总时间 */
    uint32_t cursec;            /* 当前播放时间 */
    
    totsec = (aviinfo->SecPerFrame / 1000) * aviinfo->TotalFrame;   /* 歌曲总长度(单位:ms) */
    totsec /= 1000;                                                 /* 秒钟数 */
    cursec = ((double)favi->fptr / favi->obj.objsize) * totsec;     /* 获取当前播放到多少秒 */
    
    if (oldsec != cursec)   /* 需要更新显示时间 */
    {
        buf = malloc(100);  /* 申请100字节内存 */
        oldsec = cursec;
        
        sprintf((char *)buf, "Play time:%02ld:%02ld:%02ld/%02ld:%02ld:%02ld", cursec / 3600, (cursec % 3600) / 60, cursec % 60, totsec / 3600, (totsec % 3600) / 60, totsec % 60);
        text_show_string(10, 90, lcddev.width - 10, 16, (char *)buf, 16, 0, RED);   /* 显示歌曲名字 */
        
        free(buf);
    }
}

uint8_t *jpeg_rx_buf = NULL; 
uint32_t jpeg_alloc_size = 0;

/**
 * @brief       播放MJPEG视频
 * @param       pname: 视频文件名
 * @retval      执行结果
 *              KEY0_PRES: 下一个视频
 *              KEY2_PRES: 上一个视频
 *              其他值   : 错误代码
 */
uint8_t video_play_mjpeg(uint8_t *pname)
{
    uint8_t *framebuf;      /* 视频解码buf */
    uint8_t *pbuf;          /* buf指针 */
    uint8_t res = 0;
    uint16_t offset;
    uint32_t nr;
    uint8_t key;
    FIL *favi;

    void *audiobuf[2];              /* 音频解码buf */
    void *audio_buffer = NULL;

    es8388_adda_cfg(1, 0);          /* 打开DAC,关闭ADC */
    es8388_input_cfg(0);            /* 录音关闭 */
    es8388_output_cfg(1, 0);        /* 喇叭通道和耳机通道打开 */
    es8388_hpvol_set(30);           /* 设置喇叭 */
    es8388_spkvol_set(0);          /* 设置耳机 */
    es8388_soft_mute(0);
    vTaskDelay(pdMS_TO_TICKS(20));

    framebuf =  heap_caps_malloc(AVI_VIDEO_BUF_SIZE, MALLOC_CAP_DMA);
    audiobuf[0] = heap_caps_malloc(AVI_AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
    audiobuf[1] = heap_caps_malloc(AVI_AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
    audio_buffer = audiobuf[0];

    favi = (FIL *)malloc(sizeof(FIL));                  /* 申请favi内存 */
    if ((framebuf == NULL) || (favi == NULL) || (audiobuf[0] == NULL) || (audiobuf[1] == NULL))     /* 只要最后这个视频buf申请失败, 前面的申请失不失败都不重要, 总之就是失败了 */
    {
        ESP_LOGI("videoplay", "memory error! framebuf=%p favi=%p abuf0=%p abuf1=%p",
                 framebuf, favi, audiobuf[0], audiobuf[1]);
        /* ★ 安全释放: 只释放非NULL指针 ★ */
        if (framebuf)    free(framebuf);
        if (favi)        free(favi);
        if (audiobuf[0]) free(audiobuf[0]);
        if (audiobuf[1]) free(audiobuf[1]);
        return 0xFF;     /* 直接返回, 不再执行memset */
    }

    memset(framebuf,    0, AVI_VIDEO_BUF_SIZE);
    memset(audiobuf[0], 0, AVI_AUDIO_BUF_SIZE);
    memset(audiobuf[1], 0, AVI_AUDIO_BUF_SIZE);

    while (res == 0)
    {
        res = (uint8_t)f_open(favi, (const TCHAR *)pname, FA_READ);             /* 打开文件 */
        if (res == 0)
        {
            pbuf = framebuf;
            res = (uint8_t)f_read(favi, pbuf, AVI_VIDEO_BUF_SIZE, (UINT *)&nr);  /* 开始读取 */
            if (res != 0)
            {
                ESP_LOGI(videoplay_tag, "fread error:%d", res);
                break;
            }
            
            res = avi_init(pbuf, AVI_VIDEO_BUF_SIZE);   /* AVI解析 */
            if (res != 0)
            {
                ESP_LOGI(videoplay_tag, "avi error:%d", res);
                res = KEY0_PRES;
                break;
            }

            //video_info_show(&g_avix);               /* 显示当前视频文件的相关信息(g_avix结构体在avi.c文件定义) */
            
            if ((g_avix.Width > lcddev.width) || (g_avix.Height > lcddev.height))   /* 视频尺寸大于屏幕尺寸 */
            {
                text_show_string(10, 90, lcddev.width - 10, 16,  "视频尺寸不适合现屏幕", 16, 0, RED);
                text_show_string(10, 110, lcddev.width - 10, 16, "准备切换到下一个", 16, 0, RED);
                vTaskDelay(pdMS_TO_TICKS(1000));     /* 适当延时 */
                res = KEY0_PRES;                    /* 直接下一个视频 */
                break;
            }
            
            frame_timer_init(g_avix.SecPerFrame);   /* 初始化ESP_TIMER,用于等待帧间隔 */

            offset = avi_srarch_id(pbuf, AVI_VIDEO_BUF_SIZE, "movi");   /* 寻找movi ID */
            avi_get_streaminfo(pbuf + offset + 4);                      /* 获取流信息 */
            f_lseek(favi, offset + 12);                                 /* 跳过标志ID，读地址偏移到流数据开始处 */
            
            jpeg_rx_buf = mjpegdec_init(g_avix.Width, g_avix.Height, &jpeg_alloc_size);     /* MJPEG初始化 */

            if (g_avix.SampleRate)              /* 有音频信息,才初始化 */
            {
                myi2s_init(44100, 16);                   /* 初始化i2s */  
                vTaskDelay(pdMS_TO_TICKS(50));  /* 适当延时 */
                es8388_i2s_cfg(0, 3);           /* 飞利浦标准,16位数据长度 */
                i2s_set_samplerate_bits_sample(g_avix.SampleRate, I2S_BITS_PER_SAMPLE_16BIT);   /* 设置采样率和数据位宽 */
                i2s_trx_start();                /* I2S TRX启动 */ 
            }

            while (1)   /* 循环播放文件内容 */
            {
                if (g_avix.StreamID == AVI_VIDS_FLAG)                       /* 视频流 dc */
                {
                    pbuf = framebuf;
                    f_read(favi, pbuf, g_avix.StreamSize + 8, (UINT *)&nr); /* 读取整帧+下一帧数据流ID信息 */
                    
                    res = mjpegdec_decode(pbuf, g_avix.StreamSize, jpeg_rx_buf, jpeg_alloc_size, g_avix.Width, g_avix.Height);  /* 解码一副JPEG图片 */
                    if (res != 0)
                    {
                        ESP_LOGI(videoplay_tag, "illegal frame decode error!");
                    }

                    while (g_frameup == 0);   /* 等待时间到达(在mytimer.c的中断里面设置为1) */
                    g_frameup = 0;            /* 等待播放时间到达 */
                    g_frame++;
                }
                else                                                        /* wb 音频 */
                {
                    audio_buffer = audiobuf[0] == audio_buffer ? audiobuf[1] : audiobuf[0];     /* 使用双缓冲 */

                    if (g_avix.Width < lcddev.width)        /* 满屏不显示 */
                    {
                        //video_time_show(favi, &g_avix);     /* 显示当前播放时间 */
                    }

                    f_read(favi, audio_buffer, g_avix.StreamSize + 8, (UINT *)&nr);     /* 填充framebuf */
                    pbuf = audio_buffer;
                    i2s_tx_write(audio_buffer, g_avix.StreamSize);              /* 数据发送给I2S */
                }

                key = xl9555_key_scan(0);
                
                if (key == KEY0_PRES || key == KEY2_PRES)   /* KEY0/KEY2按下,播放下一个/上一个视频 */
                {
                    res = key;
                    break;
                }
                else if (key == KEY1_PRES || key_scan(0))   /* BOOT/KEY1按下,快进,快退 */
                {
                    i2s_trx_stop();     /* 关闭音频 */
                    video_seek(favi, &g_avix, framebuf);
                    pbuf = framebuf;
                    i2s_trx_start();    /* 开启音频播放 */ 
                }

                if (avi_get_streaminfo(pbuf + g_avix.StreamSize) != 0)      /* 读取下一帧流标志 */
                {
                    ESP_LOGI(videoplay_tag, "g_frame error");
                    res = KEY0_PRES;
                    break;
                }
            }

            f_close(favi);                  /* 关闭文件 */
            i2s_trx_stop();                 /* 关闭音频 */
            i2s_deinit();                   /* I2S恢复到默认 */
            mjpegdec_free(jpeg_rx_buf);     /* 卸载硬件JPEG驱动并释放内存 */
            frame_timer_stop();             /* 停止定时器工作 */  
        }
    }

    free(framebuf);
    free(audiobuf[0]);
    free(audiobuf[1]);
    free(favi);
    
    return res;
}

/**
 * @brief       播放视频
 * @param       无
 * @retval      无
 */
void video_play(void)
{
    uint8_t res;
    FF_DIR vdir;            /* 目录 */
    FILINFO *vfileinfo;     /* 文件信息 */
    uint8_t *pname;         /* 带路径的文件名 */
    uint16_t totavinum;     /* 视频文件总数 */
    uint16_t curindex;      /* 视频文件当前索引 */
    uint32_t *voffsettbl;   /* 视频文件off block索引表 */
    uint8_t key;            /* 键值 */
    uint32_t temp;
    
    while (f_opendir(&vdir, "0:/VIDEO") != FR_OK)   /* 检查VIDEO文件夹是否存在 */
    {
        text_show_string(60, 190, 240, 16, "VIDEO文件夹错误!", 16, 0, RED);
        vTaskDelay(200);
        lcd_fill(60, 190, 300, 206, WHITE);
        vTaskDelay(200);
    }
    
    totavinum = video_get_tnum("0:/VIDEO");         /* 得到总有效文件数 */

    while (totavinum == 0)      /* 视频文件总数为0 */
    {
        text_show_string(60, 190, 240, 16, "没有视频文件!", 16, 0, RED);
        vTaskDelay(200);
        lcd_fill(60, 190, 240, 146, WHITE);
        vTaskDelay(200);
    }
    
    vfileinfo = (FILINFO *)malloc(sizeof(FILINFO));     /* 为长文件缓存区分配内存 */
    pname = (uint8_t *)malloc(2 * 255 + 1);             /* 为带路径的文件名分配内存 */
    voffsettbl = (uint32_t *)malloc(totavinum  * 4);    /* 申请4*totavinum个字节的内存,用于存放视频文件索引 */
    while ((vfileinfo == NULL) || (pname == NULL) || (voffsettbl == NULL))  /* 内存分配出错 */
    {
        text_show_string(60, 190, 240, 16, "内存分配失败!", 16, 0, RED);
        vTaskDelay(200);
        lcd_fill(60, 190, 240, 146, WHITE);
        vTaskDelay(200);
    }

    /* 记录索引 */    
    res = (uint8_t)f_opendir(&vdir, "0:/VIDEO");    /* 打开目录 */
    if (res == 0)
    {
        curindex = 0;   /* 当前索引为0 */
        
        while (1)       /* 全部查询一遍 */
        {
            temp = vdir.dptr;                               /* 记录当前dptr偏移 */
            res = (uint8_t)f_readdir(&vdir, vfileinfo);     /* 读取下一个文件 */
            if ((res != 0) || (vfileinfo->fname[0] == 0))   /* 错误或到末尾，退出 */
            {
                break;
            }
            
            res = exfuns_file_type(vfileinfo->fname);       /* 检测文件类型 */
            if ((res & 0xF0) == 0x60)                       /* 是视频文件 */
            {
                voffsettbl[curindex] = temp;                /* 记录索引 */
                curindex++;
            }
        }
    }

    curindex = 0;                                           /* 从0开始显示 */
    res = (uint8_t)f_opendir(&vdir, "0:/VIDEO");            /* 打开目录 */
    while (res == FR_OK)                                    /* 打开成功 */
    {
        atk_dir_sdi(&vdir, voffsettbl[curindex]);           /* 改变当前目录索引 */

        res = f_readdir(&vdir, vfileinfo);                  /* 读取目录下的一个文件 */
        if ((res != FR_OK) || (vfileinfo->fname[0] == 0))
        {
            break;  /* 错误了/到末尾了,退出 */
        }

        strcpy((char *)pname, "0:/VIDEO/");                         /* 复制路径(目录) */
        strcat((char *)pname, (const char *)vfileinfo->fname);      /* 将文件名接在后面 */

        lcd_clear(WHITE);               /* 清屏 准备视频解码显示 */

        //video_bmsg_show((uint8_t *)vfileinfo->fname, curindex + 1, totavinum);  /* 显示名字,索引等信息 */

        key = video_play_mjpeg(pname);  /* 播放这个视频文件 */

        if (key == KEY0_PRES)           /* 下一个视频 */
        {
            curindex++;
            if (curindex >= totavinum)
            {
                curindex = 0;           /* 到末尾的时候,自动从头开始 */
            }
        }
        else if (key == KEY2_PRES)      /* 上一个视频 */
        {
            if (curindex)
            {
                curindex--;
            }
            else
            {
                curindex = totavinum - 1;
            }
        }
        else
        {
            break;  /* 产生了错误 */
        }
    }
    
    free(vfileinfo);        /* 释放内存 */
    free(pname);            /* 释放内存 */  
    free(voffsettbl);       /* 释放内存 */
}

/**
 * @brief       AVI文件查找
 * @param       favi    : AVI文件
 * @param       aviinfo : AVI信息结构体
 * @param       mbuf    : 数据缓冲区
 * @retval      执行结果
 *   @arg       0    , 成功
 *   @arg       其他 , 错误
 */
uint8_t video_seek(FIL *favi, AVI_INFO *aviinfo, uint8_t *mbuf)
{
    uint32_t fpos = favi->fptr;
    uint8_t *pbuf;
    uint16_t offset;
    uint32_t br;
    uint32_t delta;
    uint32_t totsec;
    uint8_t key;

    totsec = (aviinfo->SecPerFrame / 1000) * aviinfo->TotalFrame;
    totsec /= 1000;                             /* 秒钟数 */
    delta = (favi->obj.objsize / totsec) * 5;   /* 每次前进5秒钟的数据量 */

    while (1)
    {
        key = xl9555_key_scan(1);

        if (key_scan(1))                   /* 快进 */
        {
            if (fpos < favi->obj.objsize)
            {
                fpos += delta;
            }

            if (fpos > (favi->obj.objsize - AVI_VIDEO_BUF_SIZE))
            {
                fpos = favi->obj.objsize - AVI_VIDEO_BUF_SIZE;
            }
        }
        else if (key == KEY1_PRES)              /* 快退 */
        {
            if (fpos > delta)
            {
                fpos -= delta;
            }
            else
            {
                fpos = 0;
            }
        }
        else
        {
            break;
        }

        f_lseek(favi, fpos);
        f_read(favi, mbuf, AVI_VIDEO_BUF_SIZE, (UINT *)&br);       /* 读入整帧+下一数据流ID信息 */
        pbuf = mbuf;
        
        if (fpos == 0)                                     /* 从0开始,得先寻找movi ID */
        {
            offset = avi_srarch_id(pbuf, AVI_VIDEO_BUF_SIZE, "movi");
        }
        else
        {
            offset = 0;
        }

        offset += avi_srarch_id(pbuf + offset, AVI_VIDEO_BUF_SIZE, g_avix.VideoFLAG); /* 寻找视频帧 */
        avi_get_streaminfo(pbuf + offset);                  /* 获取流信息 */
        f_lseek(favi, fpos + offset + 8);                   /* 跳过标志ID,读地址偏移到流数据开始处 */
        
        if (g_avix.StreamID == AVI_VIDS_FLAG)
        {
            f_read(favi, mbuf, g_avix.StreamSize + 8, (UINT *)&br); /* 读入整帧 */
            mjpegdec_decode(mbuf, g_avix.StreamSize, jpeg_rx_buf, jpeg_alloc_size, g_avix.Width, g_avix.Height);       /* 显示视频帧 */
        }
        else
        {
            ESP_LOGI(videoplay_tag, "error flag");
        }

        //video_time_show(favi, &g_avix);
    }

    return 0;
}

/* ===== 开机单次视频播放: 独立任务(避免 main 栈溢出) ===== */
static TaskHandle_t s_video_task_handle = NULL;
static SemaphoreHandle_t s_video_done_sem = NULL;

/**
 * @brief       视频播放独立任务体 (大栈: 16KB)
 *              video_play_mjpeg 调用链深 + 局部变量多, main task 的 4KB 栈装不下
 */
static void video_task_entry(void *pvParameters)
{
    const char *filepath = (const char *)pvParameters;
    uint8_t ret;

    printf("[VIDEO] Task started, file=%s\r\n", filepath);

    /* ① 检查文件是否存在 */
    FIL ftmp;
    ret = (uint8_t)f_open(&ftmp, (const TCHAR *)filepath, FA_READ);
    if (ret != 0) {
        printf("[VIDEO] ERROR: File not found! f_open=%d\r\n", ret);
        text_show_string(10, 50, lcddev.width - 10, 16, "Boot Video Not Found!", 16, 0, RED);
        vTaskDelay(pdMS_TO_TICKS(2000));
        goto done;
    }
    printf("[VIDEO] File found, size=%ld bytes\r\n", ftmp.obj.objsize);
    f_close(&ftmp);

    /* ② 清屏 + 播放 */
    lcd_clear(WHITE);
    ret = video_play_mjpeg((uint8_t *)filepath);
    printf("[VIDEO] video_play_mjpeg returned: 0x%02X\r\n", ret);

done:
    printf("[VIDEO] Task finished\r\n");
    xSemaphoreGive(s_video_done_sem);   /* 通知调用方: 播放完毕 */
    s_video_task_handle = NULL;
    vTaskDelete(NULL);                  /* 自删除 */
}

/**
 * @brief       播放指定路径的AVI视频(单次播放, 播完自动返回)
 * @param       filepath: 完整文件路径, 如 "0:/VIDEO/boot.avi"
 * @note        ★ 内部创建独立FreeRTOS任务(16KB栈)来执行播放 ★
 *              此函数会阻塞等待播放完成(通过信号量同步), 然后返回
 */
void video_play_single(const char *filepath)
{
    printf("[VIDEO] ===== video_play_single START =====\r\n");

    /* 创建完成信号量 (每次调用都新建, 防止残留) */
    if (s_video_done_sem) {
        vSemaphoreDelete(s_video_done_sem);
    }
    s_video_done_sem = xSemaphoreCreateBinary();

    /* 创建视频播放任务: 栈16KB, 优先级5(高于main的1) */
    BaseType_t ret = xTaskCreate(
        video_task_entry,
        "boot_video",
        16 * 1024,            /* ★ 16KB栈 ★ */
        (void *)filepath,
        5,                    /* 高优先级, 保证帧率 */
        &s_video_task_handle
    );

    if (ret != pdPASS) {
        printf("[VIDEO] ERROR: Failed to create task!\r\n");
        return;
    }

    /* ★ 阻塞等待视频播完 (信号量通知) ★ */
    xSemaphoreTake(s_video_done_sem, portMAX_DELAY);

    printf("[VIDEO] ===== video_play_single END =====\r\n");
}