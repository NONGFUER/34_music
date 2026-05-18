/**
 ****************************************************************************************************
 * @file        i2s.c
 * @author      sjwu
 * @version     V1.0
 * @date        2025-01-01
 * @brief       I2S驱动代码
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

#include "myi2s.h"


i2s_chan_handle_t tx_handle = NULL;     /* I2S发送通道句柄 */
i2s_chan_handle_t rx_handle = NULL;     /* I2S接收通道句柄 */
i2s_std_config_t my_std_cfg;            /* 标准模式配置结构体 */

/*
 * @brief       初始化I2S(带参数版本,避免runtime reconfig不可靠的问题)
 * @param       samplerate  :目标采样率(如16000, 44100等)
 * @param       bits_sample :数据位宽(I2S_DATA_BIT_WIDTH_16BIT等)
 * @retval      ESP_OK:初始化成功;其他:失败
 */
esp_err_t myi2s_init(int samplerate, int bits_sample)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);  /* 默认的通道配置(I2S0,主机) */
    chan_cfg.auto_clear = true;                                             /* 自动清除DMA缓冲区遗留的数据 */ 
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));    /* 分配新的I2S通道 */

    i2s_std_config_t std_cfg = {    /* 标准通信模式配置 */
        .clk_cfg  = {               /* 时钟配置 */
            .sample_rate_hz = samplerate,               /* 使用传入的目标采样率! */
            .clk_src        = I2S_CLK_SRC_DEFAULT,          /* I2S时钟源 */
            .mclk_multiple  = I2S_MCLK_MULTIPLE,            /* I2S主时钟MCLK相对于采样率的倍数(默认256) */
        },

        .slot_cfg = {               /* 声道配置 */
            .data_bit_width = bits_sample,                 /* 使用传入的目标位宽! */
            .slot_bit_width = bits_sample,                 /* 槽位宽(必须同步!) */
            .slot_mode      = I2S_SLOT_MODE_STEREO,         /* 立体声 */
            .slot_mask      = I2S_STD_SLOT_BOTH,            /* 启用通道 */
            .ws_width       = bits_sample,                  /* WS信号位宽 */
            .ws_pol         = false,                        /* WS信号极性 */
            .bit_shift      = true,                         /* 位移位(Philips模式下配置) */
            .left_align     = true,                         /* 左对齐 */
            .big_endian     = false,                        /* 小端模式 */
            .bit_order_lsb  = false                         /* MSB */
        }, 
        
        .gpio_cfg = {               /* 引脚配置 */
            .mclk = I2S_MCK_IO,     /* 主时钟线 */
            .bclk = I2S_BCK_IO,     /* 位时钟线 */
            .ws   = I2S_WS_IO,      /* 字(声道)选择线 */
            .dout = I2S_DO_IO,      /* 串行数据输出线 */
            .din  = I2S_DI_IO,      /* 串行数据输入线 */
            .invert_flags = {       /* 引脚翻转(不反相) */
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    my_std_cfg = std_cfg;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));    /* 初始化TX通道 */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));    /* 初始化RX通道 */
    /* 注意: 不在此处enable通道, 由 audio_start() -> i2s_trx_start() 统一控制启停 */

    printf("I2S init: rate=%dHz, bits=%d\r\n", samplerate, bits_sample);

    return ESP_OK;
}

/**
 * @brief       I2S TRX启动
 * @param       无
 * @retval      无
 */
void i2s_trx_start(void)
{
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

/**
 * @brief       I2S TRX停止
 * @param       无
 * @retval      无
 */
void i2s_trx_stop(void)
{
    /* 忽略返回值: 通道可能尚未enable, disable是幂等操作 */
    (void)i2s_channel_disable(tx_handle);
    (void)i2s_channel_disable(rx_handle);
}

/**
 * @brief       I2S卸载
 * @param       无
 * @retval      无
 */
void i2s_deinit(void)
{
    ESP_ERROR_CHECK(i2s_del_channel(tx_handle));
    ESP_ERROR_CHECK(i2s_del_channel(rx_handle));
}

/**
 * @brief       设置采样率和位宽
 * @param       sampleRate  :采样率
 * @param       bits_sample :位宽
 * @retval      无
 */
void i2s_set_samplerate_bits_sample(int samplerate, int bits_sample)
{
    i2s_trx_stop();
    /* 如果需要更新声道或时钟配置,需要在更新前先禁用通道 */
    my_std_cfg.slot_cfg.data_bit_width = bits_sample;    /* 数据位宽(必须同步!) */
    my_std_cfg.slot_cfg.slot_bit_width = bits_sample;    /* 槽位宽(必须同步!) */
    my_std_cfg.slot_cfg.ws_width = bits_sample;          /* WS信号位宽 */
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle, &my_std_cfg.slot_cfg));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(rx_handle, &my_std_cfg.slot_cfg));  /* 同步RX通道 */
    my_std_cfg.clk_cfg.sample_rate_hz = samplerate;      /* 设置采样率 */
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle, &my_std_cfg.clk_cfg));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(rx_handle, &my_std_cfg.clk_cfg));  /* 同步RX通道 */
}

/**
 * @brief       I2S传输数据
 * @param       buffer: 数据存储区的首地址
 * @param       frame_size: 数据大小
 * @retval      发送的数据长度
 */
size_t i2s_tx_write(uint8_t *buffer, uint32_t frame_size)
{
    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle, buffer, frame_size, &bytes_written, 1000));
    return bytes_written;
}

/**
 * @brief       I2S读取数据
 * @param       buffer: 读取数据存储区的首地址
 * @param       frame_size: 读取数据大小
 * @retval      接收的数据长度
 */
size_t i2s_rx_read(uint8_t *buffer, uint32_t frame_size)
{
    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_read(rx_handle, buffer, frame_size, &bytes_written, 1000));
    return bytes_written;
}
