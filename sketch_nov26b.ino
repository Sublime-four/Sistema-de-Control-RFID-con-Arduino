#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <vector>

// ====== CONFIG WIFI ======
const char* WIFI_SSID = "¿Eres pobre?";
const char* WIFI_PASS = "soy pobre";

WebServer server(80);

// ====== Pines RC522 (ESP32 VSPI) ======
#define SS_PIN    5   // SDA / SS
#define RST_PIN   22  // RST
// VSPI por defecto: SCK=18, MISO=19, MOSI=23

// ====== Salida para "cerradura"/relé ======
#define RELAY_PIN       4
#define RELAY_PULSE_MS  3000

// ====== Buzzer ======
// 0 = PASIVO (necesita tonos), 1 = ACTIVO (suena con HIGH)
#define BUZZER_PIN     13
#define BUZZER_ACTIVE   0

// ====== RFID ======
MFRC522 rfid(SS_PIN, RST_PIN);

// ====== NVS (memoria persistente) ======
Preferences prefs;          // namespace "rfid"
const char* NVS_NS = "rfid";
const char* KEY_MASTER  = "master";   // String UID sin ':'
const char* KEY_ALLOWED = "allowed";  // CSV de UIDs

// ====== Estados de interacción ======
enum State {
  IDLE,
  WAIT_NEW_MASTER, // tras 'C' + password correcta o primer encendido
  WAIT_ADD_TAG,    // via menú admin
  WAIT_DELETE_IDX  // tras 'B'
} state = IDLE;

// ====== Modo admin con armado por TAG maestro + '*' ======
bool adminArmed = false;
bool adminMode  = false;
unsigned long adminArmTs = 0;
unsigned long lastAdminActivityTs = 0;

// Ajustes de tiempos
const unsigned long ADMIN_ARM_TIMEOUT_MS  = 15000;  // 15 s para presionar '*'
const unsigned long ADMIN_IDLE_TIMEOUT_MS = 60000;  // 60 s sin actividad

// ====== Modo de enrolamiento rápido por TAG maestro ======
bool enrollMode = false;
unsigned long enrollStartTs = 0;
const unsigned long ENROLL_TIMEOUT_MS = 15000;       // 15 s para pasar nuevo TAG

// ====== Buffer para leer por Serial ======
String inputLine;

// ====== Datos en RAM ======
String masterUID;
std::vector<String> allowedUID;

// ====== Contadores para stats ====== 
unsigned long accessGrantedCount = 0;
unsigned long accessDeniedCount  = 0;
unsigned long tagAddedCount      = 0;

// ====== CORS helper ======
void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptionsApi() {
  addCorsHeaders();
  server.send(204);
}

// ---------- UTILIDADES RFID / STRINGS ----------
String uidToStringNoColons(const MFRC522::Uid &uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

String uidToStringWithColons(const MFRC522::Uid &uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
    if (i < uid.size - 1) s += ":";
  }
  s.toUpperCase();
  return s;
}

void splitCSV(const String& csv, std::vector<String>& out) {
  out.clear();
  if (csv.length() == 0) return;
  int start = 0;
  while (true) {
    int idx = csv.indexOf(',', start);
    if (idx == -1) {
      String item = csv.substring(start);
      item.trim();
      if (item.length() > 0) out.push_back(item);
      break;
    } else {
      String item = csv.substring(start, idx);
      item.trim();
      if (item.length() > 0) out.push_back(item);
      start = idx + 1;
    }
  }
}

String joinCSV(const std::vector<String>& v) {
  String csv;
  for (size_t i = 0; i < v.size(); i++) {
    csv += v[i];
    if (i + 1 < v.size()) csv += ",";
  }
  return csv;
}

bool uidInList(const String& uid, const std::vector<String>& v, int* posOut = nullptr) {
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] == uid) {
      if (posOut) *posOut = (int)i;
      return true;
    }
  }
  return false;
}

void loadFromNVS() {
  prefs.begin(NVS_NS, true);
  masterUID = prefs.getString(KEY_MASTER, "");
  String csv = prefs.getString(KEY_ALLOWED, "");
  prefs.end();
  splitCSV(csv, allowedUID);
}

void saveAllowedToNVS() {
  prefs.begin(NVS_NS, false);
  prefs.putString(KEY_ALLOWED, joinCSV(allowedUID));
  prefs.end();
}

void saveMasterToNVS(const String& uid) {
  prefs.begin(NVS_NS, false);
  prefs.putString(KEY_MASTER, uid);
  prefs.end();
}

void printAllowedList() {
  Serial.println("=== Lista de permitidos ===");
  if (allowedUID.empty()) {
    Serial.println("(vacia)");
    return;
  }
  for (size_t i = 0; i < allowedUID.size(); i++) {
    Serial.printf("%u) %s\n", (unsigned)(i + 1), allowedUID[i].c_str());
  }
}

// ---------- LOGS PARA GRAFICAS ----------
void logEvent(const char* type, const String& uid, const char* extra = "") {
  // Formato: LOG;millis;TIPO;UID;EXTRA
  Serial.printf("LOG;%lu;%s;%s;%s\n",
                millis(),
                type,
                uid.c_str(),
                extra);
}

// ---------- BUZZER ----------
void bzOn()  { digitalWrite(BUZZER_PIN, HIGH); }
void bzOff() { digitalWrite(BUZZER_PIN, LOW);  }

void toneSoft(int freq, int ms) {
  if (freq <= 0) { delay(ms); return; }
  unsigned long period_us = 1000000UL / (unsigned long)freq;
  unsigned long half = period_us / 2;
  unsigned long cycles = (unsigned long)freq * (unsigned long)ms / 1000UL;
  for (unsigned long i = 0; i < cycles; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(half);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(half);
  }
}

void beep_ms(int ms) {
  if (BUZZER_ACTIVE == 1) {
    bzOn(); delay(ms); bzOff();
  } else {
    toneSoft(2000, ms);
  }
}

void sfxBoot() {
  if (BUZZER_ACTIVE == 1) { beep_ms(80); delay(80); beep_ms(120); }
  else { toneSoft(1200, 120); delay(60); toneSoft(1600, 160); }
}

void sfxPrompt() {
  if (BUZZER_ACTIVE == 1) beep_ms(60);
  else toneSoft(1800, 60);
}

void sfxGranted() {
  if (BUZZER_ACTIVE == 1) { beep_ms(120); delay(60); beep_ms(180); }
  else { toneSoft(1500, 120); delay(50); toneSoft(2000, 180); }
}

void sfxDenied() {
  if (BUZZER_ACTIVE == 1) beep_ms(300);
  else { toneSoft(400, 180); delay(40); toneSoft(300, 160); }
}

void sfxAddOk() {
  if (BUZZER_ACTIVE == 1) { beep_ms(120); delay(50); beep_ms(120); }
  else { toneSoft(1800, 100); delay(40); toneSoft(2200, 120); }
}

void sfxDelOk() {
  if (BUZZER_ACTIVE == 1) { beep_ms(80); delay(50); beep_ms(80); delay(50); beep_ms(80); }
  else { toneSoft(1000, 80); delay(40); toneSoft(900, 80); delay(40); toneSoft(800, 80); }
}

void sfxError() {
  if (BUZZER_ACTIVE == 1) { beep_ms(400); delay(80); beep_ms(200); }
  else { toneSoft(300, 250); delay(80); toneSoft(350, 180); }
}

void sfxMasterSet() {
  if (BUZZER_ACTIVE == 1) { beep_ms(100); delay(40); beep_ms(100); delay(40); beep_ms(150); }
  else { toneSoft(1600, 100); delay(40); toneSoft(1900, 100); delay(40); toneSoft(2200, 150); }
}

// ---------- Modo Admin ----------
void showMenu() {
  Serial.println("\n=== MODO ADMIN (comandos por teclado) ===");
  Serial.println("[A] Agregar nuevo TAG permitido (presentar tarjeta luego)");
  Serial.println("[B] Eliminar TAG por indice (1..N)");
  Serial.println("[C] Cambiar LLAVE MAESTRA (requiere password 2003)");
  Serial.println("[L] Listar permitidos");
  Serial.println("[M] Mostrar llave maestra");
  Serial.println("[ESC] Salir de modo admin");
  Serial.println("TIP: Para agregar un TAG rápido: pase maestro + nuevo TAG (en 15s).");
  Serial.println("=========================================\n");
  sfxPrompt();
}

void enterAdminMode() {
  adminMode = true;
  lastAdminActivityTs = millis();
  showMenu();
}

void exitAdminMode() {
  if (adminMode) {
    Serial.println("Saliendo de modo admin.");
    adminMode = false;
  }
  adminArmed = false;
}

// ---------- ACCESO ----------
void grantAccess(const String& uid, const char* source = "NORMAL") {
  Serial.println(">> ACCESO CONCEDIDO: activando rele.");
  sfxGranted();
  logEvent("ACCESS_GRANTED", uid, source);
  accessGrantedCount++;
  digitalWrite(RELAY_PIN, HIGH);
  delay(RELAY_PULSE_MS);
  digitalWrite(RELAY_PIN, LOW);
}

void denyAccess(const String& uid) {
  Serial.println(">> ACCESO DENEGADO.");
  sfxDenied();
  logEvent("ACCESS_DENIED", uid, "");
  accessDeniedCount++;
}

// ---------- JSON helpers / API ----------
String buildAllowedJson() {
  String json = "{";
  json += "\"master\":\"" + masterUID + "\",";
  json += "\"tags\":[";
  for (size_t i = 0; i < allowedUID.size(); i++) {
    json += "\"" + allowedUID[i] + "\"";
    if (i + 1 < allowedUID.size()) json += ",";
  }
  json += "]}";
  return json;
}

// /api/allowed
void handleAllowedApi() {
  addCorsHeaders();
  server.send(200, "application/json", buildAllowedJson());
}

// /api/delete?idx=N (POST)
void handleDeleteApi() {
  addCorsHeaders();
  if (!server.hasArg("idx")) {
    server.send(400, "application/json", "{\"error\":\"idx requerido\"}");
    return;
  }
  int idx = server.arg("idx").toInt(); // 1..N
  if (idx <= 0 || (size_t)idx > allowedUID.size()) {
    server.send(400, "application/json", "{\"error\":\"indice invalido\"}");
    return;
  }
  String removed = allowedUID[idx - 1];
  allowedUID.erase(allowedUID.begin() + (idx - 1));
  saveAllowedToNVS();
  sfxDelOk();
  logEvent("TAG_DELETED", removed, "HTTP");
  server.send(200, "application/json", buildAllowedJson());
}

// /api/status
void handleStatusApi() {
  String json = "{";
  json += "\"masterSet\":" + String(masterUID.length() > 0 ? "true" : "false") + ",";
  json += "\"masterUID\":\"" + masterUID + "\",";
  json += "\"enrollMode\":" + String(enrollMode ? "true" : "false") + ",";
  json += "\"adminMode\":" + String(adminMode ? "true" : "false") + ",";
  json += "\"allowedCount\":" + String((unsigned long)allowedUID.size()) + ",";
  json += "\"uptimeSec\":" + String((unsigned long)(millis() / 1000));
  json += "}";
  addCorsHeaders();
  server.send(200, "application/json", json);
}

// /api/stats
void handleStatsApi() {
  String json = "{";
  json += "\"uptimeSec\":" + String((unsigned long)(millis() / 1000)) + ",";
  json += "\"granted\":" + String(accessGrantedCount) + ",";
  json += "\"denied\":" + String(accessDeniedCount) + ",";
  json += "\"tagsTotal\":" + String((unsigned long)allowedUID.size()) + ",";
  json += "\"tagsAdded\":" + String(tagAddedCount);
  json += "}";
  addCorsHeaders();
  server.send(200, "application/json", json);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  pinMode(BUZZER_PIN, OUTPUT);
  bzOff();
  sfxBoot();

  // SPI + RC522
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  delay(10);

  byte v = rfid.PCD_ReadRegister(rfid.VersionReg);
  Serial.printf("RC522 VersionReg: 0x%02X\n", v);
  if (v == 0x00 || v == 0xFF) {
    Serial.println("⚠ No hay comunicacion con el RC522. Revisa cableado y 3.3V.");
    sfxError();
  }

  // Cargar memoria persistente
  loadFromNVS();

  if (masterUID.length() == 0) {
    Serial.println("** Primer encendido: no existe LLAVE MAESTRA. **");
    Serial.println("Acerca una tarjeta/tag para establecerla como LLAVE MAESTRA...");
    state = WAIT_NEW_MASTER;
  } else {
    Serial.println("LLAVE MAESTRA cargada desde memoria.");
    Serial.println("Para admin: TAG maestro + '*' en 15s.");
    Serial.println("Para agregar TAG rápido: maestro + nuevo TAG (15s).");
  }

  // ===== Conexión WiFi =====
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi OK. IP: ");
  Serial.println(WiFi.localIP());

  // ===== WebServer (solo API) =====
  server.on("/", HTTP_GET, []() {
    addCorsHeaders();
    server.send(200, "text/plain", "ESP32 RFID API OK");
  });

  server.on("/api/allowed", HTTP_GET, handleAllowedApi);
  server.on("/api/allowed", HTTP_OPTIONS, handleOptionsApi);

  server.on("/api/delete", HTTP_POST, handleDeleteApi);
  server.on("/api/delete", HTTP_OPTIONS, handleOptionsApi);

  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/api/status", HTTP_OPTIONS, handleOptionsApi);

  server.on("/api/stats", HTTP_GET, handleStatsApi);
  server.on("/api/stats", HTTP_OPTIONS, handleOptionsApi);

  server.begin();
  Serial.println("HTTP API server iniciado.");
}

// ---------- Lógica Serial ----------
void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == 27) { // ESC
      exitAdminMode();
      continue;
    }

    if (c == '*') {
      if (adminArmed && (millis() - adminArmTs <= ADMIN_ARM_TIMEOUT_MS)) {
        enterAdminMode();
      } else {
        Serial.println("No autorizado: presente TAG maestro y luego presione * (en 15s).");
        sfxError();
      }
      continue;
    }

    if (c == '\n' || c == '\r') {
      if (inputLine.length() == 0) continue;

      String cmd = inputLine;
      inputLine = "";

      if (!adminMode) {
        Serial.println("No autorizado: presente TAG maestro y luego presione * para menú.");
        sfxError();
        continue;
      }

      lastAdminActivityTs = millis();

      if (state == WAIT_DELETE_IDX) {
        cmd.trim();
        int idx = cmd.toInt();
        if (idx <= 0 || (size_t)idx > allowedUID.size()) {
          Serial.println("Indice invalido. Operacion cancelada.");
          sfxError();
        } else {
          String removed = allowedUID[idx - 1];
          allowedUID.erase(allowedUID.begin() + (idx - 1));
          saveAllowedToNVS();
          Serial.printf("Eliminado [%d]: %s\n", idx, removed.c_str());
          sfxDelOk();
          logEvent("TAG_DELETED", removed, "ADMIN_MENU");
        }
        state = IDLE;
        showMenu();
        return;
      }

      if (cmd.startsWith("PASS ")) {
        String pwd = cmd.substring(5);
        pwd.trim();
        if (pwd == "2003") {
          Serial.println("Password correcta. Acerque una tarjeta para nueva LLAVE MAESTRA...");
          state = WAIT_NEW_MASTER;
          sfxPrompt();
        } else {
          Serial.println("Password incorrecta. Cancelado.");
          sfxError();
          state = IDLE;
          showMenu();
        }
        return;
      }

      cmd.toUpperCase();
      if (cmd == "A") {
        Serial.println("Modo AGREGAR: acerque el nuevo TAG para permitir...");
        Serial.println("TIP: también puede usar maestro + nuevo TAG (modo rápido).");
        sfxPrompt();
        state = WAIT_ADD_TAG;
      } else if (cmd == "B") {
        printAllowedList();
        Serial.println("Ingrese el indice a eliminar (1..N) y presione ENTER:");
        sfxPrompt();
        state = WAIT_DELETE_IDX;
      } else if (cmd == "C") {
        Serial.println("Ingrese la password maestra con el formato:  PASS 2003");
        Serial.println("(ejemplo:  PASS 2003)");
        sfxPrompt();
      } else if (cmd == "L") {
        printAllowedList();
        sfxPrompt();
      } else if (cmd == "M") {
        if (masterUID.length() == 0) Serial.println("(No establecida)");
        else Serial.printf("LLAVE MAESTRA: %s\n", masterUID.c_str());
        sfxPrompt();
      } else {
        Serial.println("Comando no reconocido.");
        sfxError();
        showMenu();
      }
    } else {
      inputLine += c;
    }
  }

  if (adminMode && (millis() - lastAdminActivityTs > ADMIN_IDLE_TIMEOUT_MS)) {
    Serial.println("Tiempo de inactividad excedido. Saliendo de modo admin.");
    exitAdminMode();
  }
}

// ---------- Lógica RFID ----------
void handleRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  // 🔊 Beep SIEMPRE que se lea un TAG
  beep_ms(70);

  String uidNoColons = uidToStringNoColons(rfid.uid);
  String uidColons   = uidToStringWithColons(rfid.uid);

  if (state == WAIT_NEW_MASTER) {
    masterUID = uidNoColons;
    saveMasterToNVS(masterUID);
    Serial.printf("Nueva LLAVE MAESTRA establecida: %s\n", uidNoColons.c_str());
    sfxMasterSet();
    logEvent("MASTER_SET", masterUID, "");
    state = IDLE;
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  if (state == WAIT_ADD_TAG) {
    if (uidNoColons == masterUID) {
      Serial.println("Ese TAG es la LLAVE MAESTRA. No se agrega.");
      sfxError();
    } else {
      if (uidInList(uidNoColons, allowedUID)) {
        Serial.printf("El TAG %s ya estaba en la lista.\n", uidNoColons.c_str());
        sfxError();
      } else {
        allowedUID.push_back(uidNoColons);
        saveAllowedToNVS();
        Serial.printf("TAG agregado (modo admin): %s\n", uidNoColons.c_str());
        sfxAddOk();
        tagAddedCount++;
        logEvent("TAG_ADDED", uidNoColons, "ADMIN_MENU");
      }
    }
    state = IDLE;
    if (adminMode) showMenu();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  // ====== QUICK ENROLL ======
  if (uidNoColons == masterUID) {
    Serial.printf("TAG MAESTRO detectado: %s\n", uidColons.c_str());

    if (!enrollMode) {
      grantAccess(uidNoColons, "MASTER");
      enrollMode = true;
      enrollStartTs = millis();
      Serial.println("Modo ENROLAR habilitado por 15 s. Pase el NUEVO TAG para agregarlo.");
      Serial.println("Si vuelve a pasar el maestro, se cancelará el modo ENROLAR.");
      sfxPrompt();

      adminArmed = true;
      adminArmTs = millis();
      Serial.println("Opcional: presione * en 15 s para entrar al menú admin.");
    } else {
      enrollMode = false;
      Serial.println("Modo ENROLAR cancelado por TAG maestro.");
      sfxError();
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  if (enrollMode) {
    if (uidInList(uidNoColons, allowedUID)) {
      Serial.printf("El TAG %s ya estaba en la lista. No se agrega.\n", uidColons.c_str());
      sfxError();
    } else {
      allowedUID.push_back(uidNoColons);
      saveAllowedToNVS();
      Serial.printf("TAG agregado (modo rápido): %s\n", uidColons.c_str());
      sfxAddOk();
      tagAddedCount++;
      logEvent("TAG_ADDED", uidNoColons, "QUICK_ENROLL");
    }
    enrollMode = false;
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  // Estado normal
  int pos = -1;
  if (uidInList(uidNoColons, allowedUID, &pos)) {
    Serial.printf("TAG permitido (idx %d): %s\n", pos + 1, uidColons.c_str());
    grantAccess(uidNoColons, "ALLOWED_TAG");
  } else {
    Serial.printf("TAG desconocido: %s\n", uidColons.c_str());
    denyAccess(uidNoColons);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ---------- Loop ----------
void loop() {
  handleSerial();
  handleRFID();

  server.handleClient();

  if (adminArmed && (millis() - adminArmTs > ADMIN_ARM_TIMEOUT_MS) && !adminMode) {
    adminArmed = false;
  }

  if (enrollMode && (millis() - enrollStartTs > ENROLL_TIMEOUT_MS)) {
    enrollMode = false;
    Serial.println("Tiempo de ENROLAR expirado. Debe pasar maestro de nuevo.");
    sfxError();
  }
}
