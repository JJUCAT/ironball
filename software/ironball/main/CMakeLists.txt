idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "."
  REQUIRES espressif__console_cmd_wifi console
    esp_wifi nvs_flash esp_netif esp_event
)
