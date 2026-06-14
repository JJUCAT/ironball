idf_component_register(
  # 源文件
  SRCS
    "src/wifi_manager.c" "src/wifi_config_server.c"
  # 公开包含
  INCLUDE_DIRS "include"
  # 私有包含
  PRIV_INCLUDE_DIRS "private_include"
  # 公开依赖
  REQUIRES
    esp_wifi nvs_flash esp_netif esp_event
    esp_http_server espressif__cjson
  # 文件嵌入，文件直接打包进固件，注意整个项目的嵌入文件不能同名
  EMBED_FILES
    www/wifi_manager_index.html
    www/wifi_manager_style.css
    www/wifi_manager_app.js
)
