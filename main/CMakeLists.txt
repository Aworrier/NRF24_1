idf_component_register(
    SRCS
        "app_main.c"
        "nrf24.c"
        "ble_ctrl.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_event
        esp_driver_gpio
        esp_driver_spi
<<<<<<< HEAD
        esp_netif
        esp_wifi
        nvs_flash
        lwip
=======
        bt
        nvs_flash
>>>>>>> 60d7e4e3087daa288b53c0249b3cc0d8a3e662e1
)
