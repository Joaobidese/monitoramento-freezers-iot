# Sistema IoT de Monitoramento de Freezers

Telemetria térmica contínua de freezers: um ESP32 mede a temperatura interna e externa, mostra os valores em um display OLED e em um painel web, grava um log persistente e emite alertas (sonoros e visuais) para evitar a perda de insumos.

## Integrantes da Equipe (Grupo 4)

- João Vitor Bidese
- Pedro Henrique Borges
- Pietro Maruska
- Pedro Paiva
- Rafael Velasques

## Funcionalidades

- Leitura não bloqueante de 2 sensores DS18B20 a cada 2 s
- Alerta quando a temperatura passa do limite (-10 °C)
- Temperaturas mínima e máxima desde a inicialização
- Detecção de falha persistente de sensor (5 leituras seguidas sem resposta)
- Log persistente na flash (LittleFS), com rotação a partir de 16 KB
- Data e hora reais via NTP (horário de Brasília, UTC-3)
- Reconexão automática do Wi-Fi a cada 10 s, sem travar o sistema
- Atuador (buzzer) controlado pela rede
- Painel web com atualização automática e API JSON

## Hardware Utilizado

| Componente | Quantidade |
|---|---|
| Microcontrolador ESP32 | 1 |
| Sensor de temperatura DS18B20 (interno e externo) | 2 |
| Display OLED SSD1306 128x64 (I2C) | 1 |
| Buzzer ativo | 1 |
| Resistor 4,7 kΩ (pull-up do barramento OneWire) | 1 |

### Ligações

| Componente | Pino do componente | Pino do ESP32 |
|---|---|---|
| DS18B20 (os dois, em paralelo) | DQ (dados) | GPIO 4 (com pull-up de 4,7 kΩ para 3,3 V) |
| DS18B20 | VDD / GND | 3,3 V / GND |
| OLED SSD1306 (endereço 0x3C) | SDA / SCL | GPIO 21 / GPIO 22 |
| OLED SSD1306 | VCC / GND | 3,3 V / GND |
| Buzzer | + / - | GPIO 23 / GND |

## Documentação da API / Comunicação

**Protocolo escolhido:** HTTP (servidor web embarcado na porta 80).

| Rota | Método | Descrição |
|---|---|---|
| `/` | GET | Painel web com temperaturas, rede e atuador (atualiza a cada 2 s) |
| `/log` | GET | Log do sistema gravado na flash (antigo + atual) |
| `/atuador?estado=on` | GET | Liga o buzzer e redireciona para o painel (303) |
| `/atuador?estado=off` | GET | Desliga o buzzer e redireciona para o painel (303) |
| `/status` | GET | Estado completo do sistema em JSON |

Comando inválido em `/atuador` responde **400** e não altera o estado.

### Exemplo de resposta de `/status`

```json
{
  "data_hora": "10/11/2026 14:40:00",
  "uptime_s": 3600,
  "hora_sincronizada": true,
  "limite_temperatura": -10.0,
  "interna": { "ok": true, "temperatura": -18.25, "minima": -19.50, "maxima": -17.00,
               "falhas_seguidas": 0, "alerta": false },
  "externa": { "ok": false, "temperatura": null, "minima": 24.00, "maxima": 26.50,
               "falhas_seguidas": 7, "alerta": false },
  "wifi": { "conectado": true, "ssid": "MinhaRede", "ip": "192.168.0.50", "rssi": -61 },
  "atuador_ligado": false,
  "log_disponivel": true
}
```

Valores sem leitura válida aparecem como `null`.

### Testes rápidos com curl

```bash
curl http://<IP_DO_ESP32>/status
curl -i "http://<IP_DO_ESP32>/atuador?estado=on"
curl -i "http://<IP_DO_ESP32>/atuador?estado=off"
```

## Instruções de Instalação e Execução

1. Instale a [Arduino IDE](https://www.arduino.cc/en/software) e adicione o suporte ao ESP32 (Gerenciador de Placas → **esp32** da Espressif).
2. Instale as bibliotecas pelo Gerenciador de Bibliotecas:
   - Adafruit SSD1306
   - Adafruit GFX Library
   - OneWire
   - DallasTemperature

   (`WiFi`, `WebServer` e `LittleFS` já vêm com o pacote do ESP32.)
3. Na pasta `src/esp32/esp32_server/`, copie `config.example.h` para `config.h` e preencha o nome e a senha da sua rede Wi-Fi. O `config.h` está no `.gitignore` e não vai para o repositório.
4. Abra `src/esp32/esp32_server/esp32_server.ino`, selecione a placa **ESP32 Dev Module** e a porta correta, e clique em **Carregar**.
5. Abra o Monitor Serial em **115200** baud. O IP do ESP32 aparece no log e também no display OLED.
6. Acesse `http://<IP_DO_ESP32>/` em um navegador na mesma rede.

### Identificação dos sensores (opcional)

Na inicialização, o Monitor Serial lista o endereço de cada DS18B20. Para fixar qual é o interno e qual é o externo, copie os endereços para `enderecoInterno` e `enderecoExterno` no código. Se ficarem zerados, o sistema usa a ordem do barramento.
