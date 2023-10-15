#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)
#define BUF_SIZE (256)
#define MAX_CA_CERT_SIZE (6*1024)
#define MAX_CLIENT_CERT_SIZE (4*1024)
#define MAX_CLIENT_KEY_SIZE (3*1024)


typedef bool (*RespValiPtr)(char *);

void init(void);
void send_at_cmd(uint8_t, const char *, char *);
bool ok_response(char *);
bool no_error(char *);
bool module_ready(char *);
bool query_csq(char *);
bool query_creg(char *);
bool creg_def_stat(char *);
bool query_cgatt(char *);
bool query_xiic(char *);
bool query_mqttstate(char *);
bool add_ca_cert(char *);
bool add_client_cert(char *);
bool add_client_key(char *);
bool query_mqtttls(char *);

void app_main() {
    init();

    struct {
        const char *command;
        RespValiPtr validator;
        uint8_t retry;
        int8_t goto_step;
    } commands_list[] = {
        {"AT\r", ok_response, 3, 0},
        {"AT+CGSN\r", ok_response, 1, -1},
        {"AT+CGMM\r", ok_response, 1, -1},
        {"AT+CPIN?\r", module_ready, 15, 0},
        {"AT+CIMI\r", ok_response, 1, -1},
        {"AT+CSQ\r", query_csq, 40, 0},
        {"AT+CREG=2\r", ok_response, 1, -1},
        {"AT+CREG?\r", query_creg, 40, 0},
        {"AT+CREG=0\r", creg_def_stat, 1, 10},
        {"AT+CGATT=1\r", ok_response, 1, 0},    // chained commands
        {"AT+CGATT?\r", query_cgatt, 30, 9},
        {"AT$MYSYSINFO\r", ok_response, 3, 0},
        {"AT+CGDCONT=1,\"IP\",\"jionet\"\r", ok_response, 3, 0},   // or `airtelgprs`
        {"AT+XGAUTH=1,1,\"gsm\",\"1234\"\r", ok_response, 1, -1},   // optional?
        // module initialization complete
        {"AT+XIIC=1\r", ok_response, 3, 0},
        {"AT+XIIC?\r", query_xiic, 30, 14},
        // PPP link established successfully
        {"AT+MQTTTLS=sslmode,1", ok_response, 1, -1},
        {"AT+MQTTTLS=authmode,1", ok_response, 1, -1},
        {"AT\r", add_ca_cert, 1, -1},
        {"AT\r", add_client_cert, 1, -1},
        {"AT\r", add_client_key, 1, -1},
        {"AT+SSLTCPCFG=cacert,rootca.pem\r", ok_response, 1, -1},
        {"AT+SSLTCPCFG=clientcert,nwy_all.cert.pem\r", ok_response, 1, -1},
        {"AT+SSLTCPCFG=clientkey,nwy_all.private.key\r", ok_response, 1, -1},
        {"AT+MQTTTLS=rootca,rootca.pem\r", ok_response, 1, -1},
        {"AT+MQTTTLS=clientcert,nwy_all.cert.pem\r", ok_response, 1, -1},
        {"AT+MQTTTLS=clientkey,nwy_all.private.key\r", ok_response, 1, -1},
        {"AT+MQTTTLS?\r", query_mqtttls, 1, -1},
        // MQTTS encryption parameters configured for two-way authentication
        {"AT+MQTTCONNPARAM=\"007\",\"james_bond\",\"james_bond\"\r", ok_response, 1, 16},
        {"AT+MQTTWILLPARAM=0,1,\"lastWillAndTestament\",\"Sayonara!\"\r", ok_response, 1, 17},
        {"AT+MQTTCONN=\"broker.mqtt.cool:1883\",0,60\r", ok_response, 1, 18},   // navigate to `https://testclient-cloud.mqtt.cool/` and connect to `tcp://broker.mqtt.cool:1883` broker
        {"AT+MQTTSUB=\"rainbow\",1\r", ok_response, 3, 18},
        {"AT+MQTTPUB=0,1,\"rainbow\",\"Hello! This is Neoway's N706 Cat.1 Module!\"\r", ok_response, 1, 20},
        {"AT+MQTTSTATE?\r", query_mqttstate, 1, -1},
        {"AT+MQTTDISCONN\r", ok_response, 1, -1},
    };  // length = 23

    char resp[BUF_SIZE];
    
    uint8_t i = 0;
    size_t t = sizeof (commands_list) / sizeof (commands_list[0]);
    while (i < t) {
        const char *cmd = commands_list[i].command;
        uint8_t j = 0, retry = commands_list[i].retry;
        for (; j < retry; ++j) {
            send_at_cmd(UART_NUM_1, cmd, resp);

            ESP_LOGI("TX", "i=%2u, j=%u | Command: %s", i, j, cmd);
            // ESP_LOGI("RX", "i=%2u, j=%u | Response: %s", i, j, resp);
            uint8_t l = strlen(resp);
            for (int k = 0; k < l; ++k) {
                switch (resp[k]) {
                    case '\n':
                        ESP_LOGI("RX", "%u: \\n", k);
                        break;
                    case '\r':
                        ESP_LOGI("RX", "%u: \\r", k);
                        break;
                    default:
                        ESP_LOGI("RX", "%u: %c", k, resp[k]);
                }
            }

            if (commands_list[i].validator(resp)) {
                break;
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        if (j == retry) {
            i = commands_list[i].goto_step;
        } else {
            ++i;
        }
    }
    uart_driver_delete(UART_NUM_1);
}

void init() {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uart_driver_install(UART_NUM_1, BUF_SIZE * 2, 0, 0, NULL, 0);
    
    esp_log_level_set("RX", ESP_LOG_INFO);
    esp_log_level_set("TX", ESP_LOG_INFO);
    
    vTaskDelay(2000 / portTICK_PERIOD_MS);
}

void send_at_cmd(uint8_t uart_num, const char *command, char *response_buf) {
    uart_write_bytes(uart_num, command, strlen(command));
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // wait for the command to get send and processed by n706

    int bytes_read = uart_read_bytes(uart_num, (uint8_t *) response_buf, BUF_SIZE - 1, 1000 / portTICK_PERIOD_MS);
    assert(bytes_read > 0);
    response_buf[bytes_read] = '\0';
}

bool ok_response(char *response_buf) {
    char *result = strstr(response_buf, "\r\nOK\r\n");
    return (result != NULL);
}

bool no_error(char *response_buf) {
    char *result = strstr(response_buf, "\r\nERROR\r\n");
    return (result != NULL);
}

bool module_ready(char *response_buf) {
    int8_t result = strcmp(response_buf, "\r\n+CPIN: READY\r\nOK\r\n");
    return (result == 0);
}

bool query_csq(char *response_buf) {
    const char *pattern = "\r\n+CSQ: %u,%*u\r\nOK\r\n";
    uint8_t signal;
    uint8_t result = sscanf(response_buf, pattern, &signal);
    if (result == 1) {
        return (signal >= 12 && signal <= 31);
    } else {
        return false;
    }
}

bool query_creg(char *response_buf) {
    const char *pattern = "\r\n+CREG: 2,%u\r\nOK\r\n";
    uint8_t status;
    uint8_t result = sscanf(response_buf, pattern, &status);
    if (result == 1) {
        return (status == 1 || status == 5);
    } else {
        return false;
    }
}

bool creg_def_stat(char *response_buf) {
    return (! ok_response(response_buf));
}

bool query_cgatt(char *response_buf) {
    int8_t result = strcmp(response_buf, "\r\n+CGATT: 1\r\nOK\r\n");    
    return (result == 0);
}

bool query_xiic(char *response_buf) {
    char *result = strstr(response_buf, "\r\n+XIIC: 1,");
    return (result != NULL);
}

bool query_mqttstate(char *response_buf) {
    int8_t result = strcmp(response_buf, "\r\n+MQTTSTATE: 1\r\n\r\nOK\r\n");    
    return (result == 0);
}

bool add_ca_cert(char *response_buf) {
    esp_vfs_spiffs_register(NULL);

    FILE* file_ca_cert = fopen("/spiffs/files/ssl.ca", "r");
    assert(file_ca_cert == NULL);

    char ca_cert_buf[MAX_CA_CERT_SIZE];
    size_t bytes_read = fread(ca_cert_buf, 1, sizeof (ca_cert_buf), file_ca_cert);

    char cmd_buf[BUF_SIZE];

    snprintf(cmd_buf, sizeof (cmd_buf), "AT+CERTADD=rootca.pem,%zu\r", bytes_read);
    send_at_cmd(UART_NUM_1, cmd_buf, response_buf);
    int8_t result = strcmp(response_buf, "\r\nCONNECT\r\n");
    
    uart_write_bytes(UART_NUM_1, ca_cert_buf, strlen(ca_cert_buf));
    send_at_cmd(UART_NUM_1, "\r\n", response_buf);

    fclose(file_ca_cert);
    
    esp_vfs_spiffs_unregister(NULL);

    if (result == 0 || !ok_response(response_buf)) {
        return false;
    }

    return true;
}

bool add_client_cert(char *response_buf) {
    esp_vfs_spiffs_register(NULL);

    FILE* file_client_cert = fopen("/spiffs/files/ssl.crt", "r");
    assert(file_client_cert == NULL);

    char client_cert_buf[MAX_CA_CERT_SIZE];
    size_t bytes_read = fread(client_cert_buf, 1, sizeof (client_cert_buf), file_client_cert);

    char cmd_buf[BUF_SIZE];

    snprintf(cmd_buf, sizeof (cmd_buf), "AT+CERTADD=nwy_all.cert.pem,%zu\r", bytes_read);
    send_at_cmd(UART_NUM_1, cmd_buf, response_buf);
    int8_t result = strcmp(response_buf, "\r\nCONNECT\r\n");
    
    uart_write_bytes(UART_NUM_1, client_cert_buf, strlen(client_cert_buf));
    send_at_cmd(UART_NUM_1, "\r\n", response_buf);

    fclose(file_client_cert);
    
    esp_vfs_spiffs_unregister(NULL);

    if (result == 0 || !ok_response(response_buf)) {
        return false;
    }

    return true;
}

bool add_client_key(char *response_buf) {
    esp_vfs_spiffs_register(NULL);

    FILE* file_client_key = fopen("/spiffs/files/ssl.key", "r");
    assert(file_client_key == NULL);

    char client_key_buf[MAX_CA_CERT_SIZE];
    size_t bytes_read = fread(client_key_buf, 1, sizeof (client_key_buf), file_client_key);

    char cmd_buf[BUF_SIZE];

    snprintf(cmd_buf, sizeof (cmd_buf), "AT+CERTADD=nwy_all.private.key,%zu\r", bytes_read);
    send_at_cmd(UART_NUM_1, cmd_buf, response_buf);
    int8_t result = strcmp(response_buf, "\r\nCONNECT\r\n");
    
    uart_write_bytes(UART_NUM_1, client_key_buf, strlen(client_key_buf));
    send_at_cmd(UART_NUM_1, "\r\n", response_buf);

    fclose(file_client_key);
    
    esp_vfs_spiffs_unregister(NULL);

    if (result == 0 || !ok_response(response_buf)) {
        return false;
    }

    return true;
}

bool query_mqtttls(char *response_buf) {
    int8_t result = strcmp(response_buf, "\r\n+MQTTTLS: 1,1,rootca.pem,nwy_all.cert.pem,nwy_all.private.key\r\nOK\r\n");    
    return (result == 0);

}
