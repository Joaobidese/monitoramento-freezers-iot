#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LittleFS.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>


// Wi-Fi e Telegram ficam em config.h (fora do Git, está no .gitignore).
// Sem config.h, usa o modelo config.example.h e avisa na compilação.
#if __has_include("config.h")
#include "config.h"
#else
#warning "config.h nao encontrado: usando config.example.h. Copie o modelo para config.h e preencha."
#include "config.example.h"
#endif

const char* ssid = WIFI_SSID;
const char* password = WIFI_SENHA;

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

// Endereço físico de cada DS18B20, guardado ao ligar (ordem do barramento).
// Assim, se um sensor desconectar, o outro continua no card certo.
DeviceAddress enderecoInterno;
DeviceAddress enderecoExterno;

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

// Limite de temperatura: acima disso é alerta e o buzzer liga sozinho.
// 7 °C para a demonstração com copo de gelo (~2 °C); num freezer real seria -10 °C.
const float LIMITE_TEMPERATURA = 7.0;

// Data e hora via NTP (horário de Brasília, UTC-3, sem horário de verão)
const char* FUSO_HORARIO = "<-03>3";
const char* SERVIDOR_NTP_1 = "pool.ntp.org";
const char* SERVIDOR_NTP_2 = "a.st1.ntp.br";
bool horaSincronizada = false;

// Antes do NTP o relógio do ESP32 começa em 01/01/1970. Qualquer valor
// acima deste (14/11/2023) indica que a hora já foi acertada.
const time_t EPOCH_MINIMO_VALIDO = 1700000000;

// Retorna true se o relógio já foi acertado pelo NTP
bool relogioValido() {
  return time(nullptr) > EPOCH_MINIMO_VALIDO;
}

// Reconexão do Wi-Fi
const unsigned long INTERVALO_RECONEXAO_MS = 10000;  // tenta reconectar a cada 10 s
bool wifiConectado = false;          // estado na última verificação
unsigned long inicioQueda = 0;       // quando a conexão caiu
unsigned long ultimaTentativa = 0;   // última tentativa de reconexão
bool alertaInterna = false;  // true = temperatura interna está acima do limite

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
void definirAtuador(bool ligar, const char* origem);
void enviarTelegram(const String &mensagem);

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
  if (relogioValido()) {
    time_t agora = time(nullptr);         // segundos desde 01/01/1970
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

// Quando a temperatura passa do limite: registra AVISO e liga o buzzer.
// Quando volta ao normal: registra INFO e desliga o buzzer.
// Só age na mudança de estado; assim, se alguém silenciar pelo painel,
// o buzzer não volta a tocar até a temperatura normalizar e subir de novo.
void verificarLimite(const char* nome, bool ok, float temp, bool &emAlerta) {
  if (!ok) return;  // sem leitura válida não há o que comparar
  bool acima = temp > LIMITE_TEMPERATURA;
  if (acima && !emAlerta) {
    registrarLog("AVISO", "Temp. " + String(nome) + " acima do limite: " + String(temp, 1) + " C");
    definirAtuador(true, "por alerta");
    enviarTelegram("ALERTA: temperatura " + String(nome) + " em " + String(temp, 1) +
                   " °C (limite " + String(LIMITE_TEMPERATURA, 0) + " °C). Buzzer ligado.");
  }
  if (!acima && emAlerta) {
    registrarLog("INFO", "Temp. " + String(nome) + " normalizada: " + String(temp, 1) + " C");
    definirAtuador(false, "por alerta");
    enviarTelegram("Normalizado: temperatura " + String(nome) + " em " + String(temp, 1) +
                   " °C. Buzzer desligado.");
  }
  emAlerta = acima;
}

// Liga ou desliga o buzzer e registra no log. origem: "via rede" ou "por alerta"
// tone() gera um sinal de 2000 Hz: funciona com buzzer ativo e passivo
void definirAtuador(bool ligar, const char* origem) {
  atuadorLigado = ligar;
  if (ligar) {
    tone(buzzerPin, FREQUENCIA_BUZZER);
  } else {
    noTone(buzzerPin);
    digitalWrite(buzzerPin, LOW);
  }
  registrarLog("INFO", String(ligar ? "Atuador LIGADO " : "Atuador DESLIGADO ") + origem);
}

// Codifica o texto para ir na URL (espaço vira %20, ° vira %C2%B0 etc.)
String codificarURL(const String &texto) {
  const char* hex = "0123456789ABCDEF";
  String resultado;
  for (unsigned int i = 0; i < texto.length(); i++) {
    unsigned char c = texto[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      resultado += (char)c;
    } else {
      resultado += '%';
      resultado += hex[c >> 4];
      resultado += hex[c & 0x0F];
    }
  }
  return resultado;
}

// Envia uma mensagem pelo bot do Telegram (requisição HTTPS para a API do Telegram).
// Aqui o ESP32 atua como CLIENTE HTTP: ele faz o pedido a um servidor da internet.
void enviarTelegram(const String &mensagem) {
  if (String(TELEGRAM_CHAT_ID) == "SEU_CHAT_ID") return;  // Telegram não configurado
  if (WiFi.status() != WL_CONNECTED) {
    registrarLog("ERRO", "Telegram nao enviado: sem Wi-Fi");
    return;
  }

  WiFiClientSecure cliente;
  cliente.setInsecure();  // HTTPS sem verificar o certificado (simplificação do protótipo)

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) +
               "/sendMessage?chat_id=" + String(TELEGRAM_CHAT_ID) +
               "&text=" + codificarURL(mensagem);
  http.begin(cliente, url);
  http.setTimeout(5000);   // espera no máximo 5 s pela resposta
  int codigo = http.GET(); // 200 = mensagem entregue ao Telegram
  http.end();

  if (codigo == 200) registrarLog("INFO", "Alerta enviado ao Telegram");
  else               registrarLog("ERRO", "Falha ao enviar Telegram (codigo " + String(codigo) + ")");
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

    // O limite vale só para o sensor interno (o externo mede o ambiente)
    verificarLimite("interna", internaOk, tempInterna, alertaInterna);

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

// Estilo comum do painel e da página de log
const char* ESTILO =
  "<style>"
  "body{margin:0;font-family:system-ui,-apple-system,'Segoe UI',Roboto,Arial,sans-serif;background:#f4f5f7;color:#1f2328;}"
  ".topo{background:#fff;border-bottom:1px solid #d8dbe0;padding:14px 24px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;}"
  ".topo h1{font-size:18px;margin:0;font-weight:600;}"
  ".topo small{color:#656d76;}"
  ".conteudo{max-width:920px;margin:24px auto;padding:0 16px;}"
  ".grade{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px;}"
  ".card{background:#fff;border:1px solid #d8dbe0;border-radius:6px;padding:16px 18px;}"
  ".card h2{font-size:13px;font-weight:600;color:#656d76;margin:0 0 8px;}"
  ".valor{font-size:38px;font-weight:600;font-variant-numeric:tabular-nums;margin:2px 0 8px;}"
  ".alerta .valor{color:#c62828;}"
  ".tag{display:inline-block;font-size:12px;font-weight:600;padding:2px 8px;border-radius:4px;}"
  ".tag-ok{background:#e6f4ea;color:#1e7b34;}"
  ".tag-alerta{background:#fdecea;color:#c62828;}"
  ".tag-neutra{background:#eef0f3;color:#57606a;}"
  ".detalhe{font-size:14px;color:#57606a;margin:10px 0 0;font-variant-numeric:tabular-nums;}"
  "table{width:100%;border-collapse:collapse;font-size:14px;}"
  "td{padding:6px 0;border-bottom:1px solid #eef0f3;}"
  "td:first-child{color:#656d76;width:70px;}"
  ".botoes{display:flex;gap:8px;margin-top:12px;}"
  ".botao{flex:1;text-align:center;padding:8px 0;border:1px solid #d8dbe0;border-radius:6px;text-decoration:none;color:#1f2328;font-weight:600;font-size:14px;background:#fff;}"
  ".botao:hover{background:#f4f5f7;}"
  ".ponto{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;}"
  ".ponto-ok{background:#2e9d4a;} .ponto-erro{background:#c62828;}"
  "a{color:#0b5cad;}"
  "pre{background:#fff;border:1px solid #d8dbe0;border-radius:6px;padding:14px;font-size:13px;white-space:pre-wrap;}"
  "</style>";

// Card de temperatura. A etiqueta mostra o estado:
//   interno: "Normal" (verde) ou "Acima de 7 °C" (vermelho)
//   externo: "Ambiente" (cinza), pois mede fora do freezer e não usa o limite
//   falha:   "Falha no sensor" (vermelho), nos dois
String montarCard(const char* titulo, float temp, bool ok, const char* legenda,
                  float minimo, float maximo, bool temRegistro, bool usaLimite) {
  bool acimaDoLimite = ok && usaLimite && temp > LIMITE_TEMPERATURA;
  bool alerta = !ok || acimaDoLimite;

  String etiqueta;
  if (!ok)                etiqueta = "<span class='tag tag-alerta'>Falha no sensor</span>";
  else if (acimaDoLimite) etiqueta = "<span class='tag tag-alerta'>Acima de " + String(LIMITE_TEMPERATURA, 0) + " &deg;C</span>";
  else if (usaLimite)     etiqueta = "<span class='tag tag-ok'>Normal</span>";
  else                    etiqueta = "<span class='tag tag-neutra'>Ambiente</span>";

  String valor = "--";
  if (ok) valor = String(temp, 1) + " &deg;C";

  String card = "<div class='card";
  if (alerta) card += " alerta";
  card += "'><h2>" + String(titulo) + "</h2>";
  card += "<div class='valor'>" + valor + "</div>" + etiqueta;

  // Mínimo e máximo registrados ("--" se ainda não houver leitura válida)
  String textoMin = "--";
  String textoMax = "--";
  if (temRegistro) {
    textoMin = String(minimo, 1) + " &deg;C";
    textoMax = String(maximo, 1) + " &deg;C";
  }
  card += "<p class='detalhe'>M&iacute;n " + textoMin + " &middot; M&aacute;x " + textoMax + "</p>";
  card += "<p class='detalhe'>" + String(legenda) + "</p></div>";
  return card;
}

// Card com os dados de rede: SSID, IP, MAC e status da conexão
String montarCardRede(bool conectado) {
  String card = "<div class='card'><h2>Conex&atilde;o</h2><table>";
  card += "<tr><td>Rede</td><td>" + WiFi.SSID() + "</td></tr>";
  card += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  card += "<tr><td>MAC</td><td>" + WiFi.macAddress() + "</td></tr>";
  card += "<tr><td>Status</td><td>";
  if (conectado) card += "<span class='ponto ponto-ok'></span>Conectado";
  else           card += "<span class='ponto ponto-erro'></span>Desconectado";
  card += "</td></tr></table></div>";
  return card;
}

// Card do atuador: estado atual e botões que chamam a rota /atuador
String montarCardAtuador() {
  String card = "<div class='card'><h2>Atuador (buzzer)</h2>";
  if (atuadorLigado) card += "<span class='tag tag-alerta'>Ligado</span>";
  else               card += "<span class='tag tag-neutra'>Desligado</span>";
  card += "<p class='detalhe'>Liga sozinho acima de " + String(LIMITE_TEMPERATURA, 0) + " &deg;C. Desligar silencia o alarme.</p>";
  card += "<div class='botoes'>";
  card += "<a class='botao' href='/atuador?estado=on'>Ligar</a>";
  card += "<a class='botao' href='/atuador?estado=off'>Desligar</a>";
  card += "</div></div>";
  return card;
}

void handleRoot() {
  bool conectado = (WiFi.status() == WL_CONNECTED);

  String html = "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2'>";  // atualiza a página a cada 2 segundos
  html += "<title>Monitor de Freezer</title>";
  html += ESTILO;
  html += "</head><body>";

  // Cabeçalho: título, status real do Wi-Fi e link para o log
  html += "<div class='topo'><div><h1>Monitor de Freezer</h1><small>ESP32 &middot; servidor HTTP</small></div><div>";
  if (conectado) html += "<span class='ponto ponto-ok'></span>Conectado";
  else           html += "<span class='ponto ponto-erro'></span>Desconectado";
  html += " &middot; <a href='/log'>Log do sistema</a></div></div>";

  html += "<div class='conteudo'><div class='grade'>";
  html += montarCard("Temperatura interna", tempInterna, internaOk, "Sensor DS18B20 #1",
                     minInterna, maxInterna, temRegistroInterna, true);
  html += montarCard("Temperatura externa", tempExterna, externaOk, "Sensor DS18B20 #2",
                     minExterna, maxExterna, temRegistroExterna, false);
  html += montarCardRede(conectado);
  html += montarCardAtuador();
  html += "</div></div></body></html>";

  server.send(200, "text/html", html);
}

// Rota /atuador?estado=on|off: recebe o comando pela rede
void handleAtuador() {
  String estado = server.arg("estado");  // valor depois de "estado=" na URL

  if (estado == "on") {
    definirAtuador(true, "via rede");
  } else if (estado == "off") {
    definirAtuador(false, "via rede");  // também serve para silenciar o alarme
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
  String html = "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Log do Sistema</title>";
  html += ESTILO;
  html += "</head><body>";
  html += "<div class='topo'><div><h1>Log do sistema</h1><small>Gravado na flash (LittleFS)</small></div>";
  html += "<div><a href='/'>Voltar ao painel</a></div></div>";
  html += "<div class='conteudo'>";
  if (!logDisponivel) {
    html += "<p>Sistema de arquivos indispon&iacute;vel.</p>";
  }
  html += "<pre>" + lerArquivo(ARQUIVO_LOG_ANTIGO) + lerArquivo(ARQUIVO_LOG) + "</pre></div></body></html>";
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

  // Guarda o endereço dos sensores: 1º do barramento = interno, 2º = externo.
  // Se um sensor não for encontrado, o endereço é zerado: o card mostra FALHA
  // (sem isso, a biblioteca deixaria o endereço do outro sensor e os dois cards
  // mostrariam a mesma temperatura).
  if (!sensors.getAddress(enderecoInterno, 0)) {
    memset(enderecoInterno, 0, sizeof(enderecoInterno));
    registrarLog("ERRO", "Sensor interno nao encontrado ao iniciar");
  }
  if (!sensors.getAddress(enderecoExterno, 1)) {
    memset(enderecoExterno, 0, sizeof(enderecoExterno));
    registrarLog("ERRO", "Sensor externo nao encontrado ao iniciar");
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
  if (!horaSincronizada && relogioValido()) {
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