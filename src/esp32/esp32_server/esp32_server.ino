#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Credenciais da rede Wi-Fi
const char* ssid = "Wifi";
const char* password = "12345678";

WebServer server(80);

// Configuração do Display OLED (I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Pinos dos Sensores e do Buzzer
const int oneWireBus = 4;  // Ambos os sensores DS18B20 no pino 4
const int buzzerPin = 23;  // Buzzer no pino 23

OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);

// Endereço físico (8 bytes, único de fábrica) de cada DS18B20.
// Para preencher: veja os endereços no Monitor Serial (115200).
// Se ficarem zerados, usa a ordem do barramento.
DeviceAddress enderecoInterno = {0, 0, 0, 0, 0, 0, 0, 0};
DeviceAddress enderecoExterno = {0, 0, 0, 0, 0, 0, 0, 0};

// Leitura não bloqueante: o tempo é controlado com millis()
const unsigned long INTERVALO_LEITURA_MS = 2000; // lê a cada 2 s
const unsigned long TEMPO_CONVERSAO_MS   = 750;  // tempo do DS18B20 em 12 bits

unsigned long ultimaLeitura = 0;    // quando a última leitura começou
bool aguardandoConversao = false;   // true = sensores estão medindo

// Últimos valores lidos (usados pela página web e pelo OLED)
float tempInterna = 0.0;
float tempExterna = 0.0;
bool internaOk = false;   // false = sensor desconectado ou com falha
bool externaOk = false;

void atualizarDisplay();

// Verifica se um endereço ainda está zerado (não preenchido)
bool enderecoVazio(const DeviceAddress endereco) {
  for (int i = 0; i < 8; i++) {
    if (endereco[i] != 0) return false;
  }
  return true;
}

// Imprime o endereço no formato que pode ser copiado para o código
void imprimirEndereco(const DeviceAddress endereco) {
  Serial.print("{");
  for (int i = 0; i < 8; i++) {
    Serial.print("0x");
    if (endereco[i] < 0x10) Serial.print("0");
    Serial.print(endereco[i], HEX);
    if (i < 7) Serial.print(", ");
  }
  Serial.println("}");
}

// Lê um sensor; retorna false se ele não responder (-127)
bool lerSensor(const DeviceAddress endereco, float &temperatura) {
  float valor = sensors.getTempC(endereco);
  if (valor == DEVICE_DISCONNECTED_C) {
    return false;  // mantém o último valor, mas marca como falha
  }
  temperatura = valor;
  return true;
}

// Pede a medição e, após o tempo de conversão, busca o resultado
void atualizarSensores() {
  unsigned long agora = millis();

  // Hora de uma nova leitura: inicia a medição
  if (!aguardandoConversao && agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    sensors.requestTemperatures();
    ultimaLeitura = agora;
    aguardandoConversao = true;
  }

  // Conversão concluída: lê os valores
  if (aguardandoConversao && agora - ultimaLeitura >= TEMPO_CONVERSAO_MS) {
    aguardandoConversao = false;

    bool estavaOkInterna = internaOk;
    bool estavaOkExterna = externaOk;

    internaOk = lerSensor(enderecoInterno, tempInterna);
    externaOk = lerSensor(enderecoExterno, tempExterna);

    // Informa no Serial apenas quando o estado do sensor muda
    if (estavaOkInterna != internaOk) {
      Serial.println(internaOk ? "Sensor interno OK" : "FALHA no sensor interno");
    }
    if (estavaOkExterna != externaOk) {
      Serial.println(externaOk ? "Sensor externo OK" : "FALHA no sensor externo");
    }

    atualizarDisplay();
  }
}

void atualizarDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("   MONITOR DE FREEZER"));
  display.println(F("-------------------------"));

  display.print(F("Interna: "));
  if (internaOk) { display.print(tempInterna, 1); display.println(F(" C")); }
  else           { display.println(F("FALHA")); }

  display.print(F("Externa: "));
  if (externaOk) { display.print(tempExterna, 1); display.println(F(" C")); }
  else           { display.println(F("FALHA")); }

  display.print(F("IP: ")); display.println(WiFi.localIP());
  display.display();
}

// Card de temperatura: borda vermelha acima de -10 °C ou em falha
String montarCard(const char* titulo, float temp, bool ok, const char* classeTemp, const char* legenda) {
  bool alerta = !ok || temp > -10.0;
  String borda = alerta ? "border-bottom: 5px solid #dc3545;" : "border-bottom: 5px solid #28a745;";
  String valor;
  if (ok) valor = String(temp, 1) + " &deg;C";
  else    valor = "FALHA";

  String card = "<div class='card' style='" + borda + "'><h3>" + titulo + "</h3>";
  card += "<div class='temp " + String(classeTemp) + "'>" + valor + "</div>";
  card += "<p>" + String(legenda) + "</p></div>";
  return card;
}

void handleRoot() {
  // Montagem da página HTML profissional COM AUTO-REFRESH
  String html = "<html lang='pt-BR'><head><meta charset='UTF-8'>";

  // Atualiza a página a cada 2 segundos
  html += "<meta http-equiv='refresh' content='2'>";

  html += "<title>Monitor IoT - Freezers</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;background-color:#eef2f5;color:#333;margin:0;padding:40px;text-align:center;}";
  html += ".header{margin-bottom:30px;} h1{color:#2c3e50;}";
  html += ".container{display:flex;justify-content:center;gap:20px;flex-wrap:wrap;}";
  html += ".card{background:#fff;border-radius:12px;box-shadow:0 4px 8px rgba(0,0,0,0.1);padding:25px;width:240px;border-bottom: 5px solid #28a745;}";
  html += ".temp{font-size:3em;font-weight:bold;color:#007bff;margin:15px 0;}";
  html += ".temp-ext{color:#f39c12;}";
  html += ".badge{background:#17a2b8;color:white;padding:5px 10px;border-radius:20px;font-size:0.8em;}</style></head><body>";

  html += "<div class='header'><h1>❄️ Painel de Monitoramento IoT</h1><p>ESP32 Servidor Embarcado <span class='badge'>ONLINE</span></p></div>";
  html += "<div class='container'>";

  html += montarCard("Temp. Interna", tempInterna, internaOk, "", "Sensor DS18B20 #1");
  html += montarCard("Temp. Externa", tempExterna, externaOk, "temp-ext", "Sensor DS18B20 #2");

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);

  // Garante que o buzzer inicie e permaneça desligado
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, LOW);

  // Inicializa barramento de sensores
  sensors.begin();
  sensors.setWaitForConversion(false);  // requestTemperatures() não bloqueia

  int quantidade = sensors.getDeviceCount();
  Serial.print("Sensores encontrados: ");
  Serial.println(quantidade);

  // Lista o endereço de cada sensor encontrado
  for (int i = 0; i < quantidade; i++) {
    DeviceAddress endereco;
    if (sensors.getAddress(endereco, i)) {
      Serial.print("  Sensor ");
      Serial.print(i);
      Serial.print(": ");
      imprimirEndereco(endereco);
    }
  }

  // Se os endereços não foram preenchidos, usa a ordem do barramento
  if (enderecoVazio(enderecoInterno) || enderecoVazio(enderecoExterno)) {
    Serial.println("AVISO: enderecos nao configurados. Usando ordem do barramento.");
    sensors.getAddress(enderecoInterno, 0);
    sensors.getAddress(enderecoExterno, 1);
  }

  // Inicializa I2C e Display OLED
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Falha ao iniciar o OLED"));
  }

  // Conecta ao Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi Conectado!");
  Serial.print("IP do ESP32: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.begin();

  // Primeira leitura imediata
  ultimaLeitura = millis() - INTERVALO_LEITURA_MS;
}

void loop() {
  server.handleClient();
  atualizarSensores();
}