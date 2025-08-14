idf_component_register(
    SRCS 
    "station_example_main.c"
    "mcp_websocket.c"
    "mcp_server.c"
    "mcp_sensor.c"

    PRIV_REQUIRES esp_wifi nvs_flash esp_timer json tcp_transport

    INCLUDE_DIRS ".")
