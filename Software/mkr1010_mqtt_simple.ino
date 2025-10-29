#include <SPI.h>
#include <WiFiNINA.h>
#include <PubSubClient.h>
#include "arduino_secrets.h"
#include <utility/wifi_drv.h>

const char* ssid          = SECRET_SSID;
const char* password      = SECRET_PASS;
const char* mqtt_username = SECRET_MQTTUSER;
const char* mqtt_password = SECRET_MQTTPASS;
const char* mqtt_server   = "mqtt.cetools.org";
const int   mqtt_port     = 1884;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// MQTT topic and client ID setup
String lightId    = "17"; // Set your unique device ID here
String mqtt_topic = "student/CASA0014/luminaire/" + lightId;
String clientId   = "";

const int WIDTH  = 12;
const int HEIGHT = 6;
const int NUM_LEDS = WIDTH * HEIGHT;     // 72 LEDs total
const int payload_size = NUM_LEDS * 3;   // RGB = 3 bytes per LED
byte RGBpayload[payload_size];

// LED order (bottom → top, each row left → right)
int rowOrder[NUM_LEDS];

// Returns the LED index from (x, y) coordinates
int indexFromXY(int x, int y) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return -1;
  return x * HEIGHT + y; // Column-major order
}

// Builds the bottom-up row order array
void buildRowBottomUpOrder() {
  int k = 0;
  for (int y = HEIGHT - 1; y >= 0; --y) {
    for (int x = 0; x < WIDTH; ++x) {
      rowOrder[k++] = indexFromXY(x, y);
    }
  }
}

// Set color for a specific LED index
inline void setIndexRGB(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= NUM_LEDS) return;
  RGBpayload[i * 3 + 0] = r;
  RGBpayload[i * 3 + 1] = g;
  RGBpayload[i * 3 + 2] = b;
}

// Fill all LEDs with the same RGB color
void fillAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++) {
    RGBpayload[i * 3 + 0] = r;
    RGBpayload[i * 3 + 1] = g;
    RGBpayload[i * 3 + 2] = b;
  }
}

// Publish LED frame to MQTT topic
void publishFrame() {
  mqttClient.publish(mqtt_topic.c_str(), RGBpayload, payload_size);
}

// Publish a solid color frame
void publishSolid(uint8_t r, uint8_t g, uint8_t b) {
  fillAll(r, g, b);
  publishFrame();
}

#define FSR_PIN A0
const int   FSR_BASELINE_SAMPLES = 50;  // Samples for baseline calibration
const float EMA_ALPHA = 0.15f;          // Exponential moving average filter
int   fsrBaseline = 0;
float fsrEma = 0;

int   ADC_THRESHOLD_200G = 950;         // Threshold for human detection
int   TRIGGER_DELTA      = 50;          // Trigger margin above baseline
const unsigned long SENSE_MS  = 50;     // Sampling interval
const unsigned long WINDOW_MS = 3000;   // Sampling window

// ---- Reset Button Configuration ----
#define RESET_PIN 2
const unsigned long DEBOUNCE_MS = 30;
bool btnStable = HIGH, btnLastRead = HIGH;
unsigned long btnLastChange = 0;

#define BEAM_PIN 3
const bool USE_PULLUP = true;
bool INVERTED = false;
int beamStable = HIGH, beamLastRead = HIGH;
unsigned long beamLastChange = 0;
const unsigned long BEAM_DEBOUNCE_MS = 50;

// ---- Watchdog Timer and Yellow Warning ----
const unsigned long WATCHDOG_BLOCK_MS   = 7000; // Time before obstruction alarm
const unsigned long WATCHDOG_RECOVER_MS = 800;  // Time to recover after obstruction clears
bool   watchdogActive = false;
unsigned long blockedStreakMs = 0;
unsigned long okStreakMs      = 0;
unsigned long lastStreakTick  = 0;
const unsigned long YELLOW_HINT_MS = 1500;
unsigned long yellowHintUntil = 0;

bool beamWasBroken = false;
int  intrudeCount = 0;
unsigned long blinkLastMs = 0;
bool blinkOn = true;

// Compute blinking period: more intrusions → faster blinking
unsigned long blinkPeriodForCount(int count) {
  if (count < 1) count = 1;
  long p = 1200 - (count - 1) * 150;
  if (p < 200) p = 200;
  if (p > 1200) p = 1200;
  return (unsigned long)p;
}

// One full row (12 LEDs) per intrusion
const int ROWS_PER_INC = 1;
inline int ledsForCount(int count) {
  long n = (long)count * ROWS_PER_INC * WIDTH;
  if (n > NUM_LEDS) n = NUM_LEDS;
  if (n < 0) n = 0;
  return (int)n;
}

// Light up cumulative red LEDs from bottom up; off when blinking state is false
void renderCumulativeRedBlink(int count, bool on) {
  int totalRed = ledsForCount(count);
  if (!on) {
    fillAll(0, 0, 0);
    publishFrame();
    return;
  }
  fillAll(0, 0, 0);
  for (int i = 0; i < totalRed; i++) {
    int idx = rowOrder[i];
    setIndexRGB(idx, 255, 0, 0);
  }
  publishFrame();
}

// ---- System Phases ----
enum Phase { WAITING, SAMPLING, ALARM_LATCHED };
Phase phase = WAITING;
unsigned long phaseStart = 0;
int peakValue = 0;

// External functions (defined in other files)
void startWifi();
void reconnectMQTT();
void callback(char*, byte*, unsigned int);
void LedRed(); 
void LedGreen(); 
void LedBlue();

void setLED_R(bool on) { WiFiDrv::analogWrite(25, on ? 155 : 0); }
void setLED_G(bool on) { WiFiDrv::analogWrite(26, on ? 155 : 0); }
void setLED_B(bool on) { WiFiDrv::analogWrite(27, on ? 155 : 0); }

// Read beam sensor with debounce filtering
bool readBeamStable() {
  int now = digitalRead(BEAM_PIN);
  if (now != beamLastRead) { beamLastRead = now; beamLastChange = millis(); }
  if (millis() - beamLastChange > BEAM_DEBOUNCE_MS)
    if (beamStable != now) beamStable = now;
  return (INVERTED ? (beamStable == LOW) : (beamStable == HIGH));
}

// Detect reset button press with debounce
bool pollResetPressed() {
  bool now = digitalRead(RESET_PIN);
  if (now != btnLastRead) { btnLastRead = now; btnLastChange = millis(); }
  if (millis() - btnLastChange > DEBOUNCE_MS) {
    if (btnStable != now) { bool prev = btnStable; btnStable = now;
      if (prev == LOW && btnStable == HIGH) return true;
    }
  }
  return false;
}

// Utility: print MAC address to Serial
void printMacAddress(byte mac[]) {
  for (int i = 5; i >= 0; i--) {
    if (mac[i] < 16) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i > 0) Serial.print(":");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  Serial.println("Vespera + FSR + Beam (bottom-up rows + frequency blink)");

  // Generate unique client ID from MAC address
  byte mac[6]; 
  WiFi.macAddress(mac);
  Serial.print("MAC: "); printMacAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "mkr1010-%02X%02X%02X%02X%02X%02X",
           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  clientId = String(buf);

  // Initialize pins
  pinMode(RESET_PIN, INPUT_PULLUP);
  if (USE_PULLUP) pinMode(BEAM_PIN, INPUT_PULLUP); else pinMode(BEAM_PIN, INPUT);

  // Initialize LED layout and network
  buildRowBottomUpOrder();
  startWifi();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(2000);
  mqttClient.setCallback(callback);

  // Calibrate FSR baseline
  long sum = 0; 
  for (int i = 0; i < FSR_BASELINE_SAMPLES; i++) { 
    sum += analogRead(FSR_PIN); 
    delay(5); 
  }
  fsrBaseline = sum / FSR_BASELINE_SAMPLES;
  fsrEma = fsrBaseline;
  Serial.print("FSR baseline = "); Serial.println(fsrBaseline);

  // Default LED color = Blue 
  publishSolid(0, 0, 255); 
  LedBlue();
  lastStreakTick = millis();
  Serial.println("System armed and waiting (BLUE).");
}

// ---- Main Loop ----
void loop() {
  // Maintain WiFi and MQTT connections
  if (!mqttClient.connected()) reconnectMQTT();
  if (WiFi.status() != WL_CONNECTED) startWifi();
  mqttClient.loop();

  // Handle reset button
  if (pollResetPressed() && phase == ALARM_LATCHED) {
    phase = WAITING; intrudeCount = 0; fsrEma = fsrBaseline;
    publishSolid(0, 0, 255); LedBlue();
    Serial.println("Reset pressed → WAITING (count=0, BLUE).");
  }

  // Sensor sampling timing
  static unsigned long lastSense = 0;
  if (millis() - lastSense < SENSE_MS) return;
  lastSense = millis();

  int raw = analogRead(FSR_PIN);
  fsrEma = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * fsrEma;
  bool beam_ok = readBeamStable();

  switch (phase) {

    case WAITING: {
      unsigned long nowMs = millis();
      unsigned long dt = nowMs - lastStreakTick;
      lastStreakTick = nowMs;

      // Watchdog: monitor if beam is blocked for too long
      if (!beam_ok) { blockedStreakMs = min(blockedStreakMs + dt, WATCHDOG_BLOCK_MS); okStreakMs = 0; }
      else          { okStreakMs += dt; if (okStreakMs >= WATCHDOG_RECOVER_MS) blockedStreakMs = 0; }

      if (!watchdogActive && blockedStreakMs >= WATCHDOG_BLOCK_MS) {
        watchdogActive = true;
        Serial.println("[Watchdog] Beam blocked → MAGENTA/YELLOW blink");
      }
      if (watchdogActive && okStreakMs >= WATCHDOG_RECOVER_MS) {
        watchdogActive = false;
        publishSolid(0, 0, 255);
        Serial.println("[Watchdog] Beam recovered → BLUE");
      }

      // Watchdog blinking pattern
      if (watchdogActive) {
        static unsigned long lastBlink = 0; static bool s = false;
        if (millis() - lastBlink > 600) { 
          lastBlink = millis(); s = !s;
          if (s) publishSolid(255, 0, 255); 
          else publishSolid(255, 255, 0);
        }
        break;
      }

      // Yellow hint if beam is interrupted but no pressure detected
      static bool prevBeamOk = true;
      if (!beam_ok && prevBeamOk && fsrEma < fsrBaseline + TRIGGER_DELTA) {
        yellowHintUntil = millis() + YELLOW_HINT_MS;
        publishSolid(255, 255, 0);
        Serial.println("[Hint] Object detected without pressure (YELLOW)");
      }
      prevBeamOk = beam_ok;

      if (yellowHintUntil && millis() >= yellowHintUntil) {
        yellowHintUntil = 0;
        publishSolid(0, 0, 255);
      }

      // Pressure detected: switch to sampling phase
      if (fsrEma > fsrBaseline + TRIGGER_DELTA) {
        phase = SAMPLING; phaseStart = millis(); peakValue = (int)fsrEma;
        LedBlue(); Serial.println(">>> Pressure detected, entering SAMPLING phase");
        yellowHintUntil = 0;
      }
      break;
    }


    case SAMPLING: {
      // Track maximum FSR value during the sampling window
      if ((int)fsrEma > peakValue) peakValue = (int)fsrEma;

      // Sampling window complete
      if (millis() - phaseStart > WINDOW_MS) {
        Serial.print("Sampling done. Peak = "); Serial.println(peakValue);

        // Human detection (high weight)
        if (peakValue >= ADC_THRESHOLD_200G) {
          publishSolid(0, 255, 0); LedGreen();
          Serial.println("=> Human detected (GREEN 3s)");
          delay(3000);
          publishSolid(0, 0, 255); LedBlue();
          phase = WAITING; fsrEma = fsrBaseline; intrudeCount = 0;
        } 
        // Intruder 
        else {
          intrudeCount = 1;
          blinkOn = true; blinkLastMs = millis();
          renderCumulativeRedBlink(intrudeCount, blinkOn); LedRed();
          Serial.println("=> Intruder detected (RED latch, count=1, blinking)");
          phase = ALARM_LATCHED;
          beamWasBroken = !beam_ok;
        }
      }
      break;
    }

    case ALARM_LATCHED: {
      // Count new intrusions 
      if (!beam_ok && !beamWasBroken) {
        beamWasBroken = true;
        intrudeCount++;
        Serial.print("Beam crossed. Count="); Serial.println(intrudeCount);
        blinkOn = true; blinkLastMs = millis();
        renderCumulativeRedBlink(intrudeCount, blinkOn);
      } 
      else if (beam_ok && beamWasBroken) {
        beamWasBroken = false;
      }

      // Handle LED blinking frequency
      unsigned long period = blinkPeriodForCount(intrudeCount);
      if (millis() - blinkLastMs > period / 2) {
        blinkLastMs = millis();
        blinkOn = !blinkOn;
        renderCumulativeRedBlink(intrudeCount, blinkOn);
      }
      break;
    }
  }
}

