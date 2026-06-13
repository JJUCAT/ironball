/**
 * @copyright Copyright (c) {2022} LMR
 * @author LMR (17688010148@163.com)
 * @date 2026-06-13
 * @brief 
 */



#ifndef _JUCAT_WIFI_MANAGER_H_
#define _JUCAT_WIFI_MANAGER_H_


#ifdef __cplusplus
extern "C" {
#endif





// 日志标签
static const char *WIFI_MANAGER_TAG = "jucat_wifi_manager";



/**
 * @brief 初始化 WiFi 管理器
 * @details 会执行 nvs_flash_init 和 esp_netif_init 等必要的初始化操作
 */
esp_err_t wifi_manager_init();




#ifdef __cplusplus
}
#endif

#endif // _JUCAT_WIFI_MANAGER_H_
