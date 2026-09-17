#include <WiFi.h>
#include <WebServer.h>

// Substitua pelas credenciais da sua rede Wi-Fi (pode ser a de casa)
const char* ssid = "Lovato121";
const char* password = "Lolo@1777";

// Cria o servidor web escutando na porta 80 (padrão HTTP)
WebServer server(80);

void setup() {
  // Inicia a comunicação com o computador para vermos os textos
  Serial.begin(115200); 
  
  // Conecta ao Wi-Fi
  Serial.print("Conectando ao Wi-Fi");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWi-Fi Conectado!");
  Serial.print("Endereço IP do ESP32: ");
  Serial.println(WiFi.localIP()); // O IP que usaremos para acessar o ESP32

  // Cria a rota principal do servidor ("/")
  server.on("/", []() {
    server.send(200, "text/plain", "Servidor IoT do Freezer Online!");
  });

  server.begin();
  Serial.println("Servidor HTTP iniciado com sucesso.");
}

void loop() {
  // Mantém o servidor ativo escutando as requisições dos clientes
  server.handleClient(); 
}