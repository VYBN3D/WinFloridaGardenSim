#pragma once
#include <Arduino.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_ESPNOW_NOT_INIT -2
#define ESP_ERR_ESPNOW_EXIST 3
#define ESP_ERR_ESPNOW_NOT_FOUND 4
#define WIFI_IF_STA 1
typedef int esp_err_t;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
typedef struct esp_now_recv_info { const uint8_t *src_addr; const uint8_t *des_addr; } esp_now_recv_info_t;
typedef struct esp_now_peer_info { uint8_t peer_addr[6]; uint8_t channel; int ifidx; bool encrypt; } esp_now_peer_info_t;
int esp_now_init(); int esp_now_deinit(); int esp_now_register_recv_cb(void (*)(const esp_now_recv_info_t*,const uint8_t*,int)); int esp_now_send(const uint8_t*,const uint8_t*,size_t); bool esp_now_is_peer_exist(const uint8_t*); int esp_now_add_peer(const esp_now_peer_info_t*);
