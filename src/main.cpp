#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH1106.h>
#include <esp_dmx.h>
#include <assert.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRtext.h>
#include <IRutils.h>
#include <WiFi.h>
#include <ESP32Encoder.h>
#include <IRsend.h>
#include <Preferences.h>
#include <driver/uart.h>  // kvůli uart_driver_delete()

#ifndef dmx_driver_uninstall
static inline void dmx_driver_uninstall(dmx_port_t port) {
  uart_driver_delete(port);
}
#endif

// Pin pro ovládání MAX485 (DE/RE)
#define MAX485_CTRL_PIN 32

// ========================
// WiFi nastavení
// ========================
const char* ssid     = "ESP32-Network";
const char* password = "999999999";
// Vytvoření WiFi serveru na portu 80
WiFiServer server(80);
String header;

// ========================
// Displej – Adafruit_SH1106
// ========================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SH1106 display(OLED_RESET);

// ========================
// DMX nastavení – využíváme UART1 (DMX_NUM_1 z esp_dmx)
// DMX kanály 1 až 6 jsou uloženy v data[1] až data[6]
#define ENABLE_PIN 26
dmx_port_t dmxPort = DMX_NUM_1;
uint8_t data[DMX_PACKET_SIZE];

// ========================
// IR přijímač – používá pin 16 (RMT)
IRrecv irrecv(16);
decode_results results;

// ========================
// IR vysílač – využívá pin 17 (IR LED)
IRsend irsend(17);

// ========================
// ENKODÉR – KY-040: CLK na GPIO27, DT na GPIO26, SW na GPIO25
#define ENCODER_PIN_A 27
#define ENCODER_PIN_B 26
#define ENCODER_BTN_PIN 25
ESP32Encoder encoder;

// ========================
// Aplikační režimy a stav menu
// ========================
enum AppMode {
  MODE_MENU,
  MODE_DMX_TO_IR,
  MODE_IR_TO_DM,
  MODE_IR_LEARN
};
volatile AppMode activeMode = MODE_MENU;

bool menuMode = true;       // Menu aktivní, dokud není volba potvrzena
int menuLevel = 0;          // 0 = hlavní menu, 1 = Settings, 2 = IR Learn submenu, 3 = IR Learn mode
int menuIndexMain = 0;      // Hlavní menu: položky 0: DMX to IR, 1: IR to DM, 2: IR Learn, 3: Settings
int menuIndexSettings = 0;  // Settings: 0: WiFi AP, 1: Exit
int menuIndexIRLearn = 0;   // IR Learn submenu: položky 0 až 5 (odpovídají pozicím 1 až 6), 6 = Exit
bool wifiAPEnabled = false;

// Definice IR kódů využívaných v režimu IR to DM (ilustrativně)
#define IR_SELECT 0xFFA25D
#define IR_UP     0xFF629D
#define IR_DOWN   0xFFE21D

// Pole pro uložené IR kódy pro DMX kanály (index 1 až 6)
// Používá se pro manuální zadání a metodu "learned" – u metody "library" se kód dopočítá
uint32_t learnedIRCodes[8] = {0, 0, 0, 0, 0, 0, 0, 0};

// Globální proměnná pro pozici, do které se má uložit kód při IR Learn (nastavena z submenu)
int irLearnPos = 0;

unsigned long irLearnStartTime = 0;
unsigned long lastButtonTime = 0;
const unsigned long debounceDelay = 200; // 200 ms debounce
unsigned long modeEnteredTime = 0; // Čas vstupu do režimu IR to DM

// Pro relativní indexaci – uložíme baseline hodnotu enkodéru při vstupu do menu
long menuBaseline = 0;

// Objekt Preferences pro perzistentní úložiště
Preferences preferences;

// Globální proměnná pro správné vyhodnocení stisku tlačítka – akce se provedou pouze při uvolnění a opětovném stisku
bool buttonReady = false;

//
// Pomocná funkce pro URL dekódování
//
String urldecode(String input) {
  String result = "";
  char tempChar;
  int len = input.length();
  for (int i = 0; i < len; i++) {
    char c = input.charAt(i);
    if (c == '+') {
      result += ' ';
    } else if (c == '%' && i + 2 < len) {
      String hex = input.substring(i+1, i+3);
      tempChar = (char) strtol(hex.c_str(), NULL, 16);
      result += tempChar;
      i += 2;
    } else {
      result += c;
    }
  }
  return result;
}

//
// Pomocné funkce pro enkodér a menu
//
void updateMenuBaseline() {
  menuBaseline = encoder.getCount();
}

int getRelativeIndex(int numItems) {
  long rel = encoder.getCount() - menuBaseline;
  int idx = (rel / 2) % numItems;  // každý "detent" změní hodnotu o 2
  if (idx < 0) idx += numItems;
  return idx;
}

void resetEncoder() {
  encoder.setCount(0);
}

//
// Inicializace DMX přijímače a vysílače
//
void initDMXReceiver() {
  encoder.detach();
  dmx_driver_uninstall(dmxPort);
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_driver_install(dmxPort, &config, DMX_INTR_FLAGS_DEFAULT);
  dmx_set_pin(dmxPort, 19, 18, ENABLE_PIN); // RX na GPIO18, TX na GPIO19
  Serial.println("DMX driver inicializován pro příjem (receiver)");
  encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);
}

void initDMXTransmitter() {
  encoder.detach();
  dmx_driver_uninstall(dmxPort);
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_driver_install(dmxPort, &config, DMX_INTR_FLAGS_DEFAULT);
  dmx_set_pin(dmxPort, 19, 18, ENABLE_PIN); 
  Serial.println("DMX driver inicializován pro vysílání (transmitter)");
  encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);
}

//
// Funkce pro kreslení menu na OLED displej
//
void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  int lineHeight = 8;
  
  if (menuLevel == 0) { // Hlavní menu
    display.setCursor(0, 0);
    display.println("Main Menu:");
    const char* items[4] = {"DMX to IR", "IR to DM", "IR Learn", "Settings"};
    int idx = getRelativeIndex(4);
    for (int i = 0; i < 4; i++) {
      display.setCursor(0, (i + 1) * lineHeight);
      display.print((i == idx) ? "> " : "  ");
      display.println(items[i]);
    }
    Serial.print("Main menu index: ");
    Serial.println(idx);
  }
  else if (menuLevel == 1) { // Settings menu
    display.setCursor(0, 0);
    display.println("Settings:");
    int idx = getRelativeIndex(2);
    display.setCursor(0, lineHeight);
    display.print((idx == 0) ? "> " : "  ");
    display.print("WiFi AP: ");
    display.println(wifiAPEnabled ? "ON" : "OFF");
    display.setCursor(0, 2 * lineHeight);
    display.print((idx == 1) ? "> " : "  ");
    display.println("Exit");
    Serial.print("Settings menu index: ");
    Serial.println(idx);
  }
  else if (menuLevel == 2) { // IR Learn submenu
    display.setCursor(0, 0);
    display.println("IR Learn:");
    int idx = getRelativeIndex(7);
    for (int i = 0; i < 7; i++) {
      display.setCursor(0, (i + 1) * lineHeight);
      display.print((i == idx) ? "> " : "  ");
      if (i < 6) {
        display.print(i + 1);
        display.print(": ");
        if (learnedIRCodes[i + 1] != 0) {
          char buf[9];
          sprintf(buf, "%08X", learnedIRCodes[i + 1]);
          display.print(buf);
        } else {
          display.print("----");
        }
      } else {
        display.println("Exit");
      }
    }
    display.setCursor(0, 8 * lineHeight);
    display.println("Press BTN to select");
    Serial.print("IR Learn submenu index: ");
    Serial.println(idx);
  }
  else if (menuLevel == 3) { // IR Learn mode (Waiting for code...)
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IR Learn:");
    display.setCursor(0, 10);
    display.println("Waiting for code...");
  }
  display.display();
}

//
// Režimy DMX to IR a IR to DM (nezměněno)
//
void DMXtoIR() {
  initDMXReceiver();
  dmx_read(dmxPort, data, DMX_PACKET_SIZE);
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("DMX to IR mode:");
  Serial.print("DMX values: ");
  for (int channel = 1; channel <= 6; channel++) {
    display.print("CH");
    display.print(channel);
    display.print(": ");
    display.println(data[channel]);
    Serial.print("CH");
    Serial.print(channel);
    Serial.print(": ");
    Serial.print(data[channel]);
    Serial.print("  ");
  }
  Serial.println();
  display.display();
  
  for (int channel = 1; channel <= 6; channel++) {
    if (data[channel] == 255) {
      uint32_t code = learnedIRCodes[channel];
      if (code != 0) {
        irsend.sendNEC(code, 32);
        Serial.print("Vyslán IR kód pro DMX channel ");
        Serial.print(channel);
        Serial.print(": 0x");
        Serial.println(code, HEX);
        delay(100);
      }
    }
  }
  delay(1000);
}

void runIrToDmx() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println("IR to DM");
  display.setTextSize(1);
  display.setCursor(0, 30);
  display.println("Čekám na IR");
  display.display();
  
  if (irrecv.decode(&results)) {
    String colorName = "";
    if (results.value == IR_SELECT) {
      colorName = "cervena";
      data[1] = 255; data[2] = 255; data[3] = 255;
    }
    else if (results.value == IR_UP) {
      colorName = "zelena";
      data[1] = 0; data[2] = 255; data[3] = 0;
    }
    else if (results.value == IR_DOWN) {
      colorName = "modra";
      data[1] = 0; data[2] = 0; data[3] = 255;
    } else {
      colorName = "Unknown";
    }
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.println("IR to DM");
    display.setTextSize(1);
    display.setCursor(0, 30);
    display.println(colorName);
    display.display();
    
    Serial.print("Přijatý IR kód: 0x");
    Serial.println(results.value, HEX);
    Serial.print("Nastavená barva: ");
    Serial.println(colorName);
    
    dmx_write(dmxPort, data, DMX_PACKET_SIZE);
    dmx_send(dmxPort, DMX_PACKET_SIZE);
    dmx_wait_sent(dmxPort, DMX_TIMEOUT_TICK);
    delay(1000);
    irrecv.resume();
  }
}

//
// Upravený IR Learn režim – po úspěšném naučení nebo timeoutu se vracíme do hlavního menu.
// Nyní také neprovedeme okamžitý přechod, pokud tlačítko stále drží stisknuto.
//
void runIrLearn() {
  if (irLearnStartTime == 0) {
    irLearnStartTime = millis();
  }
  
  if (millis() - irLearnStartTime >= 10000) {
    Serial.println("IR Learn timeout.");
    irLearnStartTime = 0;
    activeMode = MODE_MENU; // Vrátíme se do menu
    menuMode = true;
    menuLevel = 0;
    menuIndexIRLearn = 0;
    updateMenuBaseline();
    drawMenu();
    return;
  }
  
  drawMenu();
  
  if (irrecv.decode(&results)) {
    int pos = irLearnPos + 1;  // irLearnPos odpovídá 0-indexované pozici z menu
    learnedIRCodes[pos] = results.value;
    Serial.print("Naučený IR kód pro pozici ");
    Serial.print(pos);
    Serial.print(": 0x");
    Serial.println(results.value, HEX);
    
    char key[10];
    sprintf(key, "ircode%d", pos);
    preferences.putUInt(key, results.value);
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Learned pos ");
    display.print(pos);
    display.print(": 0x");
    char buf[9];
    sprintf(buf, "%08X", results.value);
    display.println(buf);
    display.display();
    delay(2000);
    irrecv.resume();
    irLearnStartTime = 0;
    
    // Po úspěšném naučení se vracíme do hlavního menu
    activeMode = MODE_MENU;
    menuMode = true;
    menuLevel = 0;
    menuIndexIRLearn = 0;
    resetEncoder();
    encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);
    updateMenuBaseline();
    drawMenu();
  }
}

//
// Funkce pro návrat do menu – reset enkodéru a IR Learn index
//
void checkReturnToMenu() {
  if (activeMode == MODE_IR_TO_DM && (millis() - modeEnteredTime < 1000)) {
    return;
  }
  if (digitalRead(ENCODER_BTN_PIN) == LOW && (millis() - lastButtonTime > debounceDelay)) {
    Serial.println("Návrat do menu");
    resetEncoder();
    encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);
    updateMenuBaseline();
    activeMode = MODE_MENU;
    menuMode = true;
    menuLevel = 0;
    menuIndexIRLearn = 0;
    drawMenu();
    initDMXReceiver();
    lastButtonTime = millis();
    irrecv.enableIRIn();
  }
}

//
// Pomocná funkce, která podle hodnot z knihovny vrátí odpovídající IR kód
//
uint32_t getIRCodeFromLibrary(String manufacturer, String devType, String command) {
  if(manufacturer == "Samsung" && devType == "TV") {
    if(command == "Power") return 0xE0E040BF;
    if(command == "Volume Up") return 0xE0E0E01F;
    if(command == "Volume Down") return 0xE0E0D02F;
  }
  else if(manufacturer == "LG" && devType == "TV") {
    if(command == "Power") return 0x20DF10EF;
    if(command == "Volume Up") return 0x20DF8877;
    if(command == "Volume Down") return 0x20DF9867;
  }
  else if(manufacturer == "Sony" && devType == "TV") {
    if(command == "Power") return 0xA90;
    if(command == "Volume Up") return 0x490;
    if(command == "Volume Down") return 0xC90;
  }
  return 0;
}

//
// Funkce pro obsluhu WiFi serveru s rozšířeným formulářem
//
void handleWiFiServer() {
  WiFiClient client = server.available();
  if (client) {
    unsigned long reqStartTime = millis();
    // Čekáme maximálně 2000 ms (2 sekundy) na příchozí data
    while (!client.available() && (millis() - reqStartTime < 2000)) {
      delay(1);
    }
    if (!client.available()) {  // Pokud data nepřijdou ani po timeoutu, ukončíme spojení
      client.stop();
      return;
    }
    
    String request = client.readStringUntil('\r');
    Serial.print("HTTP Request: ");
    Serial.println(request);
    
    // Zpracování parametrů pro každý DMX kanál
    for (int i = 1; i <= 6; i++) {
      String paramMethod = "channel" + String(i) + "_method=";
      int mIndex = request.indexOf(paramMethod);
      if (mIndex != -1) {
        int start = mIndex + paramMethod.length();
        int end = request.indexOf('&', start);
        if (end == -1) { end = request.indexOf(' ', start); }
        String method = request.substring(start, end);
        
        uint32_t newCode = 0;
        if (method == "manual") {
          String paramName = "code" + String(i) + "_manual=";
          int cIndex = request.indexOf(paramName);
          if (cIndex != -1) {
            int s2 = cIndex + paramName.length();
            int e2 = request.indexOf('&', s2);
            if (e2 == -1) { e2 = request.indexOf(' ', s2); }
            String codeStr = request.substring(s2, e2);
            if (codeStr.length() > 8) {
              codeStr = codeStr.substring(0, 8);
            }
            newCode = (uint32_t) strtoul(codeStr.c_str(), NULL, 16);
          }
        }
        else if (method == "library") {
          String paramManu = "code" + String(i) + "_library_manufacturer=";
          String paramDev  = "code" + String(i) + "_library_devicetype=";
          String paramCmd  = "code" + String(i) + "_library_command=";
          int idxManu = request.indexOf(paramManu);
          int idxDev  = request.indexOf(paramDev);
          int idxCmd  = request.indexOf(paramCmd);
          if (idxManu != -1 && idxDev != -1 && idxCmd != -1) {
            int sManu = idxManu + paramManu.length();
            int eManu = request.indexOf('&', sManu);
            if (eManu == -1) { eManu = request.indexOf(' ', sManu); }
            String manufacturer = urldecode(request.substring(sManu, eManu));
            
            int sDev = idxDev + paramDev.length();
            int eDev = request.indexOf('&', sDev);
            if (eDev == -1) { eDev = request.indexOf(' ', sDev); }
            String devType = urldecode(request.substring(sDev, eDev));
            
            int sCmd = idxCmd + paramCmd.length();
            int eCmd = request.indexOf('&', sCmd);
            if (eCmd == -1) { eCmd = request.indexOf(' ', sCmd); }
            String command = urldecode(request.substring(sCmd, eCmd));
            
            newCode = getIRCodeFromLibrary(manufacturer, devType, command);
            if (newCode == 0) {
              Serial.print("Neplatný výběr z knihovny pro kanál ");
              Serial.println(i);
            }
          }
        }
        else if (method == "learned") {
          String paramName = "code" + String(i) + "_learned=";
          int cIndex = request.indexOf(paramName);
          if (cIndex != -1) {
            int s2 = cIndex + paramName.length();
            int e2 = request.indexOf('&', s2);
            if (e2 == -1) { e2 = request.indexOf(' ', s2); }
            String codeStr = request.substring(s2, e2);
            newCode = (uint32_t) strtoul(codeStr.c_str(), NULL, 16);
          }
        }
        if (i >= 1 && i <= 6 && newCode != 0) {
          learnedIRCodes[i] = newCode;
          char key[10];
          sprintf(key, "ircode%d", i);
          preferences.putUInt(key, newCode);
          
          Serial.print("Kanál ");
          Serial.print(i);
          Serial.print(" aktualizován metodou ");
          Serial.print(method);
          Serial.print(" s kódem 0x");
          Serial.println(newCode, HEX);
        }
      }
    }
    
    // Sestavíme HTML stránku s formulářem
    String html = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    html += "<html><head><meta charset='UTF-8'><title>IR Code Config</title>";
    html += "<script>";
    html += "var libraryData = {";
    html += "  'Samsung': { 'TV': { 'Power': 'E0E040BF', 'Volume Up': 'E0E0E01F', 'Volume Down': 'E0E0D02F' } },";
    html += "  'LG': { 'TV': { 'Power': '20DF10EF', 'Volume Up': '20DF8877', 'Volume Down': '20DF9867' } },";
    html += "  'Sony': { 'TV': { 'Power': 'A90', 'Volume Up': '490', 'Volume Down': 'C90' } }";
    html += "};";
    html += "function updateDeviceType(channel){";
    html += "  var manu = document.getElementById('code_library_' + channel + '_manufacturer').value;";
    html += "  var devSelect = document.getElementById('code_library_' + channel + '_devicetype');";
    html += "  devSelect.innerHTML = \"<option value='TV'>TV</option>\";";
    html += "  updateCommand(channel);";
    html += "}";
    html += "function updateCommand(channel){";
    html += "  var manu = document.getElementById('code_library_' + channel + '_manufacturer').value;";
    html += "  var dev = document.getElementById('code_library_' + channel + '_devicetype').value;";
    html += "  var cmdSelect = document.getElementById('code_library_' + channel + '_command');";
    html += "  var cmds = libraryData[manu][dev];";
    html += "  var options = \"\";";
    html += "  for(var key in cmds){";
    html += "    options += \"<option value='\" + key + \"'>\" + key + \" (0x\" + cmds[key] + \")</option>\";";
    html += "  }";
    html += "  cmdSelect.innerHTML = options;";
    html += "}";
    html += "function showOptions(channel){";
    html += "  var method = document.querySelector('input[name=\"channel' + channel + '_method\"]:checked').value;";
    html += "  document.getElementById('code_manual_' + channel).style.display = (method=='manual') ? 'block' : 'none';";
    html += "  document.getElementById('code_library_' + channel).style.display = (method=='library') ? 'block' : 'none';";
    html += "  document.getElementById('code_learned_' + channel).style.display = (method=='learned') ? 'block' : 'none';";
    html += "  if(method=='library'){ updateCommand(channel); }";
    html += "}";
    html += "</script></head><body>";
    html += "<h1>IR Code Configuration</h1>";
    html += "<p>Zadejte IR kód (hex) nebo vyberte z nabídky pro daný DMX kanál, který bude vyslán při hodnotě 255.</p>";
    html += "<form action='/' method='GET'>";
    for (int i = 1; i <= 6; i++) {
      html += "<div style='border:1px solid #ccc;padding:10px;margin-bottom:10px;'>";
      html += "<h3>Kanál " + String(i) + "</h3>";
      html += "<input type='radio' name='channel" + String(i) + "_method' value='manual' checked onclick='showOptions(" + String(i) + ")'> Manual ";
      html += "<input type='radio' name='channel" + String(i) + "_method' value='library' onclick='showOptions(" + String(i) + ")'> Library ";
      html += "<input type='radio' name='channel" + String(i) + "_method' value='learned' onclick='showOptions(" + String(i) + ")'> Learned <br>";
      
      html += "<div id='code_manual_" + String(i) + "'>";
      html += "Manual: <input type='text' name='code" + String(i) + "_manual' value='";
      char buf[9];
      sprintf(buf, "%08X", learnedIRCodes[i]);
      html += buf;
      html += "'></div>";
      
      html += "<div id='code_library_" + String(i) + "' style='display:none;'>";
      html += "Manufacturer: <select name='code" + String(i) + "_library_manufacturer' id='code_library_" + String(i) + "_manufacturer' onchange='updateDeviceType(" + String(i) + ")'>";
      html += "<option value='Samsung'>Samsung</option>";
      html += "<option value='LG'>LG</option>";
      html += "<option value='Sony'>Sony</option>";
      html += "</select><br>";
      html += "Device Type: <select name='code" + String(i) + "_library_devicetype' id='code_library_" + String(i) + "_devicetype' onchange='updateCommand(" + String(i) + ")'>";
      html += "<option value='TV'>TV</option>";
      html += "</select><br>";
      html += "Command: <select name='code" + String(i) + "_library_command' id='code_library_" + String(i) + "_command'>";
      html += "</select>";
      html += "</div>";
      
      html += "<div id='code_learned_" + String(i) + "' style='display:none;'>";
      html += "Learned: <select name='code" + String(i) + "_learned'>";
      html += "<option value='0'>None</option>";
      for (int j = 1; j <= 6; j++) {
        if (learnedIRCodes[j] != 0) {
          char codeBuf[9];
          sprintf(codeBuf, "%08X", learnedIRCodes[j]);
          html += "<option value='";
          html += codeBuf;
          html += "'>Code ";
          html += String(j);
          html += " (0x";
          html += codeBuf;
          html += ")</option>";
        }
      }
      html += "</select></div>";
      
      html += "</div>";
    }
    
    html += "<input type='submit' value='Uložit nastavení'>";
    html += "</form>";
    html += "</body></html>";
    
    client.print(html);
    delay(1);
    client.stop();
    Serial.println("Client disconnected.");
  }
}


//
// setup() – inicializace modulů, načtení uložených IR kódů, spuštění WiFi AP a serveru
//
void setup() {
  Serial.begin(115200, SERIAL_8N1, 34, 1);
  delay(1000);
  Serial.println("Terminál (UART0) přemapován: RX na GPIO34, TX na GPIO1");
  
  pinMode(MAX485_CTRL_PIN, OUTPUT);
  digitalWrite(MAX485_CTRL_PIN, HIGH);
  
  initDMXTransmitter();
  
  irrecv.enableIRIn();
  Serial.println("IR přijímač inicializován na pinu 16");
  
  display.begin(SH1106_SWITCHCAPVCC, SCREEN_ADDRESS);
  Wire.beginTransmission(SCREEN_ADDRESS);
  Wire.write(0x00);
  Wire.write(0x81);
  Wire.write(0xFF);
  Wire.endTransmission();
  
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(15, 5);
  display.println("DMX<->IR");
  display.display();
  delay(1000);
  display.clearDisplay();
  
  WiFi.softAP(ssid, password);
  Serial.println("Access Point spuštěn");
  Serial.print("AP IP adresa: ");
  Serial.println(WiFi.softAPIP());
  server.begin();
  
  pinMode(18, INPUT_PULLDOWN);
  
  encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);
  resetEncoder();
  updateMenuBaseline();
  
  pinMode(ENCODER_BTN_PIN, INPUT_PULLUP);
  
  menuMode = true;
  menuLevel = 0;
  updateMenuBaseline();
  drawMenu();
  
  irsend.begin();
  
  preferences.begin("irlearn", false);
  for (int i = 1; i <= 6; i++) {
    char key[10];
    sprintf(key, "ircode%d", i);
    learnedIRCodes[i] = preferences.getUInt(key, 0);
    Serial.print("Načten IR kód pro kanál ");
    Serial.print(i);
    Serial.print(": 0x");
    Serial.println(learnedIRCodes[i], HEX);
  }
}

//
// loop() – hlavní smyčka: obsluha DMX/IR režimů a webového serveru
//
void loop() {
  // Aktualizujeme stav tlačítka – pokud je tlačítko uvolněno, nastavíme flag buttonReady
  if (digitalRead(ENCODER_BTN_PIN) == HIGH) {
    buttonReady = true;
  }
  
  handleWiFiServer();

  if (menuMode) {
    int newIndex;
    if (menuLevel == 0) {
      newIndex = getRelativeIndex(4);
      if (newIndex != menuIndexMain) {
        menuIndexMain = newIndex;
        drawMenu();
        Serial.print("Main menu index: ");
        Serial.println(menuIndexMain);
      }
    }
    else if (menuLevel == 1) {
      newIndex = getRelativeIndex(2);
      if (newIndex != menuIndexSettings) {
        menuIndexSettings = newIndex;
        drawMenu();
        Serial.print("Settings menu index: ");
        Serial.println(menuIndexSettings);
      }
    }
    else if (menuLevel == 2) {
      newIndex = getRelativeIndex(7);
      if (newIndex != menuIndexIRLearn) {
        menuIndexIRLearn = newIndex;
        drawMenu();
        Serial.print("IR Learn submenu index: ");
        Serial.println(menuIndexIRLearn);
      }
    }
    
    // Zpracování stisku tlačítka pouze pokud byl tlačítko uvolněno dříve
    if (digitalRead(ENCODER_BTN_PIN) == LOW && buttonReady && (millis() - lastButtonTime > debounceDelay)) {
      lastButtonTime = millis();
      buttonReady = false; // Zablokujeme opakované zpracování, dokud tlačítko nebude uvolněno
      Serial.println("Tlačítko stisknuto v menu!");
      if (menuLevel == 0) {
        if (menuIndexMain == 0) {
          activeMode = MODE_DMX_TO_IR;
          menuMode = false;
          Serial.println("Vybráno: DMX to IR");
          updateMenuBaseline();
          initDMXReceiver();
          digitalWrite(MAX485_CTRL_PIN, LOW);
        } else if (menuIndexMain == 1) {
          activeMode = MODE_IR_TO_DM;
          menuMode = false;
          Serial.println("Vybráno: IR to DM");
          updateMenuBaseline();
          initDMXTransmitter();
          digitalWrite(MAX485_CTRL_PIN, HIGH);
          modeEnteredTime = millis();
          while (digitalRead(ENCODER_BTN_PIN) == LOW) { delay(10); }
          display.clearDisplay();
          display.setTextSize(2);
          display.setCursor(0, 0);
          display.println("IR to DM");
          display.setTextSize(1);
          display.setCursor(0, 30);
          display.println("Čekám na IR");
          display.display();
        } else if (menuIndexMain == 2) {
          // Při výběru IR Learn z hlavního menu se nejprve nastaví submenu a vyčkáme na nový stisk
          menuLevel = 2;
          menuIndexIRLearn = 0; // Reset submenu index
          updateMenuBaseline();
          drawMenu();
          Serial.println("Vybráno: IR Learn (submenu)");
        } else if (menuIndexMain == 3) {
          menuLevel = 1;
          updateMenuBaseline();
          drawMenu();
          Serial.println("Vybráno: Settings");
        }
      }
      else if (menuLevel == 1) {
        if (menuIndexSettings == 0) {
          wifiAPEnabled = !wifiAPEnabled;
          if (wifiAPEnabled) {
            WiFi.softAP(ssid, password);
            Serial.println("WiFi AP zapnut");
          } else {
            WiFi.softAPdisconnect(true);
            Serial.println("WiFi AP vypnut");
          }
          drawMenu();
        } else if (menuIndexSettings == 1) {
          menuLevel = 0;
          updateMenuBaseline();
          drawMenu();
          Serial.println("Návrat z Settings");
        }
      }
      else if (menuLevel == 2) {
        if (menuIndexIRLearn < 6) {
          // Přejdeme do režimu IR Learn až pokud tlačítko bylo uvolněno a následně stisknuto
          irLearnPos = menuIndexIRLearn;
          activeMode = MODE_IR_LEARN;
          menuLevel = 3;
          menuMode = false;
          irLearnStartTime = millis();
          updateMenuBaseline();
          drawMenu();
          Serial.print("Nastavuji IR Learn pro pozici ");
          Serial.println(irLearnPos + 1);
        } else {
          menuLevel = 0;
          updateMenuBaseline();
          menuIndexIRLearn = 0;
          drawMenu();
          Serial.println("Návrat z IR Learn");
        }
      }
      delay(50);
    }
  }
  else {
    if (activeMode == MODE_IR_TO_DM) {
      runIrToDmx();
      checkReturnToMenu();
    } else if (activeMode == MODE_DMX_TO_IR) {
      DMXtoIR();
      checkReturnToMenu();
    } else if (activeMode == MODE_IR_LEARN) {
      if (menuLevel == 3) {
        runIrLearn();
      }
      checkReturnToMenu();
    }
  }
}
