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

void handleRoot() {
  sensors.requestTemperatures();
  
  float tempInterna = sensors.getTempCByIndex(0);
  float tempExterna = sensors.getTempCByIndex(1);

  if (tempInterna == -127.00) tempInterna = 0.0;
  if (tempExterna == -127.00) tempExterna = 0.0;

  // Montagem da página HTML profissional COM AUTO-REFRESH
  String html = "<html lang='pt-BR'><head><meta charset='UTF-8'>";
  
  // Atualiza a página a cada 2 segundos
  html += "<meta http-equiv='refresh' content='2'>"; 
  
  html += "<title>Monitor IoT - Freezers</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;background-color:#eef2f5;color:#333;margin:0;padding:40px;text-align:center;}";
  html += ".header{margin-bottom:30px;} h1{color:#2c3e50;}";
  html += ".container{display:flex;justify-content:center;gap:20px;flex-wrap:wrap;}";
  html += ".card{background:#fff;border-radius:12px;box-shadow:0 4px 8px rgba(0,0,0,0.1);padding:25px;width:240px;border-bottom: 5px solid #28a745;}";
  
  // Lógica visual: Muda a cor da borda do card se o respectivo sensor estiver acima do limite de -10°C
  String borderInterna = (tempInterna > -10.0) ? "border-bottom: 5px solid #dc3545;" : "border-bottom: 5px solid #28a745;";
  String borderExterna = (tempExterna > -10.0) ? "border-bottom: 5px solid #dc3545;" : "border-bottom: 5px solid #28a745;";
  
  html += ".temp{font-size:3em;font-weight:bold;color:#007bff;margin:15px 0;}";
  html += ".temp-ext{color:#f39c12;}";
  html += ".badge{background:#17a2b8;color:white;padding:5px 10px;border-radius:20px;font-size:0.8em;}</style></head><body>";

  html += "<div class='header'><h1>❄️ Painel de Monitoramento IoT</h1><p>ESP32 Servidor Embarcado <span class='badge'>ONLINE</span></p></div>";
  html += "<div class='container'>";
  
  // Card Temp Interna
  html += "<div class='card' style='" + borderInterna + "'><h3>Temp. Interna</h3><div class='temp'>" + String(tempInterna, 1) + " &deg;C</div><p>Sensor DS18B20 #1</p></div>";
  
  // Card Temp Externa
  html += "<div class='card' style='" + borderExterna + "'><h3>Temp. Externa</h3><div class='temp temp-ext'>" + String(tempExterna, 1) + " &deg;C</div><p>Sensor DS18B20 #2</p></div>";
  
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
  Serial.print("Sensores encontrados: ");
  Serial.println(sensors.getDeviceCount());

  // Inicializa I2C e Display OLED
  Wire.begin(21, 22);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
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
}

void loop() {
  server.handleClient();

  // Atualiza leituras para exibir também no Display OLED localmente
  sensors.requestTemperatures();
  float t1 = sensors.getTempCByIndex(0);
  float t2 = sensors.getTempCByIndex(1);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("   MONITOR DE FREEZER"));
  display.println(F("-------------------------"));
  display.print(F("Sensor 1: ")); display.print(t1, 1); display.println(F(" C"));
  display.print(F("Sensor 2: ")); display.print(t2, 1); display.println(F(" C"));
  display.print(F("IP: ")); display.println(WiFi.localIP());
  display.display();

  delay(2000); // Atualiza a cada 2 segundos
}