#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)
#define BUF_SIZE (1024)

void app_main() {
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

    const char *commands[] = {
        "AT",   // OK
        "AT+CGSN",  // +CGSN: <IMEI_NUMBER>\nOK
        "AT+CGMM",  // +CGMM: N706\nOK
        "AT+CPIN?", // +CPIN: READY\nOK
        "AT+CIMI",  // +CIMI: <IMSI_NUMBER>\nOK
        "AT+CSQ",   // +CSQ: <signal>,<ber>\nOK    -- condition: 12 <= signal <= 31
        "AT+CREG=2",    // OK
        "AT+CREG?", // +CREG: 2,1\nOK    -- or `+CREG: 2,5\nOK`
        "AT+CREG=0",    // OK
        "AT+CGATT=1",   // OK
        "AT+CGATT?",    // +CGATT: 1
        "AT$MYSYSINFO", // OK
        "AT+CGDCONT=1,\"IP\",\"jionet\"",   // OK    -- or `airtelgprs`
        // "AT+XGAUTH=1,1,\"gsm\",\"1234\"",   // OK    -- this is optional i guess, hence can be skipped
        "AT+XIIC=1",    // OK
        "AT+XIIC?", // +XIIC:1,<IP_ADDRESS>\nOK
        "AT+MQTTCONNPARAM=\"007\",\"james_bond\",\"james_bond\"",   // OK
        "AT+MQTTWILLPARAM=0,1,\"lastWillAndTestament\",\"Sayonara!\"",  // OK
        "AT+MQTTCONN=\"broker.mqtt.cool:1883\",0,60",   // OK    -- navigate to `https://testclient-cloud.mqtt.cool/` and connect to `tcp://broker.mqtt.cool:1883` broker
        "AT+MQTTSUB=\"rainbow\",1", // OK
        "AT+MQTTPUB=0,1,\"rainbow\",\"Hello! This is a message published under the topic rainbow.\"",   // OK
        "AT+MQTTSTATE?",    // +MQTTSTATE: 1\nOK
        "AT+MQTTDISCONN",   // OK
    };  // length = 22

    char response[BUF_SIZE];

    esp_log_level_set("RX", ESP_LOG_INFO);
    esp_log_level_set("TX", ESP_LOG_INFO);

    for (int i = 0; i < 22; ++i) {
        uart_write_bytes(UART_NUM_1, commands[i], strlen(commands[i]));
        vTaskDelay(1000 / portTICK_PERIOD_MS); // delay to ensure the command is sent and response is received
        uart_read_bytes(UART_NUM_1, (uint8_t *)response, BUF_SIZE, 1000 / portTICK_PERIOD_MS);

        ESP_LOGI("TX", "Command: %s", commands[i]);
        ESP_LOGI("RX", "Response: %s", response);

        // parsing and processing the response...

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    uart_driver_delete(UART_NUM_1);
}
