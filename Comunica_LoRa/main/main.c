#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

/* ============================================================================
 * Projeto: comunicacao entre dois kits ESP32 + SX1276 + OLED
 * Funcao: transmitir mensagens numeradas e identificar o kit de origem
 * ========================================================================== */
#define TAG "COMUNICA_LORA"

/* ----------------------------- Hardware ---------------------------------- */
/* Barramento SPI utilizado pelo transceptor SX1276. */
#define LORA_SCK GPIO_NUM_14
#define LORA_MISO GPIO_NUM_16
#define LORA_MOSI GPIO_NUM_13
#define LORA_CS GPIO_NUM_25
#define LORA_RST GPIO_NUM_26

/* Barramento I2C utilizado pelo display OLED SSD1306. */
#define OLED_SDA GPIO_NUM_5
#define OLED_SCL GPIO_NUM_4
#define OLED_ADDRESS 0x3C

#define LORA_FREQUENCY_HZ 915000000UL
#define TRANSMISSION_INTERVAL_US (5LL * 60LL * 1000000LL)

/*
 * Identificacao dos kits:
 * - Firmware do kit 1: LORA_NODE_ID = 1
 * - Firmware do kit 2: LORA_NODE_ID = 2
 */
#define LORA_NODE_ID 1
#define LORA_PEER_ID (LORA_NODE_ID == 1 ? 2 : 1)

/* -------------------------- Registradores LoRa --------------------------- */
enum {
	REG_FIFO = 0x00,
	REG_OP_MODE = 0x01,
	REG_FRF_MSB = 0x06,
	REG_FRF_MID = 0x07,
	REG_FRF_LSB = 0x08,
	REG_PA_CONFIG = 0x09,
	REG_FIFO_ADDR_PTR = 0x0D,
	REG_FIFO_TX_BASE_ADDR = 0x0E,
	REG_FIFO_RX_BASE_ADDR = 0x0F,
	REG_FIFO_RX_CURRENT_ADDR = 0x10,
	REG_IRQ_FLAGS = 0x12,
	REG_RX_NB_BYTES = 0x13,
	REG_MODEM_CONFIG_1 = 0x1D,
	REG_MODEM_CONFIG_2 = 0x1E,
	REG_PREAMBLE_MSB = 0x20,
	REG_PREAMBLE_LSB = 0x21,
	REG_PAYLOAD_LENGTH = 0x22,
	REG_MODEM_CONFIG_3 = 0x26,
	REG_DIO_MAPPING_1 = 0x40,
	REG_VERSION = 0x42,
};

#define LORA_LONG_RANGE_MODE 0x80
#define LORA_MODE_SLEEP 0x00
#define LORA_MODE_STDBY 0x01
#define LORA_MODE_TX 0x03
#define LORA_MODE_RX_CONTINUOUS 0x05
#define LORA_IRQ_TX_DONE 0x08
#define LORA_IRQ_RX_DONE 0x40
#define LORA_IRQ_PAYLOAD_CRC_ERROR 0x20

static spi_device_handle_t lora_spi;

/* Escreve um byte em um registrador do SX1276 atraves do SPI. */
static esp_err_t lora_write(uint8_t address, uint8_t value)
{
	uint8_t data[2] = { (uint8_t)(address | 0x80), value };
	spi_transaction_t transaction = {
		.length = 16,
		.tx_buffer = data,
	};
	return spi_device_transmit(lora_spi, &transaction);
}

/* Le um byte de um registrador do SX1276 atraves do SPI. */
static uint8_t lora_read(uint8_t address)
{
	uint8_t tx_data[2] = { (uint8_t)(address & 0x7F), 0 };
	uint8_t rx_data[2] = { 0 };
	spi_transaction_t transaction = {
		.length = 16,
		.tx_buffer = tx_data,
		.rx_buffer = rx_data,
	};
	ESP_ERROR_CHECK(spi_device_transmit(lora_spi, &transaction));
	return rx_data[1];
}

/* ------------------------------- OLED ------------------------------------ */
static void oled_command(uint8_t command)
{
	uint8_t data[2] = { 0x00, command };
	ESP_ERROR_CHECK(i2c_master_write_to_device(I2C_NUM_0, OLED_ADDRESS, data,
											   sizeof(data), pdMS_TO_TICKS(100)));
}

static void oled_init(void)
{
	const uint8_t commands[] = {
		0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
		0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F, 0xD9, 0xF1,
		0xDB, 0x40, 0xA4, 0xA6, 0xAF,
	};

	for (size_t index = 0; index < sizeof(commands); index++) {
		oled_command(commands[index]);
	}
}

/* Limpa as oito paginas do display OLED de 128 x 64 pixels. */
static void oled_clear(void)
{
	uint8_t data[129] = { 0x40 };
	for (uint8_t page = 0; page < 8; page++) {
		oled_command(0xB0 | page);
		oled_command(0x00);
		oled_command(0x10);
		ESP_ERROR_CHECK(i2c_master_write_to_device(I2C_NUM_0, OLED_ADDRESS, data,
												   sizeof(data), pdMS_TO_TICKS(100)));
	}
}

static const uint8_t *oled_glyph(char character)
{
	static const uint8_t digits[10][5] = {
		{ 0x3E, 0x51, 0x49, 0x45, 0x3E }, { 0x00, 0x42, 0x7F, 0x40, 0x00 },
		{ 0x42, 0x61, 0x51, 0x49, 0x46 }, { 0x21, 0x41, 0x45, 0x4B, 0x31 },
		{ 0x18, 0x14, 0x12, 0x7F, 0x10 }, { 0x27, 0x45, 0x45, 0x45, 0x39 },
		{ 0x3C, 0x4A, 0x49, 0x49, 0x30 }, { 0x01, 0x71, 0x09, 0x05, 0x03 },
		{ 0x36, 0x49, 0x49, 0x49, 0x36 }, { 0x06, 0x49, 0x49, 0x29, 0x1E },
	};
	static const uint8_t tx[] = { 0x7E, 0x09, 0x09, 0x09, 0x06 };
	static const uint8_t xx[] = { 0x63, 0x14, 0x08, 0x14, 0x63 };
	static const uint8_t rr[] = { 0x7F, 0x09, 0x19, 0x29, 0x46 };
	static const uint8_t blank[] = { 0, 0, 0, 0, 0 };

	if (character >= '0' && character <= '9') {
		return digits[character - '0'];
	}
	if (character == 'T') {
		return tx;
	}
	if (character == 'X') {
		return xx;
	}
	if (character == 'R') {
		return rr;
	}
	return blank;
}

/* Mostra caracteres numericos e os indicadores TX/RX no OLED. */
static void oled_show(const char *text)
{
	uint8_t data[129] = { 0x40 };
	oled_command(0xB0);
	oled_command(0x00);
	oled_command(0x10);
	for (size_t index = 0; text[index] != '\0' && index < 20; index++) {
		const uint8_t *glyph = oled_glyph(text[index]);
		memcpy(&data[1], glyph, 5);
		data[6] = 0;
		ESP_ERROR_CHECK(i2c_master_write_to_device(I2C_NUM_0, OLED_ADDRESS, data, 7,
												   pdMS_TO_TICKS(100)));
	}
}

/* ------------------------------ Transceptor ------------------------------- */
/* Configura frequencia, potencia, modem e modo inicial do SX1276. */
static void lora_init(void)
{
	gpio_set_level(LORA_RST, 0);
	vTaskDelay(pdMS_TO_TICKS(10));
	gpio_set_level(LORA_RST, 1);
	vTaskDelay(pdMS_TO_TICKS(10));

	uint8_t version = lora_read(REG_VERSION);
	ESP_LOGI(TAG, "SX1276 versao: 0x%02X", version);
	ESP_ERROR_CHECK(lora_write(REG_OP_MODE, LORA_LONG_RANGE_MODE | LORA_MODE_SLEEP));
	vTaskDelay(pdMS_TO_TICKS(10));

	uint64_t frf = ((uint64_t)LORA_FREQUENCY_HZ << 19) / 32000000ULL;
	ESP_ERROR_CHECK(lora_write(REG_FRF_MSB, (uint8_t)(frf >> 16)));
	ESP_ERROR_CHECK(lora_write(REG_FRF_MID, (uint8_t)(frf >> 8)));
	ESP_ERROR_CHECK(lora_write(REG_FRF_LSB, (uint8_t)frf));
	ESP_ERROR_CHECK(lora_write(REG_FIFO_TX_BASE_ADDR, 0x00));
	ESP_ERROR_CHECK(lora_write(REG_FIFO_RX_BASE_ADDR, 0x00));
	ESP_ERROR_CHECK(lora_write(REG_PA_CONFIG, 0x80 | 0x0F));
	ESP_ERROR_CHECK(lora_write(REG_MODEM_CONFIG_1, 0x72));
	ESP_ERROR_CHECK(lora_write(REG_MODEM_CONFIG_2, 0x74));
	ESP_ERROR_CHECK(lora_write(REG_MODEM_CONFIG_3, 0x04));
	ESP_ERROR_CHECK(lora_write(REG_PREAMBLE_MSB, 0x00));
	ESP_ERROR_CHECK(lora_write(REG_PREAMBLE_LSB, 0x08));
	ESP_ERROR_CHECK(lora_write(REG_DIO_MAPPING_1, 0x40));
	ESP_ERROR_CHECK(lora_write(REG_IRQ_FLAGS, 0xFF));
	ESP_ERROR_CHECK(lora_write(REG_OP_MODE, LORA_LONG_RANGE_MODE | LORA_MODE_STDBY));
}

static bool lora_receive(char *message, size_t message_size)
{
	uint8_t irq_flags = lora_read(REG_IRQ_FLAGS);
	if (!(irq_flags & LORA_IRQ_RX_DONE)) {
		return false;
	}

	if (irq_flags & LORA_IRQ_PAYLOAD_CRC_ERROR) {
		ESP_ERROR_CHECK(lora_write(REG_IRQ_FLAGS, irq_flags));
		return false;
	}

	uint8_t length = lora_read(REG_RX_NB_BYTES);
	if (length >= message_size) {
		length = (uint8_t)(message_size - 1);
	}
	ESP_ERROR_CHECK(lora_write(REG_FIFO_ADDR_PTR, lora_read(REG_FIFO_RX_CURRENT_ADDR)));
	for (uint8_t index = 0; index < length; index++) {
		message[index] = (char)lora_read(REG_FIFO);
	}
	message[length] = '\0';
	ESP_ERROR_CHECK(lora_write(REG_IRQ_FLAGS, irq_flags));
	return true;
}

static bool parse_message(const char *message, int *source, uint32_t *sequence)
{
	int destination;
		unsigned long parsed_sequence;
	bool valid = sscanf(message, "SRC=%d;DST=%d;SEQ=%lu", source, &destination,
						&parsed_sequence) == 3;
		if (valid) {
			*sequence = (uint32_t)parsed_sequence;
		}
		return valid && destination == LORA_NODE_ID;
}

/* Coloca um pacote no FIFO e aguarda a conclusao da transmissao. */
static void lora_send(const char *message)
{
	size_t length = strlen(message);
/* Le um pacote recebido e o retira do FIFO do SX1276. */
	if (length > 255) {
		return;
	}

	ESP_ERROR_CHECK(lora_write(REG_OP_MODE, LORA_LONG_RANGE_MODE | LORA_MODE_STDBY));
/* Aceita somente pacotes destinados a este kit e extrai origem e contador. */
	ESP_ERROR_CHECK(lora_write(REG_FIFO_ADDR_PTR, 0x00));
	for (size_t index = 0; index < length; index++) {
		ESP_ERROR_CHECK(lora_write(REG_FIFO, (uint8_t)message[index]));
	}
	ESP_ERROR_CHECK(lora_write(REG_PAYLOAD_LENGTH, (uint8_t)length));
	ESP_ERROR_CHECK(lora_write(REG_IRQ_FLAGS, 0xFF));
	ESP_ERROR_CHECK(lora_write(REG_OP_MODE, LORA_LONG_RANGE_MODE | LORA_MODE_TX));

	int64_t deadline = esp_timer_get_time() + 10000000;
	while (!(lora_read(REG_IRQ_FLAGS) & LORA_IRQ_TX_DONE)) {
		if (esp_timer_get_time() > deadline) {
			ESP_LOGE(TAG, "Timeout na transmissao LoRa");
			return;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	ESP_ERROR_CHECK(lora_write(REG_IRQ_FLAGS, LORA_IRQ_TX_DONE));
	ESP_ERROR_CHECK(lora_write(REG_OP_MODE, LORA_LONG_RANGE_MODE | LORA_MODE_RX_CONTINUOUS));
}

/* -------------------------- Inicializacao --------------------------------- */
/* Configura GPIO, SPI e I2C antes de iniciar os perifericos. */
static void hardware_init(void)
{
	gpio_config_t lora_output = {
		.pin_bit_mask = (1ULL << LORA_CS) | (1ULL << LORA_RST),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&lora_output));
	gpio_set_level(LORA_CS, 1);

	spi_bus_config_t spi_config = {
		.mosi_io_num = LORA_MOSI,
		.miso_io_num = LORA_MISO,
		.sclk_io_num = LORA_SCK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi_config, SPI_DMA_CH_AUTO));
	spi_device_interface_config_t device_config = {
		.clock_speed_hz = 8000000,
		.mode = 0,
		.spics_io_num = LORA_CS,
		.queue_size = 1,
	};
	ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device_config, &lora_spi));

	i2c_config_t i2c_config = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = OLED_SDA,
		.scl_io_num = OLED_SCL,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 400000,
	};
	ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_config));
	ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, i2c_config.mode, 0, 0, 0));
}

/* ------------------------------ Aplicacao --------------------------------- */
void app_main(void)
{
	/* O contador permanece salvo mesmo depois de desligar o ESP32. */
	ESP_ERROR_CHECK(nvs_flash_init());
	hardware_init();
	oled_init();
	oled_clear();
	lora_init();

	nvs_handle_t nvs_handle;
	uint32_t counter = 0;
	ESP_ERROR_CHECK(nvs_open("comunicacao", NVS_READWRITE, &nvs_handle));
	nvs_get_u32(nvs_handle, "contador", &counter);
	ESP_ERROR_CHECK(lora_write(REG_OP_MODE, LORA_LONG_RANGE_MODE | LORA_MODE_RX_CONTINUOUS));
	/* Desloca o primeiro envio do kit 2 para reduzir colisao na inicializacao. */
	int64_t next_transmission = LORA_NODE_ID == 1 ? 0 : TRANSMISSION_INTERVAL_US / 2;

	while (true) {
		char received[64];
		if (lora_receive(received, sizeof(received))) {
			int source;
			uint32_t received_sequence;
			if (parse_message(received, &source, &received_sequence)) {
				ESP_LOGI(TAG, "Recebido do kit %d: %s", source, received);
				oled_clear();
				char display[24];
				snprintf(display, sizeof(display), "RX %d %lu", source,
						 (unsigned long)received_sequence);
				oled_show(display);
			}
		}

		if (esp_timer_get_time() >= next_transmission) {
			char message[64];
			counter++;
			snprintf(message, sizeof(message), "SRC=%d;DST=%d;SEQ=%lu", LORA_NODE_ID,
					 LORA_PEER_ID, (unsigned long)counter);
			ESP_LOGI(TAG, "Kit %d transmitindo para kit %d: %s", LORA_NODE_ID,
					 LORA_PEER_ID, message);
			oled_clear();
			char display[24];
			snprintf(display, sizeof(display), "TX %d %lu", LORA_NODE_ID,
					 (unsigned long)counter);
			oled_show(display);
			lora_send(message);
			ESP_ERROR_CHECK(nvs_set_u32(nvs_handle, "contador", counter));
			ESP_ERROR_CHECK(nvs_commit(nvs_handle));
			next_transmission = esp_timer_get_time() + TRANSMISSION_INTERVAL_US;
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}
