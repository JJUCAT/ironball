/**
 * @copyright Copyright (c) {2022} LMR
 * @author LMR (17688010148@163.com)
 * @date 2026-06-13
 * @brief
 */


#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_manager.h"
#include "wifi_config_server.h"


// WiFi 监控任务栈大小
#define WIFI_MONITOR_TASK_STACK_SIZE 4096
// WiFi 监控任务优先级
#define WIFI_MONITOR_TASK_PRIORITY 5
// WiFi 监控任务轮询间隔 (ms)
#define WIFI_MONITOR_TASK_INTERVAL_MS 1000

// WiFi 监控任务句柄
static TaskHandle_t s_wifi_monitor_task_handle = NULL;

// 断开重联检查次数
const int kWifiReconnectMaxChecks = 3;
// WiFi 配置网页任务是否正在运行的全局标志
bool gWifiWebTaskRunning = false; 

// WiFi 配置网页任务栈大小
#define WIFI_WEB_TASK_STACK_SIZE 6144
// WiFi 配置网页任务优先级
#define WIFI_WEB_TASK_PRIORITY 5
// WiFi 配置网页任务轮询间隔 (ms)
#define WIFI_WEB_TASK_INTERVAL_MS 1000



// ---------- 声明 ----------

/**
 * @brief 监控 wifi 状态任务
 * @param pvParameters 任务参数（暂未使用）
 */
void wifi_monitor_task(void *pvParameters);


/**
 * @brief WiFi 配置网页任务
 * @param  pvParameters 任务参数（暂未使用）
 */
void wifi_web_task(void *pvParameters);


// ---------- 定义 ----------

esp_err_t wifi_manager_init()
{
    // ESP_ERROR_CHECK 是 ESP 的异常检查，出现异常输出日志并推出程序
    // 初始化 NVS 存储器，wifi 驱动依赖 NVS 存储
    ESP_ERROR_CHECK(nvs_flash_init());
    // 创建网络接口管理器
    ESP_ERROR_CHECK(esp_netif_init());


    // 初始化 wifi
    // 绑定 netif 网络接口
    esp_netif_create_default_wifi_sta();

    // WIFI_INIT_CONFIG_DEFAULT 是初始化 wifi 驱动配置的结构体数据
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    // 初始化 wifi 驱动配置
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    // wifi_init_config_t 存储在 nvs
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    // STA 站点模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_MANAGER_TAG, "finished wifi config");


    // 创建 WiFi 监控任务
    BaseType_t ret = xTaskCreate(
        wifi_monitor_task, // 任务函数
        "wifi_monitor", // 任务名称
        WIFI_MONITOR_TASK_STACK_SIZE, // 栈深度
        NULL, // 任务参数
        WIFI_MONITOR_TASK_PRIORITY, // 优先级
        &s_wifi_monitor_task_handle // 任务句柄
    );
    if (ret != pdPASS) {
        ESP_LOGE(WIFI_MANAGER_TAG, "创建 wifi_monitor_task 失败");
    }

    return ESP_OK;
}


void wifi_monitor_task(void *pvParameters)
{
    ESP_LOGI(WIFI_MANAGER_TAG, "WiFi 监控任务已启动");

    uint32_t last_reconnect_check = 0;

    while (1) {
        wifi_ap_record_t ap_info = {0};
        esp_err_t status = esp_wifi_sta_get_ap_info(&ap_info);

        if (status == ESP_OK) {
            // 已连接
            last_reconnect_check = 0;
            ESP_LOGD(WIFI_MANAGER_TAG, "WiFi 已连接, rssi=%d", ap_info.rssi);
        } else if (status == ESP_ERR_WIFI_NOT_CONNECT) {
            // wifi 配置期间不进行重连检查，等待配置完成后自动连接
            if (gWifiWebTaskRunning) {
                ESP_LOGW(WIFI_MANAGER_TAG, "wifi_web_task 已在运行，跳过重复创建");
                last_reconnect_check = 0;
                continue;
            }

            ESP_LOGW(WIFI_MANAGER_TAG, "WiFi 连接断开，等待下次检查");
            last_reconnect_check++;
            if (last_reconnect_check >= kWifiReconnectMaxChecks) {
                ESP_LOGW(WIFI_MANAGER_TAG, "尝试重新连接 WiFi...");

                // 检查是否配置过 wifi
                wifi_config_t cfg = {0};
                esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
                if (err == ESP_OK && strlen((char*)cfg.sta.ssid) > 0) {
                    ESP_LOGI(WIFI_MANAGER_TAG, "已有 WiFi: %s", cfg.sta.ssid);
                    esp_wifi_set_config(WIFI_IF_STA, &cfg);
                    esp_wifi_connect();
                }
                // 未配置过 wifi，需要开启网页端配置
                else {
                    ESP_LOGW(WIFI_MANAGER_TAG, "未配置 WiFi，启动网页配置...");
                    gWifiWebTaskRunning = true;

                    // 创建 WiFi 配置网页任务
                    BaseType_t ret = xTaskCreate(
                        wifi_web_task,         // 任务函数
                        "wifi_web_config",     // 任务名称
                        WIFI_WEB_TASK_STACK_SIZE, // 栈深度
                        NULL,                  // 任务参数
                        WIFI_WEB_TASK_PRIORITY, // 优先级
                        NULL                   // 不需要保存句柄
                    );
                    if (ret != pdPASS) {
                        ESP_LOGE(WIFI_MANAGER_TAG, "创建 wifi_web_task 失败");
                        gWifiWebTaskRunning = false;
                    } else {
                        // 等待配置任务完成（配置任务内部会自行退出）
                        ESP_LOGI(WIFI_MANAGER_TAG, "wifi_web_task 已创建，等待配置完成");
                    }
                }
                last_reconnect_check = 0;
            }
        } else {
            ESP_LOGD(WIFI_MANAGER_TAG, "WiFi 状态未知 (ret=%d)", status);
        }

        // 轮询间隔
        vTaskDelay(pdMS_TO_TICKS(WIFI_MONITOR_TASK_INTERVAL_MS));
    }

    // 正常情况下不会到达这里
    vTaskDelete(NULL);
}


void wifi_web_task(void *pvParameters)
{
    ESP_LOGI(WIFI_MANAGER_TAG, "WiFi 网页配置任务已启动");

    httpd_handle_t server_handle = NULL;

    // 1. 停止当前 WiFi（STA 模式）
    ESP_ERROR_CHECK(esp_wifi_stop());

    // 2. 创建 SoftAP netif
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    // 3. 设置为 APSTA 模式（同时支持 AP 和 STA）
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // 4. 配置 SoftAP
    wifi_config_t ap_config = {
        .ap = {
            .ssid = CONFIG_WIFI_AP_SSID,
            .password = CONFIG_WIFI_AP_PASSWORD,
            .ssid_len = 0, // 自动计算 strlen
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = false,
            .max_connection = CONFIG_WIFI_AP_MAX_CONNECT,
            .beacon_interval = 100,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // 5. 启动 WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_MANAGER_TAG, "AP 模式已启动，SSID: %s",
             CONFIG_WIFI_AP_SSID);

    // 6. 启动 HTTP 配置服务器
    if (wifi_config_server_start(&server_handle) != ESP_OK) {
        ESP_LOGE(WIFI_MANAGER_TAG, "启动配置服务器失败");
        esp_wifi_stop();
        esp_netif_destroy(ap_netif);
        gWifiWebTaskRunning = false;
        vTaskDelete(NULL);
        return;
    }

    // 7. 等待用户完成配置（轮询标志，不超时）
    ESP_LOGI(WIFI_MANAGER_TAG, "等待用户配置 WiFi...");

    while (1) {
        if (wifi_config_has_new_config()) {
            ESP_LOGI(WIFI_MANAGER_TAG, "检测到新配置，退出配置模式");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_WEB_TASK_INTERVAL_MS));
    }

    // 8. 停止 HTTP 服务器
    wifi_config_server_stop(server_handle);

    // 9. 删除 AP netif（但不停止 STA 模式，让上层继续尝试连接）
    esp_netif_destroy_default_wifi(ap_netif);

    // 10. 重新设置 STA 模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(WIFI_MANAGER_TAG, "WiFi 网页配置任务结束");
    gWifiWebTaskRunning = false;
    vTaskDelete(NULL);
}
