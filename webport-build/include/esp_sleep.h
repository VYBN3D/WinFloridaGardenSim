#pragma once
#define ESP_SLEEP_WAKEUP_EXT0 3
#define ESP_SLEEP_WAKEUP_TIMER 4
#define GPIO_NUM_0 0
int esp_sleep_get_wakeup_cause(); void esp_sleep_enable_ext0_wakeup(int,int); void esp_sleep_enable_timer_wakeup(unsigned long long); void esp_sleep_disable_wakeup_source(int); void esp_deep_sleep_start(); void esp_light_sleep_start();
