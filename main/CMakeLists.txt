set(srcs
        "board.c")

idf_component_register(SRCS "main.c" "http_server.c" "lamp_nvs.c" "${srcs}"
                    INCLUDE_DIRS  ".")

set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/bluetooth/esp_ble_mesh/common_components/example_init
                         $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)
