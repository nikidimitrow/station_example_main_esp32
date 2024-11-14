#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/uart.h"
#include <lwip/sockets.h>
#include <esp_netif.h>


#include <stdio.h>
#include <errno.h>

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#define LOG_BUFFER_SIZE 4096
#define UART_NUM UART_NUM_1
#define UART_TX_PIN 17
#define UART_RX_PIN 16
#define BUF_SIZE 1024

static char log_buffer[LOG_BUFFER_SIZE];
static int log_buffer_index = 0;
static const char *TAG = "wifi station";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static u32_t counter = 0;
static u32_t m_TCPsocket = 0;

void add_log_to_buffer(const char *log) {
    int len = strlen(log);
    if (log_buffer_index + len < LOG_BUFFER_SIZE) {
        strcpy(&log_buffer[log_buffer_index], log);
        log_buffer_index += len;
    } else {
        // Use circular buffer approach
        int space_left = LOG_BUFFER_SIZE - log_buffer_index;
        if (space_left > 0) {
            strncpy(&log_buffer[log_buffer_index], log, space_left); 
        }
        log_buffer_index = (log_buffer_index + len) % LOG_BUFFER_SIZE;
    }
}

static esp_err_t hello_get_handler(httpd_req_t *req) {
    const char* resp_str = "Hello from ESP32!";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void uart_task(void *arg) {
    uint8_t data[BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            data[len] = '\0';
            add_log_to_buffer((const char *)data);
            ESP_LOGI(TAG, "UART Data Received: %s", data);
        }
    }
}

void init_uart() {
    
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 10, NULL);
}

 
static esp_err_t log_handler(httpd_req_t *req) {

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");
    int chunk_size = 512;
    for (int i = 0; i < log_buffer_index; i += chunk_size) {
        int len = (log_buffer_index - i < chunk_size) ? (log_buffer_index - i) : chunk_size;
        httpd_resp_send_chunk(req, &log_buffer[i], len);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Log handler called, log_buffer_index: %d", log_buffer_index);
    return ESP_OK;
}

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        // Регистриране на URI за главната страница "/"
        httpd_uri_t hello_uri = { 
            .uri = "/", 
            .method = HTTP_GET, 
            .handler = hello_get_handler, 
            .user_ctx = NULL 
        };
        httpd_register_uri_handler(server, &hello_uri);

        httpd_uri_t log_uri = { 
            .uri = "/log", 
            .method = HTTP_GET, 
            .handler = log_handler, 
            .user_ctx = NULL 
        };
        httpd_register_uri_handler(server, &log_uri);
    }
    return server;
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
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

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

int custom_log_handler(const char *fmt, va_list args) {
    char log_msg[256];
    int len = vsnprintf(log_msg, sizeof(log_msg), fmt, args);
    
    // int16_t broadcast = 0;
    // int16_t sendBytes = 0;
    // int16_t port      = 3000;
    // int32_t ip        = 0xc0a80068;
    // if(setsockopt(m_TCPsocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0)
        // {
            // ESP_LOGW(TAG, "Error in setting socket options: %s", strerror(errno));
        // }
// 
    // struct sockaddr_in clientAddr;
    // clientAddr.sin_family = AF_INET;
    // clientAddr.sin_port = htons(port); // 13401
    // clientAddr.sin_addr.s_addr = htonl(ip); // INADDR_LOOPBACK
// 
        // /* send the data files*/
    // sendBytes = sendto(m_TCPsocket, log_msg, len, 0,  (struct sockaddr*)(&clientAddr), sizeof(struct sockaddr_in));
        // if(sendBytes > 0)
        // {
        //   ESP_LOGI(TAG, "UDP OUT sent bytes: %d", sendBytes);
        // }
        // else
        // {
        //    ESP_LOGE(TAG, "UDP OUT send error: %s", strerror(errno));
        // }

    
    return vprintf(fmt, args);  // Продължава да отпечатва логовете и на стандартния терминал
}


void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    
    // Настройка на потребителския log handler
    esp_log_set_vprintf(custom_log_handler);

    // Тестови логове
    ESP_LOGI(TAG, "Starting HTTP server...");
    ESP_LOGI(TAG, "Initializing UART...");

    init_uart();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Starting HTTP server");
        m_TCPsocket = socket( AF_INET, SOCK_STREAM, 0);

        if(m_TCPsocket > 0) {
            /* Set Socket to nonblocking */
            u32_t flags = fcntl(m_TCPsocket, F_GETFL);
            int error = fcntl(m_TCPsocket, F_SETFL, flags | O_NONBLOCK);

            /* Error when setting the socket flags */
            if (error < 0)
            {
                ESP_LOGW(TAG, "Setting TCP socket flags error: %s", strerror(errno));
            }


            //start_webserver();
        }
    }
     
    char log_msg[256];

    log_msg[0] = {1, 2};
     
    
    while (1)
    {
       
       ESP_LOGI(TAG, "Initializing UART... test %ld", counter);
       counter++;
       log_msg[0]++;
       
       vTaskDelay(1000);
       	int16_t broadcast = 0;
        int16_t sendBytes = 0;
        int16_t port      = 3000;
        int32_t ip        = 0xc0a80068;
            if(setsockopt(m_TCPsocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0)
            {
                ESP_LOGW(TAG, "Error in setting socket options: %s", strerror(errno));
            }

        struct sockaddr_in clientAddr;
        clientAddr.sin_family = AF_INET;
        clientAddr.sin_port = htons(port); // 13401
        clientAddr.sin_addr.s_addr = htonl(ip); // INADDR_LOOPBACK

        /* send the data files*/
        sendBytes = sendto(m_TCPsocket, log_msg, 2, 0,  (struct sockaddr*)(&clientAddr), sizeof(struct sockaddr_in));
        if(sendBytes > 0)
        {
           // ESP_LOGI(TAG, "UDP OUT sent bytes: %d", sendBytes);
        }
        else
        {
            //ESP_LOGE(TAG, "UDP OUT send error: %s", strerror(errno));
        }

    }
    
}
 