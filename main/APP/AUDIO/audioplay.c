/**
 ****************************************************************************************************
 * @file        audioplay.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       音乐播放器 应用代码
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

#include "audioplay.h"
#include "rs485.h"
#include "piclib.h"


__audiodev g_audiodev;          /* 音乐播放控制器 */

/* RS485远程控制变量 */
volatile uint8_t rs485_target_index = 0;    /* 目标曲目编号(1-based), 0=无效 */
volatile uint8_t rs485_cmd_flag = 0;         /* 新命令标志 */

/**
 * @brief       开始音频播放
 * @param       无
 * @retval      无
 */
void audio_start(void)
{
    g_audiodev.status = 3 << 0;
    i2s_trx_start();
}

/**
 * @brief       停止音频播放
 * @param       无
 * @retval      无
 */
void audio_stop(void)
{
    g_audiodev.status = 0;
    i2s_trx_stop();
}

/**
 * @brief       得到path路径下，目标文件的总数
 * @param       path : 文件路径
 * @retval      有效文件总数
 */
uint16_t audio_get_tnum(uint8_t *path)
{
    uint8_t res;
    uint16_t rval = 0;
    FF_DIR tdir;
    FILINFO *tfileinfo;
    
    tfileinfo = (FILINFO*)malloc(sizeof(FILINFO));
    
    res = f_opendir(&tdir, (const TCHAR*)path);
    
    if ((res == FR_OK) && tfileinfo)
    {
        while (1)
        {
            res = f_readdir(&tdir, tfileinfo);
            
            if ((res != FR_OK) || (tfileinfo->fname[0] == 0))
            {
                break;
            }

            res = exfuns_file_type(tfileinfo->fname);
            
            if ((res & 0xF0) == 0x40)
            {
                rval++; /* 有效文件数增加1 */
            }
        }
    }
    
    free(tfileinfo);
    
    return rval;
}

/**
 * @brief       显示曲目索引
 * @param       index : 当前索引
 * @param       total : 总文件数
 * @retval      无
 */
void audio_index_show(uint16_t index, uint16_t total)
{
    lcd_show_num(30 + 0, 230, index, 3, 16, RED);
    lcd_show_char(30 + 24, 230, '/', 16, 0, RED);
    lcd_show_num(30 + 32, 230, total, 3, 16, RED);
}

/**
 * @brief       显示播放时间,比特率 信息
 * @param       totsec : 音频文件总时间长度
 * @param       cursec : 当前播放时间
 * @param       bitrate: 比特率(位速)
 * @retval      无
 */
void audio_msg_show(uint32_t totsec, uint32_t cursec, uint32_t bitrate)
{
    static uint16_t playtime = 0xFFFF;
    
    if (playtime != cursec)
    {
        playtime = cursec;
        
        lcd_show_xnum(30, 210, playtime / 60, 2, 16, 0X80, RED);
        lcd_show_char(30 + 16, 210, ':', 16, 0, RED);
        lcd_show_xnum(30 + 24, 210, playtime % 60, 2, 16, 0X80, RED);
        lcd_show_char(30 + 40, 210, '/', 16, 0, RED);
        
        lcd_show_xnum(30 + 48, 210, totsec / 60, 2, 16, 0X80, RED);
        lcd_show_char(30 + 64, 210, ':', 16, 0, RED);
        lcd_show_xnum(30 + 72, 210, totsec % 60, 2, 16, 0X80, RED);
        
        lcd_show_num(30 + 110, 210, bitrate / 1000, 4, 16, RED);
        lcd_show_string(30 + 110 + 32 , 210, 200, 16, 16, "Kbps", RED);
    }
}

/**
 * @brief       转换
 * @param       fs:文件系统对象
 * @param       clst:转换
 * @retval      =0:扇区号，0:失败
 */
static LBA_t atk_clst2sect(FATFS* fs,DWORD clst)
{
    clst -= 2;          /* Cluster number is origin from 2 */

    if (clst >= fs->n_fatent - 2)
    {
        return 0;       /* Is it invalid cluster number? */
    }

    return fs->database + (LBA_t)fs->csize * clst;  /* Start sector number of the cluster */
}

/**
 * @brief       偏移
 * @param       dp:指向目录对象
 * @param       Offset:目录表的偏移量
 * @retval      FR_OK(0):成功，!=0:错误
 */
FRESULT atk_dir_sdi(FF_DIR* dp,DWORD ofs)
{
    DWORD clst;
    FATFS *fs = dp->obj.fs;

    if (ofs >= (DWORD)((FF_FS_EXFAT && fs->fs_type == FS_EXFAT) ? 0x10000000 : 0x200000) || ofs % 32)
    {
        /* Check range of offset and alignment */
        return FR_INT_ERR;
    }

    dp->dptr = ofs;         /* Set current offset */
    clst = dp->obj.sclust;  /* Table start cluster (0:root) */

    if (clst == 0 && fs->fs_type >= FS_FAT32)
    {	/* Replace cluster# 0 with root cluster# */
        clst = (DWORD)fs->dirbase;

        if (FF_FS_EXFAT)
        {
            dp->obj.stat = 0;
        }
        /* exFAT: Root dir has an FAT chain */
    }

    if (clst == 0)
    {	/* Static table (root-directory on the FAT volume) */
        if (ofs / 32 >= fs->n_rootdir)
        {
            return FR_INT_ERR;  /* Is index out of range? */
        }

        dp->sect = fs->dirbase;

    }
    else
    {   /* Dynamic table (sub-directory or root-directory on the FAT32/exFAT volume) */
        dp->sect = atk_clst2sect(fs, clst);
    }

    dp->clust = clst;   /* Current cluster# */

    if (dp->sect == 0)
    {
        return FR_INT_ERR;
    }

    dp->sect += ofs / fs->ssize;             /* Sector# of the directory entry */
    dp->dir = fs->win + (ofs % fs->ssize);   /* Pointer to the entry in the win[] */

    return FR_OK;
}

/**
 * @brief       播放音乐
 * @param       无
 * @retval      无
 */
void audio_play(void)
{
    uint8_t res;
    FF_DIR wavdir;
    FILINFO *wavfileinfo;
    uint8_t *pname;
    uint16_t totwavnum;
    uint16_t curindex;
    uint8_t key;
    uint32_t temp;
    uint32_t *wavoffsettbl;

    while (f_opendir(&wavdir, "0:/MUSIC"))
    {
        text_show_string(30, 190, 240, 16, "MUSIC文件夹错误!", 16, 0, BLUE);
        vTaskDelay(200);
        lcd_fill(30, 190, 240, 206, WHITE);
        vTaskDelay(200);
    }

    totwavnum = audio_get_tnum((uint8_t *)"0:/MUSIC");          /* 得到总有效文件数 */
    
    while (totwavnum == 0)
    {
        text_show_string(30, 190, 240, 16, "没有音乐文件!", 16, 0, BLUE);
        vTaskDelay(200);
        lcd_fill(30, 190, 240, 146, WHITE);
        vTaskDelay(200);
    }
    
    wavfileinfo = (FILINFO*)malloc(sizeof(FILINFO));
    pname = malloc(255 * 2 + 1);
    wavoffsettbl = malloc(4 * totwavnum);
    
    while (!wavfileinfo || !pname || !wavoffsettbl)
    {
        text_show_string(30, 190, 240, 16, "内存分配失败!", 16, 0, BLUE);
        vTaskDelay(200);
        lcd_fill(30, 190, 240, 146, WHITE);
        vTaskDelay(200);
    }
    
    res = f_opendir(&wavdir, "0:/MUSIC");
    
    if (res == FR_OK)
    {
        curindex = 0;                                           /* 当前索引为0 */
        
        while (1)
        {
            temp = wavdir.dptr;                                 /* 记录当前index */

            res = f_readdir(&wavdir, wavfileinfo);              /* 读取目录下的一个文件 */
            
            if ((res != FR_OK) || (wavfileinfo->fname[0] == 0))
            {
                break;                                          /* 错误了/到末尾了,退出 */
            }

            res = exfuns_file_type(wavfileinfo->fname);
            
            if ((res & 0xF0) == 0x40)
            {
                wavoffsettbl[curindex] = temp;                   /* 记录索引 */
                curindex++;
            }
        }
    }
    
    curindex = 0;                                               /* 从0开始显示 */
    res = f_opendir(&wavdir, (const TCHAR*)"0:/MUSIC");

    while (res == FR_OK)                                        /* 打开目录 */
    {
        /* ★ RS485远程控制: 检查是否有来自PC的切歌命令 */
        if (rs485_cmd_flag && rs485_target_index > 0 && rs485_target_index <= totwavnum)
        {
            curindex = rs485_target_index - 1;                 /* 转为0-based索引 */
            rs485_cmd_flag = 0;                                 /* 清除标志 */
            printf("[RS485] Switch to song #%d (index=%d)\r\n", rs485_target_index, curindex);
        }

        atk_dir_sdi(&wavdir, wavoffsettbl[curindex]);           /* 改变当前目录索引 */

        res = f_readdir(&wavdir, wavfileinfo);                  /* 读取文件 */
        
        if ((res != FR_OK) || (wavfileinfo->fname[0] == 0))
        {
            break;
        }
        
        strcpy((char *)pname, "0:/MUSIC/");
        strcat((char *)pname, (const char *)wavfileinfo->fname);

        /* ★ 显示对应封面图片 (0:/PIC/01.jpg 等) */
        {
            char pic_path[64];
            snprintf(pic_path, sizeof(pic_path), "0:/PICTURE/%02d", curindex + 1);
            printf("[PIC] Trying image: %s\r\n", pic_path);
            const char *exts[] = {".jpg", ".jpeg", ".bmp", ".png"};
            uint8_t loaded = 0;
            for (int e = 0; e < 4; e++)
            {
                char full_path[72];
                snprintf(full_path, sizeof(full_path), "%s%s", pic_path, exts[e]);
                uint8_t ret = piclib_ai_load_picfile(full_path, 0, 0, lcddev.width, lcddev.height);
                printf("[PIC] load %s -> %d\r\n", full_path, ret);
                if (ret == 0)
                {
                    printf("[PIC] Loaded OK: %s\r\n", full_path);
                    loaded = 1;
                    break;
                }
            }
            if (!loaded) {
                printf("[PIC] WARN: No image found for song #%d!\r\n", curindex + 1);
            }
        }

        key = audio_play_song(pname);

        if (rs485_cmd_flag && rs485_target_index > 0 && rs485_target_index <= totwavnum)
        {
            /* ★ RS485远程命令: 播放期间收到指令, 跳转到指定曲目 */
            curindex = rs485_target_index - 1;
            rs485_cmd_flag = 0;
            printf("[RS485] Interrupt jump to song #%d\r\n", rs485_target_index);
        }
        else
        {
            break;   /* ★ 播放结束(无新RS485指令) → 退出循环,回到等待状态 */
        }
    }

    free(wavfileinfo);
    free(pname);
    free(wavoffsettbl);
}

/**
 * @brief       播放某个音频文件
 * @param       fname : 文件名
 * @retval      按键值
 *   @arg       KEY0_PRES , 下一曲.
 *   @arg       KEY1_PRES , 上一曲.
 *   @arg       其他 , 错误
 */
uint8_t audio_play_song(uint8_t *fname)
{
    uint8_t res;  
    
    res = exfuns_file_type((char *)fname); 

    switch (res)
    {
        case T_WAV:
            res = wav_play_song(fname);
            break;
        case T_MP3:
            /* 自行实现 */
            break;

        default:            /* 其他文件,自动跳转到下一曲 */
            printf("can't play:%s\r\n", fname);
            res = KEY0_PRES;
            break;
    }
    return res;
}
