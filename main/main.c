/* (c) 2025 M10tech
 * busdisplaydriver for ESP32
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
// #include "lcm_api.h"
#include <udplogger.h>
#include "driver/uart.h"
#include "soc/uart_reg.h"
#include "ping/ping_sock.h"
#include "hal/wdt_hal.h"
#include "mqtt_client.h"
#include "esp_ota_ops.h" //for esp_app_get_description
#include <cJSON.h>

// You must set version.txt file to match github version tag x.y.z for LCM4ESP32 to work

int display_idx=0;
char txt1[64],   txt2[64];
int font1=0x58, font2=0x58, layout=0x31, addr=0x33;
int mqtt_order=0;

static void log_error_if_nonzero(const char *message, int error_code) {if (error_code != 0) UDPLUS("Last error %s: 0x%x\n", message, error_code);}
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
//     UDPLUS("Event dispatched from event loop base=%s, event_id=%lx\n", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    const cJSON *json_txt1=NULL,*json_txt2=NULL,*json_font1=NULL,*json_font2=NULL,*json_type=NULL,*json_addr=NULL;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        UDPLUS("MQTT_EVENT_CONNECTED\n");
        msg_id = esp_mqtt_client_subscribe(client, "bus_panel/message", 0);
        UDPLUS("sent subscribe successful, msg_id=%d\n", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        UDPLUS("MQTT_EVENT_DISCONNECTED\n");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        UDPLUS("MQTT_EVENT_SUBSCRIBED, msg_id=%d\n", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        //UDPLUS( "MQTT_EVENT_DATA\n");
        UDPLUS("TOPIC=%.*s DATA=\n", event->topic_len, event->topic);
        UDPLUS("%.*s\n", event->data_len, event->data);
        cJSON *json = cJSON_ParseWithLength(event->data, event->data_len);
        if (json) {
//             char *myJson = cJSON_Print(json);
//             UDPLUS("JSON: %s\n",myJson);
//             free(myJson);
            json_txt1 = cJSON_GetObjectItemCaseSensitive(json, "txt1");
            if (cJSON_IsString(json_txt1) && (json_txt1->valuestring != NULL)) {
                UDPLUS("Text 1 is \"%s\"\n", json_txt1->valuestring);
                strcpy(txt1,json_txt1->valuestring);
            }
            json_txt2 = cJSON_GetObjectItemCaseSensitive(json, "txt2");
            if (cJSON_IsString(json_txt2) && (json_txt2->valuestring != NULL)) {
                UDPLUS("Text 2 is \"%s\"\n", json_txt2->valuestring);
                strcpy(txt2,json_txt2->valuestring);
            }
            json_font1 = cJSON_GetObjectItemCaseSensitive(json, "font1");
            if (cJSON_IsNumber(json_font1)) {
                UDPLUS("Font 1 is \"0x%02x\"\n", json_font1->valueint);
                font1=json_font1->valueint;
            }
            json_font2 = cJSON_GetObjectItemCaseSensitive(json, "font2");
            if (cJSON_IsNumber(json_font2)) {
                UDPLUS("Font 2 is \"0x%02x\"\n", json_font2->valueint);
                font2=json_font2->valueint;
            }
            json_type = cJSON_GetObjectItemCaseSensitive(json, "layout");
            if (cJSON_IsNumber(json_type)) {
                UDPLUS("Layout is \"0x%02x\"\n", json_type->valueint);
                layout=json_type->valueint;
            }
            json_addr = cJSON_GetObjectItemCaseSensitive(json, "addr");
            if (cJSON_IsNumber(json_addr)) {
                UDPLUS("Address = \"0x%02x\"\n", json_addr->valueint);
                addr=json_addr->valueint;
            }
            cJSON_Delete(json);
            mqtt_order=1;
        } else {
        }
        break;
    case MQTT_EVENT_ERROR:
        UDPLUS("MQTT_EVENT_ERROR\n");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            UDPLUS("Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        UDPLUS("Other event id:%d", event->event_id);
        break;
    }
}

char *broker_uri=NULL;
static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg={};
    if (broker_uri==NULL) {
        char line[128];
        int count = 0;
        UDPLUS("Please enter url of mqtt broker\n");
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
        UDPLUS("Broker url: %s\n", line);
    } else {
        mqtt_cfg.broker.address.uri = broker_uri;
    }
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

char    *pinger_target=NULL;

wdt_hal_context_t rtc_wdt_ctx = RWDT_HAL_CONTEXT_DEFAULT(); //RTC WatchDogTimer context
int ping_count=60,ping_delay=1; //seconds
static void ping_success(esp_ping_handle_t hdl, void *args) {
    ping_count+=20;
    if (ping_count>120) ping_count=120;
    //uint32_t elapsed_time;
    //ip_addr_t response_addr;
    //esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    //esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,  &response_addr,  sizeof(response_addr));
    //UDPLUS("good ping from %s %lu ms -> count: %d s\n", inet_ntoa(response_addr.u_addr.ip4), elapsed_time, ping_count);
    //feed the RTC WatchDog
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_feed(&rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&rtc_wdt_ctx);
}
static void ping_timeout(esp_ping_handle_t hdl, void *args) {
    //ping_count--; ping_delay=1;
    //UDPLUS("failed ping -> count: %d s\n", ping_count);
    //feed the RTC WatchDog ANYHOW, until the code is changed to ping the default gateway automatically
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_feed(&rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&rtc_wdt_ctx);
}
void ping_task(void *argv) {
    ip_addr_t target_addr;
    ipaddr_aton(pinger_target,&target_addr);
    esp_ping_handle_t ping;
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.timeout_ms = 900; //should time-out before our 1s delay
    ping_config.count = 1; //one ping at a time means we can regulate the interval at will
    esp_ping_callbacks_t cbs = {.on_ping_success=ping_success, .on_ping_timeout=ping_timeout, .on_ping_end=NULL, .cb_args=NULL}; //set callback functions
    esp_ping_new_session(&ping_config, &cbs, &ping);
    
    UDPLUS("Pinging IP %s\n", ipaddr_ntoa(&target_addr));
    //re-enable RTC WatchDogTimer (don't depend on bootloader to not disable it)
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_enable(&rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&rtc_wdt_ctx);    
    while(ping_count){
        vTaskDelay((ping_delay-1)*(1000/portTICK_PERIOD_MS)); //already waited 1 second...
        esp_ping_start(ping);
        vTaskDelay(1000/portTICK_PERIOD_MS); //waiting for answer or timeout to update ping_delay value
    }
    UDPLUS("restarting because can't ping home-hub\n");
    vTaskDelay(1000/portTICK_PERIOD_MS); //allow UDPlog to flush output
    esp_restart(); //TODO: disable GPIO outputs
}

void time_task(void *argv) {
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1); tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        UDPLUS("Still waiting for system time to sync\n");
    }
    time_t ts = time(NULL);
    UDPLUS("TIME SET: %u=%s\n", (unsigned int) ts, ctime(&ts));
    vTaskDelete(NULL);
}

char localhost[]="127.0.0.1";
static void ota_string() {
    char *display_nr=NULL;
    esp_err_t status;
    nvs_handle_t lcm_handle;
    char *otas=NULL;
    size_t  size;
    status = nvs_open("LCM", NVS_READONLY, &lcm_handle);
    
    if (!status && nvs_get_str(lcm_handle, "ota_string", NULL, &size) == ESP_OK) {
        otas = malloc(size);
        nvs_get_str(lcm_handle, "ota_string", otas, &size);
        display_nr=strtok(otas,";");
        broker_uri=strtok(NULL,";");
        pinger_target=strtok(NULL,";");
    }
    if (display_nr==NULL) display_idx=0; else display_idx=atoi(display_nr);
    if (pinger_target==NULL) pinger_target=localhost;
    //DO NOT free the otas since it carries the config pieces
}


uint8_t checksum(uint8_t *payload, size_t len) {
    uint8_t cs=0;
    for(int i = 0; i < len; i++) cs ^= payload[i];
    return cs;
}

void main_task(void *arg) {
    udplog_init(3);
    vTaskDelay(300); //Allow Wi-Fi to connect
    UDPLUS("\n\nBus-Panel-Driver %s\n",esp_app_get_description()->version);

//     nvs_handle_t lcm_handle;nvs_open("LCM", NVS_READWRITE, &lcm_handle);nvs_set_str(lcm_handle,"ota_string", "3;mqtt://busdisplay:notthesecret@192.168.178.5;192.168.178.5");
//     nvs_commit(lcm_handle); //can be used if not using LCM
    ota_string();

    const uart_port_t uart_num = UART_NUM_1;
    const int uart_buffer_size = (1024 * 2);        // RX               TX
    ESP_ERROR_CHECK(uart_driver_install(uart_num, uart_buffer_size, uart_buffer_size, 20, NULL, 0));
    uart_config_t uart_config = {
        .baud_rate = 600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config)); // Configure UART parameters
    uart_intr_config_t uart_intr = {
        .intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M | UART_RXFIFO_TOUT_INT_ENA_M, //?? |UART_INTR_RXFIFO_TOUT,
        .rxfifo_full_thresh = 255,
        .rx_timeout_thresh = 3,
    };
    ESP_ERROR_CHECK(uart_intr_config(uart_num, &uart_intr));
    ESP_ERROR_CHECK(uart_enable_rx_intr(uart_num)); // Enable UART RX FIFO full threshold and timeout interrupts
                                        // TX  RX  RTS   CTS
    ESP_ERROR_CHECK(uart_set_pin(uart_num, 13, 14, 12, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(uart_num,UART_MODE_RS485_HALF_DUPLEX));
    xTaskCreate(time_task,"Time", 2048, NULL, 6, NULL);
    xTaskCreate(ping_task,"PingT",2048, NULL, 1, NULL);

    mqtt_app_start();

    uint8_t payload_random_test[]={0x02,0x05,0x23,0x46,0x54,0x0d,0x31,0x39,0x0d,0x02,0x03,0x00}; //init message
    payload_random_test[sizeof(payload_random_test)-1]=checksum(payload_random_test,sizeof(payload_random_test)-1);
    uart_write_bytes(uart_num,payload_random_test,sizeof(payload_random_test));

    uint8_t payload[32]={0x02,0x05,0x00,0x33,0x31,0x0d,0x58,0x00};
    int count=0,size=0,menu=0;
    int length1=0, length2=0;
    char line[128];
    txt1[0]='\0', txt2[0]='\0';
    while (true) {
        line[127]='\0', txt1[63]='\0', txt2[63]='\0';
        UDPLUS("Menu:\n0=write\n1=txt1 = \"%s\"\n2=txt2 = \"%s\"\n3=layout=\"0x%02x\"\n4=font1= \"0x%02x\"\n5=font2= \"0x%02x\"\n6=addr = \"0x%02x\"\n", \
        txt1,txt2,layout,font1,font2,addr);
        while ((menu = fgetc(stdin))==EOF && mqtt_order==0) vTaskDelay(1);
        if (mqtt_order) {mqtt_order=0; menu=0x30;} //write
        switch (menu) {
            case 0x31: { //txt1
                count=0;
                UDPLUS("1\nPlease enter message txt1: \n");
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
                strcpy(txt1,line);
            } break;
            case 0x32: { //txt2
                count=0;
                UDPLUS("2\nPlease enter message txt2: \n");
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
                strcpy(txt2,line);
            } break;
            case 0x33: { //layout
                UDPLUS("3\nPlease enter layout code: \n");
                while ((layout = fgetc(stdin))==EOF) vTaskDelay(1);
            } break;
            case 0x34: { //font
                UDPLUS("4\nPlease enter font1 code: \n");
                while ((font1 = fgetc(stdin))==EOF) vTaskDelay(1);
            } break;
            case 0x35: { //font
                UDPLUS("5\nPlease enter font2 code: \n");
                while ((font2 = fgetc(stdin))==EOF) vTaskDelay(1);
            } break;
            case 0x36: { //address
                UDPLUS("6\nPlease enter addres code: \n");
                while ((addr = fgetc(stdin))==EOF) vTaskDelay(1);
            } break;
            case 0x30: { //write
                payload[3]=addr; payload[4]=layout;
                switch (layout) {
                    case 0x30: { //no message = '0'
                        payload[2]=0x20; payload[6]=0x02; payload[7]=0x03;
                        payload[8]=checksum(payload,8);
                        size=9;
                    } break;
                    case 0x31: { //1 message = '1'
                        length1=strlen(txt1);
                        UDPLUS("0\nMessage1: \"%s\"(%d)\n", txt1, length1);
                        payload[2]=length1+0x20+2;
                        payload[6]=font1;
                        for (int i=0; i<length1; i++) {
                            payload[7+i]=txt1[i];
                        }
                        payload[7+length1]=0x0d; payload[8+length1]=0x02; payload[9+length1]=0x03;
                        payload[10+length1]=checksum(payload,10+length1);
                        size=11+length1;
                    } break;
                    case 0x32: case 0x34: { //2 messages, 0x32='2'=in-line, 0x34='4'=above one another
                        length1=strlen(txt1);
                        length2=strlen(txt2);
                        UDPLUS("0\nMessage1: \"%s\"(%d) + Message2: \"%s\"(%d)\n", txt1, length1, txt2, length2);
                        payload[2]=length1+length2+0x20+4;
                        payload[6]=font1;
                        for (int i=0; i<length1; i++) {
                            payload[7+i]=txt1[i];
                        }
                        payload[7+length1]=0x0d;
                        payload[8+length1]=font2;
                        for (int i=0; i<length2; i++) {
                            payload[9+length1+i]=txt2[i];
                        }
                        payload[9+length1+length2]=0x0d;
                        payload[10+length1+length2]=0x02; payload[11+length1+length2]=0x03;
                        payload[12+length1+length2]=checksum(payload,12+length1+length2);
                        size=13+length1+length2;
                    } break;
                    case 0x53: { //'S'=status request  //doesn't do anything, right?
                    } break;
                    case 0x54: case 0x56:{ //0x54='T'=test mode(2x), 0x56='V'=level(1x)
                        length1=strlen(txt1);
                        UDPLUS("0\nKey value: %s\n", txt1);
                        payload[2]=length1+0x20+1;
                        for (int i=0; i<length1; i++) {
                            payload[6+i]=txt1[i];
                        }
                        payload[6+length1]=0x0d; payload[7+length1]=0x02; payload[8+length1]=0x03;
                        payload[9+length1]=checksum(payload,9+length1);
                        size=10+length1;
                    } break;
                    default:
                    UDPLUS("Unknown layout choice\n");
                }
                for (int i=0; i<size; i++) UDPLUS("%02x ",payload[i]);
                UDPLUS("\n");
                uart_write_bytes(uart_num,payload,size);
            } break;
            default:
            UDPLUS("Unknown menu choice\n");
        }
    }
}    

void app_main(void) {
    printf("app_main-start\n");

    //The code in this function would be the setup for any app that uses wifi which is set by LCM
    //It is all boilerplate code that is also used in common_example code
    esp_err_t err = nvs_flash_init(); // Initialize NVS
    if (err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); //NVS partition truncated and must be erased
        err = nvs_flash_init(); //Retry nvs_flash_init
    } ESP_ERROR_CHECK( err );

    //TODO: if no wifi setting found, trigger otamain
    
    //block that gets you WIFI with the lowest amount of effort, and based on FLASH
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    esp_netif_config.route_prio = 128;
    esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);
    esp_wifi_set_default_wifi_sta_handlers();
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    //end of boilerplate code

    xTaskCreate(main_task,"main",4096,NULL,1,NULL);
    while (true) {
        vTaskDelay(1000); 
    }
    printf("app_main-done\n"); //will never exit here
}
