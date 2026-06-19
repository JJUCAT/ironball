/**
 * @copyright Copyright (c) {2022} LMR
 * @author LMR (lmr2887@163.com)
 * @date 2026-06-15
 * @brief WiFi 配置 HTTP 服务端
 */

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"

#include "wifi_manager.h"
#include "wifi_config_server.h"

/** 新配置已提交标志 */
static bool g_has_new_config = false;

/** 最大扫描 AP 数量 */
#define SCAN_AP_MAX 20

/** SoftAP 网关 IP（硬编码，与 esp_netif_create_default_wifi_ap() 默认值一致） */
#define AP_GATEWAY_IP "192.168.4.1"

/** DNS 服务器端口 */
#define DNS_PORT 53

// ====================================================================
// DNS 应答器：将 Kconfig 中定义的域名解析为 AP 网关 IP
// ====================================================================

/** DNS 报文头部 */
// DNS 客户端和 DNS 服务器之间交换的标准网络数据包格式（请求 + 响应）
// __attribute__((packed)) 禁止结构体对齐，紧凑排列
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/** DNS 应答中的资源记录 */
typedef struct __attribute__((packed)) {
    uint16_t name;
    uint16_t type;
    uint16_t dclass;
    uint32_t ttl;
    uint16_t rdlength;
    uint32_t rdata;
} dns_answer_t;

/** DNS 套接字句柄 */
static int g_dns_sock = -1;

/** 将点分十进制 IP 字符串转为 uint32（网络字节序） */
static uint32_t ip_to_u32(const char *ip_str)
{
    struct in_addr addr;
    inet_aton(ip_str, &addr);
    return addr.s_addr;
}

/** 单客户端应答：将域名对应的 A 记录写入 pkt，返回响应长度 */
static uint16_t dns_make_response(const uint8_t *query, uint16_t qlen,
                                   uint8_t *resp, uint16_t rlen)
{
    if (rlen < sizeof(dns_header_t) + sizeof(dns_answer_t)) return 0;

    const dns_header_t *req_hdr = (const dns_header_t *)query;
    dns_header_t *res_hdr = (dns_header_t *)resp;

    // 复制请求头，设置应答标志
    memcpy(res_hdr, req_hdr, sizeof(dns_header_t));
    res_hdr->flags = htons(0x8180); // QR=1, AA=1, 无错误
    res_hdr->ancount = htons(0);    // 暂设为 0，下面匹配到再改

    // 跳过头部，解析问题域
    const uint8_t *qname = query + sizeof(dns_header_t);
    uint8_t *resp_qname = resp + sizeof(dns_header_t);

    // 复制问题域（copy 到响应中，应答用指针引用）
    const uint8_t *p = qname;
    uint8_t *dst = resp_qname;
    while (*p) {
        uint8_t len = *p;
        *dst++ = len;
        p++;
        for (uint8_t i = 0; i < len; i++) *dst++ = *p++;
    }
    *dst++ = 0; // root terminator
    uint16_t qname_len = dst - resp_qname;

    // 检查问题类型是否为 A 记录
    const uint8_t *qtype_ptr = qname + qname_len;
    uint16_t qtype = (qtype_ptr[0] << 8) | qtype_ptr[1];
    // qclass 在 qtype_ptr+2

    if (qtype != 1) { // 不是 A 记录，返回无应答
        res_hdr->ancount = 0;
        return sizeof(dns_header_t) + qname_len + 4; // +4 for type+class
    }

    // 提取域名并匹配
    char domain[128] = {0};
    char *dp = domain;
    p = qname;
    while (*p) {
        uint8_t len = *p; p++;
        if (dp != domain) *dp++ = '.';
        for (uint8_t i = 0; i < len; i++) *dp++ = *p++;
    }
    *dp = '\0';

    ESP_LOGD(WIFI_MANAGER_TAG, "DNS 查询: %s", domain);

    if (strcasecmp(domain, CONFIG_WIFI_AP_DOMAIN) != 0) {
        res_hdr->ancount = htons(0);
        return sizeof(dns_header_t) + qname_len + 4;
    }

    // 匹配成功，追加 A 记录
    dns_answer_t *ans = (dns_answer_t *)(resp + sizeof(dns_header_t) + qname_len + 4);
    ans->name    = htons(0xc00c);
    ans->type    = htons(1);   // A
    ans->dclass  = htons(1);   // IN
    ans->ttl     = htonl(120); // 120s
    ans->rdlength = htons(4);
    ans->rdata   = ip_to_u32(AP_GATEWAY_IP);

    res_hdr->ancount = htons(1);
    return sizeof(dns_header_t) + qname_len + 4 + sizeof(dns_answer_t);
}

/**
 * DNS 服务器任务：监听 DNS 查询并应答 AP 网关 IP
 */
static void dns_server_task(void *pvParameters)
{
    ESP_LOGI(WIFI_MANAGER_TAG, "DNS server task start (port: %d)", DNS_PORT);

    // 创建 socket,ipv4 地址协议，SOCK_DGRAM 是 UDP 协议
    g_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (g_dns_sock < 0) {
        ESP_LOGE(WIFI_MANAGER_TAG, "create DNS socket FAILED");
        vTaskDelete(NULL);
        return;
    }

    // ip + 端口
    struct sockaddr_in addr = {
        .sin_family = AF_INET, // ipv4
        .sin_port   = htons(DNS_PORT), // DNS 端口
        .sin_addr   = { .s_addr = htonl(INADDR_ANY) }, // ip 地址 0.0.0.0，表示本机任意 ip 地址
    };

    // g_dns_sock 绑定本机任意 ip 地址下的 DNS 端口
    // 不管设备的 ip 怎么变化，端口 DNS_PORT 都能收到 DNS 查询
    if (bind(g_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(WIFI_MANAGER_TAG, "DNS bind socket FAILED");
        close(g_dns_sock);
        g_dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    uint8_t query[512];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (1) {
        // socket 接收 DNS 查询，from 端口地址
        int ret = recvfrom(g_dns_sock, query, sizeof(query), 0,
                           (struct sockaddr *)&from, &from_len);
        if (ret < 0) {
            if (g_dns_sock < 0) break; // 被关闭
            continue;
        }

        // 检查最小长度
        if (ret < (int)sizeof(dns_header_t) + 5) continue;

        uint8_t response[sizeof(dns_header_t) + 512];
        // 响应请求
        uint16_t rlen = dns_make_response(query, ret, response, sizeof(response));
        if (rlen == 0) continue;

        sendto(g_dns_sock, response, rlen, 0,
               (struct sockaddr *)&from, from_len);
    }

    vTaskDelete(NULL);
}

/** 启动 DNS 应答器 */
static void dns_server_start(void)
{
    BaseType_t ret = xTaskCreate(
        dns_server_task, "wifi_dns", 3072, NULL,
        tskIDLE_PRIORITY + 1, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(WIFI_MANAGER_TAG, "create DNS task FAILED");
    }
}

/** 停止 DNS 应答器 */
static void dns_server_stop(void)
{
    if (g_dns_sock >= 0) {
        close(g_dns_sock);
        g_dns_sock = -1;
        ESP_LOGI(WIFI_MANAGER_TAG, "DNS server stopped");
    }
}

// ====================================================================
// 嵌入的静态文件（通过 CMake EMBED_FILES 引入）
// ====================================================================
// asm 告诉编译器，前面的变量对应的连接器名是 asm() 括号内的名字
// 这里的 www_wifi_manager_index_html_start 变量不需要定义，由 CMakelist.txt 中 EMBED_FILES 将文件二进制化生成
extern const uint8_t www_wifi_manager_index_html_start[] asm("_binary_wifi_manager_index_html_start");
extern const uint8_t www_wifi_manager_index_html_end[]   asm("_binary_wifi_manager_index_html_end");
extern const uint8_t www_wifi_manager_style_css_start[]  asm("_binary_wifi_manager_style_css_start");
extern const uint8_t www_wifi_manager_style_css_end[]    asm("_binary_wifi_manager_style_css_end");
extern const uint8_t www_wifi_manager_app_js_start[]     asm("_binary_wifi_manager_app_js_start");
extern const uint8_t www_wifi_manager_app_js_end[]       asm("_binary_wifi_manager_app_js_end");

// ====================================================================
// JSON 辅助函数
// ====================================================================

/** 将 cJSON 树序列化为字符串并发送 HTTP 响应 */
static esp_err_t json_response(httpd_req_t *req, const cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (!str) {
        // http 响应类型，相当于 http 头
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        // 响应空数据，buf_len=2 是为了发送 "{}"
        return httpd_resp_send(req, "{}", 2);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/** 发送纯文本 JSON 字符串（用于小体积极简响应） */
static esp_err_t json_string_response(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, strlen(json));
}

// ====================================================================
// HTTP 处理器
// ====================================================================

/** 服务嵌入的静态文件 */
static esp_err_t serve_file(httpd_req_t *req,
                            const char *content_type,
                            const uint8_t *start,
                            const uint8_t *end)
{
    httpd_resp_set_type(req, content_type);
    return httpd_resp_send(req, (const char *)start, end - start);
}

// 响应客户端浏览器 html 文件请求，返回嵌入的 index.html
static esp_err_t index_get_handler(httpd_req_t *req)
{
    return serve_file(req, "text/html; charset=utf-8",
                      www_wifi_manager_index_html_start, www_wifi_manager_index_html_end);
}

// 响应客户端浏览器 css 文件请求，返回嵌入的 style.css
static esp_err_t style_get_handler(httpd_req_t *req)
{
    return serve_file(req, "text/css; charset=utf-8",
                      www_wifi_manager_style_css_start, www_wifi_manager_style_css_end);
}

// 响应客户端浏览器 js 文件请求，返回嵌入的 app.js
static esp_err_t script_get_handler(httpd_req_t *req)
{
    return serve_file(req, "application/javascript; charset=utf-8",
                      www_wifi_manager_app_js_start, www_wifi_manager_app_js_end);
}

/** GET /api/wifi-scan — 扫描附近 AP，返回 JSON 列表 */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    ESP_LOGI(WIFI_MANAGER_TAG, "start WiFi scan...");

    // 扫描配置：全信道主动扫描
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGE(WIFI_MANAGER_TAG, "WiFi scan FAILED: %s", esp_err_to_name(ret));
        json_string_response(req, "[]");
        return ESP_OK;
    }

    uint16_t count = SCAN_AP_MAX;
    wifi_ap_record_t records[SCAN_AP_MAX] = {0};
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&count, records));
    esp_wifi_clear_ap_list();

    ESP_LOGI(WIFI_MANAGER_TAG, "scan found %d APs", count);

    cJSON *root = cJSON_CreateArray();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    for (int i = 0; i < count; i++) {
        cJSON *ap = cJSON_CreateObject();
        if (!ap) continue;

        cJSON_AddStringToObject(ap, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);

        const char *auth_str = "OPEN";
        switch (records[i].authmode) {
            case WIFI_AUTH_WEP:          auth_str = "WEP";        break;
            case WIFI_AUTH_WPA_PSK:      auth_str = "WPA";        break;
            case WIFI_AUTH_WPA2_PSK:     auth_str = "WPA2";       break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA/WPA2";   break;
            case WIFI_AUTH_WPA3_PSK:     auth_str = "WPA3";       break;
            case WIFI_AUTH_WPA2_WPA3_PSK:auth_str = "WPA2/WPA3";  break;
            default: break;
        }
        cJSON_AddStringToObject(ap, "auth", auth_str);

        cJSON_AddItemToArray(root, ap);
    }

    ret = json_response(req, root);
    cJSON_Delete(root);
    return ret;
}

/** GET /api/wifi-status — 返回当前 STA 连接状态 JSON */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap_info = {0};
    esp_err_t sta_status = esp_wifi_sta_get_ap_info(&ap_info);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    if (sta_status == ESP_OK) {
        cJSON_AddBoolToObject(root, "connected", true);
        cJSON_AddStringToObject(root, "ssid", (const char *)ap_info.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
        cJSON_AddStringToObject(root, "status", "connected");
    } else {
        cJSON_AddBoolToObject(root, "connected", false);
        cJSON_AddStringToObject(root, "ssid", "");
        cJSON_AddNumberToObject(root, "rssi", 0);
        cJSON_AddStringToObject(root, "status",
            (sta_status == ESP_ERR_WIFI_NOT_CONNECT) ? "disconnected" : "connecting");
    }

    // 获取 STA IP（仅在 STA 模式下有效）
    char ip_str[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        }
    }
    cJSON_AddStringToObject(root, "ip", ip_str);

    esp_err_t ret = json_response(req, root);
    cJSON_Delete(root);
    return ret;
}

/** POST /api/wifi-config — 接收新的 WiFi 配置并保存 */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    // 读取请求 body
    char content[384];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        return json_string_response(req,
            "{\"status\":\"error\",\"message\":\"empty body\"}");
    }
    content[recv_len] = '\0';

    // 用 cJSON 解析
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        return json_string_response(req,
            "{\"status\":\"error\",\"message\":\"JSON parse error\"}");
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        return json_string_response(req,
            "{\"status\":\"error\",\"message\":\"SSID 是必填项\"}");
    }

    char ssid[33] = {0};
    char password[65] = {0};
    strlcpy(ssid, ssid_item->valuestring, sizeof(ssid));

    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(pass_item)) {
        strlcpy(password, pass_item->valuestring, sizeof(password));
    }
    cJSON_Delete(root);

    ESP_LOGI(WIFI_MANAGER_TAG, "receive new config: SSID=%s", ssid);

    // 保存到 NVS
    wifi_config_t cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi = -127,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, password, strlen(password));

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(WIFI_MANAGER_TAG, "set WiFi config FAILED: %s", esp_err_to_name(ret));
        cJSON *err_resp = cJSON_CreateObject();
        if (err_resp) {
            cJSON_AddStringToObject(err_resp, "status", "error");
            char msg[128];
            snprintf(msg, sizeof(msg), "save FAILED: %s", esp_err_to_name(ret));
            cJSON_AddStringToObject(err_resp, "message", msg);
            esp_err_t ret2 = json_response(req, err_resp);
            cJSON_Delete(err_resp);
            return ret2;
        }
        return json_string_response(req, "{\"status\":\"error\",\"message\":\"保存失败\"}");
    }

    // 持久化到 flash（storage 已在 init 时设为 FLASH）
    // 尝试连接
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(WIFI_MANAGER_TAG, "connect WiFi FAILED: %s", esp_err_to_name(ret));
    }

    g_has_new_config = true;
    ESP_LOGI(WIFI_MANAGER_TAG, "WiFi config saved, SSID=%s", ssid);

    return json_string_response(req,
        "{\"status\":\"ok\",\"message\":\"配置已保存\"}");
}

// ====================================================================
// 注册 URI
// ====================================================================

static const httpd_uri_t uris[] = {
    // 第一个就是 html
    { .uri = "/", .method = HTTP_GET, .handler = index_get_handler },
    { .uri = "/wifi_manager_style.css", .method = HTTP_GET, .handler = style_get_handler },
    { .uri = "/wifi_manager_app.js", .method = HTTP_GET, .handler = script_get_handler },
    { .uri = "/api/wifi-scan", .method = HTTP_GET, .handler = scan_get_handler },
    { .uri = "/api/wifi-status", .method = HTTP_GET, .handler = status_get_handler },
    { .uri = "/api/wifi-config", .method = HTTP_POST, .handler = config_post_handler },
};
static const int uri_count = sizeof(uris) / sizeof(uris[0]);

// ====================================================================
// 公开 API
// ====================================================================

esp_err_t wifi_config_server_start(httpd_handle_t *handle)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = uri_count + 2; // 为一些插件隐式注册的 URI 留出空间
    config.lru_purge_enable = true; // 超最大 TCP 连接数目时候，自动关闭最久未使用的连接
    config.stack_size = 4096;

    esp_err_t ret = httpd_start(handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(WIFI_MANAGER_TAG, "start HTTP server FAILED: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < uri_count; i++) {
        // 注册 uri 资源，当 HTTP 请求匹配到对应 URI 和 method 时，调用 handler 处理
        ret = httpd_register_uri_handler(*handle, &uris[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(WIFI_MANAGER_TAG, "register URI %s FAILED: %s",
                     uris[i].uri, esp_err_to_name(ret));
            httpd_stop(*handle);
            return ret;
        }
    }

    ESP_LOGI(WIFI_MANAGER_TAG, "WiFi config server started (http://%s / http://%s)",
             AP_GATEWAY_IP, CONFIG_WIFI_AP_DOMAIN);
    dns_server_start();
    return ESP_OK;
}

esp_err_t wifi_config_server_stop(httpd_handle_t handle)
{
    dns_server_stop();
    if (handle) {
        httpd_stop(handle);
        ESP_LOGI(WIFI_MANAGER_TAG, "WiFi config server stopped");
    }
    return ESP_OK;
}

bool wifi_config_has_new_config(void)
{
    bool ret = g_has_new_config;
    g_has_new_config = false;  // 一次性读取
    return ret;
}
