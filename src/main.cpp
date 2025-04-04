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
int menuIndexIRLearn = 0;   // IR Learn submenu: 0 až 5 pro pozice 1 až 6, 6 = Exit
bool wifiAPEnabled = false;

#define IR_SELECT 0xFFA25D
#define IR_UP     0xFF629D
#define IR_DOWN   0xFFE21D

uint32_t learnedIRCodes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
unsigned long irLearnStartTime = 0;

unsigned long lastButtonTime = 0;
const unsigned long debounceDelay = 200; // 200 ms debounce

unsigned long modeEnteredTime = 0; // Čas vstupu do režimu IR to DM

// Pro relativní indexaci – uložíme baseline hodnotu enkodéru při vstupu do menu
long menuBaseline = 0;

void updateMenuBaseline() {
  menuBaseline = encoder.getCount();
}

int getRelativeIndex(int numItems) {
  long rel = encoder.getCount() - menuBaseline;
  int idx = (rel / 2) % numItems;  // dělení 2, protože každý detent inkrementuje hodnotu o 2
  if (idx < 0) idx += numItems;
  return idx;
}

void resetEncoder() {
  encoder.setCount(0);
}

void initDMXReceiver() {
  dmx_driver_uninstall(dmxPort);
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_driver_install(dmxPort, &config, DMX_INTR_FLAGS_DEFAULT);
  // Nastavujeme RX na GPIO18, TX na GPIO19
  dmx_set_pin(dmxPort, 19, 18, ENABLE_PIN);
  Serial.println("DMX driver inicializován pro příjem (receiver)");
}

void initDMXTransmitter() {
  dmx_driver_uninstall(dmxPort);
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_driver_install(dmxPort, &config, DMX_INTR_FLAGS_DEFAULT);
  dmx_set_pin(dmxPort, 19, 18, ENABLE_PIN);
  Serial.println("DMX driver inicializován pro vysílání (transmitter)");
}

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
    Serial.print("IR Learn menu index: ");
    Serial.println(idx);
  }
  else if (menuLevel == 3) { // IR Learn mode
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IR Learn:");
    display.setCursor(0, 10);
    display.println("Waiting for code...");
  }
  display.display();
}

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
  // Ihned při vstupu do režimu IR to DM aktualizujeme displej
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

void runIrLearn() {
  if (irLearnStartTime == 0) {
    irLearnStartTime = millis();
  }
  
  if (millis() - irLearnStartTime >= 10000) {
    Serial.println("IR Learn timeout.");
    irLearnStartTime = 0;
    menuLevel = 2;
    menuMode = true;
    updateMenuBaseline();
    drawMenu();
    return;
  }
  
  drawMenu();
  
  if (irrecv.decode(&results)) {
    // Uložíme IR kód do pozice podle relativního indexu
    int pos = getRelativeIndex(6) + 1;
    learnedIRCodes[pos] = results.value;
    Serial.print("Naučený IR kód pro pozici ");
    Serial.print(pos);
    Serial.print(": 0x");
    Serial.println(results.value, HEX);
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
    menuLevel = 2;
    menuMode = true;
    updateMenuBaseline();
    drawMenu();
  }
}

void checkReturnToMenu() {
  if (activeMode == MODE_IR_TO_DM && (millis() - modeEnteredTime < 1000)) {
    return;
  }
  if (digitalRead(ENCODER_BTN_PIN) == LOW) {
    Serial.println("Návrat do menu");
    updateMenuBaseline();
    activeMode = MODE_MENU;
    menuMode = true;
    menuLevel = 0;
    updateMenuBaseline();
    drawMenu();
    initDMXReceiver();
    lastButtonTime = millis();
  }
}

void setup() {
  Serial.begin(115200, SERIAL_8N1, 34, 1); // UART0: RX na GPIO34, TX na GPIO1
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
}

void loop() {
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
        Serial.print("IR Learn menu index: ");
        Serial.println(menuIndexIRLearn);
      }
    }
    
    if (digitalRead(ENCODER_BTN_PIN) == LOW && (millis() - lastButtonTime > debounceDelay)) {
      lastButtonTime = millis();
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
          // Počkejme, dokud tlačítko nebude uvolněno
          while (digitalRead(ENCODER_BTN_PIN) == LOW) {
            delay(10);
          }
          display.clearDisplay();
          display.setTextSize(2);
          display.setCursor(0, 0);
          display.println("IR to DM");
          display.setTextSize(1);
          display.setCursor(0, 30);
          display.println("Čekám na IR");
          display.display();
        } else if (menuIndexMain == 2) {
          activeMode = MODE_IR_LEARN;
          menuLevel = 2;
          updateMenuBaseline();
          drawMenu();
          Serial.println("Vybráno: IR Learn");
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
          menuLevel = 3;
          menuMode = false;
          irLearnStartTime = millis();
          updateMenuBaseline();
          drawMenu();
          Serial.print("Nastavuji IR Learn pro pozici ");
          Serial.println(menuIndexIRLearn + 1);
        } else {
          menuLevel = 0;
          updateMenuBaseline();
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


// ahoj HAOJ