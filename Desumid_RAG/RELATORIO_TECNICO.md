# Relatório Técnico - Sistema de Monitoramento e Controle de Umidade e Temperatura

## 1. Introdução
Este projeto tem como objetivo monitorar e controlar condições ambientais de um ambiente de desumidificação, com foco em temperatura, umidade relativa e pressão atmosférica. O sistema foi desenvolvido para operar em um microcontrolador ESP32 e utiliza sensores digitais para leitura contínua das variáveis ambientais, além de atuar sobre dispositivos de aquecimento e ventilação.

O sistema contempla:
- Sensor AHT20 para temperatura e umidade relativa
- Sensor BMP280 para temperatura e pressão
- Sensor DS18B20 para monitoramento da temperatura do aquecedor
- Aquecedor controlado pelo GPIO 18
- Ventoinha controlada pelo GPIO 19

## 2. Objetivo do sistema
O sistema foi projetado para:
- medir temperatura e umidade do ambiente;
- medir temperatura e pressão atmosférica;
- manter monitoramento do aquecedor;
- controlar a ventoinha e o aquecedor conforme regras de segurança e conforto;
- emitir alertas em situações críticas, como superaquecimento.

## 3. Arquitetura do sistema
O sistema é composto por:
- sensores conectados à interface I2C e One-Wire;
- atuadores ligados aos GPIOs do ESP32;
- lógica de controle implementada no laço principal da aplicação;
- mensagens de log em console para acompanhamento em tempo real.

### 3.1. Sensor AHT20
O AHT20 é conectado ao barramento I2C do ESP32. Ele fornece:
- temperatura ambiente
- umidade relativa

### 3.2. Sensor BMP280
O BMP280 também usa comunicação I2C e realiza:
- leitura de temperatura ambiente
- leitura de pressão atmosférica

### 3.3. Sensor DS18B20
O DS18B20 usa protocolo One-Wire e é conectado ao GPIO 4. Sua função principal é:
- monitorar a temperatura do aquecedor para evitar superaquecimento.

### 3.4. Atuadores
- Aquecedor: GPIO 18
- Ventoinha: GPIO 19

## 4. Hardware e configuração
### 4.1. Pinos usados
- SDA I2C: GPIO 21
- SCL I2C: GPIO 22
- OneWire: GPIO 4
- Aquecedor: GPIO 18
- Ventoinha: GPIO 19

### 4.2. Inicialização dos GPIOs
Os dois atuadores são configurados como saídas digitais:
- GPIO_MODE_OUTPUT
- sem pull-up
- sem pull-down
- sem interrupções

No início da execução, os atuadores são forçados para nível baixo, garantindo que permaneçam desligados até que a lógica de controle decida sua ativação.

## 5. Inicialização do sistema
A inicialização segue a sequência abaixo:
1. Configuração dos GPIOs de saída;
2. Inicialização da camada I2C com `i2cdev_init()`;
3. Inicialização do AHT20;
4. Inicialização do BMP280;
5. Detecção do DS18B20 no barramento One-Wire;
6. Entrada no laço principal de operação.

### 5.1. AHT20
O sensor é inicializado por meio do descritor `aht_t`, e o endereço I2C usado é `AHT_I2C_ADDRESS_GND`. A leitura é feita com a função `aht_get_data()`.

### 5.2. BMP280
O BMP280 é inicializado com `bmp280_init_default_params()` e `bmp280_init()`. A leitura da pressão é feita pela função `bmp280_read_float()`, sendo a pressão convertida de Pascal para hPa antes de exibição.

### 5.3. DS18B20
O sensor é detectado com `ds18x20_scan_devices()`. Se não houver dispositivo encontrado, o sistema entra em estado de erro e mantém o ESP32 em loop de espera. Isso evita funcionamento indevido sem sensor.

## 6. Lógica de controle
A lógica de controle é implementada dentro do laço principal e considera principalmente:
- temperatura do aquecedor;
- umidade relativa do ambiente.

### 6.1. Situação de emergência
Se a temperatura do aquecedor atingir ou exceder 50 °C:
- aquecedor desligado;
- ventoinha ligada;
- log de alerta emitido.

Essa abordagem evita superaquecimento do sistema e protege o hardware.

### 6.2. Umidade acima do limite
Se a umidade relativa for maior que 45%:
- aquecedor ligado;
- ventoinha ligada;
- mensagem de controle exibida.

### 6.3. Condição estável
Se a umidade estiver dentro do limite e não houver risco de superaquecimento:
- aquecedor desligado;
- ventoinha desligada.

## 7. Fluxo operacional
O algoritmo principal segue este fluxo:
1. Comando de conversão do DS18B20;
2. Espera de 750 ms para conclusão da conversão;
3. Leitura do AHT20;
4. Leitura do BMP280;
5. Leitura do DS18B20;
6. Exibição dos valores via serial;
7. Análise das condições;
8. Ativação/desativação dos atuadores;
9. Delay de 1,75 s antes do próximo ciclo.

## 8. Observações técnicas
- O AHT20 e o BMP280 utilizam comunicação I2C.
- O DS18B20 utiliza protocolo One-Wire, exigindo cuidado com o pull-up.
- A leitura de pressão do BMP280 deve ser convertida para hPa para facilitar a interpretação.
- O sistema foi implementado com foco em controle de segurança e operação estável.

## 9. Pontos de atenção
Alguns pontos importantes devem ser verificados no projeto real:
- Confirmar a tensão e as ligações do sensor DS18B20;
- Validar a presença do resistor pull-up de 4,7 kΩ no barramento One-Wire;
- Confirmar o endereço I2C correto do BMP280 (`0x76` ou `0x77`, conforme módulo utilizado);
- Verificar se os sensores estão funcionando corretamente em conjunto no mesmo barramento I2C;
- Garantir que a alimentação do aquecedor e da ventoinha esteja compatível com o módulo e com a carga.

## 10. Conclusão
O sistema desenvolvido oferece uma solução prática e eficiente para monitoramento ambiental e controle de umidade em ambientes que exigem estabilidade operacional. A arquitetura combina leitura de sensores, lógica de decisão e atuação em tempo real, garantindo controle automatizado de temperatura e umidade, com proteção contra superaquecimento do aquecedor.

Esse projeto pode ser expandido futuramente com:
- registro em memória;
- envio de dados via Wi-Fi/Bluetooth;
- interface gráfica em dashboard;
- ajuste dinâmico dos limites de temperatura e umidade;
- análise histórica dos dados coletados.