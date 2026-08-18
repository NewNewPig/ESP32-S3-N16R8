#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define AT_UART_NUM UART_NUM_0
#define AT_BUFFER_SIZE 256
#define LED_PIN GPIO_NUM_2
#define TCP_TX_BUF_SIZE 512

static char at_buffer[AT_BUFFER_SIZE];
static size_t at_len = 0;
static bool led_state = false;
static bool wifi_connected = false;
static char connected_ssid[33] = {0};
static int tcp_client_sock = -1;
static bool tcp_connected = false;
static bool send_mode_pending = false;
static size_t pending_send_len = 0;
static size_t pending_send_index = 0;
static char pending_send_buf[TCP_TX_BUF_SIZE];

static void send_line(const char *text)
{
    uart_write_bytes(AT_UART_NUM, text, strlen(text));
}

static void send_ok(void)
{
    send_line("OK\r\n");
}

static void send_error(void)
{
    send_line("ERROR\r\n");
}

static void trim_in_place(char *str)
{
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n' || str[len - 1] == ' ' || str[len - 1] == '\t')) {
        str[len - 1] = '\0';
        len--;
    }
    while (*str != '\0' && (*str == ' ' || *str == '\t')) {
        memmove(str, str + 1, strlen(str));
    }
}

static void normalize_at_command(char *str)
{
    for (char *p = str; *p != '\0'; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
}

static void close_tcp_socket(void)
{
    if (tcp_client_sock >= 0) {
        shutdown(tcp_client_sock, 0);
        close(tcp_client_sock);
        tcp_client_sock = -1;
    }
    tcp_connected = false;
    send_mode_pending = false;
    pending_send_len = 0;
    pending_send_index = 0;
}

static void update_connected_info(void)
{
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        snprintf(connected_ssid, sizeof(connected_ssid), "%s", (char *)cfg.sta.ssid);
        wifi_connected = true;
    } else {
        connected_ssid[0] = '\0';
        wifi_connected = false;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                wifi_connected = false;
                connected_ssid[0] = '\0';
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            update_connected_info();
        }
    }
}

static void wifi_init_station(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(sta_netif == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void send_wifi_scan_result(void)
{
    uint16_t ap_count = 10;
    wifi_ap_record_t ap_info[10];
    memset(ap_info, 0, sizeof(ap_info));

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        send_error();
        return;
    }

    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        send_error();
        return;
    }

    if (ap_count > 10) {
        ap_count = 10;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_info);
    if (err != ESP_OK) {
        send_error();
        return;
    }

    char line[128];
    for (int i = 0; i < ap_count; i++) {
        snprintf(line, sizeof(line), "%s,%d,%d,%d\r\n",
                 ap_info[i].ssid,
                 ap_info[i].rssi,
                 ap_info[i].primary,
                 ap_info[i].authmode);
        send_line(line);
    }
    send_ok();
}

static void handle_wifi_connect(const char *cmd)
{
    const char *p = cmd + strlen("AT+CWJAP=");
    if (*p != '"') {
        send_error();
        return;
    }
    p++;

    char ssid[33] = {0};
    char password[64] = {0};
    char *ssid_end = strchr(p, '"');
    if (ssid_end == NULL) {
        send_error();
        return;
    }
    size_t ssid_len = (size_t)(ssid_end - p);
    if (ssid_len >= sizeof(ssid)) {
        send_error();
        return;
    }
    memcpy(ssid, p, ssid_len);

    const char *comma = strchr(ssid_end + 1, ',');
    if (comma == NULL || *(comma + 1) != '"') {
        send_error();
        return;
    }
    const char *pass_start = comma + 2;
    const char *pass_end = strrchr(pass_start, '"');
    if (pass_end == NULL) {
        send_error();
        return;
    }
    size_t pass_len = (size_t)(pass_end - pass_start);
    if (pass_len >= sizeof(password)) {
        send_error();
        return;
    }
    memcpy(password, pass_start, pass_len);

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
    send_ok();
}

static bool parse_tcp_start_command(const char *cmd, char *type, size_t type_len, char *ip, size_t ip_len, int *port)
{
    const char *p = cmd + strlen("AT+CIPSTART=");
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '"') {
        return false;
    }

    p++;
    char *type_end = strchr((char *)p, '"');
    if (type_end == NULL) {
        return false;
    }
    size_t type_size = (size_t)(type_end - p);
    if (type_size >= type_len) {
        return false;
    }
    memcpy(type, p, type_size);
    type[type_size] = '\0';

    char *after_type = type_end + 1;
    while (*after_type == ' ' || *after_type == ',' || *after_type == '\t') {
        after_type++;
    }
    if (*after_type != '"') {
        return false;
    }
    after_type++;
    char *ip_end = strchr(after_type, '"');
    if (ip_end == NULL) {
        return false;
    }
    size_t ip_size = (size_t)(ip_end - after_type);
    if (ip_size >= ip_len) {
        return false;
    }
    memcpy(ip, after_type, ip_size);
    ip[ip_size] = '\0';

    char *port_str = ip_end + 1;
    while (*port_str == ' ' || *port_str == ',' || *port_str == '\t') {
        port_str++;
    }
    *port = atoi(port_str);
    return (*port > 0);
}

static void handle_tcp_start(const char *cmd)
{
    if (tcp_connected && tcp_client_sock >= 0) {
        send_error();
        return;
    }

    char type[16] = {0};
    char ip[32] = {0};
    int port = 0;
    if (!parse_tcp_start_command(cmd, type, sizeof(type), ip, sizeof(ip), &port)) {
        send_error();
        return;
    }

    if (strcasecmp(type, "TCP") != 0 && strcasecmp(type, "UDP") != 0) {
        send_error();
        return;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        send_error();
        return;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        close(sock);
        send_error();
        return;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        close(sock);
        send_error();
        return;
    }

    tcp_client_sock = sock;
    tcp_connected = true;
    send_ok();
}

static void handle_tcp_server_start(const char *cmd)
{
    const char *p = cmd + strlen("AT+CIPSERVER=");
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    int mode = atoi(p);
    int server_port = 8080;
    if (mode == 1) {
        char *comma = strchr((char *)p, ',');
        if (comma != NULL) {
            server_port = atoi(comma + 1);
        }
    }

    if (mode == 1 && server_port > 0) {
        int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock < 0) {
            send_error();
            return;
        }

        struct sockaddr_in server_addr = {0};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons((uint16_t)server_port);

        if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
            close(listen_sock);
            send_error();
            return;
        }

        if (listen(listen_sock, 1) != 0) {
            close(listen_sock);
            send_error();
            return;
        }

        tcp_client_sock = accept(listen_sock, NULL, NULL);
        if (tcp_client_sock < 0) {
            close(listen_sock);
            send_error();
            return;
        }
        close(listen_sock);
        tcp_connected = true;
        send_ok();
        return;
    }

    if (mode == 0) {
        close_tcp_socket();
        send_ok();
        return;
    }

    send_error();
}

static void handle_tcp_data_send(const char *cmd)
{
    if (!tcp_connected || tcp_client_sock < 0) {
        send_error();
        return;
    }

    size_t prefix_len = strlen("AT+CIPSEND=");
    if (strlen(cmd) <= prefix_len) {
        send_error();
        return;
    }

    char length_str[32] = {0};
    snprintf(length_str, sizeof(length_str), "%s", cmd + prefix_len);
    trim_in_place(length_str);
    size_t length = (size_t)strtoul(length_str, NULL, 10);
    if (length == 0 || length >= sizeof(pending_send_buf)) {
        send_error();
        return;
    }

    send_mode_pending = true;
    pending_send_len = length;
    pending_send_index = 0;
    pending_send_buf[0] = '\0';
    send_line(">\r\n");
}

static void handle_tcp_close(void)
{
    close_tcp_socket();
    send_ok();
}

static void handle_tcp_status(void)
{
    if (tcp_connected && tcp_client_sock >= 0) {
        send_line("STATUS:TCP_CONNECTED\r\n");
    } else {
        send_line("STATUS:TCP_DISCONNECTED\r\n");
    }
    send_ok();
}

static void send_received_tcp_data(void)
{
    if (tcp_client_sock < 0) {
        return;
    }

    char rx_buf[128];
    int recv_len = recv(tcp_client_sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (recv_len > 0) {
        rx_buf[recv_len] = '\0';
        send_line("+IPD,");
        char len_str[16];
        snprintf(len_str, sizeof(len_str), "%d", recv_len);
        send_line(len_str);
        send_line(":");
        send_line(rx_buf);
        send_line("\r\n");
    }
}

static void handle_at_command(char *cmd)
{
    trim_in_place(cmd);
    normalize_at_command(cmd);

    if (strcmp(cmd, "AT") == 0) {
        send_ok();
        return;
    }

    if (strcmp(cmd, "ATI") == 0 || strcmp(cmd, "AT+GMR") == 0) {
        send_line("ESP32-S3-N16R8 WiFi TCP AT DEMO\r\n");
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+VERSION") == 0) {
        send_line("1.3.0\r\n");
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+INFO") == 0) {
        send_line("ESP32-S3-N16R8 WiFi TCP AT firmware\r\n");
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+RST") == 0) {
        send_line("RESETTING\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    if (strcmp(cmd, "AT+LED=ON") == 0) {
        gpio_set_level(LED_PIN, 1);
        led_state = true;
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+LED=OFF") == 0) {
        gpio_set_level(LED_PIN, 0);
        led_state = false;
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+LED?") == 0) {
        send_line(led_state ? "ON\r\n" : "OFF\r\n");
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+CWMODE?") == 0) {
        send_line("1\r\n");
        send_ok();
        return;
    }

    if (strncmp(cmd, "AT+CWMODE=", 10) == 0) {
        char *mode = cmd + 10;
        if (strcmp(mode, "1") == 0 || strcmp(mode, "2") == 0 || strcmp(mode, "3") == 0) {
            send_ok();
            return;
        }
        send_error();
        return;
    }

    if (strcmp(cmd, "AT+CWLAP") == 0) {
        send_wifi_scan_result();
        return;
    }

    if (strcmp(cmd, "AT+CWQAP") == 0) {
        esp_wifi_disconnect();
        wifi_connected = false;
        connected_ssid[0] = '\0';
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+CWJAP?") == 0) {
        if (wifi_connected && connected_ssid[0] != '\0') {
            char line[64];
            snprintf(line, sizeof(line), "%s\r\n", connected_ssid);
            send_line(line);
        } else {
            send_line("NO AP\r\n");
        }
        send_ok();
        return;
    }

    if (strncmp(cmd, "AT+CWJAP=", 9) == 0) {
        handle_wifi_connect(cmd);
        return;
    }

    if (strcmp(cmd, "AT+CIPMUX=0") == 0) {
        send_ok();
        return;
    }

    if (strcmp(cmd, "AT+CIPMODE=0") == 0 || strcmp(cmd, "AT+CIPMODE=1") == 0) {
        send_ok();
        return;
    }

    if (strncmp(cmd, "AT+CIPSERVER=", 13) == 0) {
        handle_tcp_server_start(cmd);
        return;
    }

    if (strcmp(cmd, "AT+CIPSTATUS") == 0) {
        handle_tcp_status();
        return;
    }

    if (strncmp(cmd, "AT+CIPSTART=", 12) == 0) {
        handle_tcp_start(cmd);
        return;
    }

    if (strncmp(cmd, "AT+CIPSEND=", 11) == 0) {
        handle_tcp_data_send(cmd);
        return;
    }

    if (strcmp(cmd, "AT+CIPCLOSE") == 0) {
        handle_tcp_close();
        return;
    }

    if (strcmp(cmd, "ATE0") == 0 || strcmp(cmd, "ATE1") == 0) {
        send_ok();
        return;
    }

    send_error();
}

static void init_led(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);
}

static void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(AT_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(AT_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(AT_UART_NUM, AT_BUFFER_SIZE * 2, 0, 0, NULL, 0));
}

void app_main(void)
{
    init_led();
    init_uart();
    wifi_init_station();

    send_line("ESP32-S3-N16R8 WIFI TCP AT READY\r\n");
    send_ok();

    while (1) {
        uint8_t ch;
        int len = uart_read_bytes(AT_UART_NUM, &ch, 1, pdMS_TO_TICKS(20));

        if (len > 0) {
            if (send_mode_pending) {
                if (ch == '\r' || ch == '\n') {
                    continue;
                }
                if (pending_send_index < pending_send_len) {
                    pending_send_buf[pending_send_index++] = (char)ch;
                    if (pending_send_index == pending_send_len) {
                        if (tcp_client_sock >= 0) {
                            send(tcp_client_sock, pending_send_buf, pending_send_index, 0);
                            send_ok();
                        } else {
                            send_error();
                        }
                        send_mode_pending = false;
                        pending_send_len = 0;
                        pending_send_index = 0;
                    }
                }
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                if (at_len > 0) {
                    at_buffer[at_len] = '\0';
                    handle_at_command(at_buffer);
                    at_len = 0;
                }
            } else {
                if (at_len < AT_BUFFER_SIZE - 1) {
                    at_buffer[at_len++] = (char)ch;
                }
            }
        }

        if (tcp_connected && tcp_client_sock >= 0) {
            send_received_tcp_data();
        }
    }
}
