#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include <string.h>


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

#define WIFI_SSID "RAG"
#define WIFI_PASSWORD "AER101023"
#define UBIDOTS_TOKEN "BBUS-RgZxBqNhqPJ80lxtNuY3W4Lm7cDQeV"
#define UBIDOTS_DEVICE "desumidificador"
#define UBIDOTS_MQTT_URI "mqtt://industrial.api.ubidots.com:1883"
#define UBIDOTS_TOPIC "/v1.6/devices/" UBIDOTS_DEVICE

static const char *TAG = "DESUMID_RAG";
static EventGroupHandle_t wifi_event_group;
static esp_mqtt_client_handle_t mqtt_client;
static const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi desconectado; tentando novamente");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi conectado");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Conectado ao Ubidots");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT desconectado");
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Erro na conexão MQTT");
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
}

static void mqtt_init(void)
{
    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = UBIDOTS_MQTT_URI,
        .credentials.username = UBIDOTS_TOKEN,
        .credentials.authentication.password = "",
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_config);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}

static void publish_sensor_data(float aht_temperature, float aht_humidity,
                                float bmp_temperature, float bmp_pressure,
                                float ds_heater_temperature, bool heater_on,
                                bool fan_on)
{
    if (mqtt_client == NULL ||
        strcmp(UBIDOTS_TOKEN, "COLOQUE_SEU_TOKEN_UBIDOTS_AQUI") == 0) {
        return;
    }

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"aht_temperature\":%.2f,\"humidity\":%.2f,"
             "\"bmp_temperature\":%.2f,\"pressure\":%.2f,"
             "\"heater_temperature\":%.2f,\"heater\":%d,\"fan\":%d}",
             aht_temperature, aht_humidity, bmp_temperature, bmp_pressure,
             ds_heater_temperature, heater_on, fan_on);

    int message_id = esp_mqtt_client_publish(mqtt_client, UBIDOTS_TOPIC,
                                              payload, 0, 1, 0);
    ESP_LOGI(TAG, "Dados publicados no Ubidots (id=%d)", message_id);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    mqtt_init();

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
        bool heater_on;
        bool fan_on;
        if (res_ds == ESP_OK && ds_heater_temperature >= 50.0) {
            gpio_set_level(GPIO_AQUECEDOR, 0); // Desliga calor imediatamente
            gpio_set_level(GPIO_VENTOINHA, 1); // Mantém ventoinha para resfriamento rápido
            heater_on = false;
            fan_on = true;
            printf("[ALERTA]   -> EMERGÊNCIA: Aquecedor superaquecido! Forçando resfriamento.\n");
        }
        else if (aht_humidity > 45.0) {
            gpio_set_level(GPIO_AQUECEDOR, 1);
            gpio_set_level(GPIO_VENTOINHA, 1);
            heater_on = true;
            fan_on = true;
            printf("[CONTROLE] -> Umidade acima de 45%%! Atuadores ligados para resfriamento e desumidificação.\n");
        }
        else {
            gpio_set_level(GPIO_AQUECEDOR, 0);
            gpio_set_level(GPIO_VENTOINHA, 0);
            heater_on = false;
            fan_on = false;
            printf("[CONTROLE] -> Umidade estável. Atuadores desligados.\n");
        }
          
        // Trata os dados coletados do BMP280
        if (res_bmp == ESP_OK) {
            // Divide o resultado da pressão por 100 para converter de Pascal (Pa) para Hectopascal (hPa)
            bmp_pressure = bmp_pressure / 100.0;
            printf("[BMP280] -> Temperatura: %.2f °C  |  Pressão: %.2f hPa\n", bmp_temperature, bmp_pressure);
        }

        if (res_aht == ESP_OK && res_bmp == ESP_OK && res_ds == ESP_OK) {
            publish_sensor_data(aht_temperature, aht_humidity, bmp_temperature,
                                bmp_pressure, ds_heater_temperature, heater_on,
                                fan_on);
        }

        printf("===========================================================\n\n");
    
        // Tempo de espera seguro recomendado para evitar autoaquecimento dos sensores
        vTaskDelay(pdMS_TO_TICKS(1750));
}
}


