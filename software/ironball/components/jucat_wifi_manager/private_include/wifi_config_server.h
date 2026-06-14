/**
 * @copyright Copyright (c) {2022} LMR
 * @author LMR (17688010148@163.com)
 * @date 2026-06-13
 * @brief WiFi 配置 HTTP 服务器
 */

#ifndef _JUCAT_WIFI_CONFIG_SERVER_H_
#define _JUCAT_WIFI_CONFIG_SERVER_H_

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 WiFi 配置 HTTP 服务器
 * @param[out] handle 输出服务器句柄，用于后续停止
 * @return ESP_OK 成功，否则失败
 */
esp_err_t wifi_config_server_start(httpd_handle_t *handle);

/**
 * @brief 停止 WiFi 配置 HTTP 服务器
 * @param handle 服务器句柄
 * @return ESP_OK 成功，否则失败
 */
esp_err_t wifi_config_server_stop(httpd_handle_t handle);

/**
 * @brief 检查是否有新的 WiFi 配置已提交
 * @return true 用户已提交新配置
 */
bool wifi_config_has_new_config(void);

#ifdef __cplusplus
}
#endif

#endif /* _JUCAT_WIFI_CONFIG_SERVER_H_ */
