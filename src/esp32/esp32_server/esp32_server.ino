#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LittleFS.h>
#include <time.h>

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
const int buzzerPin = 23;  // Buzzer (atuador) no pino 23

// Estado do atuador: começa desligado
bool atuadorLigado = false;
const int FREQUENCIA_BUZZER = 2000;  // Hz (som do buzzer)

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

// Limite de temperatura do freezer: acima disso é alerta
const float LIMITE_TEMPERATURA = -10.0;

// Data e hora via NTP (horário de Brasília, UTC-3, sem horário de verão)
const char* FUSO_HORARIO = "<-03>3";
const char* SERVIDOR_NTP_1 = "pool.ntp.org";
const char* SERVIDOR_NTP_2 = "a.st1.ntp.br";
bool horaSincronizada = false;

// Reconexão do Wi-Fi
const unsigned long INTERVALO_RECONEXAO_MS = 10000;  // tenta reconectar a cada 10 s
bool wifiConectado = false;          // estado na última verificação
unsigned long inicioQueda = 0;       // quando a conexão caiu
unsigned long ultimaTentativa = 0;   // última tentativa de reconexão
bool alertaInterna = false;  // true = temperatura está acima do limite
bool alertaExterna = false;

// Log persistente na flash (LittleFS)
const char* ARQUIVO_LOG = "/log.txt";
const char* ARQUIVO_LOG_ANTIGO = "/log_antigo.txt";
const size_t LIMITE_LOG_BYTES = 16 * 1024;  // acima de 16 KB o log é rotacionado
bool logDisponivel = false;                 // false = LittleFS não montou

// Temperaturas mínima e máxima desde que o sistema ligou (ficam na RAM)
float minInterna = 0.0, maxInterna = 0.0;
float minExterna = 0.0, maxExterna = 0.0;
bool temRegistroInterna = false;  // false = ainda não houve leitura válida
bool temRegistroExterna = false;

void atualizarDisplay();
String montarCardAtuador();

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

// Horário do registro: data e hora reais (NTP) ou, sem internet,
// o tempo desde que o sistema ligou (+hh:mm:ss)
String dataHora() {
  time_t agora = time(nullptr);           // segundos desde 01/01/1970
  if (agora > 1700000000) {               // relógio já foi acertado pelo NTP
    struct tm local;
    localtime_r(&agora, &local);          // converte para o fuso configurado
    char texto[24];
    strftime(texto, sizeof(texto), "%d/%m/%Y %H:%M:%S", &local);
    return String(texto);
  }
  unsigned long s = millis() / 1000;
  char texto[16];
  snprintf(texto, sizeof(texto), "+%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return String(texto);
}

// Grava uma linha no log (Serial + arquivo na flash). nivel: INFO, AVISO ou ERRO
void registrarLog(const char* nivel, const String &mensagem) {
  String linha = "[" + dataHora() + "] " + nivel + " " + mensagem;
  Serial.println(linha);
  if (!logDisponivel) return;

  // Arquivo passou do limite: vira o log antigo (o antigo anterior é apagado)
  if (LittleFS.exists(ARQUIVO_LOG)) {
    File atual = LittleFS.open(ARQUIVO_LOG, FILE_READ);
    size_t tamanho = atual.size();
    atual.close();
    if (tamanho > LIMITE_LOG_BYTES) {
      LittleFS.remove(ARQUIVO_LOG_ANTIGO);
      LittleFS.rename(ARQUIVO_LOG, ARQUIVO_LOG_ANTIGO);
    }
  }

  // FILE_APPEND: escreve no final do arquivo, sem apagar o que já existe
  File arquivo = LittleFS.open(ARQUIVO_LOG, FILE_APPEND);
  if (arquivo) {
    arquivo.println(linha);
    arquivo.close();
  }
}

// Lê um arquivo inteiro da flash ("" se não existir)
String lerArquivo(const char* caminho) {
  if (!LittleFS.exists(caminho)) return "";
  File arquivo = LittleFS.open(caminho, FILE_READ);
  String conteudo = arquivo.readString();
  arquivo.close();
  return conteudo;
}

// Registra quando a temperatura passa do limite ou volta ao normal
void verificarLimite(const char* nome, bool ok, float temp, bool &emAlerta) {
  if (!ok) return;  // sem leitura válida não há o que comparar
  bool acima = temp > LIMITE_TEMPERATURA;
  if (acima && !emAlerta) {
    registrarLog("AVISO", "Temp. " + String(nome) + " acima do limite: " + String(temp, 1) + " C");
  }
  if (!acima && emAlerta) {
    registrarLog("INFO", "Temp. " + String(nome) + " normalizada: " + String(temp, 1) + " C");
  }
  emAlerta = acima;
}

// Liga ou desliga o buzzer e registra o comando no log
// tone() gera um sinal de 2000 Hz: funciona com buzzer ativo e passivo
void definirAtuador(bool ligar) {
  atuadorLigado = ligar;
  if (ligar) {
    tone(buzzerPin, FREQUENCIA_BUZZER);
  } else {
    noTone(buzzerPin);
    digitalWrite(buzzerPin, LOW);
  }
  registrarLog("INFO", ligar ? "Atuador LIGADO via rede" : "Atuador DESLIGADO via rede");
}

// Atualiza o mínimo e o máximo com uma nova leitura válida
void atualizarMinMax(float temp, float &minimo, float &maximo, bool &temRegistro) {
  if (!temRegistro) {
    // Primeira leitura válida: vira o mínimo e o máximo
    minimo = temp;
    maximo = temp;
    temRegistro = true;
    return;
  }
  if (temp < minimo) minimo = temp;
  if (temp > maximo) maximo = temp;
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

    // Leituras com falha não entram no mínimo e no máximo
    if (internaOk) atualizarMinMax(tempInterna, minInterna, maxInterna, temRegistroInterna);
    if (externaOk) atualizarMinMax(tempExterna, minExterna, maxExterna, temRegistroExterna);

    // Registra no log apenas quando o estado do sensor muda
    if (estavaOkInterna != internaOk) {
      if (internaOk) registrarLog("INFO", "Sensor interno OK");
      else           registrarLog("ERRO", "Falha no sensor interno (sem leitura)");
    }
    if (estavaOkExterna != externaOk) {
      if (externaOk) registrarLog("INFO", "Sensor externo OK");
      else           registrarLog("ERRO", "Falha no sensor externo (sem leitura)");
    }

    verificarLimite("interna", internaOk, tempInterna, alertaInterna);
    verificarLimite("externa", externaOk, tempExterna, alertaExterna);

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
  display.print(F("Wi-Fi: "));
  display.println(WiFi.status() == WL_CONNECTED ? F("OK") : F("OFF"));
  display.print(F("Buzzer: "));
  display.println(atuadorLigado ? F("ON") : F("OFF"));
  display.display();
}

// Card de temperatura: borda vermelha acima de -10 °C ou em falha
String montarCard(const char* titulo, float temp, bool ok, const char* classeTemp, const char* legenda,
                  float minimo, float maximo, bool temRegistro) {
  bool alerta = !ok || temp > LIMITE_TEMPERATURA;
  String borda = alerta ? "border-bottom: 5px solid #dc3545;" : "border-bottom: 5px solid #28a745;";
  String valor;
  if (ok) valor = String(temp, 1) + " &deg;C";
  else    valor = "FALHA";

  String card = "<div class='card' style='" + borda + "'><h3>" + titulo + "</h3>";
  card += "<div class='temp " + String(classeTemp) + "'>" + valor + "</div>";
  // Mínimo e máximo registrados ("--" se ainda não houver leitura válida)
  String textoMin = "--";
  String textoMax = "--";
  if (temRegistro) {
    textoMin = String(minimo, 1) + " &deg;C";
    textoMax = String(maximo, 1) + " &deg;C";
  }
  card += "<p class='minmax'>M&iacute;n: <b>" + textoMin + "</b> | M&aacute;x: <b>" + textoMax + "</b></p>";

  card += "<p>" + String(legenda) + "</p></div>";
  return card;
}

// Card com os dados de rede: SSID, IP, MAC e status da conexão
String montarCardRede(bool conectado) {
  String card = "<div class='card rede'><h3>Conex&atilde;o</h3>";
  card += "<p><b>Rede:</b> " + WiFi.SSID() + "</p>";
  card += "<p><b>IP:</b> " + WiFi.localIP().toString() + "</p>";
  card += "<p><b>MAC:</b> " + WiFi.macAddress() + "</p>";
  card += "<p><b>Status:</b> ";
  card += conectado ? "<span class='ok'>&#9679; Conectado</span>" : "<span class='erro'>&#9679; Desconectado</span>";
  card += "</p></div>";
  return card;
}

void handleRoot() {
  bool conectado = (WiFi.status() == WL_CONNECTED);

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
  html += ".minmax{color:#555;font-size:0.95em;}";
  html += ".badge{color:white;padding:5px 10px;border-radius:20px;font-size:0.8em;}";
  html += ".badge-ok{background:#28a745;} .badge-erro{background:#dc3545;}";
  html += ".rede{border-bottom:5px solid #17a2b8;text-align:left;} .rede p{margin:8px 0;}";
  html += ".ok{color:#28a745;font-weight:bold;} .erro{color:#dc3545;font-weight:bold;}";
  html += ".atuador{border-bottom:5px solid #6f42c1;}";
  html += ".botao{display:inline-block;margin:6px;padding:10px 18px;border-radius:8px;color:#fff;text-decoration:none;font-weight:bold;}";
  html += ".ligar{background:#28a745;} .desligar{background:#dc3545;}</style></head><body>";

  // Selo de status baseado no estado real do Wi-Fi
  String selo = conectado ? "<span class='badge badge-ok'>CONECTADO</span>"
                          : "<span class='badge badge-erro'>DESCONECTADO</span>";
  html += "<div class='header'><h1>&#10052;&#65039; Painel de Monitoramento IoT</h1><p>ESP32 Servidor Embarcado " + selo + "</p>";
  html += "<p><a href='/log'>Ver log do sistema</a></p></div>";
  html += "<div class='container'>";

  html += montarCard("Temp. Interna", tempInterna, internaOk, "", "Sensor DS18B20 #1",
                     minInterna, maxInterna, temRegistroInterna);
  html += montarCard("Temp. Externa", tempExterna, externaOk, "temp-ext", "Sensor DS18B20 #2",
                     minExterna, maxExterna, temRegistroExterna);

  html += montarCardRede(conectado);
  html += montarCardAtuador();

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// Card do atuador: estado atual e botões que chamam a rota /atuador
String montarCardAtuador() {
  String card = "<div class='card atuador'><h3>Atuador (Buzzer)</h3>";
  card += "<p><b>Estado:</b> ";
  if (atuadorLigado) card += "<span class='erro'>&#9679; LIGADO</span>";
  else               card += "<span class='ok'>&#9679; DESLIGADO</span>";
  card += "</p>";
  card += "<a class='botao ligar' href='/atuador?estado=on'>Ligar</a>";
  card += "<a class='botao desligar' href='/atuador?estado=off'>Desligar</a>";
  card += "</div>";
  return card;
}

// Rota /atuador?estado=on|off: recebe o comando pela rede
void handleAtuador() {
  String estado = server.arg("estado");  // valor depois de "estado=" na URL

  if (estado == "on") {
    definirAtuador(true);
  } else if (estado == "off") {
    definirAtuador(false);
  } else {
    // Comando inválido: responde erro 400 e não muda nada
    registrarLog("AVISO", "Comando invalido em /atuador: '" + estado + "'");
    server.send(400, "text/plain", "Comando invalido. Use /atuador?estado=on ou /atuador?estado=off");
    return;
  }

  // Volta para o painel (303 = redirecionamento); o texto aparece em testes com curl
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", atuadorLigado ? "Atuador LIGADO" : "Atuador DESLIGADO");
}

// Rota /log: mostra o log gravado na flash (antigo + atual)
void handleLog() {
  String html = "<html lang='pt-BR'><head><meta charset='UTF-8'><title>Log do Sistema</title>";
  html += "<style>body{font-family:'Segoe UI', Tahoma, sans-serif;background:#eef2f5;margin:0;padding:30px;}";
  html += "pre{background:#fff;border-radius:8px;padding:15px;box-shadow:0 2px 6px rgba(0,0,0,0.1);white-space:pre-wrap;}</style></head><body>";
  html += "<h2>Log do Sistema</h2><p><a href='/'>&larr; Voltar ao painel</a></p>";
  if (!logDisponivel) {
    html += "<p>Sistema de arquivos indispon&iacute;vel.</p>";
  }
  html += "<pre>" + lerArquivo(ARQUIVO_LOG_ANTIGO) + lerArquivo(ARQUIVO_LOG) + "</pre></body></html>";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);

  // Monta o sistema de arquivos da flash (true = formata na primeira vez)
  logDisponivel = LittleFS.begin(true);
  if (!logDisponivel) {
    registrarLog("ERRO", "Falha ao montar o LittleFS: log so no Serial");
  }
  registrarLog("INFO", "Sistema iniciado");

  // Garante que o buzzer inicie e permaneça desligado
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, LOW);

  // Inicializa barramento de sensores
  sensors.begin();
  sensors.setWaitForConversion(false);  // requestTemperatures() não bloqueia

  int quantidade = sensors.getDeviceCount();
  registrarLog("INFO", "Sensores encontrados: " + String(quantidade));
  if (quantidade < 2) {
    registrarLog("AVISO", "Esperados 2 sensores, encontrados " + String(quantidade));
  }

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
    registrarLog("ERRO", "Falha ao iniciar o display OLED");
  }

  // Conecta ao Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  registrarLog("INFO", "Wi-Fi conectado. IP " + WiFi.localIP().toString());
  wifiConectado = true;

  // Pede a hora aos servidores NTP (a resposta chega em segundo plano)
  configTzTime(FUSO_HORARIO, SERVIDOR_NTP_1, SERVIDOR_NTP_2);

  server.on("/", handleRoot);
  server.on("/log", handleLog);
  server.on("/atuador", handleAtuador);
  server.begin();

  // Primeira leitura imediata
  ultimaLeitura = millis() - INTERVALO_LEITURA_MS;
}

// Registra no log quando o relógio é acertado pelo NTP (uma vez)
void verificarHora() {
  if (!horaSincronizada && time(nullptr) > 1700000000) {
    horaSincronizada = true;
    registrarLog("INFO", "Data e hora sincronizadas via NTP");
  }
}

// Detecta queda do Wi-Fi, tenta reconectar a cada 10 s e registra no log
void verificarWiFi() {
  bool conectadoAgora = (WiFi.status() == WL_CONNECTED);
  unsigned long agora = millis();

  // Acabou de cair
  if (wifiConectado && !conectadoAgora) {
    wifiConectado = false;
    inicioQueda = agora;
    ultimaTentativa = agora;
    registrarLog("ERRO", "Wi-Fi desconectado");
  }

  // Acabou de voltar
  if (!wifiConectado && conectadoAgora) {
    wifiConectado = true;
    unsigned long segundosFora = (agora - inicioQueda) / 1000;
    registrarLog("INFO", "Wi-Fi reconectado apos " + String(segundosFora) + " s. IP " + WiFi.localIP().toString());
  }

  // Continua fora: nova tentativa a cada 10 s (sem travar o loop)
  if (!conectadoAgora && agora - ultimaTentativa >= INTERVALO_RECONEXAO_MS) {
    ultimaTentativa = agora;
    Serial.println("Tentando reconectar ao Wi-Fi...");
    WiFi.reconnect();
  }
}

void loop() {
  server.handleClient();
  atualizarSensores();
  verificarWiFi();
  verificarHora();
}