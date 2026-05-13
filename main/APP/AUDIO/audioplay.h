/**
 ****************************************************************************************************
 * @file        audioplay.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V2.0
 * @date        2025-01-01
 * @brief       音乐播放器应用层 - 接口定义与数据结构
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 *
 * @note        V2.0优化内容:
 *              - 新增音频索引缓存机制,消除RS485切歌时的重复目录扫描延迟
 *              - 新增 audio_build_index() / audio_free_index() 接口供外部管理
 *              - 优化内存布局与命名规范
 ****************************************************************************************************
 */

#ifndef __AUDIOPLAY_H
#define __AUDIOPLAY_H

#include "ff.h"
#include "wavplay.h"
#include "exfuns.h"
#include "myi2s.h"
#include "mipi_lcd.h"
#include "text.h"

/* ================================================================== */
/*                         数据结构定义                                */
/* ================================================================== */

/**
 * @brief 音频设备控制结构体
 * @note  管理音频播放状态、文件指针和传输缓冲区
 */
typedef struct {
    uint8_t *tbuf;          /* DMA传输缓冲区(由heap_caps_malloc分配) */
    FIL    *file;           /* 当前打开的音频文件句柄(DMA对齐) */
    uint8_t status;         /* 播放状态位域:
                             *   bit0: 0=暂停, 1=继续/播放中
                             *   bit1: 0=停止, 1=开启
                             *   组合值: 0x00=停止, 0x01=暂停, 0x03=播放中 */
} __audiodev;

/* 全局音频设备实例 */
extern __audiodev g_audiodev;

/* ================================================================== */
/*                      RS485远程控制变量                              */
/* ================================================================== */
/** @note volatile修饰: 被RS485任务和主任务跨上下文访问,防止编译器优化导致读写顺序异常 */

extern volatile uint8_t rs485_target_index;    /* 目标曲目编号(1-based), 0表示无效/无命令 */
extern volatile uint8_t rs485_cmd_flag;         /* 新命令待处理标志: 1=有待处理的切歌命令 */

extern volatile uint8_t rs485_volume_val;       /* 音量设定值(0~33), 0xFF=无效/无音量命令 */
extern volatile uint8_t rs485_volume_flag;      /* 音量命令标志: 1=有待处理的音量指令 */

/* ================================================================== */
/*                          函数声明                                  */
/* ================================================================== */

/* 基础播放控制 */
void audio_start(void);                                                 /* 启动I2S DMA传输 */
void audio_stop(void);                                                  /* 停止I2S DMA传输 */

/* 目录扫描与索引(内部使用,由audio_play自动管理) */
uint16_t audio_get_tnum(const uint8_t *path);                           /* 统计指定路径下有效音频文件数 */

/* UI显示 */
void audio_index_show(uint16_t index, uint16_t total);                  /* LCD显示: 曲目 N / 总数 */
void audio_msg_show(uint32_t totsec, uint32_t cursec, uint32_t bitrate);/* LCD显示: 当前时间/总时长/比特率 */

/* 核心播放接口 */
void audio_play(void);                                                  /* 音乐播放主入口(含索引缓存快速路径) */
uint8_t audio_play_song(uint8_t *fname);                                /* 根据扩展名分派到具体解码器 */

/* 封面图加载 */
void audio_load_cover(uint16_t song_index);                             /* 同步加载指定曲目封面到LCD(阻塞SPI) */
void audio_load_cover_async(uint16_t song_index);                       /* 异步触发封面加载(非阻塞, 由独立低优先级任务执行) */
void audio_init_cover_task(void);                                       /* 初始化封面加载任务(需在piclib_init后调用一次) */

/* 当前播放曲目索引(0-based), 供封面任务读取 */
extern volatile uint16_t g_current_song_index;

#endif /* __AUDIOPLAY_H */
