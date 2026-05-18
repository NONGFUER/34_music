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
#include <stdio.h>

/* ================================================================== */
/*                    动态布局参数 (运行时计算)                          */
/* ================================================================== */

/** 屏幕实际尺寸 (从 lcddev 读取) */
static uint16_t s_scr_w = 0;
static uint16_t s_scr_h = 0;

/** 报警闪烁任务句柄 */
static TaskHandle_t s_alarm_task_handle = NULL;

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
 */
static void cook_init_layout(void)
{
    s_scr_w = lcddev.width;
    s_scr_h = lcddev.height;
    printf("[COOK_UI] Layout: %dx%d\r\n", s_scr_w, s_scr_h);

    /* 根据实际屏幕尺寸更新菜盒位置 */
    uint16_t box_total_w = UI_BOX_W * 3 + BOX_GAP * 2;
    uint16_t box_start_x = (s_scr_w > box_total_w) ? (s_scr_w - box_total_w) / 2 : 0;

    g_cook_status->box[0].x = box_start_x;
    g_cook_status->box[1].x = box_start_x + UI_BOX_W + BOX_GAP;
    g_cook_status->box[2].x = box_start_x + (UI_BOX_W + BOX_GAP) * 2;
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
    if (g_cook_status->sys_changed) {
        cook_draw_statusbar();
        g_cook_status->sys_changed = 0;
    }

    /* ---- 3. 菜盒 ---- */
    for (int i = 0; i < BOX_COUNT; i++) {
        if (g_cook_status->box[i].changed) {
            cook_draw_box(i);
            g_cook_status->box[i].changed = 0;
        }
    }

    /* ---- 4. 报警闪烁 ---- */
    if (g_cook_status->sys_state == SYS_ALARM_TEMP ||
        g_cook_status->sys_state == SYS_ALARM_FIRE) {
        cook_draw_alarm_flash();
    }

    /* ---- 5. 音频信息区 (cook_ui接管audioplay的显示) ---- */
    if (g_cook_status->audio.cover_changed) {
        cook_draw_audio_cover();
        g_cook_status->audio.cover_changed = 0;
        g_cook_status->audio.changed = 1;   /* 封面变化后刷新文字信息 */
    }
    if (g_cook_status->audio.changed) {
        cook_draw_audio_info();
        g_cook_status->audio.changed = 0;
    }

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

    if (piclib_ai_load_picfile((char *)filename,
                               0, 0, s_scr_w, s_scr_h) == 0) {
        printf("[COOK_UI] BG scene %d OK\r\n", scene);
    } else {
        printf("[COOK_UI] BG scene %d FAILED, clear\r\n", scene);
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
 * @brief 绘制单个菜盒
 * @param box_id 0~2
 */
void cook_draw_box(uint8_t box_id)
{
    if (box_id >= BOX_COUNT || !s_scr_w) return;

    cook_box_t *box = &g_cook_status->box[box_id];
    uint16_t bx = box->x;
    int16_t by = (int16_t)s_scr_h - BOX_AREA_H;
    if (by < 0) by = 0;

    uint16_t bxe = bx + UI_BOX_W - 1;
    uint16_t bye = by + UI_BOX_H - 1;

    /* 边界检查: 完全超出屏幕则跳过 */
    if (bx >= s_scr_w || by >= s_scr_h) return;

    /* 钳位终点 */
    if (bxe >= s_scr_w) bxe = s_scr_w - 1;
    if (bye >= s_scr_h) bye = s_scr_h - 1;

    /* 外框 */
    lcd_draw_rectangle(bx, by, bxe, bye, WHITE);

    /* 内部底色 */
    safe_fill(bx + 1, by + 1, bxe - 1, bye - 1, 0xEF7D);

    /* 分隔线 */
    uint16_t mid_y = by + (bye - by) / 2;
    if (mid_y < bye) {
        lcd_draw_hline(bx + 1, mid_y, (bxe - bx - 1), LGRAY);
    }

    /* 上半: 编号 */
    if (mid_y > by + 6) {
        char lbl[12];
        snprintf(lbl, sizeof(lbl), "BOX %d", box_id + 1);
        lcd_show_string(bx + 4, by + 4, (bxe-bx)/2, 14, 10, lbl, DARKBLUE);
    }

    /* 下半: 状态文字 */
    if (bye > mid_y + 4) {
        const char *stext = "?";
        uint16_t scolor = GRAY;
        switch (box->status) {
            case BOX_READY:   stext="READY"; scolor=GREEN; break;
            case BOX_DONE:    stext="DONE";  scolor=BLUE; break;
            case BOX_POURING: stext="POUR"; scolor=YELLOW; break;
        }
        lcd_show_string(bx + 4, mid_y + 4, (bxe-bx)-8, 14, 10,
                        (char *)stext, scolor);
    }

    /* 右侧: 圆形状态指示器 */
    if (bxe - bx > ICON_SIZE + 8 && bye - by > ICON_SIZE + 4) {
        uint16_t ix = bxe - ICON_SIZE - 4;
        uint16_t iy = by + ((bye - by) - ICON_SIZE) / 2;
        if (ix + ICON_SIZE <= s_scr_w && iy + ICON_SIZE <= s_scr_h) {
            cook_draw_icon(ix, iy, box->status);
        }
    }
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

        if (piclib_ai_load_picfile(pic_path, 0, 0, s_scr_w, s_scr_h) == 0) {
            printf("[COOK_UI] Cover loaded: song #%d\r\n", idx);
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
    if (scene > 9) scene = 9;
    if (g_cook_status->bg_scene != scene) {
        g_cook_status->bg_scene = scene;
        g_cook_status->bg_changed = 1;
    }
}

/* ================================================================== */
/*                     综合命令                                       */
/* ================================================================== */

void cook_cmd_pour_box(uint8_t box_id)
{
    if (box_id < 1 || box_id > BOX_COUNT) return;
    uint8_t idx = box_id - 1;

    cook_set_box(box_id, BOX_POURING);
    cook_set_bg_scene(VOICE_POUR1 + idx);
    rs485_target_index = VOICE_POUR1 + idx;
    rs485_cmd_flag = 1;
    printf("[COOK_UI] CMD: POUR BOX%d\r\n", box_id);
}

void cook_cmd_reset(void)
{
    for (int i = 0; i < BOX_COUNT; i++) cook_set_box(i + 1, BOX_READY);
    cook_set_bg_scene(5);
    rs485_target_index = VOICE_RESET;
    rs485_cmd_flag = 1;
    printf("[COOK_UI] CMD: RESET\r\n");
}

void cook_cmd_start(void)
{
    g_cook_status->sys_state = SYS_COOKING;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(6);
    rs485_target_index = VOICE_START;
    rs485_cmd_flag = 1;
    printf("[COOK_UI] CMD: START COOKING\r\n");
}

void cook_cmd_finish(void)
{
    g_cook_status->sys_state = SYS_DONE;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(7);
    rs485_target_index = VOICE_FINISH;
    rs485_cmd_flag = 1;
    printf("[COOK_UI] CMD: FINISH\r\n");
}

void cook_cmd_alarm_temp(void)
{
    g_cook_status->sys_state = SYS_ALARM_TEMP;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(8);
    rs485_target_index = VOICE_ALARM_TEMP;
    rs485_cmd_flag = 1;
    if (!s_alarm_task_handle) {
        xTaskCreate(cook_alarm_flash_task, "alarm_fl", 2048, NULL, 2, &s_alarm_task_handle);
    }
    printf("[COOK_UI] CMD: ALARM TEMP\r\n");
}

void cook_cmd_alarm_fire(void)
{
    g_cook_status->sys_state = SYS_ALARM_FIRE;
    g_cook_status->sys_changed = 1;
    cook_set_bg_scene(9);
    rs485_target_index = VOICE_ALARM_FIRE;
    rs485_cmd_flag = 1;
    if (!s_alarm_task_handle) {
        xTaskCreate(cook_alarm_flash_task, "alarm_fl", 2048, NULL, 2, &s_alarm_task_handle);
    }
    printf("[COOK_UI] CMD: ALARM FIRE\r\n");
}

void cook_cmd_idle(void)
{
    if (s_alarm_task_handle) { vTaskDelete(s_alarm_task_handle); s_alarm_task_handle = NULL; }
    g_cook_status->alarm_flash_on = 0;
    g_cook_status->sys_state = SYS_IDLE;
    g_cook_status->sys_changed = 1;
    for (int i = 0; i < BOX_COUNT; i++) cook_set_box(i + 1, BOX_READY);
    cook_set_bg_scene(1);
    printf("[COOK_UI] CMD: IDLE\r\n");
}
