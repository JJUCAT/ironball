idf_component_register(
  SRCS "wifi_manager.c" "wifi_config_server.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_http_server espressif__cjson
  EMBED_FILES www/index.html www/style.css www/app.js)
