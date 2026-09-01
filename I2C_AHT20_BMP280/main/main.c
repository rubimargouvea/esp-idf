#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Inclusão dos cabeçalhos das bibliotecas baixadas
#include <i2cdev.h> 
#include <bmp280.h>
#include <aht.h>

// Definições físicas fixadas de acordo com os pinos seguros descobertos
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define I2C_PORT_NUM 0

static const char *TAG = "ESTACAO_METEO";

void app_main(void)
{
    // -------------------------------------------------------------------------
    // 1. INICIALIZAÇÃO DA CAMADA DE HARDWARE I2C (i2cdev)
    // -------------------------------------------------------------------------
    // A biblioteca esp-idf-lib gerencia internamente os barramentos usando i2cdev
    ESP_ERROR_CHECK(i2cdev_init());

    // -------------------------------------------------------------------------
    // 2. CONFIGURAÇÃO E INICIALIZAÇÃO DO SENSOR AHT20 (0x38)
    // -------------------------------------------------------------------------
    aht_t aht_device = { 0 };
    aht_device.type = AHT_TYPE_AHT20; // Define explicitamente o modelo AHT20

    // Vincula o descritor de software do AHT20 aos pinos físicos e endereço
    while (aht_init_desc(&aht_device, AHT_I2C_ADDRESS_GND, I2C_PORT_NUM, I2C_SDA_PIN, I2C_SCL_PIN) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao mapear pinos do AHT20. Tentando novamente...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Inicializa o chip físico AHT20 (executa calibração interna em segundo plano)
    while (aht_init(&aht_device) != ESP_OK) {
        ESP_LOGE(TAG, "Falha na calibração/inicialização física do AHT20.");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Sensor AHT20 inicializado com sucesso!");

    // -------------------------------------------------------------------------
    // 3. CONFIGURAÇÃO E INICIALIZAÇÃO DO SENSOR BMP280 (0x77)
    // -------------------------------------------------------------------------
    bmp280_params_t bmp_params;
    bmp280_init_default_params(&bmp_params); // Obtém configurações seguras de leitura contínua
    
    bmp280_t bmp_device = { 0 };

    // Vincula o descritor de software do BMP280 aos pinos físicos e ao endereço 0x77
    while (bmp280_init_desc(&bmp_device, BMP280_I2C_ADDRESS_1, I2C_PORT_NUM, I2C_SDA_PIN, I2C_SCL_PIN) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao mapear pinos do BMP280. Tentando novamente...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Inicializa o chip físico BMP280 e confere se o ID interno bate com o datasheet
    while (bmp280_init(&bmp_device, &bmp_params) != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização física do chip BMP280.");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Sensor BMP280 inicializado com sucesso!");

    // Variáveis que vão armazenar os dados finais já convertidos
    float aht_temperature, aht_humidity;
    float bmp_temperature, bmp_pressure;

    ESP_LOGI(TAG, "Iniciando monitoramento meteorológico unificado...\n");

    // -------------------------------------------------------------------------
    // 4. LOOP PRINCIPAL DE LEITURA (A cada 2 segundos)
    // -------------------------------------------------------------------------
    while (1) {
        // Solicita leitura ao AHT20
        esp_err_t res_aht = aht_get_data(&aht_device, &aht_temperature, &aht_humidity);
        
        // Solicita leitura ao BMP280 (Passamos NULL na umidade pois o BMP280 não possui esse recurso)
        esp_err_t res_bmp = bmp280_read_float(&bmp_device, &bmp_temperature, &bmp_pressure, NULL);

        printf("====================== LEITURA ATUAL ======================\n");
        
        // Trata os dados coletados do AHT20
        if (res_aht == ESP_OK) {
            printf("[AHT20]  -> Temperatura: %.2f °C  |  Umidade: %.2f %%\n", aht_temperature, aht_humidity);
        } else {
            ESP_LOGE(TAG, "Erro ao ler dados do sensor AHT20.");
        }

        // Trata os dados coletados do BMP280
        if (res_bmp == ESP_OK) {
            // Divide o resultado da pressão por 100 para converter de Pascal (Pa) para Hectopascal (hPa)
            float pressure_hpa = bmp_pressure / 100.0;
            printf("[BMP280] -> Temperatura: %.2f °C  |  Pressão: %.2f hPa\n", bmp_temperature, pressure_hpa);
        } else {
            ESP_LOGE(TAG, "Erro ao ler dados do sensor BMP280.");
        }

        printf("===========================================================\n\n");

        // Tempo de espera seguro recomendado para evitar autoaquecimento dos sensores
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
