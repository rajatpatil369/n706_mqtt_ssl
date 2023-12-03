#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <errno.h>

#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)
#define BUF_SIZE (256)
#define MAX_CA_CERT_SIZE (6*1024)
#define MAX_CLIENT_CERT_SIZE (4*1024)
#define MAX_CLIENT_KEY_SIZE (3*1024)

char temp[BUF_SIZE];

typedef bool (*RespValiPtr)(char *);

void init() {
    vTaskDelay(2000 / portTICK_PERIOD_MS);

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

void deinit() {
    uart_driver_delete(UART_NUM_1);
}

void escape_cr_nl(const char *input, char *output) {
    int m = 0, n = 0;
    for (; input[m] != '\0'; ++m, ++n) {
        if (input[m] == '\r') {
            output[n] = '\\';
            ++n;
            output[n] = 'r';
        } else if (input[m] == '\n') {
            output[n] = '\\';
            ++n;
            output[n] = 'n';
        } else {
            output[n] = input[m];
        }
    }
    output[n] = '\0';
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

bool _error(char *response_buf) {
    char *result = strstr(response_buf, "\r\nERROR\r\n");
    return (result == NULL);
}

bool module_ready(char *response_buf) {
    int8_t result = strcmp(response_buf, "AT+CPIN?\r\r\n+CPIN: READY\r\nOK\r\n");
    return (result == 0);
}

bool query_csq(char *response_buf) {
    const char *pattern = "AT_CSQ\r\r\n+CSQ: %u,%*u\r\nOK\r\n";
    uint8_t signal;
    uint8_t result = sscanf(response_buf, pattern, &signal);
    if (result == 1) {
        return (signal >= 12 && signal <= 31);
    } else {
        return false;
    }
}

bool query_creg(char *response_buf) {
    // "AT+CREG?\r\r\n+CREG: 2,1,"17cc","0e22f26f",7\r\nOK\r\n"
    const char *pattern = "\r\n+CREG: 2,%u,\"%*x\",\"%*x\",%*u\r\nOK\r\n";
    uint8_t status;
    uint8_t result = sscanf(response_buf, pattern, &status);
    if (result == 1) {
        return (status == 1 || status == 5);
    } else {
        return false;
    }
}

bool creg_def_stat(char *response_buf) {
    return (!ok_response(response_buf));
}

bool cmd_cgatt(char *resp_buf) {
    const char *cmd = "AT+CGATT=1\r";
    for (int a = 0; a < 30; ++a) {
        escape_cr_nl(cmd, temp); ESP_LOGI("RX", "a=%2u | *Command: \"%s\"", a, temp);
        send_at_cmd(UART_NUM_1, cmd, resp_buf);
        // AT+CGATT=1\r\r\nOK\r\n
        escape_cr_nl((const char *) resp_buf, temp); ESP_LOGI("RX", "a=%2u | *Response: \"%s\"", a, temp);
        if (!ok_response(resp_buf)) {
            continue;
        }
        cmd = "AT+CGATT?\r";
        for (int b = 0; b < 30; ++b) {
            escape_cr_nl(cmd, temp); ESP_LOGI("RX", "a=%2u, b=%2u | *Command: \"%s\"", a, b, temp);
            send_at_cmd(UART_NUM_1, cmd, resp_buf);
            // AT+CGATT?\r\r\n+CGATT: 1\r\nOK\r\n
            escape_cr_nl((const char *) resp_buf, temp); ESP_LOGI("RX", "a=%2u, b=%2u | *Response: \"%s\"", a, b, temp);
            if (strcmp(resp_buf, "\r\n+CGATT: 1\r\nOK\r\n") == 0) {
                return true;
            }
        }
    }
    return false;
}

bool query_xiic(char *response_buf) {
    char *result = strstr(response_buf, "\r\n+XIIC: 1,");
    return (result != NULL);
}

bool _add_cert(const char *file_path, const char *cert_name, const char *log_tag, uint16_t cert_buf_size, char *response_buf) {
    ESP_LOGI(log_tag, "Initializing SPIFFS");
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(log_tag, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(log_tag, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(log_tag, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return false;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(log_tag, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(log_tag, "Partition size: total: %zu, used: %zu", total, used);
    }
    
    ESP_LOGI(log_tag, "Reading \"%s\"", file_path);
    FILE* f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(log_tag, "Failed to open \"%s\": %s", file_path, strerror(errno));
        return false;
    }
    char *cert_buf = (char *)malloc(cert_buf_size);    
    uint16_t bytes_read = fread(cert_buf, 1, cert_buf_size, f);    
    cert_buf[bytes_read] = '\0';
    fclose(f);
    ESP_LOGI(log_tag, "Total bytes read from \"%s\": %hu",file_path, bytes_read);
    ESP_LOGI(log_tag, "Content of \"%s\":\n%s", file_path, cert_buf);

    esp_vfs_spiffs_unregister(NULL);
    ESP_LOGI(log_tag, "All done, unmounted partition and disabled SPIFFS.");


    char cmd_buf[BUF_SIZE];
    snprintf(cmd_buf, sizeof (cmd_buf), "AT+CERTADD=%s,%hu\r", cert_name, bytes_read);
    send_at_cmd(UART_NUM_1, cmd_buf, response_buf);
    int8_t result = strcmp(response_buf, "\r\nCONNECT\r\n");
    
    uart_write_bytes(UART_NUM_1, cert_buf, bytes_read);
    free(cert_buf);
    send_at_cmd(UART_NUM_1, "\r\n", response_buf);
    
    if (result == 0 || !ok_response(response_buf)) {
        return false;
    }

    return true;
}

bool add_ca_cert(char *response_buf) {
    return _add_cert("/spiffs/ssl.ca", "rootca.pem", "add_ca_cert", MAX_CA_CERT_SIZE, response_buf);
}

bool add_client_cert(char *response_buf) {
    return _add_cert("/spiffs/ssl.crt", "nwy_all.cert.pem", "add_client_cert", MAX_CLIENT_CERT_SIZE, response_buf);    
}

bool add_client_key(char *response_buf) {
    return _add_cert("/spiffs/ssl.key", "nwy_all.private.key", "add_client_key", MAX_CLIENT_KEY_SIZE, response_buf);    
}

bool query_mqtttls(char *response_buf) {
    int8_t result = strcmp(response_buf, "\r\n+MQTTTLS: 1,1,rootca.pem,nwy_all.cert.pem,nwy_all.private.key\r\nOK\r\n");    
    return (result == 0);
}

bool query_mqttstate(char *response_buf) {
    int8_t result = strcmp(response_buf, "\r\n+MQTTSTATE: 1\r\n\r\nOK\r\n");    
    return (result == 0);
}

void app_main() {
    init();

    struct {
        const char *command;
        RespValiPtr validator;
        uint8_t retry;
        int8_t goto_step;
    } commands_list[] = {   // a="""<logs>"""; ''.join([l.split("'")[1] for l in a.split('I (') if len(l)>0])
        {"AT\r", ok_response, 3, 0},
        // AT\r\r\nOK\r\n
        {"AT+CGSN\r", ok_response, 1, -1},
        // AT+CGSN\r\r\n+CGSN: 862140060226843\r\nOK\r\n                        
        {"AT+CGMM\r", ok_response, 1, -1},
        // AT+CGMM\r\r\n+CGMM: N706\r\nOK\r\n
        {"AT+CPIN?\r", module_ready, 15, 0},
        // AT+CPIN?\r\r\n+CPIN: READY\r\nOK\r\n
        {"AT+CIMI\r", ok_response, 1, -1},
        // AT+CIMI\r\r\n+CIMI: 405864140242618\r\nOK\r\n
        {"AT+CSQ\r", query_csq, 40, 0},
        // AT+CSQ\r\r\n+CSQ: 16,99\r\nOK\r\n
        {"AT+CREG=2\r", ok_response, 1, -1},
        // AT+CREG=2\r\r\nOK\r\n
        {"AT+CREG?\r", query_creg, 40, 0},
        // AT+CREG?\r\r\n+CREG: 2,1,"17cc","0e22f265",7\r\nOK\r\n
        {"AT+CREG=0\r", creg_def_stat, 1, -1},
        // AT+CREG=0\r\r\nOK\r\n
        {"*", cmd_cgatt, 1, 0},
        {"AT$MYSYSINFO\r", ok_response, 3, 0},
        // AT$MYSYSINFO\r\r\n$MYSYSINFO: 4,4\r\nOK\r\n
        {"AT+CGDCONT=1,\"IP\",\"jionet\"\r", ok_response, 3, 0},   // or `airtelgprs`
        // AT+CGDCONT=1,"IP","AIRTELGPRS"\r\r\nOK\r\n
        {"AT+XGAUTH=1,1,\"gsm\",\"1234\"\r", ok_response, 1, -1},   // optional?
        // AT+XGAUTH=1,1,"gsm","1234"\r\r\nOK\r\n
    // module initialization complete
        {"AT+XIIC=1\r", ok_response, 3, 0},
        // AT+XIIC=1\r\r\nOK\r\n
        {"AT+XIIC?\r", query_xiic, 30, 13},
        // AT+XIIC?\r\r\n+XIIC:    1,100.66.152.54\r\n+XIIC:    1,2401:4900:7971:4C79::C36:A899\r\nOK\r\n
    // PPP link established successfully
        {"AT+MQTTTLS=sslmode,1", ok_response, 1, -1},
        // AT+MQTTTLS=sslmode,1     ...kuch toh garbad hai dayaa
        {"AT+MQTTTLS=authmode,1", ok_response, 1, -1},
        // AT+MQTTTLS=authmode,1    ...kuch toh garbad hai dayaa
        {"AT\r", add_ca_cert, 1, -1},
        {"AT\r", add_client_cert, 1, -1},
        {"AT\r", add_client_key, 1, -1},
        {"AT+SSLTCPCFG=cacert,rootca.pem\r", ok_response, 1, -1},
        // AT+SSLTCPCFG=cacert,rootca.pem\r\r\nOK\r\n
        {"AT+SSLTCPCFG=clientcert,nwy_all.cert.pem\r", ok_response, 1, -1},
        // AT+SSLTCPCFG=clientcert,nwy_all.cert.pem\r\r\nOK\r\n
        {"AT+SSLTCPCFG=clientkey,nwy_all.private.key\r", ok_response, 1, -1},
        // AT+SSLTCPCFG=clientkey,nwy_all.private.key\r\r\nOK\r\n
        {"AT+MQTTTLS=rootca,rootca.pem\r", ok_response, 1, -1},
        // AT+MQTTTLS=rootca,rootca.pem\r\r\nOK\r\n
        {"AT+MQTTTLS=clientcert,nwy_all.cert.pem\r", ok_response, 1, -1},
        // AT+MQTTTLS=clientcert,nwy_all.cert.pem\r\r\nOK\r\n
        {"AT+MQTTTLS=clientkey,nwy_all.private.key\r", ok_response, 1, -1},
        // AT+MQTTTLS=clientkey,nwy_all.private.key\r\r\nOK\r\n
        {"AT+MQTTTLS?\r", query_mqtttls, 1, -1},
        // AT+MQTTTLS?\r\r\n+MQTTTLS:0,0,rootca.pem,nwy_all.cert.pem,nwy_all.private.key,3\r\nOK\r\n
    // MQTTS encryption parameters configured for two-way authentication
        {"AT+MQTTCONNPARAM=\"007\",\"james_bond\",\"james_bond\"\r", ok_response, 1, 15},
        // AT+MQTTCONNPARAM="007","james_bond","james_bond"\r\r\nOK\r\n
        {"AT+MQTTWILLPARAM=0,1,\"lastWillAndTestament\",\"Sayonara!\"\r", ok_response, 1, 16},
        // AT+MQTTWILLPARAM=0,1,"lastWillAndTestament","Sayonara!"\r\r\nOK\r\n
        {"AT+MQTTCONN=\"broker.mqtt.cool:1883\",0,60\r", ok_response, 1, 17},   // navigate to `https://testclient-cloud.mqtt.cool/` and connect to `tcp://broker.mqtt.cool:1883` broker
        // AT+MQTTCONN="broker.mqtt.cool:1883",0,60\r\r\nOK\r\n
        {"AT+MQTTSUB=\"rainbow\",1\r", ok_response, 3, 17},
        // AT+MQTTSUB="rainbow",1\r\r\nOK\r\n
        {"AT+MQTTPUB=0,1,\"rainbow\",\"Hello! This is Neoway's N706 Cat.1 Module!\"\r", ok_response, 1, 19},
        // AT+MQTTPUB=0,1,"rainbow","Hello! This is Neoway's N706 Cat.1 Module!"\r\r\nOK\r\n\r\n+MQTTSUB:3,"rainbow",42,Hello! This is Neoway's N706 Cat.1 Module!\r\n
        {"AT+MQTTSTATE?\r", query_mqttstate, 1, -1},
        // AT+MQTTSTATE?\r\r\n+MQTTSTATE: 1\r\n\r\nOK\r\n
        {"AT+MQTTDISCONN\r", ok_response, 1, -1},
        // AT+MQTTDISCONN\r\r\nOK\r\n
    };

    char resp[BUF_SIZE];
    
    uint8_t i = 0;
    size_t t = sizeof (commands_list) / sizeof (commands_list[0]);
    while (i < t) {
        const char *cmd = commands_list[i].command;
        uint8_t j = 0, retry = commands_list[i].retry;
        for (; j < retry; ++j) {
            escape_cr_nl(cmd, temp); ESP_LOGI("TX", "i=%2u, j=%2u | Command: \"%s\"", i, j, temp);
            if (strcmp(cmd, "*") != 0) {
                send_at_cmd(UART_NUM_1, cmd, resp);
                escape_cr_nl((const char *) resp, temp); ESP_LOGI("RX", "i=%2u, j=%2u | Response: \"%s\"", i, j, temp);
            } else {
                ESP_LOGI("RX", "i=%2u, j=%2u | Chained Commands:", i, j);
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
    
    deinit();
}
