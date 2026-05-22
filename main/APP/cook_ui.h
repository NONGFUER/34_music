/**
 ****************************************************************************************************
 * @file        cook_ui.h
 * @author      sjwu
 * @version     V1.0
 * @date        2025-01-01
 * @brief       炒菜机副屏UI - 状态管理与渲染引擎 (动态布局)
 ****************************************************************************************************
 */

#ifndef __COOK_UI_H
#define __COOK_UI_H

#include "lcd.h"
#include <stdint.h>

/* ================================================================== */
/*                          常量定义                                   */
/* ================================================================== */

#define BOX_COUNT           3       /* 菜盒数量 */
#define VOICE_TOTAL         9       /* 语音总数(缺002.wav: 1+8=9) */
#define ICON_SIZE           40      /* 状态图标尺寸 40×40 */

/* ---- 原水平布局(待机模式保留) ---- */
#define UI_BOX_W            180     /* 菜盒宽度 */
#define UI_BOX_H            70      /* 菜盒高度 */

/* ---- 炒菜模式垂直卡片布局 ---- */
#define COOK_BOX_W          300     /* 炒菜模式: 卡片宽度 */
#define COOK_BOX_H          66      /* 炒菜模式: 卡片高度 */
#define COOK_BOX_GAP        14      /* 炒菜模式: 卡片间距 */

/* ================================================================== */
/*                           枚举                                      */
/* ================================================================== */

typedef enum {
    BOX_READY   = 0,
    BOX_DONE    = 1,
    BOX_POURING = 2,
} cook_box_status_e;

typedef enum {
    SYS_IDLE        = 0,
    SYS_COOKING     = 1,
    SYS_DONE        = 2,
    SYS_ALARM_TEMP  = 3,
    SYS_ALARM_FIRE  = 4,
} cook_sys_state_e;

typedef enum {
    VOICE_BOOT           = 1,   /* 001.wav - 目录第1位 */
    VOICE_POUR1          = 2,   /* 003.wav - 目录第2位(缺002) */
    VOICE_POUR2          = 3,   /* 004.wav - 目录第3位 */
    VOICE_POUR3          = 4,   /* 005.wav - 目录第4位 */
    VOICE_BOX_RETURN     = 5,   /* 006.wav - 目录第5位 */
    VOICE_START_COOK     = 6,   /* 007.wav - 目录第6位 */
    VOICE_FINISH         = 7,   /* 008.wav - 目录第7位 */
    VOICE_ALARM_TEMP     = 9,   /* 009.wav */
    VOICE_ALARM_FIRE     = 10,  /* 010.wav */
} voice_id_e;

/* ================================================================== */
/*                          数据结构                                   */
/* ================================================================== */

typedef struct {
    cook_box_status_e status;
    volatile uint8_t  changed;
    volatile uint16_t x;
    volatile uint16_t y;             /* 炒菜模式垂直布局Y坐标 */
} cook_box_t;

/**
 * @brief 音频播放信息(由 audioplay 模块填充, cook_ui 统一渲染)
 * @note  cook_ui 完全接管LCD后, audioplay 不再直接调用 LCD API,
 *        所有音频相关信息通过此结构体传递给渲染引擎
 */
typedef struct {
    volatile uint16_t song_index;     /* 当前曲目序号(1-based), 0=无播放 */
    volatile uint16_t total_songs;    /* 曲目总数 */
    volatile uint32_t cur_sec;        /* 当前播放位置(秒) */
    volatile uint32_t tot_sec;        /* 总时长(秒) */
    volatile uint32_t bitrate;        /* 比特率(bps) */
    volatile uint8_t  playing;        /* 是否正在播放: 0=停止, 1=播放中 */
    volatile uint8_t  changed;        /* 音频信息脏区标志(变化时置1) */
    volatile uint8_t  cover_changed;  /* 封面图脏区标志(切歌时置1) */
} cook_audio_info_t;

typedef struct {
    cook_box_t               box[BOX_COUNT];
    cook_audio_info_t        audio;              /* 音频播放信息 */
    volatile cook_sys_state_e sys_state;
    volatile uint8_t          bg_scene;
    volatile uint8_t          sys_changed;
    volatile uint8_t          bg_changed;
    volatile uint8_t          alarm_flash_on;
    volatile uint8_t          rendering;
} cook_status_t;

extern cook_status_t *g_cook_status;

/* ================================================================== */
/*                       SD卡资源文件名                                */
/* ================================================================== */

#define BG_FILE_STANDBY     "0:/PICTURE/bg_standby.jpg"
#define BG_FILE_POUR1       "0:/PICTURE/bg_pour1.jpg"
#define BG_FILE_POUR2       "0:/PICTURE/bg_pour2.jpg"
#define BG_FILE_POUR3       "0:/PICTURE/bg_pour3.jpg"
#define BG_FILE_RESET       "0:/PICTURE/bg_reset.jpg"
#define BG_FILE_COOKING     "0:/PICTURE/bg_cooking.jpg"
#define BG_FILE_BG          "0:/PICTURE/bg.jpg"
#define BG_FILE_DONE        "0:/PICTURE/bg_done.jpg"
#define BG_FILE_ALARM_TEMP  "0:/PICTURE/bg_alarm_temp.jpg"
#define BG_FILE_ALARM_FIRE  "0:/PICTURE/bg_alarm_fire.jpg"
#define BG_FILE_ALARM_TEMP_NULL "0:/PICTURE/bg_alarm_temp_null.jpg"   /* 温度异常闪烁交替图 */
#define BG_FILE_ALARM_FIRE_NULL "0:/PICTURE/bg_alarm_fire_null.jpg"   /* 火警闪烁交替图 */
#define BG_FILE_POUR1_DONE "0:/PICTURE/bg_pour1_done.jpg"   /* 一号菜归位完成 */
#define BG_FILE_POUR2_DONE "0:/PICTURE/bg_pour2_done.jpg"   /* 二号菜归位完成 */
#define BG_FILE_POUR3_DONE "0:/PICTURE/bg_pour3_done.jpg"   /* 三号菜归位完成 */
#define BG_FILE_POUR1_NULL "0:/PICTURE/bg_pour1_null.jpg"     /* 一号菜倒菜动画-空盘(闪烁用) */
#define BG_FILE_POUR2_NULL "0:/PICTURE/bg_pour2_null.jpg"     /* 二号菜倒菜动画-空盘(闪烁用) */
#define BG_FILE_POUR3_NULL "0:/PICTURE/bg_pour3_null.jpg"     /* 三号菜倒菜动画-空盘(闪烁用) */

/* ================================================================== */
/*                          函数声明                                   */
/* ================================================================== */

void cook_ui_init(void);
void cook_render(void);
void cook_draw_background(uint8_t scene);
void cook_draw_statusbar(void);
void cook_draw_box(uint8_t box_id);
void cook_draw_alarm_flash(void);

/* ---- 音频信息渲染 (cook_ui 接管) ---- */
void cook_draw_audio_info(void);                    /* 渲染音乐信息区(曲目/进度/比特率) */
void cook_draw_audio_cover(void);                   /* 渲染封面图 */
void cook_audio_update(uint16_t index, uint16_t total, /* 更新音频数据(供audioplay调用) */
                       uint32_t cur_sec, uint32_t tot_sec, uint32_t bitrate);
void cook_audio_set_playing(uint8_t playing);       /* 设置播放状态 */
void cook_audio_set_cover_dirty(void);              /* 标记封面需刷新 */

void cook_set_box(uint8_t box_id, cook_box_status_e status);
cook_box_status_e cook_get_box(uint8_t box_id);
void cook_set_bg_scene(uint8_t scene);

void cook_cmd_pour_box(uint8_t box_id);
void cook_cmd_reset(void);
void cook_cmd_start(void);
void cook_cmd_finish(void);
void cook_cmd_c1(void);                                /* C1: 显示归位界面(bg_reset) */
void cook_cmd_alarm_temp(void);
void cook_cmd_alarm_fire(void);
void cook_cmd_idle(void);
void cook_cmd_boot(void);                              /* 开机(自动播放+待机) */
void cook_cmd_box_return(uint8_t box_id);              /* 菜盒归位(box_id:1~3) */
void cook_cmd_stop_voice(void);                        /* 停止播报(停语音+停报警,界面不变) */
void cook_alarm_stop(void);                            /* 停止报警循环 */
uint8_t  cook_is_alarm_loop(void);                     /* 查询报警循环是否激活 */
uint8_t  cook_alarm_voice_index(void);                 /* 获取当前报警语音编号 */
uint8_t  cook_is_alarm_force_vol(void);                /* 查询报警是否强制最大音量(忽略静音) */

void cook_alarm_flash_task(void *arg);

#endif /* __COOK_UI_H */
