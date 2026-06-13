#include <stdio.h>
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "console_wifi.h"
#include "esp_log.h"
#include "logger.h"
#include "wifi_manager.h"


void app_main(void)
{
    // 日志 LOGGER_SYS 用 INFO 等级输出
    ESP_LOGI(LOGGER_SYS, "Ironball! ROLLING!");
    // 编译器会根据平台将 PRIu32 展开为 u，lu，I32u 等
    ESP_LOGI(LOGGER_SYS, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(LOGGER_SYS, "[APP] IDF version: %s", esp_get_idf_version());
    // 所有日志都是 INFO 等级，个别日志 "websocket_client"，"transport_ws"，"trans_tcp" 单独设置日志等级
    esp_log_level_set("*", ESP_LOG_INFO);

    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // wifi 管理器初始化
    ESP_ERROR_CHECK(wifi_manager_init());

    // 终端交互指令
    ESP_ERROR_CHECK(console_cmd_init());
    // 注册 wifi 交互指令
    ESP_ERROR_CHECK(console_cmd_wifi_register());
    // 启动终端交互
    ESP_ERROR_CHECK(console_cmd_start());

}
