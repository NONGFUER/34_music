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
 *              - MODBUS RTU 0x0201~0x0204 寄存器协议支持
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
    printf("[COOK_UI] CMD: POUR BOX%d\r\n", box_id);
}

/**
 * @brief 投料盒归位 (旧接口保留, 默认归位全部)
 */
void cook_cmd_reset(void)
{
    if (s_alarm_loop_active) cook_alarm_stop();

    for (int i = 0; i < BOX_COUNT; i++) cook_set_box(i + 1, BOX_READY);
    cook_set_bg_scene(5);
    rs485_target_index = VOICE_BOX_RETURN;
    rs485_cmd_flag     = 1;
    printf("[COOK_UI] CMD: RESET ALL\r\n");
}

/**
 * @brief 菜盒归位(指定菜盒) + 显示对应归位完成界面
 * @param box_id  菜盒编号(1~3), 来自 REG_BOX_RETURN 的数据区值
 */
void cook_cmd_box_return(uint8_t box_id)
{
    if (box_id < 1 || box_id > BOX_COUNT) return;

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
 * @brief 温度异常 (激活循环播报)
 */
void cook_cmd_alarm_temp(void)
{
    g_cook_status->sys_state = SYS_ALARM_TEMP;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(8);
    s_alarm_loop_active = 1;

    /* 仅在非播放状态下才首次立即播放(避免重复触发打断正在播的报警语音) */
    if (g_audiodev.status != 0x03) {
        rs485_target_index = VOICE_ALARM_TEMP;
        rs485_cmd_flag     = 1;
    }

    if (!s_alarm_task_handle) {
        xTaskCreate(cook_alarm_flash_task, "alarm_fl", 2048, NULL, 2, &s_alarm_task_handle);
    }
    printf("[COOK_UI] CMD: ALARM TEMP (loop ON)\r\n");
}

/**
 * @brief 火警 (激活循环播报)
 */
void cook_cmd_alarm_fire(void)
{
    g_cook_status->sys_state = SYS_ALARM_FIRE;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(9);
    s_alarm_loop_active = 1;

    /* 仅在非播放状态下才首次立即播放(避免重复触发打断正在播的报警语音) */
    if (g_audiodev.status != 0x03) {
        rs485_target_index = VOICE_ALARM_FIRE;
        rs485_cmd_flag     = 1;
    }

    if (!s_alarm_task_handle) {
        xTaskCreate(cook_alarm_flash_task, "alarm_fl", 2048, NULL, 2, &s_alarm_task_handle);
    }
    printf("[COOK_UI] CMD: ALARM FIRE (loop ON)\r\n");
}

/**
 * @brief 复位待机 (停止一切: 报警闪烁任务 / 循环播报 / 完成定时器)
 */
void cook_cmd_idle(void)
{
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
