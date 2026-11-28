#pragma once
#include <Arduino.h>

// Minimal I2S standard-mode shim: i2s_channel_write feeds the browser audio
// ring (16-bit stereo @ 16 kHz), everything else is bookkeeping.
typedef void *i2s_chan_handle_t;
struct i2s_chan_config_t { int dma_desc_num; int dma_frame_num; };
struct i2s_std_clk_config_t { int sample_rate; int mclk_multiple; };
struct i2s_std_slot_config_t { int data_bit_width; int slot_mode; };
struct i2s_std_gpio_config_t { int mclk, bclk, ws, dout, din; struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags; };
struct i2s_std_config_t { i2s_std_clk_config_t clk_cfg; i2s_std_slot_config_t slot_cfg; i2s_std_gpio_config_t gpio_cfg; };

#define I2S_NUM_AUTO 0
#define I2S_ROLE_MASTER 1
#define I2S_MCLK_MULTIPLE_256 256
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_GPIO_UNUSED -1

#define I2S_CHANNEL_DEFAULT_CONFIG(id, role) { 8, 480 }
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { rate, 256 }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, slots) { bits, slots }

int i2s_new_channel(const i2s_chan_config_t *ch, i2s_chan_handle_t *handle, void *ev);
int i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *cfg);
int i2s_channel_enable(i2s_chan_handle_t handle);
int i2s_channel_disable(i2s_chan_handle_t handle);
int i2s_del_channel(i2s_chan_handle_t handle);
int i2s_channel_write(i2s_chan_handle_t handle, const void *data, size_t size, size_t *written, int timeout_ms);
#ifndef GPIO_NUM_4
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8
#endif
