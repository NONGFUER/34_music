/**
 ****************************************************************************************************
 * @file        mytimer.c
 *  
 * @version     V1.0
 * @date        2025-01-01
 * @brief       视频播放器实验定时器 驱动代码
 *  
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

#include "mytimer.h"


/* 视频播放帧率控制全局变量 */
volatile uint8_t g_frameup;             /* 视频播放时隙控制变量,当等于1的时候,可以更新下一帧视频 */
uint16_t g_frame;                       /* 播放帧率 */

esp_timer_handle_t frame_tim_handle;    /* 系统定时器句柄,用于帧间隔等待  */
gptimer_handle_t gptimer = NULL;        /* 通用定时器句柄,用于帧率显示 */

/**
 * @brief       定时器回调函数
 * @param       arg: 不携带参数
 * @retval      无
 */
void frame_timer_callback(void *arg)
{
    g_frameup = 1;  /* 表明一帧间隔时间到了 */
}

/**
 * @brief       初始化用于等待帧间隔时间定时器(ESP_TIMER)
 * @param       tps: 定时器周期,以微秒为单位(μs).
 *              若以一秒为定时器周期来执行一次定时器中断,那此处tps = 1s = 1000000μs
 * @retval      无
 */

void frame_timer_init(uint64_t tps)
{
    /* 定义一个定时器结构体设置定时器配置参数 */
    esp_timer_create_args_t timer_arg = {
        .callback = &frame_timer_callback,      /* 计时时间到达时执行的回调函数 */
        .arg = NULL,                            /* 传递给回调函数的参数 */
        .dispatch_method = ESP_TIMER_TASK,      /* 进入回调方式,从定时器任务进入 */
        .name = "frame Timer",                  /* 定时器名称 */
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_arg, &frame_tim_handle));   /* 创建定时器 */
    ESP_ERROR_CHECK(esp_timer_start_periodic(frame_tim_handle, tps));   /* 启动周期性定时器,tps设置定时器周期(us单位) */
}


/**
 * @brief       定时器重新开启
 * @param       tps: 定时器周期,以微秒为单位(μs).
 * @retval      无
 */
void frame_timer_start(uint64_t tps)
{   
    esp_timer_restart(frame_tim_handle, tps);
}

/**
 * @brief       定时器暂停工作
 * @param       无
 * @retval      无
 */
void frame_timer_stop(void)
{
    esp_timer_stop(frame_tim_handle);
}

/**
 * @brief       定时器回调函数
 * @param       无
 * @retval      无
 */
bool IRAM_ATTR frame_rate_print(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data)
{
    ESP_DRAM_LOGI("TimerGroup", "frame:%d fps\r\n", g_frame);
    g_frame = 0;
    return pdTRUE;
}


/**
 * @brief       用于帧率显示的定时器初始化函数
 * @param       无
 * @retval      无
 */
void frame_rate_timer(void)
{
    gptimer_config_t timer_config = {
        .clk_src   = GPTIMER_CLK_SRC_DEFAULT,   /* 定时器默认时钟源 */
        .direction = GPTIMER_COUNT_UP,          /* 向上计数 */
        .resolution_hz = 1000000,               /* 分辨率 */
    };
    gptimer_new_timer(&timer_config, &gptimer); /* 将配置设置到定时器 */

    gptimer_event_callbacks_t cbs = {           /* 绑定一个回调函数 */
        .on_alarm = frame_rate_print,           
    };
    gptimer_register_event_callbacks(gptimer, &cbs, NULL);  /* 设置定时器的回调函数cbs,不需要传入参数 */

    gptimer_enable(gptimer);                    /* 使能定时器 */

    gptimer_alarm_config_t alarm_config = {
        .reload_count               = 0,        /* 重载计数值为0 */
        .alarm_count                = 1000000,  /* 报警值,1s */
        .flags.auto_reload_on_alarm = true,     /* 开启重加载 */
    };
    gptimer_set_alarm_action(gptimer, &alarm_config);

    gptimer_start(gptimer);
}