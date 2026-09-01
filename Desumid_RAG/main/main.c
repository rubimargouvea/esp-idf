#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"


// Inclusão dos cabeçalhos das bibliotecas baixadas
#include <i2cdev.h> 
#include <bmp280.h>
#include <aht.h>
#include <ds18x20.h>

// Definições do BArramento I2C
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define I2C_PORT_NUM 0

//Definição do Sensor OneWire
#define ONEWIRE_GPIO 4

//Definicoes dos atuadores
#define GPIO_AQUECEDOR 18
#define GPIO_VENTOINHA 19

static const char *TAG = "DESUMID_RAG";

void app_main(void)
{
     // -------------------------------------------------------------------------
    // 1. CONFIGURAÇÃO DAS SAÍDAS DIGITAIS (Aquecedor e Ventoinha)
    // -------------------------------------------------------------------------
    // Criamos uma máscara de bits combinando os dois pinos selecionados
    uint64_t pin_mask = (1ULL << GPIO_AQUECEDOR) | (1ULL << GPIO_VENTOINHA);
    
    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,            // Configura estritamente como saída
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE       // Sem interrupções nestes pinos
    };
    gpio_config(&io_conf);

    // Garante que os atuadores iniciem DESLIGADOS (nível lógico baixo)
    gpio_set_level(GPIO_AQUECEDOR, 0);
    gpio_set_level(GPIO_VENTOINHA, 0);
    // -------------------------------------------------------------------------
    // 2. INICIALIZAÇÃO DA CAMADA DE HARDWARE I2C (i2cdev)
    // -------------------------------------------------------------------------
    // A biblioteca esp-idf-lib gerencia internamente os barramentos usando i2cdev
    ESP_ERROR_CHECK(i2cdev_init());

    // -------------------------------------------------------------------------
    // 3. CONFIGURAÇÃO E INICIALIZAÇÃO DO SENSOR AHT20 (0x38)
    // -------------------------------------------------------------------------
    aht_t aht_device = { 0 };
    aht_device.type = AHT_TYPE_AHT20; // Define explicitamente o modelo AHT20

    // Vincula o descritor de software do AHT20 aos pinos físicos e endereço
    while (aht_init_desc(&aht_device, AHT_I2C_ADDRESS_GND, I2C_PORT_NUM, I2C_SDA_PIN, I2C_SCL_PIN) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao mapear pinos do AHT20. Tentando novamente...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    // -------------------------------------------------------------------------
    // 4. CONFIGURAÇÃO E INICIALIZAÇÃO DO SENSOR BMP280 (0x77)
    // -------------------------------------------------------------------------
    bmp280_params_t bmp_params;
    bmp280_init_default_params(&bmp_params); // Obtém configurações padronizadas do BMP280
    bmp280_t bmp_device = { 0 };
    
    // Vincula o descritor de software do BMP280 aos pinos físicos e ao endereço 0x77
    while (bmp280_init_desc(&bmp_device, BMP280_I2C_ADDRESS_1, I2C_PORT_NUM, I2C_SDA_PIN, I2C_SCL_PIN) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao mapear pinos do BMP280...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    bmp280_init(&bmp_device, &bmp_params); // Inicializa o BMP280 com os parâmetros padrão

    ESP_LOGI(TAG, "Sensor BMP280 inicializado com sucesso!");


    //-------------------------------------------------------------------------
    // 5. Inicializando o DS18B20 conectado ao barramento One-Wire
    //-------------------------------------------------------------------------
    ds18x20_addr_t ds_addr;
    size_t devices_found = 0;

    while (1) {
        ESP_LOGI(TAG, "Escaneando barramento One-Wire no GPIO %d...", ONEWIRE_GPIO);
        // Procura por apenas 1 dispositivo conectado no pino
        esp_err_t res_ds = ds18x20_scan_devices(ONEWIRE_GPIO, &ds_addr, 1, &devices_found);
        if (res_ds != ESP_OK || devices_found == 0) {
            ESP_LOGE(TAG, "Nenhum sensor DS18B20 encontrado no barramento One-Wire. Verifique o resistor de pull-up de 4.7k.");
            while(1) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            ESP_LOGI(TAG, "DS18B20 Detectado com sucesso!");
            break; // Sai do loop se o sensor for encontrado
        }
    }
        // Variáveis que vão armazenar os dados finais já convertidos
    float aht_temperature, aht_humidity;
    float bmp_temperature, bmp_pressure;
    float ds_heater_temperature;
     
    ESP_LOGI(TAG, "Iniciando Sistema de Monitoramento e Controle de Umidade!\n");

    // -------------------------------------------------------------------------
    // 6. LOOP PRINCIPAL COM TOMADA DE DECISÃO
    // -------------------------------------------------------------------------
    while (1) {
        // Envia comando de conversão ao DS18B20 e aguarda a conclusão da medição
        ds18x20_measure(ONEWIRE_GPIO, ds_addr, true);
        vTaskDelay(pdMS_TO_TICKS(750)); // Tempo de espera para conversão (750ms para 12 bits)
        // Coleta de dados dos sensores AHT20, BMP280 e DS18B20
        
        // Solicita leitura ao AHT20
        esp_err_t res_aht = aht_get_data(&aht_device, &aht_temperature, &aht_humidity);       
        // Solicita leitura ao BMP280 (Passamos NULL na umidade pois o BMP280 não possui esse recurso)
        esp_err_t res_bmp = bmp280_read_float(&bmp_device, &bmp_temperature, &bmp_pressure, NULL);
        // Solicita leitura ao DS18B20
        esp_err_t res_ds = ds18x20_read_temperature(ONEWIRE_GPIO, ds_addr, &ds_heater_temperature);
        
        if (res_ds == ESP_OK) {
            printf("[DS18B20] -> Temperatura do aquecedor: %.2f °C\n", ds_heater_temperature);
        } else {
            ESP_LOGE(TAG, "Erro de leitura noDS18B20.");
        }

        if(res_aht == ESP_OK) {
            printf("[AHT20]  -> Temperatura: %.2f °C  |  Umidade: %.2f %%\n", aht_temperature, aht_humidity);
        } else {
            ESP_LOGE(TAG, "Erro ao ler dados do sensor AHT20.");
        }
       
        printf("====================== LEITURA ATUAL ======================\n");
        
        // Tomada de decisão com base nos dados coletados do AHT20
        if (res_ds == ESP_OK && ds_heater_temperature >= 50.0) {
            gpio_set_level(GPIO_AQUECEDOR, 0); // Desliga calor imediatamente
            gpio_set_level(GPIO_VENTOINHA, 1); // Mantém ventoinha para resfriamento rápido
            printf("[ALERTA]   -> EMERGÊNCIA: Aquecedor superaquecido! Forçando resfriamento.\n");
        }
        else if (aht_humidity > 45.0) {
            gpio_set_level(GPIO_AQUECEDOR, 1);
            gpio_set_level(GPIO_VENTOINHA, 1);
            printf("[CONTROLE] -> Umidade acima de 45%%! Atuadores ligados para resfriamento e desumidificação.\n");
        }
        else {
            gpio_set_level(GPIO_AQUECEDOR, 0);
            gpio_set_level(GPIO_VENTOINHA, 0);
            printf("[CONTROLE] -> Umidade estável. Atuadores desligados.\n");
        }
          
        // Trata os dados coletados do BMP280
        if (res_bmp == ESP_OK) {
            // Divide o resultado da pressão por 100 para converter de Pascal (Pa) para Hectopascal (hPa)
            bmp_pressure = bmp_pressure / 100.0;
            printf("[BMP280] -> Temperatura: %.2f °C  |  Pressão: %.2f hPa\n", bmp_temperature, bmp_pressure);
        }

        printf("===========================================================\n\n");
    
        // Tempo de espera seguro recomendado para evitar autoaquecimento dos sensores
        vTaskDelay(pdMS_TO_TICKS(1750));
}
}


