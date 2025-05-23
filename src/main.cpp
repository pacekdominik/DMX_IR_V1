#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH1106.h>
#include <esp_dmx.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
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
const char* ssid     = "DMX IR converter";
const char* password = "999999999";
// Vytvoření WiFi serveru na portu 80
WiFiServer server(80);

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
#define ENABLE_PIN 13 //nezapojený pin, pouze jako dummy
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
  MODE_IR_TO_DMX,
  MODE_IR_LEARN
};
volatile AppMode activeMode = MODE_MENU;

bool menuMode = true;       // Menu aktivní, dokud není volba potvrzena
int menuLevel = 0;          // 0 = hlavní menu, 1 = Settings, 2 = IR Learn submenu, 3 = IR Learn mode
int menuIndexMain = 0;      // Hlavní menu: položky 0: DMX to IR, 1: IR to DMX, 2: IR Learn, 3: Settings
int menuIndexSettings = 0;  // Settings: 0: WiFi AP, 1: Exit
int menuIndexIRLearn = 0;   // IR Learn submenu: položky 0 až 5 (odpovídají pozicím 1 až 6), 6 = Exit
bool wifiAPEnabled = true;

// Pole pro uložené IR kódy pro DMX kanály (index 1 až 6)
// Používá se pro manuální zadání a metodu "learned" – u metody "library" se kód dopočítá
uint32_t learnedIRCodes[8] = {0, 0, 0, 0, 0, 0, 0, 0};

// Uložené DMX scény: 6 scén × 64 kanálů (0–255)
uint8_t scenes[6][64] = { {0} };

// Globální proměnná pro pozici, do které se má uložit kód při IR Learn (nastavena z submenu)
int irLearnPos = 0;

unsigned long irLearnStartTime = 0;
unsigned long lastButtonTime = 0;
const unsigned long debounceDelay = 200; // 200 ms debounce

// pro nový DMX→IR režim
static bool  dmxToIrFirstEntry = true;
static uint8_t dmxPrev[7] = {0};         // 1..6, drží předchozí hodnotu (<255 nebo ==255)
static unsigned long lastDmxPoll = 0;    // čas posledního snímku

static uint32_t lastIrSent[7] = {0};     //pamatujee si poslední odeslaný IR kód pro každý kanál

static bool dmxInstalled = false;

// pro IR→DMX režim
bool  irToDmxFirstEntry = true;
int   irToDmxLastScene  = -1;

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
  int idx = (rel) % numItems;  // každý "detent" změní hodnotu
  if (idx < 0) idx += numItems;
  return idx;
}

void resetEncoder() {
  encoder.setCount(0);
}

//
// Inicializace DMX přijímače a vysílače
//
void initDMXTransciever() {
  encoder.detach();
  if (!dmxInstalled) {
    // jednorázová instalace
    dmx_driver_uninstall(dmxPort);
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    dmx_driver_install(dmxPort, &config, DMX_INTR_FLAGS_DEFAULT);
    dmx_set_pin(dmxPort, 19, 18, ENABLE_PIN);
    Serial.println("DMX driver inicializován");
    dmxInstalled = true;
  }
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
    const char* items[4] = {"DMX to IR", "IR to DMX", "IR Learn", "Settings"};
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


// Režimy DMX to IR a IR to DMX 

void runDmxToIr() {
  if (dmxToIrFirstEntry) {
    initDMXTransciever();
    lastDmxPoll       = 0;
    memset(dmxPrev,   0, sizeof(dmxPrev));
    memset(lastIrSent,0, sizeof(lastIrSent));
    dmxToIrFirstEntry = false;
  }

  unsigned long now = millis();
  if (now - lastDmxPoll < 100) return;   // 10× za sekundu
  lastDmxPoll = now;

  // 1) Přečti DMX kanály
  dmx_read(dmxPort, data, DMX_PACKET_SIZE);

  // 2) Vykresli název módu + stavy + kódy
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Mode: DMX->IR");
  for (int ch = 1; ch <= 6; ch++) {
    int y = ch * 8;
    display.setCursor(0, y);
    char buf[32];
    sprintf(buf, "CH%d:%3u", ch, data[ch]);
    if (lastIrSent[ch]) {
      strcat(buf, "  ");
      sprintf(buf + strlen(buf), "%08X", lastIrSent[ch]);
    }
    display.println(buf);
  }
  display.display();

  // 3) EDGE-detekce, odeslání IR a mazání kódu, když klesne pod 255
  for (int ch = 1; ch <= 6; ch++) {
    bool highNow = (data[ch] == 255);
    if (highNow && !dmxPrev[ch]) {
      // právě přechod z <255 na 255 → pošli IR
      uint32_t code = learnedIRCodes[ch];
      if (code) {
        irsend.sendNEC(code, 32);
        lastIrSent[ch] = code;
      }
    }
    else if (!highNow) {
      // jakmile poklesne pod 255, smažeme zobrazený kód
      lastIrSent[ch] = 0;
    }
    dmxPrev[ch] = highNow;
  }
}



void runIrToDmx() {
  // 1) Při každém vstupu (firstEntry=true) vykreslí hlavičku + waiting
   if (irToDmxFirstEntry) {
    // PROBUĎ IRrecv, aby poslouchal hned od začátku:
    irrecv.resume();

    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.println("IR to DMX");
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.print("Waiting for IR");
    display.display();

    irToDmxFirstEntry = false;
  }

  // 2) Detekce nového IR kódu
  if (irrecv.decode(&results)) {
    irrecv.resume();
    for (int i = 1; i <= 6; i++) {
      if (results.value == learnedIRCodes[i]) {
        // jen když se změnila scéna, překreslí scénu
        if (i != irToDmxLastScene) {
          display.fillRect(0, 40, SCREEN_WIDTH, 8, BLACK);
          display.setCursor(0, 40);
          display.print("Scene: ");
          display.println(i);
          display.display();
          irToDmxLastScene = i;
        }
        break;
      }
    }
  }

  // 3) Pokud máme zvolenu scénu, vysíláme DMX paket pořád dokola
  if (irToDmxLastScene != -1) {
    initDMXTransciever();
    digitalWrite(MAX485_CTRL_PIN, HIGH);
    for (int ch = 1; ch <= 64; ch++) {
      data[ch] = scenes[irToDmxLastScene - 1][ch - 1];
    }
    dmx_write(dmxPort, data, 65);
    dmx_send(dmxPort, 65);
    dmx_wait_sent(dmxPort, DMX_TIMEOUT_TICK);
  }
}






//
// Upravený IR Learn režim – při uložení kódu ověříme, zda knihovna IRremoteESP8266 rozpoznala protokol.
// Pokud ano, do terminálu se vypíše název protokolu a kód se uloží.
// Pokud ne, kód se odmítne.
// Po úspěšném naučení (nebo timeoutu) se vracíme do hlavního menu.
//
void runIrLearn() {
  if (irLearnStartTime == 0) {
    irLearnStartTime = millis();
  }
  if (millis() - irLearnStartTime >= 10000) {
    // timeout – návrat do menu
    irLearnStartTime = 0;
    activeMode = MODE_MENU;
    menuMode = true; menuLevel = 0; menuIndexIRLearn = 0;
    updateMenuBaseline();
    drawMenu();
    return;
  }

  drawMenu();

  if (!irrecv.decode(&results)) return;
  irrecv.resume();

  // ignor NEC-repeat
  if (results.decode_type == NEC && results.value == 0xFFFFFFFFFFFFFFFFULL) return;
  if (results.decode_type == UNKNOWN) return;

  // zjistíme protokol
  String proto;
  switch (results.decode_type) {
    case NEC:  proto = "NEC";  break;
    case SONY: proto = "SONY"; break;
    case RC5:  proto = "RC5";  break;
    case RC6:  proto = "RC6";  break;
    default:   proto = "OTHER";break;
  }

  // uložíme kód
  int pos = irLearnPos + 1;
  uint32_t code = (uint32_t)results.value;
  learnedIRCodes[pos] = code;
  char key[12];
  sprintf(key, "ircode%d", pos);
  preferences.putUInt(key, code);

  // vykreslíme protokol a kód na OLED
   display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 0);
  display.println("IR code learned");
  display.println("");                    // prázdný řádek

  display.print("Protocol:");
  display.setCursor(70, 16);
  display.println(proto);

  char buf[9];
  sprintf(buf, "%08X", code);
  display.print("Code:");
  display.setCursor(70, 24);
  display.println(buf);

  display.display();
  delay(2500);

  // návrat do menu
  irLearnStartTime = 0;
  activeMode = MODE_MENU;
  menuMode = true; menuLevel = 0; menuIndexIRLearn = 0;
  resetEncoder();
  encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);
  updateMenuBaseline();
  drawMenu();
}



//
// Funkce pro návrat do menu – reset enkodéru a IR Learn index
//
void checkReturnToMenu() {
  if (activeMode == MODE_IR_TO_DMX && (millis() < 1000)) {
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
    initDMXTransciever();
    lastButtonTime = millis();
    
  }
}

//
// Pomocná funkce, která podle hodnot z knihovny vrátí odpovídající IR kód
//
uint32_t getIRCodeFromLibrary(String manufacturer, String devType, String command) {
  // Samsung
  if (manufacturer == "Samsung") {
    if (devType == "TV") {
      if (command == "Power")        return 0xE0E040BF;
      if (command == "Volume Up")    return 0xE0E0E01F;
      if (command == "Volume Down")  return 0xE0E0D02F;
      if (command == "Channel Up")   return 0xE0E048B7;
      if (command == "Channel Down") return 0xE0E008F7;
    }
    else if (devType == "Soundbar") {
      if (command == "Power")     return 0xE0E0F00F;
      if (command == "Mute")      return 0xE0E0D00F;
      if (command == "Volume Up") return 0xE0E0E01F;
    }
  }
  // LG
  else if (manufacturer == "LG") {
    if (devType == "TV") {
      if (command == "Power")        return 0x20DF10EF;
      if (command == "Volume Up")    return 0x20DF8877;
      if (command == "Volume Down")  return 0x20DF9867;
      if (command == "Input HDMI1")  return 0x20DF00FF;
    }
  }
  // Sony
  else if (manufacturer == "Sony") {
    if (devType == "TV") {
      if (command == "Power")       return 0xA90;
      if (command == "Volume Up")   return 0x490;
      if (command == "Volume Down") return 0xC90;
      if (command == "Mute")        return 0x290;
    }
  }
  // Panasonic
  else if (manufacturer == "Panasonic") {
    if (devType == "TV") {
      if (command == "Power")        return 0x4004;
      if (command == "Volume Up")    return 0x400C;
      if (command == "Volume Down")  return 0x400E;
    }
    else if (devType == "DVD") {
      if (command == "Play")         return 0x500F;
      if (command == "Pause")        return 0x5010;
      if (command == "Stop")         return 0x500B;
    }
  }
  // Philips
  else if (manufacturer == "Philips") {
    if (devType == "TV") {
      if (command == "Power")       return 0x30CF;
      if (command == "Volume Up")   return 0x30DF;
      if (command == "Volume Down") return 0x30EF;
    }
  }
  // Yamaha
  else if (manufacturer == "Yamaha") {
    if (devType == "AV Receiver") {
      if (command == "Power")       return 0xA55A;
      if (command == "Mute")        return 0xA45A;
      if (command == "Volume Up")   return 0xA15E;
      if (command == "Volume Down") return 0xA05E;
    }
  }
  // Air Conditioner (generic NEC)
  else if (manufacturer == "Generic" && devType == "Air Conditioner") {
    if (command == "Power")        return 0x20DF10EF;
    if (command == "Temp Up")      return 0x20DF40BF;
    if (command == "Temp Down")    return 0x20DF807F;
    if (command == "Mode Cool")    return 0x20DFC03F;
    if (command == "Mode Heat")    return 0x20DF20DF;
  }
  return 0;
}


//
// Funkce pro obsluhu WiFi serveru s rozšířeným formulářem
//
void handleWiFiServer() {
  WiFiClient client = server.available();
  if (!client) return;

  unsigned long reqStartTime = millis();
  while (!client.available() && millis() - reqStartTime < 2000) {
    delay(1);
  }
  if (!client.available()) {
    client.stop();
    return;
  }

  // Cteni radku request
  String request = client.readStringUntil('\r');
  Serial.print("HTTP Request: ");
  Serial.println(request);

  // Parsovani cesty a dotazu (query)
  int firstSpace = request.indexOf(' ');
  int secondSpace = request.indexOf(' ', firstSpace + 1);
  String fullPath = request.substring(firstSpace + 1, secondSpace);
  String path = fullPath;
  String query = "";
  int qm = fullPath.indexOf('?');
  if (qm >= 0) {
    path  = fullPath.substring(0, qm);
    query = fullPath.substring(qm + 1);
  }

  // Pokud je "/scenes", zobraz nebo uloz DMX sceny
  if (path == "/scenes") {
    // Pokud je pritomen dotaz (query), rozparsuj ho a uloz do pole scenes []
    if (query.length()) {
      int idx = 0;
      while (idx < query.length()) {
        int amp = query.indexOf('&', idx);
        if (amp < 0) amp = query.length();
        String pair = query.substring(idx, amp);
        int eq = pair.indexOf('=');
        if (eq > 0) {
          String name   = pair.substring(0, eq);
          String value  = urldecode(pair.substring(eq + 1));
          if (name.startsWith("scene")) {
            int us    = name.indexOf('_');
            int sNum  = name.substring(5, us).toInt();    // 1..6
            int cNum  = name.substring(us + 3).toInt();   // 1..64
            if (sNum >= 1 && sNum <= 6 && cNum >= 1 && cNum <= 64) {
              scenes[sNum - 1][cNum - 1] = constrain(value.toInt(), 0, 255);
            }
          }
        }
        idx = amp + 1;
      }
      // Ukladani kazde sceny do NVS
      for (int s = 0; s < 6; s++) {
        char key[12];
        sprintf(key, "scene%d", s + 1);
        preferences.putBytes(key, scenes[s], sizeof(scenes[s]));
      }
    }

    // Vytvorit HTML pro konfiguraci scen
    String html = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n";
    html += "<html><head><meta charset='UTF-8'><title>DMX Scenes</title></head><body>";
    html += "<button onclick=\"window.location='/'\">&larr; Back to IR Codes</button>";
    html += "<h1>Configure DMX Scenes</h1>";
    html += "<form method='GET' action='/scenes'>";
    for (int s = 0; s < 6; s++) {
      html += "<fieldset><legend>Scene " + String(s + 1) + "</legend>";
      for (int c = 0; c < 64; c++) {
        html += "Ch" + String(c + 1) + ": ";
        html += "<input type='number' name='scene" + String(s + 1) + "_ch" + String(c + 1) + 
                "' min='0' max='255' value='" + String(scenes[s][c]) + "' style='width:50px;'> ";
        if ((c + 1) % 8 == 0) html += "<br>";
      }
      html += "</fieldset><br>";
    }
    html += "<input type='submit' value='Save Scenes'></form>";
    html += "</body></html>";

    client.print(html);
    delay(1);
    client.stop();
    return;
  }

  // Stranka pro konfiguraci IR kodu
  // 1) Zpracovani nastaveni IR kodu z prichoziho pozadavku
  for (int i = 1; i <= 6; i++) {
    String paramMethod = "channel" + String(i) + "_method=";
    int mIndex = request.indexOf(paramMethod);
    if (mIndex != -1) {
      int start = mIndex + paramMethod.length();
      int end   = request.indexOf('&', start);
      if (end == -1) end = request.indexOf(' ', start);
      String method = request.substring(start, end);
      uint32_t newCode = 0;

      if (method == "manual") {
        String p = "code" + String(i) + "_manual=";
        int ci = request.indexOf(p);
        if (ci != -1) {
          int s2 = ci + p.length();
          int e2 = request.indexOf('&', s2);
          if (e2 == -1) e2 = request.indexOf(' ', s2);
          String codeStr = request.substring(s2, e2);
          if (codeStr.length() > 8) codeStr = codeStr.substring(0, 8);
          newCode = strtoul(codeStr.c_str(), NULL, 16);
        }
      } else if (method == "library") {
        String pm = "code" + String(i) + "_library_manufacturer=";
        String pd = "code" + String(i) + "_library_devicetype=";
        String pc = "code" + String(i) + "_library_command=";
        int im = request.indexOf(pm), id = request.indexOf(pd), ic = request.indexOf(pc);
        if (im != -1 && id != -1 && ic != -1) {
          int sm = im + pm.length();
          int em = request.indexOf('&', sm);
          if (em == -1) em = request.indexOf(' ', sm);
          String manu = urldecode(request.substring(sm, em));

          int sd = id + pd.length();
          int ed = request.indexOf('&', sd);
          if (ed == -1) ed = request.indexOf(' ', sd);
          String devt = urldecode(request.substring(sd, ed));

          int sc = ic + pc.length();
          int ec = request.indexOf('&', sc);
          if (ec == -1) ec = request.indexOf(' ', sc);
          String cmd = urldecode(request.substring(sc, ec));

          newCode = getIRCodeFromLibrary(manu, devt, cmd);
          if (newCode == 0) {
            Serial.print("Neplatný výběr z knihovny pro kanál ");
            Serial.println(i);
          }
        }
      } else if (method == "learned") {
        String p = "code" + String(i) + "_learned=";
        int ci = request.indexOf(p);
        if (ci != -1) {
          int s2 = ci + p.length();
          int e2 = request.indexOf('&', s2);
          if (e2 == -1) e2 = request.indexOf(' ', s2);
          String codeStr = request.substring(s2, e2);
          newCode = strtoul(codeStr.c_str(), NULL, 16);
        }
      }

      if (newCode != 0) {
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

  // 2) Vygenerovani HTML pro konfiguraci IR kodu (s tlacitkem na /scenes)
  String html = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n";
  html += "<html><head><meta charset='UTF-8'><title>IR Code Config</title></head><body>";
  html += "<button onclick=\"window.location='/scenes'\">DMX Scenes</button>";
  html += "<h1>IR Code Configuration</h1>";
  html += "<p>Zadejte IR kód (hex) nebo vyberte z nabídky pro daný DMX kanál, který bude vyslán při hodnotě 255.</p>";
  html += "<form action='/' method='GET'>";

  for (int i = 1; i <= 6; i++) {
    html += "<div style='border:1px solid #ccc;padding:10px;margin-bottom:10px;'>";
    html += "<h3>Kanál " + String(i) + "</h3>";
    html += "<input type='radio' name='channel" + String(i) + "_method' value='manual' checked onclick='showOptions(" + String(i) + ")'> Manual ";
    html += "<input type='radio' name='channel" + String(i) + "_method' value='library' onclick='showOptions(" + String(i) + ")'> Library ";
    html += "<input type='radio' name='channel" + String(i) + "_method' value='learned' onclick='showOptions(" + String(i) + ")'> Learned <br>";

    // Manualni vstup
    html += "<div id='code_manual_" + String(i) + "'>";
    html += "Manual: <input type='text' name='code" + String(i) + "_manual' value='";
    char buf[9];
    sprintf(buf, "%08X", learnedIRCodes[i]);
    html += buf;
    html += "'></div>";

    // Vstupy z library
    html += "<div id='code_library_" + String(i) + "' style='display:none;'>";
    html += "Manufacturer: <select name='code" + String(i) + "_library_manufacturer' id='code_library_" + String(i) + "_manufacturer' onchange='updateDeviceType(" + String(i) + ")'>";
    html += "<option value='Samsung'>Samsung</option>";
    html += "<option value='LG'>LG</option>";
    html += "<option value='Sony'>Sony</option>";
    html += "<option value='Panasonic'>Panasonic</option>";
    html += "<option value='Philips'>Philips</option>";
    html += "<option value='Yamaha'>Yamaha</option>";
    html += "<option value='Generic'>Air Conditioner</option>";
    html += "</select><br>";
    html += "Device Type: <select name='code" + String(i) + "_library_devicetype' id='code_library_" + String(i) + "_devicetype' onchange='updateCommand(" + String(i) + ")'>";
    html += "<option value='TV'>TV</option>";
    html += "</select><br>";
    html += "Command: <select name='code" + String(i) + "_library_command' id='code_library_" + String(i) + "_command'></select>";
    html += "</div>";

    // Zvolení learned
    html += "<div id='code_learned_" + String(i) + "' style='display:none;'>";
    html += "Learned: <select name='code" + String(i) + "_learned'><option value='0'>None</option>";
    for (int j = 1; j <= 6; j++) {
      if (learnedIRCodes[j] != 0) {
        char codeBuf[9];
        sprintf(codeBuf, "%08X", learnedIRCodes[j]);
        html += "<option value='" + String(codeBuf) + "'>Code " + String(j) + " (0x" + String(codeBuf) + ")</option>";
      }
    }
    html += "</select></div>";
    html += "</div>";
  }

  // Odeslání a skripty
  html += "<input type='submit' value='Uložit nastavení'></form>";
  html += R"(
<script>
var libraryData = {
  'Samsung': {
    'TV': {
      'Power': 'E0E040BF', 'Volume Up': 'E0E0E01F', 'Volume Down': 'E0E0D02F',
      'Channel Up': 'E0E048B7', 'Channel Down': 'E0E008F7'
    },
    'Soundbar': {
      'Power': 'E0E0F00F', 'Mute': 'E0E0D00F', 'Volume Up': 'E0E0E01F'
    }
  },
  'LG': {
    'TV': {
      'Power': '20DF10EF', 'Volume Up': '20DF8877', 'Volume Down': '20DF9867',
      'Input HDMI1': '20DF00FF'
    }
  },
  'Sony': {
    'TV': {
      'Power': 'A90', 'Volume Up': '490', 'Volume Down': 'C90', 'Mute': '290'
    }
  },
  'Panasonic': {
    'TV': {
      'Power': '4004', 'Volume Up': '400C', 'Volume Down': '400E'
    },
    'DVD': {
      'Play': '500F', 'Pause': '5010', 'Stop': '500B'
    }
  },
  'Philips': {
    'TV': {
      'Power': '30CF', 'Volume Up': '30DF', 'Volume Down': '30EF'
    }
  },
  'Yamaha': {
    'AV Receiver': {
      'Power': 'A55A', 'Mute': 'A45A', 'Volume Up': 'A15E', 'Volume Down': 'A05E'
    }
  },
  'Generic': {
    'Air Conditioner': {
      'Power': '20DF10EF', 'Temp Up': '20DF40BF', 'Temp Down': '20DF807F',
      'Mode Cool': '20DFC03F', 'Mode Heat': '20DF20DF'
    }
  }
};

function updateDeviceType(channel) {
  var manu = document.getElementById('code_library_' + channel + '_manufacturer').value;
  var devSelect = document.getElementById('code_library_' + channel + '_devicetype');
  // Vynulovat všechny typy podle manu
  var opts = "";
  for (var dev in libraryData[manu]) {
    opts += "<option value='" + dev + "'>" + dev + "</option>";
  }
  devSelect.innerHTML = opts;
  updateCommand(channel);
}

function updateCommand(channel) {
  var manu = document.getElementById('code_library_' + channel + '_manufacturer').value;
  var dev  = document.getElementById('code_library_' + channel + '_devicetype').value;
  var cmds = libraryData[manu][dev];
  var options = "";
  for (var k in cmds) {
    options += "<option value='" + k + "'>" + k + " (0x" + cmds[k] + ")</option>";
  }
  document.getElementById('code_library_' + channel + '_command').innerHTML = options;
}

function showOptions(channel) {
  var m = document.querySelector('input[name="channel' + channel + '_method"]:checked').value;
  document.getElementById('code_manual_'  + channel).style.display  = (m=='manual')  ? 'block':'none';
  document.getElementById('code_library_' + channel).style.display  = (m=='library') ? 'block':'none';
  document.getElementById('code_learned_' + channel).style.display  = (m=='learned') ? 'block':'none';
  if (m=='library') updateDeviceType(channel);
}
</script>
</body></html>
)";

  client.print(html);
  delay(1);
  client.stop();
  Serial.println("Client disconnected.");
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
  
  initDMXTransciever();
  
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

  // Načtení uložených DMX scén
  for (int i = 0; i < 6; i++) {
    char key[12];
    sprintf(key, "scene%d", i + 1);
    size_t len = preferences.getBytes(key, scenes[i], sizeof(scenes[i]));
    Serial.printf("Načtena scéna %d: načteno %u bajtů, první kanál = %d\n",
                  i + 1, (unsigned)len, scenes[i][0]);
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
          initDMXTransciever();
          digitalWrite(MAX485_CTRL_PIN, LOW);
        } else if (menuIndexMain == 1) {
          activeMode = MODE_IR_TO_DMX;
          menuMode   = false;
          Serial.println("Vybráno: IR to DMX");
          updateMenuBaseline();
          digitalWrite(MAX485_CTRL_PIN, HIGH);
          
          
          // reset stavů
          irToDmxFirstEntry = true;
          irToDmxLastScene  = -1;
} else if (menuIndexMain == 2) {
          // Při výběru IR Learn z hlavního menu přejdeme do submenu a resetujeme index
          menuLevel = 2;
          menuIndexIRLearn = 0;
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
          // Přechod do IR Learn režimu až po uvolnění a následném stisku tlačítka
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
    if (activeMode == MODE_IR_TO_DMX) {
      runIrToDmx();
      checkReturnToMenu();
    } else if (activeMode == MODE_DMX_TO_IR) {
      runDmxToIr();
      checkReturnToMenu();
    } else if (activeMode == MODE_IR_LEARN) {
      if (menuLevel == 3) {
        runIrLearn();
      }
      checkReturnToMenu();
    }
  }
}