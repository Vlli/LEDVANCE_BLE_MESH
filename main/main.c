/* main.c - Application main entry point */

/*
 * SPDX-FileCopyrightText: 2017 Intel Corporation
 * SPDX-FileContributor: 2018-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "cJSON.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_lighting_model_api.h"

#include "board.h"
#include "ble_mesh_example_init.h"
#include "ble_mesh_example_nvs.h"

//wifi librairies
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"

//mqtt
//#include "protocol_examples_common.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt_client.h"

#include "esp_http_server.h"

#include "lamp_nvs.h"
#include "http_server.h"

// Define web server URI
#define EXAMPLE_URI "/control"
// Define the maximum number of lamps
#define MAX_LAMPS 20

#define ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

#define LIVING_LAMP 0x0013
#define OFFICE_LAMP 0x0014
#define FLUR_LAMP 0x0015

// Global variable to store the current number of lamps
int g_num_lamps = 0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

#define TAG "LIGHT"

#define CID_ESP 0x02E5

static uint8_t dev_uuid[16] = { 0xcc, 0xcc };

esp_mqtt_client_handle_t mqtt_client;

static struct example_info_store {
    // Mesh-Netzwerk Parameter
    uint16_t net_idx;    /* NetKey Index */
    uint16_t app_idx;    /* AppKey Index */
    
    // Gerätestatus
    uint8_t  onoff;      /* Remote OnOff */
    uint8_t  tid;        /* Message TID */
    
    // Lichtwerte
    float    hue;        /* 0.0-360.0 Grad */
    float    saturation; /* 0.0-100.0 % */
    float    lightness;  /* 0.0-100.0 % (ersetzt brightness) */
} __attribute__((packed)) store = {
    .net_idx = ESP_BLE_MESH_KEY_UNUSED,
    .app_idx = ESP_BLE_MESH_KEY_UNUSED,
    .onoff = LED_OFF,
    .tid = 0x0,
    .hue = 0.0f,
    .saturation = 0.0f,
    .lightness = 0.0f,  // Früher 'brightness'
};

static nvs_handle_t NVS_HANDLE;
static const char * NVS_KEY = "onoff_client";

static esp_ble_mesh_client_t onoff_client;
static esp_ble_mesh_client_t level_client;
static esp_ble_mesh_client_t light_client;
static esp_ble_mesh_client_t hsl_client;

static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
    .default_ttl = 10,
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(4, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(4, 20),
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_cli_pub, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(level_cli_pub, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(light_cli_pub, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(hsl_cli_pub, 2 + 1, ROLE_NODE);

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_cli_pub, &onoff_client),
    ESP_BLE_MESH_MODEL_GEN_LEVEL_CLI(&level_cli_pub, &level_client),
    ESP_BLE_MESH_MODEL_LIGHT_LIGHTNESS_CLI(&light_cli_pub, &light_client),
    ESP_BLE_MESH_MODEL_LIGHT_HSL_CLI(&hsl_cli_pub, &hsl_client), // <--- NEU
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

/* Disable OOB security for SILabs Android app */
static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
#if 0
    .output_size = 4,
    .output_actions = ESP_BLE_MESH_DISPLAY_NUMBER,
    .input_actions = ESP_BLE_MESH_PUSH,
    .input_size = 4,
#else
    .output_size = 0,
    .output_actions = 0,
#endif
};

static void mesh_info_store(void)
{
    ble_mesh_nvs_store(NVS_HANDLE, NVS_KEY, &store, sizeof(store));
}

static void mesh_info_restore(void)
{
    esp_err_t err = ESP_OK;
    bool exist = false;

    err = ble_mesh_nvs_restore(NVS_HANDLE, NVS_KEY, &store, sizeof(store), &exist);
    if (err != ESP_OK) {
        return;
    }

    if (exist) {
        ESP_LOGI(TAG, "Restore, net_idx 0x%04x, app_idx 0x%04x, onoff %u, tid 0x%02x, brightness %f",
            store.net_idx, store.app_idx, store.onoff, store.tid, store.lightness);
    }
}

static void prov_complete(uint16_t net_idx, uint16_t addr, uint8_t flags, uint32_t iv_index)
{
    ESP_LOGI(TAG, "net_idx: 0x%04x, addr: 0x%04x", net_idx, addr);
    ESP_LOGI(TAG, "flags: 0x%02x, iv_index: 0x%08" PRIx32, flags, iv_index);
    board_led_operation(GPIO_NUM_2, 0);
    ESP_LOGW(TAG, "LEDs_OFF provisioning completed");
    store.net_idx = net_idx;
    /* mesh_info_store() shall not be invoked here, because if the device
     * is restarted and goes into a provisioned state, then the following events
     * will come:
     * 1st: ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT
     * 2nd: ESP_BLE_MESH_PROV_REGISTER_COMP_EVT
     * So the store.net_idx will be updated here, and if we store the mesh example
     * info here, the wrong app_idx (initialized with 0xFFFF) will be stored in nvs
     * just before restoring it.
     */
}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d", param->prov_register_comp.err_code);
        mesh_info_restore(); /* Restore proper mesh info */
        break;
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT, err_code %d", param->node_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT, bearer %s",
            param->node_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT, bearer %s",
            param->node_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT");
        prov_complete(param->node_prov_complete.net_idx, param->node_prov_complete.addr,
            param->node_prov_complete.flags, param->node_prov_complete.iv_index);
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        break;
    case ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT, err_code %d", param->node_set_unprov_dev_name_comp.err_code);
        break;
    default:
        break;
    }
}

void ble_mesh_get_gen_onoff_status(uint16_t a_addr)
{
    esp_ble_mesh_generic_client_get_state_t get = {0};
    esp_ble_mesh_client_common_param_t common = {0};
    esp_err_t err = ESP_OK;

    common.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET;
    common.model = onoff_client.model;
    common.ctx.net_idx = store.net_idx;
    common.ctx.app_idx = store.app_idx;
    common.ctx.addr = a_addr;   /* Address of the server whose status is requested */
    common.ctx.send_ttl = 10;
    common.ctx.send_rel = true;
    common.msg_timeout = 0;     /* 0 indicates that timeout value from menuconfig will be used */
    common.msg_role = ROLE_NODE;

    err = esp_ble_mesh_generic_client_get_state(&common, &get);
    if (err) {
        ESP_LOGE(TAG, "Get Generic OnOff State failed");
        return;
    }

    // Handle the response in the callback function registered for the Generic OnOff Client model.
}

void ble_mesh_send_gen_onoff_set(int a_state, uint16_t a_addr)
{
    esp_ble_mesh_generic_client_set_state_t set = {0};
    esp_ble_mesh_client_common_param_t common = {0};
    esp_err_t err = ESP_OK;

    common.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET;
    common.model = onoff_client.model;
    common.ctx.net_idx = store.net_idx;
    common.ctx.app_idx = store.app_idx;
    common.ctx.addr = a_addr;   /* to all nodes = 0xFFFF */
    common.ctx.send_ttl = 10;
    common.ctx.send_rel = true;
    common.msg_timeout = 0;     /* 0 indicates that timeout value from menuconfig will be used */
    common.msg_role = ROLE_NODE;

    set.onoff_set.op_en = false;
    set.onoff_set.onoff = a_state;
    set.onoff_set.tid = store.tid++;

    err = esp_ble_mesh_generic_client_set_state(&common, &set);
    if (err) {
        ESP_LOGE(TAG, "Send Generic OnOff Set Unack failed");
        return;
    }
}

void ble_mesh_send_gen_brightness_set(int a_brightness, uint16_t a_addr, esp_mqtt_client_handle_t a_client, char* a_topic)
{
    esp_ble_mesh_light_client_set_state_t set = {0};
    esp_ble_mesh_client_common_param_t common = {0};
    esp_err_t err = ESP_OK;

    cJSON *root = cJSON_CreateObject();

    common.opcode = ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK;
    common.model = light_client.model;
    common.ctx.net_idx = store.net_idx;
    common.ctx.app_idx = store.app_idx;
    common.ctx.addr = a_addr;   /* to all nodes= 0xFFFF*/
    common.ctx.send_ttl = 10;
    common.ctx.send_rel = true;
    common.msg_timeout = 0;     /* 0 indicates that timeout value from menuconfig will be used */
    common.msg_role = ROLE_NODE;

    set.lightness_set.op_en = false;
    set.lightness_set.lightness = (uint16_t)(a_brightness * 65535.0 / 100.0);
    set.lightness_set.tid = store.tid++;

    err = esp_ble_mesh_light_client_set_state(&common, &set);
    if (err) {
        ESP_LOGE(TAG, "Send Light Lightness Set Unack failed");
        return;
    }
    //ble_mesh_get_gen_onoff_status(a_addr);
    // build JSON for home assistant
    cJSON_AddItemToObject(root, "state", cJSON_CreateString("ON"));
    cJSON_AddItemToObject(root, "brightness", cJSON_CreateNumber(a_brightness));
    char *string = cJSON_PrintUnformatted(root);
    //printf("JSON: %s\n", string);
    esp_mqtt_client_publish(a_client, a_topic, string, 0, 0, 0);
    ESP_LOGI(TAG, "Set brightness successful %d", a_brightness);

    store.lightness = a_brightness;
   // store.onoff = !store.onoff;
    mesh_info_store(); /* Store proper mesh info */
}

void ble_mesh_send_gen_hsl_set(float hsl_hue, float hsl_saturation, float hsl_lightness, uint16_t a_addr, esp_mqtt_client_handle_t a_client, char* a_topic)
{
    esp_ble_mesh_light_client_set_state_t set = {0};
    esp_ble_mesh_client_common_param_t common = {0};
    esp_err_t err = ESP_OK;

    // 1. Werteskalierung in BLE-Mesh-Format
    uint16_t hsl_hue_scaled = (uint16_t)(hsl_hue * 65535.0 / 360.0);
    uint16_t hsl_saturation_scaled = (uint16_t)(hsl_saturation* 65535.0 / 100.0);
    uint16_t hsl_lightness_scaled = (uint16_t)(hsl_lightness * 65535.0 / 100.0 / 2);
    ESP_LOGI(TAG, "Values to lamp: Hue: %d Saturation: %d Lightness: %d", hsl_hue_scaled, hsl_saturation_scaled, hsl_lightness_scaled);
    set.hsl_set.hsl_hue = hsl_hue_scaled;
    set.hsl_set.hsl_saturation = hsl_saturation_scaled;
    set.hsl_set.hsl_lightness = hsl_lightness_scaled;

    // 2. Parameter validieren
    if (hsl_hue < 0 || hsl_hue > 360 || 
        hsl_saturation < 0 || hsl_saturation > 100 ||
        hsl_lightness < 0 || hsl_lightness > 100) {
        ESP_LOGE(TAG, "Invalid HSL values: H=%.1f S=%.1f L=%.1f", 
                hsl_hue, hsl_saturation, hsl_lightness);
        return;
    }

    // 3. BLE-Mesh-Konfiguration (wie ursprünglich)
    common.opcode = ESP_BLE_MESH_MODEL_OP_LIGHT_HSL_SET_UNACK;
    common.model = hsl_client.model;
    common.ctx.net_idx = store.net_idx;
    common.ctx.app_idx = store.app_idx;
    common.ctx.addr = a_addr;
    common.ctx.send_ttl = 10;
    common.ctx.send_rel = true;
    common.msg_timeout = 0;
    common.msg_role = ROLE_NODE;

    set.hsl_set.tid = store.tid++;

    // 4. Befehl senden
    ESP_LOGI(TAG, "Try to set HSL");
    err = esp_ble_mesh_light_client_set_state(&common, &set);
    if (err) {
        ESP_LOGE(TAG, "Send Light HSL Set Unack failed");
        return;
    }

    // 5. MQTT-Status mit Original-Float-Werten
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "state", cJSON_CreateString("ON"));
    cJSON_AddItemToObject(root, "color", cJSON_CreateObject());
    cJSON_AddNumberToObject(cJSON_GetObjectItem(root, "color"), "h", hsl_hue);
    cJSON_AddNumberToObject(cJSON_GetObjectItem(root, "color"), "s", hsl_saturation);
    cJSON_AddNumberToObject(root, "lightness", hsl_lightness);
    
    char *string = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(a_client, a_topic, string, 0, 0, 0);
    cJSON_Delete(root);

    // 6. Speicherung als Float (für spätere Abfragen)
    store.hue = hsl_hue;
    store.saturation = hsl_saturation;
    store.lightness = hsl_lightness;
    mesh_info_store();
}

static void example_ble_mesh_generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                                               esp_ble_mesh_generic_client_cb_param_t *param)
{
    ESP_LOGI(TAG, "Generic client, event %u, error code %d, opcode is 0x%04" PRIx32,
        event, param->error_code, param->params->opcode);

    switch (event) {
    case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT");
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET) {
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET, onoff %d", param->status_cb.onoff_status.present_onoff);
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT");
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET, onoff %d", param->status_cb.onoff_status.present_onoff);
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT");
        if (param->params->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS, onoff %d", param->status_cb.onoff_status.present_onoff);
            // Get the address of the device that sent the response
            uint16_t sender_addr = param->params->ctx.addr;
            cJSON *root = cJSON_CreateObject();
            // Extract and handle the response as needed
            uint8_t onoff_state = param->status_cb.onoff_status.present_onoff;
            ESP_LOGI(TAG, "Received Generic OnOff Get response from device 0x%X. OnOff State: %d", sender_addr, onoff_state);

            cJSON_AddItemToObject(root, "state", cJSON_CreateNumber(onoff_state));
            
            char topic_state[100];
            char *ha_topic = "test";
            bool found = false;

            // Iterate through all lamps in NVS
            for (int i = 0; i < MAX_LAMPS; i++) {
                LampInfo lamp_info;
                esp_err_t err = load_lamp_info(&lamp_info, i);
                if (err == ESP_OK) {
                    // Compare lamp address with sender address
                    if (lamp_info.address == sender_addr) {
                        // Match found, set appropriate topic
                        snprintf(topic_state, sizeof(topic_state), "homeassistant/light/%s/state", lamp_info.name);
                        ha_topic = topic_state;
                        found = true;
                        break;  // Exit loop once a match is found
                    }
                }
            }

            if (!found) {
                // Unknown sender address or no matching lamp found
                ESP_LOGW(TAG, "Received Generic OnOff Get response from unknown device");
                return;
            }
            
            // Access the global MQTT client instance
            if (mqtt_client == NULL) {
                ESP_LOGE(TAG, "MQTT client not initialized!");
                return;
            }
            char *string = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            esp_mqtt_client_publish(mqtt_client, ha_topic, string, 0, 0, 0);
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT");
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
            /* If failed to get the response of Generic OnOff Set, resend Generic OnOff Set  */
            //ble_mesh_send_gen_onoff_set(store.onoff, 0xFFFF);
        }
        break;
    default:
        break;
    }
}

static void example_ble_mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                              esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        switch (param->ctx.recv_op) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD");
            ESP_LOGI(TAG, "net_idx 0x%04x, app_idx 0x%04x",
                param->value.state_change.appkey_add.net_idx,
                param->value.state_change.appkey_add.app_idx);
            ESP_LOG_BUFFER_HEX("AppKey", param->value.state_change.appkey_add.app_key, 16);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND");
            ESP_LOGI(TAG, "elem_addr 0x%04x, app_idx 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_app_bind.element_addr,
                param->value.state_change.mod_app_bind.app_idx,
                param->value.state_change.mod_app_bind.company_id,
                param->value.state_change.mod_app_bind.model_id);
            if (param->value.state_change.mod_app_bind.company_id == 0xFFFF &&
                param->value.state_change.mod_app_bind.model_id == ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI) {
                store.app_idx = param->value.state_change.mod_app_bind.app_idx;
                mesh_info_store(); /* Store proper mesh info */
            }
            break;
        default:
            break;
        }
    }
}

static esp_err_t ble_mesh_init(void)
{
    esp_err_t err = ESP_OK;

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_generic_client_callback(example_ble_mesh_generic_client_cb);
    esp_ble_mesh_register_config_server_callback(example_ble_mesh_config_server_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mesh stack (err %d)", err);
        return err;
    }

    err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable mesh node (err %d)", err);
        return err;
    }

    ESP_LOGI(TAG, "BLE Mesh Node initialized");

    board_led_operation(2, LED_OFF);

    return err;
}

typedef struct {
    const char *topic;
    char *payload; // Dynamic payload
} MqttMessage;

// Function to create MQTT payload dynamically using cJSON
char *createPayload(const char *variable1, const char *variable2, const char *variable3) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        fprintf(stderr, "Failed to create cJSON object\n");
        exit(EXIT_FAILURE);
    }

    // Grundlegende Light-Konfiguration
    cJSON_AddItemToObject(root, "name", cJSON_CreateString(variable1));
    cJSON_AddItemToObject(root, "~", cJSON_CreateString(variable2));
    cJSON_AddItemToObject(root, "cmd_t", cJSON_CreateString("~/set"));
    cJSON_AddItemToObject(root, "stat_t", cJSON_CreateString("~/state"));
    cJSON_AddItemToObject(root, "schema", cJSON_CreateString("json"));
    
    // Farb- und Helligkeitseinstellungen
    cJSON_AddItemToObject(root, "brightness", cJSON_CreateBool(true));
    cJSON_AddItemToObject(root, "color_mode", cJSON_CreateBool(true));
    cJSON_AddItemToObject(root, "bri_scl", cJSON_CreateNumber(100));  // 0-100% Skalierung
    
    // Unterstützte Farbmodi (HSL)
    cJSON *color_modes = cJSON_CreateArray();
    cJSON_AddItemToArray(color_modes, cJSON_CreateString("hs"));
    cJSON_AddItemToObject(root, "supported_color_modes", color_modes);

    // Payload-Vorlagen
    cJSON_AddItemToObject(root, "pl_on", cJSON_CreateString("ON"));
    cJSON_AddItemToObject(root, "pl_off", cJSON_CreateString("OFF"));
    
    // Geräteinformationen
    cJSON *dev = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "dev", dev);
    
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(variable3));
    cJSON_AddItemToObject(dev, "ids", ids);
    cJSON_AddItemToObject(dev, "name", cJSON_CreateString("Lamp"));
    cJSON_AddItemToObject(dev, "mf", cJSON_CreateString("BLE-Mesh"));
    cJSON_AddItemToObject(dev, "mdl", cJSON_CreateString("HSL-Light"));

    // Unique ID für Home Assistant
    cJSON_AddItemToObject(root, "uniq_id", cJSON_CreateString(variable3));

    char *string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return string;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "Event dispatched from event loop" );
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    mqtt_client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, "homeassistant/status", 0);
            ESP_LOGI(TAG, "Subscribed to homeassistant/status, msg_id=%d", msg_id);
            // Load lamp names from NVS and subscribe to corresponding MQTT topics
            for (int i = 0; i < MAX_LAMPS; i++) {
                LampInfo lamp_info;
                esp_err_t err = load_lamp_info(&lamp_info, i);
                if (err == ESP_OK) {
                    char topic[100];
                    snprintf(topic, sizeof(topic), "homeassistant/light/%s/set", lamp_info.name);
                    int msg_id = esp_mqtt_client_subscribe(client, topic, 0);
                    ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", topic, msg_id);
                }
            }
            esp_mqtt_client_publish(client, "homeassistant/status", "", 0, 0, 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
                            
            bool setMessage = 0;
            uint16_t net_addr = 0xFFFF;
            char *ha_topic = "abc";
            ESP_LOGI(TAG, "Found lamps %d", g_num_lamps);
            // Iterate through all lamps in NVS
            for (int i = 0; i < MAX_LAMPS; i++) {
                ESP_LOGI(TAG, "Found lamps %d", i);
                LampInfo lamp_info;
                esp_err_t err = load_lamp_info(&lamp_info, i);
                if (err == ESP_OK) {
                    // Dynamically generate MQTT topics based on lamp names
                    char topic_set[100];
                    snprintf(topic_set, sizeof(topic_set), "homeassistant/light/%s/set", lamp_info.name);

                    // Compare MQTT topic with dynamically generated topics
                    if (strncmp(topic_set, event->topic, strlen(topic_set)) == 0) {
                        // Match found, set appropriate values
                        char *endptr;
                        net_addr = (uint16_t)strtol(lamp_info.address, &endptr, 0);
                        if (*endptr != '\0') {
                            // Handle conversion error
                            ESP_LOGE("TAG", "Failed to convert lamp_info.address to integer");
                            // Optionally, you can set a default value for net_addr or handle the error in another way
                        }
                        char topic_state[100];
                        snprintf(topic_state, sizeof(topic_state), "homeassistant/light/%s/state", lamp_info.name);
                        ha_topic = topic_state;
                        setMessage = true;
                        ESP_LOGI(TAG, "Found msg to %s, %s, %d", lamp_info.name, topic_state, net_addr);
                        break;  // Exit loop once a match is found
                    }
                }
            }
            
           if (setMessage){
                //parse received json data
                ESP_LOGI(TAG, "Within set message block");
                cJSON *json = cJSON_Parse(event->data);
                if (json == NULL) 
                {
                    const char *error_ptr = cJSON_GetErrorPtr();
                    if (error_ptr != NULL) {
                        fprintf(stderr, "Error before: %s\n", error_ptr);
                    } 
                    break;                   
                }
                // Extract json data
                cJSON *brightness = cJSON_GetObjectItemCaseSensitive(json, "brightness");
                cJSON *actstate = cJSON_GetObjectItemCaseSensitive(json, "state");
                cJSON *color = cJSON_GetObjectItemCaseSensitive(json, "color");
                if (color) {
                    cJSON *h = cJSON_GetObjectItemCaseSensitive(color, "h");   // "h" statt "hue"
                    cJSON *s = cJSON_GetObjectItemCaseSensitive(color, "s");   // "s" statt "saturation"
                    if (cJSON_IsNumber(h) && cJSON_IsNumber(s)) {
                        double hue = h->valuedouble;        // 0.0-360.0 Grad
                        double saturation = s->valuedouble; // 0.0-100.0 %
                        
                        // Lightness (Helligkeit) aus root-Objekt
                        cJSON *brightness = cJSON_GetObjectItemCaseSensitive(json, "brightness");
                        double lightness = (cJSON_IsNumber(brightness)) ? brightness->valuedouble : store.lightness;

                        // Validierung
                        if (hue < 0.0 || hue > 360.0 || saturation < 0.0 || saturation > 100.0 || lightness < 0.0 || lightness > 100.0) {
                            ESP_LOGE(TAG, "Ungültige HSL-Werte: H=%.1f S=%.1f L=%.1f", hue, saturation, lightness);
                            return;
                        }

                        // Befehl senden
                        ble_mesh_send_gen_hsl_set(hue, saturation, lightness, net_addr, client, ha_topic);
                    }
                }

                //printf("State: %s\n", actstate->valuestring);
                // HSL-Befehl verarbeiten

                else if (cJSON_IsNumber(brightness)) {
                    ESP_LOGI(TAG, "MQTT Message is for brightness");
                    //printf("brightness: %d\n", brightness->valueint);
                    int bright = brightness->valueint;
                    ble_mesh_send_gen_brightness_set(bright, net_addr, client, ha_topic);
                }
                else if(strncmp("ON",actstate->valuestring,2)==0){
                    ble_mesh_send_gen_onoff_set(1, net_addr);
                    esp_mqtt_client_publish(client, ha_topic, "{\"state\":\"ON\"}", 0, 0, 0);
                }
                else if(strncmp("OFF",actstate->valuestring,3)==0){
                    ble_mesh_send_gen_onoff_set(0, net_addr);
                    esp_mqtt_client_publish(client, ha_topic, "{\"state\":\"OFF\"}", 0, 0, 0);
                }
                //printf("DATA=%.*s\r\n", event->data_len, event->data); 
                cJSON_Delete(json);
            }
            

            if (strncmp("homeassistant/status", event->topic, 20) == 0) 
            {
                // Fetch lamp data from NVS and generate MQTT messages
                for (int i = 0; i < MAX_LAMPS; i++) {
                ESP_LOGI(TAG, "Found lamps %d", i);
                        
                // Load lamp info from NVS
                LampInfo lamp_info;
                esp_err_t err = load_lamp_info(&lamp_info, i);
                if (err == ESP_OK) {
                    // Generate MQTT message for this lamp
                    char topic[100];
                    char config_topic[100];
                    char payload[500];  // Adjust size as needed
                
                    // Create unique topic for this lamp (adjust as per your requirement)
                    snprintf(topic, sizeof(topic), "homeassistant/light/%s", lamp_info.name);

                    // Create config topic for this lamp (adjust as per your requirement)
                    snprintf(config_topic, sizeof(config_topic), "homeassistant/light/%s/config", lamp_info.name);

                    // Create payload for this lamp
                    snprintf(payload, sizeof(payload), createPayload(lamp_info.name, topic, lamp_info.address));
                    
                    // Publish each message
                    printf("Publishing to topic: %s, payload: %s\n", config_topic, payload);
                    // Call your MQTT publishing function here passing messages[i].topic and payload_str
                    esp_mqtt_client_publish(client, config_topic, payload, 0, 0, 0);
                } else {
                    // Failed to load lamp info, break loop
                    break;
                }
            }
                //get current status of all lights
                //ble_mesh_get_gen_onoff_status(0xFFFF);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            // if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                // log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                // log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                // log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
                // ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            // }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .credentials.username = CONFIG_USERNAME_MQTT,
        .credentials.authentication.password = CONFIG_PASSWORD_MQTT,
    };
    #if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
    #endif /* CONFIG_BROKER_URL_FROM_STDIN */

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);

    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

//WIFI
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
	     * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing...");

    board_init();
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    err = bluetooth_init();
    if (err) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return;
    }

    /* Open nvs namespace for storing/restoring mesh example info */
    err = ble_mesh_nvs_open(&NVS_HANDLE);
    if (err) {
        return;
    }

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    err = ble_mesh_init();
    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
    }
    mqtt_app_start();

    // Retrieve the current number of lamps from NVS and store it in the global variable
    g_num_lamps = getCurrentNumberOfLamps();
    // Start the web server
    //httpd_handle_t server = start_webserver();
    start_webserver();
    
}