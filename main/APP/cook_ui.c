/**
 ****************************************************************************************************
 * @file        cook_ui.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       炒菜机副屏UI - 状态管理与渲染引擎
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 *
 * @note        方案B实现:
 *              - 背景图(场景大图) + 代码绘制的菜盒状态图标叠加
 *              - 脏区检测优化: 仅重绘变化区域
 *              - ASCII 指令协议支持 (A1~AC/B1~B2/C1~C3)
 *              - 动态布局: 坐标基于 lcddev.width/height 运行时计算
 ****************************************************************************************************
 */

#include "cook_ui.h"
#include "piclib.h"
#include "audioplay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include <stdio.h>
#include <inttypes.h>   /* PRId32 for printf format */

/* ================================================================== */
/*                    动态布局参数 (运行时计算)                          */
/* ================================================================== */

/** 屏幕实际尺寸 (从 lcddev 读取) */
static uint16_t s_scr_w = 0;
static uint16_t s_scr_h = 0;

/** 报警闪烁任务句柄 */
static TaskHandle_t s_alarm_task_handle = NULL;

/** 报警循环播报标志 (温度异常/火警激活后置1, cook_cmd_idle清0) */
static volatile uint8_t  s_alarm_loop_active  = 0;

/** 炒菜完成3秒定时器句柄 (到期自动跳转待机) */
static TimerHandle_t     s_finish_timer       = NULL;

/* ===== 倒菜闪烁动画 (3个菜盒共用框架) ===== */
#define POUR_BLINK_INTERVAL_MS  500   /* 闪烁间隔(毫秒) */

/**
 * @brief 单个菜盒的倒菜闪烁状态 (预加载帧缓冲 + 定时器)
 *        用结构体数组管理3个菜盒, 避免重复代码
 */
typedef struct {
    TimerHandle_t     timer;          /* 周期定时器句柄 */
    volatile uint8_t  fire;           /* 回调置1, 主循环消费后清0 */
    uint8_t           show_null;      /* 当前显示哪张(0=有菜图, 1=空盘图) */
    volatile uint8_t  pending;        /* 等待音频播完才开闪 */
    uint8_t          *frame_buf;      /* 有菜图解码后的RGB565数据 */
    uint8_t          *null_buf;       /* 空盘图解码后的RGB565数据 */
    int32_t           frame_w;        /* 解码宽度 */
    int32_t           frame_h;        /* 解码高度 */
} pour_blink_state_t;

/** 3个菜盒各自的闪烁状态 [0]=pour1, [1]=pour2, [2]=pour3 */
static pour_blink_state_t s_pour_blink[BOX_COUNT];

/** 各菜盒对应的背景图文件名表 (有菜图 / 空盘图) */
static const char * const s_pour_bg_files[BOX_COUNT][2] = {
    { BG_FILE_POUR1,       BG_FILE_POUR1_NULL },
    { BG_FILE_POUR2,       BG_FILE_POUR2_NULL },
    { BG_FILE_POUR3,       BG_FILE_POUR3_NULL },
};

/* ================================================================== */
/*                    全局状态实例 (唯一)                               */
/* ================================================================== */

/** cook_status 全局实例(由 cook_ui.h 声明 extern, 此处定义) */
cook_status_t s_cook_status_obj;
cook_status_t *g_cook_status = &s_cook_status_obj;

/** 状态栏高度 */
#define STATUS_BAR_H     36

/** 菜盒区域高度 */
#define BOX_AREA_H       80

/** 菜盒间距 */
#define BOX_GAP          15

/**
 * @brief 初始化动态布局参数 (在cook_ui_init中调用)
 * @note  炒菜模式: 3个菜盒垂直居中排列(大卡片)
 */
static void cook_init_layout(void)
{
    s_scr_w = lcddev.width;
    s_scr_h = lcddev.height;
    printf("[COOK_UI] Layout: %dx%d\r\n", s_scr_w, s_scr_h);

    /* ---- 炒菜模式垂直卡片布局 ---- */
    uint16_t box_x = (s_scr_w > COOK_BOX_W) ? (s_scr_w - COOK_BOX_W) / 2 : 0;
    uint16_t total_h = BOX_COUNT * COOK_BOX_H + (BOX_COUNT - 1) * COOK_BOX_GAP;
    uint16_t start_y = (s_scr_h > total_h) ? (s_scr_h - total_h) / 2 : 0;

    for (int i = 0; i < BOX_COUNT; i++) {
        g_cook_status->box[i].x = box_x;
        g_cook_status->box[i].y = start_y + i * (COOK_BOX_H + COOK_BOX_GAP);
    }
}

/* ================================================================== */
/*                       私有辅助函数                                   */
/* ================================================================== */

/** 音乐信息区布局常量 */
#define AUDIO_INFO_Y       190     /* 音频信息区起始Y坐标(状态栏上方) */
#define AUDIO_INFO_H       50      /* 音频信息区高度 */

/**
 * @brief 坐标钳位到屏幕范围
 */
static void clamp_rect(uint16_t *sx, uint16_t *sy, uint16_t *ex, uint16_t *ey)
{
    if (*sx >= s_scr_w || *sy >= s_scr_h) {
        *sx = 0; *sy = 0; *ex = 0; *ey = 0; return;
    }
    if (*ex >= s_scr_w) *ex = s_scr_w - 1;
    if (*ey >= s_scr_h) *ey = s_scr_h - 1;
}

/**
 * @brief 安全填充矩形(带边界检查)
 */
static void safe_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    clamp_rect(&sx, &sy, &ex, &ey);
    lcd_fill(sx, sy, ex, ey, color);
}

/**
 * @brief 根据场景编号获取背景图文件名
 */
static const char *cook_get_bg_filename(uint8_t scene)
{
    switch (scene) {
        case 1:  return BG_FILE_STANDBY;
        case 2:  return BG_FILE_POUR1;
        case 3:  return BG_FILE_POUR2;
        case 4:  return BG_FILE_POUR3;
        case 5:  return BG_FILE_RESET;
        case 6:  return BG_FILE_COOKING;
        case 7:  return BG_FILE_DONE;
        case 8:  return BG_FILE_ALARM_TEMP;
        case 9:  return BG_FILE_ALARM_FIRE;
        case 10: return BG_FILE_POUR1_DONE;   /* 一号菜归位完成 */
        case 11: return BG_FILE_POUR2_DONE;   /* 二号菜归位完成 */
        case 12: return BG_FILE_POUR3_DONE;   /* 三号菜归位完成 */
        default: return BG_FILE_STANDBY;
    }
}

/* ================================================================== */
/*                     报警闪烁 FreeRTOS 任务                          */
/* ================================================================== */

void cook_alarm_flash_task(void *arg)
{
    (void)arg;
    while (1) {
        g_cook_status->alarm_flash_on = 1;
        vTaskDelay(pdMS_TO_TICKS(500));
        g_cook_status->alarm_flash_on = 0;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ================================================================== */
/*           倒菜闪烁动画 (定时器 + 主循环刷图) — 3菜盒通用               */
/*                                                                      */
/*  架构: 定时器回调只翻标志位(Tmr Svc轻量安全)                        */
/*        cook_render主循环检测标志位后从PSRAM帧缓冲区刷屏              */
/*        A3/A4/A5设pending → 等音频播完 → 自动启动闪烁                */
/* ================================================================== */

/**
 * @brief 闪烁定时器回调 — 仅翻标志位(轻量), 通过 pvTimerID 获取 box_idx
 * @note  所有3个菜盒共用此回调, pvTimerID 存储的是 box_id(0-based)
 */
static void pour_blink_callback(TimerHandle_t xTimer)
{
    uint8_t idx = (uint8_t)(size_t)pvTimerGetTimerID(xTimer);
    if (idx < BOX_COUNT) {
        s_pour_blink[idx].show_null = !s_pour_blink[idx].show_null;
        s_pour_blink[idx].fire = 1;      /* 通知主循环刷图 */
    }
}

/* ================================================================== */
/*           倒菜闪烁帧预加载 (SD卡 → PSRAM, 仅一次)                    */
/*   ★ 必须在 cook_start/stop 之前定义 (static函数)                     */
/* ================================================================== */

/**
 * @brief 预加载指定菜盒的闪烁两帧JPG到PSRAM缓冲区
 * @param idx  菜盒索引(0=pour1, 1=pour2, 2=pour3)
 */
static void cook_preload_blink_frames(uint8_t idx)
{
    if (idx >= BOX_COUNT) return;

    pour_blink_state_t *s = &s_pour_blink[idx];
    if (s->frame_buf != NULL && s->null_buf != NULL) {
        return;  /* 已有有效缓冲区, 跳过 */
    }

    int8_t ret;
    uint8_t *buf = NULL;
    size_t  buf_size = 0;
    int32_t w = 0, h = 0;

    /* ---- 预加载有菜帧 ---- */
    if (s->frame_buf == NULL) {
        ret = jpeg_decode_to_buffer(s_pour_bg_files[idx][0], s_scr_w, s_scr_h,
                                    &buf, &buf_size, &w, &h);
        if (ret == 0 && buf != NULL) {
            s->frame_buf = buf;
            s->frame_w   = w > 0 ? w : s_scr_w;
            s->frame_h   = h > 0 ? h : s_scr_h;
            printf("[BLINK-PRELOAD] pour%d frame OK: %" PRId32 "x%" PRId32 ", %zu bytes\r\n",
                   idx + 1, w, h, buf_size);
        } else {
            printf("[BLINK-PRELOAD] pour%d frame FAILED (ret=%d)\r\n", idx + 1, ret);
        }
    }

    /* ---- 预加载空盘帧 ---- */
    if (s->null_buf == NULL) {
        buf = NULL; buf_size = 0; w = 0; h = 0;
        ret = jpeg_decode_to_buffer(s_pour_bg_files[idx][1], s_scr_w, s_scr_h,
                                    &buf, &buf_size, &w, &h);
        if (ret == 0 && buf != NULL) {
            s->null_buf = buf;
            printf("[BLINK-PRELOAD] pour%d null OK: %" PRId32 "x%" PRId32 ", %zu bytes\r\n",
                   idx + 1, w, h, buf_size);
        } else {
            printf("[BLINK-PRELOAD] pour%d null FAILED (ret=%d)\r\n", idx + 1, ret);
        }
    }

    if (s->frame_buf && s->null_buf) {
        printf("[BLINK-PRELOAD] Pour%d both frames ready.\r\n", idx + 1);
    }
}

/**
 * @brief 启动指定菜盒的倒菜闪烁动画
 * @param idx  菜盒索引(0=pour1/A3, 1=pour2/A4, 2=pour3/A5)
 */
static void cook_start_pour_loop(uint8_t idx)
{
    if (idx >= BOX_COUNT) return;

    pour_blink_state_t *s = &s_pour_blink[idx];

    if (s->timer != NULL) {
        xTimerStop(s->timer, 0);
    } else {
        char name[16];
        snprintf(name, sizeof(name), "p%dblink", idx + 1);
        s->timer = xTimerCreate(name,
                                pdMS_TO_TICKS(POUR_BLINK_INTERVAL_MS),
                                pdTRUE,
                                (void *)(size_t)idx,   /* pvTimerID = box index */
                                pour_blink_callback);
    }

    if (s->timer) {
        s->show_null = 0;
        s->fire      = 0;
        xTimerReset(s->timer, 0);
        cook_preload_blink_frames(idx);
        printf("[BLINK] Pour%d blink STARTED\r\n", idx + 1);
    }
}

/**
 * @brief 停止指定菜盒的倒菜闪烁动画并释放预加载帧缓冲区
 * @param idx  菜盒索引(0~2), >= BOX_COUNT 则停止全部
 */
static void cook_stop_pour_loop(uint8_t idx)
{
    for (uint8_t i = 0; i < BOX_COUNT; i++) {
        if (idx < BOX_COUNT && i != idx) continue;  /* 指定单个则跳过其他 */

        pour_blink_state_t *s = &s_pour_blink[i];
        if (s->timer != NULL) {
            xTimerStop(s->timer, 0);  /* 不删除定时器句柄(复用) */
        }
        s->fire    = 0;
        s->pending = 0;

        if (s->frame_buf) { free(s->frame_buf); s->frame_buf = NULL; }
        if (s->null_buf)  { free(s->null_buf);  s->null_buf  = NULL; }
        s->frame_w = 0;
        s->frame_h = 0;
    }
}

/* ================================================================== */
/*                     初始化                                         */
/* ================================================================== */

void cook_ui_init(void)
{
    cook_init_layout();

    /* 强制全量重绘(开机首次) */
    g_cook_status->sys_changed   = 1;
    g_cook_status->bg_changed    = 1;
    for (int i = 0; i < BOX_COUNT; i++) {
        g_cook_status->box[i].changed = 1;
    }

    printf("[COOK_UI] initialized: sys=IDLE, box=all READY\r\n");
}

/* ================================================================== */
/*                     渲染引擎核心                                    */
/* ================================================================== */

void cook_render(void)
{
    if (!s_scr_w) return;  /* 未初始化则跳过 */

    if (g_cook_status->rendering) return;
    g_cook_status->rendering = 1;

    /* ---- 1. 背景 ---- */
    if (g_cook_status->bg_changed) {
        cook_draw_background(g_cook_status->bg_scene);
        g_cook_status->bg_changed = 0;
        for (int i = 0; i < BOX_COUNT; i++)
            g_cook_status->box[i].changed = 1;
        g_cook_status->sys_changed = 1;
    }

    /* ---- 1.5 倒菜闪烁: A3/A4/A5设pending, 等音频播完再开闪 ---- */
    for (uint8_t i = 0; i < BOX_COUNT; i++) {
        if (s_pour_blink[i].pending) {
            if ((g_audiodev.status & 0x0F) != 0x03) {
                s_pour_blink[i].pending = 0;
                printf("[COOK_UI] Voice done -> Start Pour%d BLINK\r\n", i + 1);
                cook_start_pour_loop(i);
            }
        }
    }

    /* ---- 1.6 闪烁刷图 (预加载帧 → 直接从PSRAM刷屏, 零SD卡I/O) ---- */
    for (uint8_t i = 0; i < BOX_COUNT; i++) {
        pour_blink_state_t *s = &s_pour_blink[i];
        if (s->fire && s->timer != NULL) {
            s->fire = 0;
            uint8_t *buf = s->show_null ? s->null_buf : s->frame_buf;
            if (buf != NULL && s->frame_w > 0 && s->frame_h > 0) {
                pic_phy.multicolor(0, 0, s->frame_w, s->frame_h, (uint16_t *)buf);
            } else {
                printf("[BLINK] Pour%d buf NULL, retry preload\r\n", i + 1);
                cook_preload_blink_frames(i);
            }
        }
    }

    /* ---- 2. 状态栏 ---- */
    // if (g_cook_status->sys_changed) {
    //     cook_draw_statusbar();
    //     g_cook_status->sys_changed = 0;
    // }

    /* ---- 3. 菜盒 ---- */
    // for (int i = 0; i < BOX_COUNT; i++) {
    //     if (g_cook_status->box[i].changed) {
    //         cook_draw_box(i);
    //         g_cook_status->box[i].changed = 0;
    //     }
    // }

    /* ---- 4. 报警闪烁 (已禁用: 不绘制底部红条) ---- */
    /* if (g_cook_status->sys_state == SYS_ALARM_TEMP ||
        g_cook_status->sys_state == SYS_ALARM_FIRE) {
        cook_draw_alarm_flash();
    } */

    /* ---- 5. 音频信息区 (cook_ui接管audioplay的显示) ---- */
    // if (g_cook_status->audio.cover_changed) {
    //     cook_draw_audio_cover();
    //     g_cook_status->audio.cover_changed = 0;
    //     g_cook_status->audio.changed = 1;   /* 封面变化后刷新文字信息 */
    // }
    // if (g_cook_status->audio.changed) {
    //     cook_draw_audio_info();
    //     g_cook_status->audio.changed = 0;
    // }

    g_cook_status->rendering = 0;
}

/* ================================================================== */
/*                     底层绘制函数                                    */
/* ================================================================== */

/** 前置声明: cook_draw_box 内部调用 cook_draw_icon, 需提前声明 */
void cook_draw_icon(uint16_t x, uint16_t y, cook_box_status_e status);

/**
 * @brief 绘制背景图(全屏)
 */
void cook_draw_background(uint8_t scene)
{
    const char *filename = cook_get_bg_filename(scene);

    int64_t t0 = esp_timer_get_time();
    if (piclib_ai_load_picfile((char *)filename,
                               0, 0, s_scr_w, s_scr_h) == 0) {
        int64_t cost_us = esp_timer_get_time() - t0;
        printf("[COOK_UI] BG scene %d OK (%lld ms)\r\n", scene, cost_us / 1000);
    } else {
        int64_t cost_us = esp_timer_get_time() - t0;
        printf("[COOK_UI] BG scene %d FAILED (%lld ms), clear\r\n", scene, cost_us / 1000);
        lcd_clear(BLACK);
    }
}

/**
 * @brief 绘制系统状态栏 (底部上方区域)
 *
 *  布局: 屏幕底部往上排列:
 *    [状态栏]  高度 STATUS_BAR_H
 *    [菜盒区]  高度 BOX_AREA_H
 */
void cook_draw_statusbar(void)
{
    /* TODO: 暂时隐藏, 等UI优化后恢复 */
    return;
    int16_t bar_y = s_scr_h - STATUS_BAR_H - BOX_AREA_H;
    if (bar_y < 0) bar_y = 0;

    safe_fill(0, bar_y, s_scr_w - 1, bar_y + STATUS_BAR_H - 1, 0x1110);

    const char *status_text = "";
    uint16_t status_color = WHITE;

    switch (g_cook_status->sys_state) {
        case SYS_IDLE:         status_text = "IDLE";      status_color = GREEN; break;
        case SYS_COOKING:      status_text = "COOKING";   status_color = YELLOW; break;
        case SYS_DONE:         status_text = "DONE";      status_color = BLUE; break;
        case SYS_ALARM_TEMP:   status_text = "!TEMP ABNORMAL!"; status_color = RED; break;
        case SYS_ALARM_FIRE:   status_text = "!! FIRE ALARM !!";  status_color = RED; break;
    }

    if (bar_y + 8 <= s_scr_h) {
        lcd_show_string(4, bar_y + 8, s_scr_w / 3, 16, 12,
                        (char *)status_text, status_color);
    }

    char scene_str[24];
    snprintf(scene_str, sizeof(scene_str), "SC:%d", g_cook_status->bg_scene);
    if (bar_y + 8 <= s_scr_h && s_scr_w > 60) {
        lcd_show_string(s_scr_w - 60, bar_y + 8, 60, 16, 12,
                        scene_str, CYAN);
    }
}

/**
 * @brief 绘制单个菜盒(炒菜模式垂直大卡片)
 *
 *  布局:
 *  ┌──────────────────────────┐
 *  │        N号菜盒           │  小字, 黑色
 *  │          就绪            │  大字, 绿色/黄/蓝
 *  └──────────────────────────┘
 */
void cook_draw_box(uint8_t box_id)
{
    /* TODO: 暂时隐藏, 等UI优化后恢复 */
    (void)box_id;
    return;
    if (box_id >= BOX_COUNT || !s_scr_w) return;

    cook_box_t *box = &g_cook_status->box[box_id];
    uint16_t bx = box->x;
    uint16_t by = box->y;

    /* 边界检查 */
    if (bx >= s_scr_w || by >= s_scr_h) return;

    uint16_t bxe = bx + COOK_BOX_W - 1;
    uint16_t bye = by + COOK_BOX_H - 1;
    if (bxe >= s_scr_w) bxe = s_scr_w - 1;
    if (bye >= s_scr_h) bye = s_scr_h - 1;

/* ---- 外框: 白色(带简易圆角) ---- */
lcd_draw_rectangle(bx, by, bxe, bye, WHITE);
/* 四角补画小圆弧模拟圆角 */
{
    uint8_t r = 6;
    /* 左上角 */
    if (bx + r < s_scr_w && by + r < s_scr_h)
        lcd_draw_circle(bx + r, by + r, r, WHITE);
    /* 右上角 */
    if (bxe - r < s_scr_w && by + r < s_scr_h)
        lcd_draw_circle(bxe - r, by + r, r, WHITE);
    /* 左下角 */
    if (bx + r < s_scr_w && bye - r < s_scr_h)
        lcd_draw_circle(bx + r, bye - r, r, WHITE);
    /* 右下角 */
    if (bxe - r < s_scr_w && bye - r < s_scr_h)
        lcd_draw_circle(bxe - r, bye - r, r, WHITE);
}

    /* ---- 内部底色: 浅灰白 ---- */
    safe_fill(bx + 2, by + 2, bxe - 2, bye - 2, 0xF800 | 0x07E0);  /* 近白色 */

    /* ---- 第一行: "N号菜盒" 居中(小字, 深蓝色) ---- */
    char title[16];
    snprintf(title, sizeof(title), "%d号菜盒", box_id + 1);
    uint16_t title_w = (strlen((char *)title)) * 12;   /* 估算字宽 */
    uint16_t title_x = (bxe - bx > title_w) ? bx + (COOK_BOX_W - title_w) / 2 : bx + 4;
    lcd_show_string(title_x, by + 6, COOK_BOX_W - 8, 14, 12, title, DARKBLUE);

    /* ---- 第二行: 状态文字 居中(大字, 彩色) ---- */
    const char *stext = "?";
    uint16_t scolor = GRAY;
    switch (box->status) {
        case BOX_READY:   stext = "就绪";   scolor = GREEN;  break;
        case BOX_DONE:    stext = "已完成"; scolor = BLUE;   break;
        case BOX_POURING: stext = "倒菜中"; scolor = YELLOW; break;
    }
    uint16_t status_w = strlen((char *)stext) * 16;         /* 大字估算 */
    uint16_t status_x = (bxe - bx > status_w) ? bx + (COOK_BOX_W - status_w) / 2 : bx + 4;
    lcd_show_string(status_x, by + 24, COOK_BOX_W - 8, 18, 16, (char *)stext, scolor);
}

/**
 * @brief 绘制圆形状态图标
 */
void cook_draw_icon(uint16_t x, uint16_t y, cook_box_status_e status)
{
    uint16_t cx = x + ICON_SIZE / 2;
    uint16_t cy = y + ICON_SIZE / 2;
    uint16_t r  = ICON_SIZE / 2 - 3;

    if (r < 3) r = 3;
    if (cx >= s_scr_w || cy >= s_scr_h) return;

    uint16_t bg_color, border_color;
    const char *label = "?";

    switch (status) {
        case BOX_READY:   bg_color=GREEN; border_color=DARKBLUE; label="R"; break;
        case BOX_DONE:    bg_color=BLUE;  border_color=DARKBLUE; label="D"; break;
        case BOX_POURING: bg_color=YELLOW;border_color=RED; label="P"; break;
        default:          bg_color=GRAY;  border_color=BLACK;  label="?"; break;
    }

    lcd_draw_circle(cx, cy, r, border_color);
    lcd_fill_circle(cx, cy, r - 1, bg_color);
    lcd_show_string(x + 3, cy - 5, ICON_SIZE - 6, 10, 10,
                    (char *)label, WHITE);
}

/**
 * @brief 报警闪烁效果
 */
void cook_draw_alarm_flash(void)
{
    int16_t bar_y = s_scr_h - STATUS_BAR_H - BOX_AREA_H;
    if (bar_y < 0) bar_y = 0;

    if (g_cook_status->alarm_flash_on) {
        safe_fill(0, bar_y, s_scr_w - 1, bar_y + STATUS_BAR_H - 1, RED);
        const char *atxt = (g_cook_status->sys_state == SYS_ALARM_TEMP)
                           ? "!TEMP ABNORMAL!" : "!! FIRE ALARM !!";
        if (bar_y + 8 <= s_scr_h) {
            lcd_show_string(4, bar_y + 8, s_scr_w / 2, 16, 12,
                            (char *)atxt, WHITE);
        }
    } else {
        cook_draw_statusbar();
    }
}

/* ================================================================== */
/*                   音频信息渲染 (cook_ui接管)                         */
/* ================================================================== */

/**
 * @brief 更新音频播放数据(供 audioplay 模块调用, 不直接写屏)
 */
void cook_audio_update(uint16_t index, uint16_t total,
                       uint32_t cur_sec, uint32_t tot_sec, uint32_t bitrate)
{
    g_cook_status->audio.song_index = index;
    g_cook_status->audio.total_songs = total;
    g_cook_status->audio.cur_sec     = cur_sec;
    g_cook_status->audio.tot_sec     = tot_sec;
    g_cook_status->audio.bitrate     = bitrate;
    g_cook_status->audio.changed     = 1;
}

void cook_audio_set_playing(uint8_t playing)
{
    g_cook_status->audio.playing = playing;
    g_cook_status->audio.changed  = 1;
}

void cook_audio_set_cover_dirty(void)
{
    g_cook_status->audio.cover_changed = 1;
}

/**
 * @brief 渲染音乐信息区 (曲目编号 + 进度时间 + 比特率)
 * @note  原audioplay的 audio_index_show() / audio_msg_show() 合并到此函数
 */
void cook_draw_audio_info(void)
{
    cook_audio_info_t *au = &g_cook_status->audio;
    int16_t ay = AUDIO_INFO_Y;

    if (!au->playing && au->song_index == 0) return;

    safe_fill(0, ay, s_scr_w - 1, ay + AUDIO_INFO_H - 1, 0x1084);

    if (ay + 6 <= s_scr_h) {
        lcd_show_num(30, ay + 2, au->song_index, 3, 16, RED);
        lcd_show_char(30 + 24, ay + 2, '/', 16, 0, RED);
        lcd_show_num(30 + 32, ay + 2, au->total_songs, 3, 16, RED);

        lcd_show_xnum(30, ay + 22, au->cur_sec / 60, 2, 16, 0X80, RED);
        lcd_show_char(30 + 16, ay + 22, ':', 16, 0, RED);
        lcd_show_xnum(30 + 24, ay + 22, au->cur_sec % 60, 2, 16, 0X80, RED);
        lcd_show_char(30 + 40, ay + 22, '/', 16, 0, RED);
        lcd_show_xnum(30 + 48, ay + 22, au->tot_sec / 60, 2, 16, 0X80, RED);
        lcd_show_char(30 + 64, ay + 22, ':', 16, 0, RED);
        lcd_show_xnum(30 + 72, ay + 22, au->tot_sec % 60, 2, 16, 0X80, RED);

        lcd_show_num(30 + 110, ay + 22, au->bitrate / 1000, 4, 16, RED);
        lcd_show_string(30 + 110 + 32, ay + 22, 200, 16, 16, "Kbps", RED);
    }
}

/**
 * @brief 渲染当前曲目封面图
 * @note  由 cook_render 统一调度, 避免与 I2S DMA 冲突
 */
void cook_draw_audio_cover(void)
{
    char pic_path[72];
    uint16_t idx = g_cook_status->audio.song_index;

    if (idx == 0) return;

    const char *exts[] = {".bmp", ".jpg", ".jpeg", ".png"};

    for (uint8_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        snprintf(pic_path, sizeof(pic_path), "0:/PICTURE/%02d%s", idx, exts[e]);

        FILINFO finfo;
        if (f_stat(pic_path, &finfo) != FR_OK) continue;

        int64_t t0 = esp_timer_get_time();
        if (piclib_ai_load_picfile(pic_path, 0, 0, s_scr_w, s_scr_h) == 0) {
            int64_t cost_us = esp_timer_get_time() - t0;
            printf("[COOK_UI] Cover #%d OK (%lld ms)\r\n", idx, cost_us / 1000);
            return;
        }
    }
    printf("[COOK_UI] No cover for song #%d\r\n", idx);
}

/* ================================================================== */
/*                   菜盒状态操作                                     */
/* ================================================================== */

void cook_set_box(uint8_t box_id, cook_box_status_e status)
{
    if (box_id < 1 || box_id > BOX_COUNT) return;
    if (g_cook_status->box[box_id - 1].status != status) {
        g_cook_status->box[box_id - 1].status = status;
        g_cook_status->box[box_id - 1].changed = 1;
    }
}

cook_box_status_e cook_get_box(uint8_t box_id)
{
    if (box_id < 1 || box_id > BOX_COUNT) return BOX_READY;
    return g_cook_status->box[box_id - 1].status;
}

void cook_set_bg_scene(uint8_t scene)
{
    if (scene < 1) scene = 1;
    if (scene > 12) scene = 12;   /* 扩展: 支持3个归位完成界面(10~12) */
    if (g_cook_status->bg_scene != scene) {
        g_cook_status->bg_scene = scene;
        g_cook_status->bg_changed = 1;
    }
}

/* ================================================================== */
/*                     综合命令 (地址即语义)                            */
/* ================================================================== */

/**
 * @brief 炒菜完成3秒定时器回调 → 自动跳转待机
 */
static void finish_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    printf("[COOK_UI] Finish timer expired -> IDLE\r\n");
    cook_cmd_idle();
}

/**
 * @brief 开机: 播放001.wav + 进入待机界面
 */
void cook_cmd_boot(void)
{
    g_cook_status->sys_state = SYS_IDLE;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(1);    /* 待机界面 */

    /* 开机视频模式: 通过 g_skip_voice 标志跳过 001.wav, 避免与视频 I2S 冲突 */
    extern uint8_t g_skip_voice;
    if (!g_skip_voice) {
        rs485_target_index = VOICE_BOOT;   /* 001.wav */
        rs485_cmd_flag     = 1;
        printf("[COOK_UI] CMD: BOOT (voice=%d)\r\n", VOICE_BOOT);
    } else {
        printf("[COOK_UI] CMD: BOOT (skip voice for video)\r\n");
    }
}

/**
 * @brief 倒菜 (box_id: 1~3)
 */
void cook_cmd_pour_box(uint8_t box_id)
{
    if (box_id < 1 || box_id > BOX_COUNT) return;

    /* 停止报警循环(任何操作都中断报警) */
    if (s_alarm_loop_active) cook_alarm_stop();

    cook_set_box(box_id, BOX_POURING);

    uint8_t scene_map[] = {0, 2, 3, 4};   /* box_id→scene: 1→2, 2→3, 3→4 */
    cook_set_bg_scene(scene_map[box_id]);

    uint8_t voice_map[] = {0, VOICE_POUR1, VOICE_POUR2, VOICE_POUR3};
    rs485_target_index = voice_map[box_id];
    rs485_cmd_flag     = 1;

    /* ★ 倒菜闪烁: 切换时必须彻底停止其他菜盒的定时器+释放帧 ★ */
    uint8_t idx = box_id - 1;   /* 转为0-based索引 */
    /* 停止其他所有菜盒的闪烁(定时器+帧缓冲), 同一时刻只允许一个在闪 */
    for (uint8_t i = 0; i < BOX_COUNT; i++) {
        if (i != idx) cook_stop_pour_loop(i);
    }
    printf("[COOK_UI] POUR BOX%d -> pending blink after voice\r\n", box_id);
    s_pour_blink[idx].pending = 1;       /* cook_render 检测到音频停了就开闪 */

    printf("[COOK_UI] CMD: POUR BOX%d\r\n", box_id);
}

/**
 * @brief 投料盒归位 (旧接口保留, 默认归位全部)
 */
void cook_cmd_reset(void)
{
    /* 停止所有菜盒的倒菜闪烁动画 */
    cook_stop_pour_loop(BOX_COUNT);  /* >= BOX_COUNT 表示停止全部 */

    if (s_alarm_loop_active) cook_alarm_stop();

    for (int i = 0; i < BOX_COUNT; i++) cook_set_box(i + 1, BOX_READY);
    cook_set_bg_scene(5);
    rs485_target_index = VOICE_BOX_RETURN;
    rs485_cmd_flag     = 1;
    printf("[COOK_UI] CMD: RESET ALL\r\n");
}

/**
 * @brief 菜盒归位(指定菜盒) + 显示对应归位完成界面
 * @param box_id  菜盒编号(1~3), 来自 A6/A7/A8 指令
 */
void cook_cmd_box_return(uint8_t box_id)
{
    if (box_id < 1 || box_id > BOX_COUNT) return;

    /* 停止所有菜盒的倒菜闪烁动画 */
    cook_stop_pour_loop(BOX_COUNT);

    if (s_alarm_loop_active) cook_alarm_stop();

    cook_set_box(box_id, BOX_DONE);

    /* 每个菜盒有独立的归位完成界面: 10/11/12 */
    uint8_t scene_map[] = {0, 10, 11, 12};   /* box_id→scene */
    cook_set_bg_scene(scene_map[box_id]);

    rs485_target_index = VOICE_BOX_RETURN;    /* 006.wav 归位音 */
    rs485_cmd_flag     = 1;
    printf("[COOK_UI] CMD: BOX%d RETURN (scene=%d)\r\n", box_id, scene_map[box_id]);
}

/**
 * @brief 开始炒菜
 */
void cook_cmd_start(void)
{
    /* 停止所有菜盒的倒菜闪烁动画 */
    cook_stop_pour_loop(BOX_COUNT);

    if (s_alarm_loop_active) cook_alarm_stop();

    g_cook_status->sys_state = SYS_COOKING;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(5);      /* 开始炒菜 → 归位界面(bg_reset.bmp) */
    rs485_target_index = VOICE_START_COOK;
    rs485_cmd_flag     = 1;
    printf("[COOK_UI] CMD: START COOKING -> reset scene\r\n");
}

/**
 * @brief 炒菜完成 → 显示done界面 + 3秒后自动跳转待机
 */
void cook_cmd_finish(void)
{
    if (s_alarm_loop_active) cook_alarm_stop();

    g_cook_status->sys_state = SYS_DONE;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(7);      /* 完成界面 */
    rs485_target_index = VOICE_FINISH;
    rs485_cmd_flag     = 1;

    /* 创建/复位3秒单次定时器, 到期自动回待机 */
    if (!s_finish_timer) {
        s_finish_timer = xTimerCreate("fin_tmr",
                                      pdMS_TO_TICKS(3000),
                                      pdFALSE,          /* 单次模式 */
                                      NULL,
                                      finish_timer_callback);
    }
    if (s_finish_timer) {
        xTimerReset(s_finish_timer, 0);
        printf("[COOK_UI] CMD: FINISH (timer 3s -> IDLE)\r\n");
    } else {
        printf("[COOK_UI] CMD: FINISH (no timer!)\r\n");
    }
}

/**
 * @brief 温度异常 (单次播报, 不循环)
 */
void cook_cmd_alarm_temp(void)
{
    g_cook_status->sys_state = SYS_ALARM_TEMP;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(8);

    /* 单次播放报警语音 */
    rs485_target_index = VOICE_ALARM_TEMP;
    rs485_cmd_flag     = 1;

    if (!s_alarm_task_handle) {
        xTaskCreate(cook_alarm_flash_task, "alarm_fl", 2048, NULL, 2, &s_alarm_task_handle);
    }
    printf("[COOK_UI] CMD: ALARM TEMP (once)\r\n");
}

/**
 * @brief 火警 (单次播报, 不循环)
 */
void cook_cmd_alarm_fire(void)
{
    g_cook_status->sys_state = SYS_ALARM_FIRE;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(9);

    /* 单次播放报警语音 */
    rs485_target_index = VOICE_ALARM_FIRE;
    rs485_cmd_flag     = 1;

    if (!s_alarm_task_handle) {
        xTaskCreate(cook_alarm_flash_task, "alarm_fl", 2048, NULL, 2, &s_alarm_task_handle);
    }
    printf("[COOK_UI] CMD: ALARM FIRE (once)\r\n");
}

/**
 * @brief 复位待机 (停止一切: 报警闪烁任务 / 循环播报 / 完成定时器)
 */
void cook_cmd_idle(void)
{
    /* 停止所有菜盒的倒菜闪烁动画 */
    cook_stop_pour_loop(BOX_COUNT);

    /* 停止报警闪烁FreeRTOS任务 */
    if (s_alarm_task_handle) {
        vTaskDelete(s_alarm_task_handle);
        s_alarm_task_handle = NULL;
    }
    g_cook_status->alarm_flash_on = 0;

    /* 停止报警循环播报 */
    s_alarm_loop_active = 0;

    /* 停止完成定时器(如果正在倒计时) */
    if (s_finish_timer) {
        xTimerStop(s_finish_timer, 0);
    }

    /* 状态恢复 */
    g_cook_status->sys_state = SYS_IDLE;
    g_cook_status->sys_changed = 1;
    for (int i = 0; i < BOX_COUNT; i++) cook_set_box(i + 1, BOX_READY);
    cook_set_bg_scene(1);       /* 待机界面 */
    printf("[COOK_UI] CMD: IDLE (all cleared)\r\n");
}

/**
 * @brief 停止报警循环播报 (不改变界面, 仅停止循环重播)
 */
void cook_alarm_stop(void)
{
    s_alarm_loop_active = 0;
    printf("[COOK_UI] Alarm loop stopped\r\n");
}

/**
 * @brief 查询报警循环是否激活
 * @return 1=激活中, 0=未激活
 */
uint8_t cook_is_alarm_loop(void)
{
    return s_alarm_loop_active;
}

/**
 * @brief 获取当前报警语音编号 (供主循环循环播报用)
 * @return VOICE_ALARM_TEMP 或 VOICE_ALARM_FIRE, 未报警时返回0
 */
uint8_t cook_alarm_voice_index(void)
{
    if (!s_alarm_loop_active) return 0;
    if (g_cook_status->sys_state == SYS_ALARM_TEMP) return VOICE_ALARM_TEMP;
    if (g_cook_status->sys_state == SYS_ALARM_FIRE) return VOICE_ALARM_FIRE;
    return 0;
}

/**
 * @brief 停止播报 (数据区 0x0000 触发)
 * @note  停止当前正在播放的语音 + 停止报警循环, 但**保持界面不变**
 *
 *        ★ 退出链路 (必须同步三个环节, 否则主循环卡死) ★
 *        1. audio_stop()         -> status=0, I2S DMA停
 *        2. i2s_play_end=ESP_OK   -> 通知wav_play_song()监控循环退出
 *        3. i2s_play_next_prev   -> 通知music任务从暂停等待中跳出并自删除
 */
void cook_cmd_stop_voice(void)
{
    /* 停止报警循环 */
    s_alarm_loop_active = 0;

    /* 仅在有活跃播放任务时才中断, 避免操作未初始化外设 */
    if (g_audiodev.status == 0x03) {
        audio_stop();                       /* 1: 停I2S DMA + status清零 */
        i2s_play_end      = ESP_OK;         /* 2: 通知wav_play_song监控循环退出 */
        i2s_play_next_prev = ESP_OK;        /* 3: 通知music任务退出并自删除 */
    }

    printf("[COOK_UI] CMD: STOP VOICE (scene unchanged)\r\n");
}
