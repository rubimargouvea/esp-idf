#include <stdio.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define I2C_TIMEOUT_MS 50

static const char *TAG = "I2C_SCANNER";

void i2c_scanner_task(void *pvParameters)
{
    //Configura e inicializa o I2C master bus
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };

    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    while(1){
        ESP_LOGI(TAG, "Starting I2C scan...");

        uint8_t found_addresses[128] = {0};
        int devices_found = 0;

        //OPCIONAL
        printf("   0 1 2 3 4 5 6 7 8 9 a b c d e f\n ");

        for (int addr = 0x00; addr <= 0x7F; addr++) {
            if (addr % 16 == 0){
                printf("%02x: ", addr);
            }
            // Pular chamada geral (0x00) e endereços reservados
            esp_err_t ret = i2c_master_probe(bus_handle, addr, I2C_TIMEOUT_MS);
            if (ret == ESP_OK) {
                printf("%02x ", addr);
                if (addr >=0x08 && addr <= 0x77) { // Encontrar contador de 7 bits?
                    found_addresses[devices_found++] = addr;
                }
            } else if (ret == ESP_ERR_NOT_FOUND) {
                printf("-- ");
            } else {
                printf("UU "); // timeout ou outro erro
            }
             
            if ((addr + 1) % 16 == 0) {
                printf("\n");
            }
            
        }
        //Imprime sumario
        printf("\nEscaneamento completo: %d dispositivo(s) encontrados.\n", devices_found);
        for (int i = 0; i < devices_found; i++) {
            printf("%d. Dispositivo 0x%02x address\n", i + 1 , found_addresses[i]);
        }
        if (devices_found == 0) {
            printf("\n");
        }
        //Repetir escaneamento apos 10 segundos
        vTaskDelay(pdMS_TO_TICKS(1000));
    }    
}
void app_main(void)
{
    ESP_LOGI(TAG, "I2C scanner starting on SDA=%d, SCL=%d", I2C_SDA_PIN, I2C_SCL_PIN);
    xTaskCreate(i2c_scanner_task, "i2c_scanner", 4096, NULL, 5, NULL);
}
