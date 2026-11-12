# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS main.c
  PRIV_REQUIRES tiny_edge esp_wifi esp_netif esp_event nvs_flash console
    esp_app_format)
