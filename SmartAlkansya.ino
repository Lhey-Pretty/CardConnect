#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

LiquidCrystal_I2C lcd(0x27, 20, 4);

// ================= PINS =================
#define SS_PIN       5
#define RST_PIN      22
#define COIN_PIN     4
#define SOLENOID_PIN 16

MFRC522 mfrc522(SS_PIN, RST_PIN);

// ================= WIFI =================
const char* WIFI_SSID = "Hellooo";
const char* WIFI_PASS = "@lheyprtty";

// ================= FIREBASE / FIRESTORE =================
const char* FIREBASE_PROJECT = "cardconnect-c94ae-931d2";
const char* FIREBASE_API_KEY = "AIzaSyBoKkbBZwJd-AxGZHg3ENiQEr-MGqdKgKs";
const char* FIREBASE_DOC = "admin";
const char* ACCOUNT_NAME = "Lheyyy";

// ================= KEYPAD =================
byte rowPins[4] = {13, 12, 14, 27};
byte colPins[4] = {26, 25, 33, 32};

char keys[4][4] = {
  {'1','4','7','*'},
  {'2','5','8','0'},
  {'3','6','9','#'},
  {'A','B','C','D'}
};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, 4, 4);

// ================= FIRESTORE CACHE =================
String authorizedUIDs[12];
int authorizedUIDCount = 0;

// ================= VARIABLES =================
volatile int pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
unsigned long lastWifiReconnectAttempt = 0;

float currentBalance = 0.00;
float sessionDepositTotal = 0.0;
String correctPIN = "1234";
String sessionUID = "";
bool menuShown = false;

// ================= COIN VALUE =================
float getCoinValue(int pulses) {
  if (pulses == 1) return 1;
  if (pulses >= 4 && pulses <= 6) return 5;
  if (pulses >= 9 && pulses <= 11) return 10;
  if (pulses >= 18 && pulses <= 22) return 20;
  return 0;
}

// ================= INTERRUPT =================
void IRAM_ATTR countPulse() {
  unsigned long now = millis();
  if (now - lastPulseTime > 120) {
    pulseCount++;
    lastPulseTime = now;
  }
}

// ================= DISPLAY =================
void updateLog(int line, String msg) {
  if (line == 99) {
    lcd.clear();
  } else {
    lcd.setCursor(0, line);
    lcd.print("                    ");
    lcd.setCursor(0, line);
    lcd.print(msg);
  }
  Serial.println("[SYSTEM]: " + msg);
}

void showIdleMessage() {
  updateLog(99, "");
  updateLog(1, "  SMART ALKANSYA");
  updateLog(2, "  READY TO SCAN...");
}

// ================= UID HELPER =================
String getUID() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
    if (i < mfrc522.uid.size - 1) uid += " ";
  }
  uid.toUpperCase();
  return uid;
}

String normalizeUID(String uid) {
  uid.toUpperCase();
  uid.replace("-", "");
  uid.replace(":", "");
  uid.replace(" ", "");
  return uid;
}

// ================= SESSION TOKEN =================
String generateToken(String uid) {
  String raw = uid + String(millis());
  raw.replace(" ", "");
  if (raw.length() > 16) raw = raw.substring(0, 16);
  return raw;
}

// ================= WIFI =================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  updateLog(0, "Connecting WiFi...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    updateLog(0, "WiFi Connected");
    Serial.println("\nIP: " + WiFi.localIP().toString());
  } else {
    updateLog(0, "WiFi Failed");
  }
  delay(1000);
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiReconnectAttempt > 10000) {
    lastWifiReconnectAttempt = millis();
    connectWiFi();
  }
}

// ================= FIRESTORE HELPERS =================
String buildFirestoreURL(String path) {
  return "https://firestore.googleapis.com/v1/projects/" +
         String(FIREBASE_PROJECT) +
         "/databases/(default)/documents/" + path +
         "?key=" + String(FIREBASE_API_KEY);
}

bool getJsonDocument(String path, DynamicJsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, buildFirestoreURL(path));
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  String res = http.getString();
  http.end();

  DeserializationError err = deserializeJson(doc, res);
  return !err;
}

String getStringField(DynamicJsonDocument& doc, const char* key, String fallback = "") {
  if (!doc["fields"].containsKey(key)) return fallback;
  JsonObject field = doc["fields"][key];
  if (field.containsKey("stringValue")) return String((const char*)field["stringValue"]);
  return fallback;
}

float getNumberField(DynamicJsonDocument& doc, const char* key, float fallback = 0.0) {
  if (!doc["fields"].containsKey(key)) return fallback;
  JsonObject field = doc["fields"][key];
  if (field.containsKey("doubleValue")) return field["doubleValue"].as<float>();
  if (field.containsKey("integerValue")) return String((const char*)field["integerValue"]).toFloat();
  return fallback;
}

void refreshAuthorizedUIDs() {
  DynamicJsonDocument doc(8192);
  authorizedUIDCount = 0;

  if (!getJsonDocument(String("users/") + FIREBASE_DOC, doc)) {
    updateLog(0, "UID Sync Failed");
    return;
  }

  JsonArray arr = doc["fields"]["authorizedUids"]["arrayValue"]["values"].as<JsonArray>();
  if (arr.isNull()) {
    updateLog(0, "No authorizedUids");
    return;
  }

  for (JsonVariant v : arr) {
    if (authorizedUIDCount >= 12) break;
    String raw = v["stringValue"] | "";
    raw.toUpperCase();
    authorizedUIDs[authorizedUIDCount++] = normalizeUID(raw);
  }

  updateLog(0, "UIDs Loaded: " + String(authorizedUIDCount));
}

float getBalanceFromFirebase() {
  DynamicJsonDocument doc(8192);
  if (!getJsonDocument(String("users/") + FIREBASE_DOC, doc)) return 0.0;
  return getNumberField(doc, "balance", 0.0);
}

void updateBalanceInFirebase(float newBalance) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://firestore.googleapis.com/v1/projects/" +
               String(FIREBASE_PROJECT) +
               "/databases/(default)/documents/users/" + FIREBASE_DOC +
               "?updateMask.fieldPaths=balance&key=" + String(FIREBASE_API_KEY);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.PATCH("{\"fields\":{\"balance\":{\"doubleValue\":" + String(newBalance, 2) + "}}}");
  http.end();
}

void addTransactionToFirebase(String type, float amount, float balanceAfter) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, buildFirestoreURL("transactions"));
  http.addHeader("Content-Type", "application/json");

  String body = "{\"fields\":{\"user\":{\"stringValue\":\"" + String(FIREBASE_DOC) + "\"},";
  body += "\"uid\":{\"stringValue\":\"" + sessionUID + "\"},";
  body += "\"type\":{\"stringValue\":\"" + type + "\"},";
  body += "\"amount\":{\"doubleValue\":" + String(amount, 2) + "},";
  body += "\"balanceAfter\":{\"doubleValue\":" + String(balanceAfter, 2) + "},";
  body += "\"timestamp\":{\"integerValue\":" + String(millis()) + "}}}";

  http.POST(body);
  http.end();
}

void writeSessionToken(String token) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://firestore.googleapis.com/v1/projects/" +
               String(FIREBASE_PROJECT) +
               "/databases/(default)/documents/sessions/" + FIREBASE_DOC +
               "?key=" + String(FIREBASE_API_KEY);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"fields\":{\"token\":{\"stringValue\":\"" + token + "\"},";
  body += "\"uid\":{\"stringValue\":\"" + sessionUID + "\"},";
  body += "\"user\":{\"stringValue\":\"" + String(FIREBASE_DOC) + "\"},";
  body += "\"active\":{\"booleanValue\":true},";
  body += "\"timestamp\":{\"integerValue\":" + String(millis()) + "}}}";
  http.PATCH(body);
  http.end();
}

void clearSessionToken() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://firestore.googleapis.com/v1/projects/" +
               String(FIREBASE_PROJECT) +
               "/databases/(default)/documents/sessions/" + FIREBASE_DOC +
               "?key=" + String(FIREBASE_API_KEY);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"fields\":{\"token\":{\"stringValue\":\"\"},";
  body += "\"uid\":{\"stringValue\":\"" + sessionUID + "\"},";
  body += "\"user\":{\"stringValue\":\"" + String(FIREBASE_DOC) + "\"},";
  body += "\"active\":{\"booleanValue\":false},";
  body += "\"timestamp\":{\"integerValue\":" + String(millis()) + "}}}";
  http.PATCH(body);
  http.end();
}

bool isAllowedUID(String uid) {
  String normalized = normalizeUID(uid);
  for (int i = 0; i < authorizedUIDCount; i++) {
    if (authorizedUIDs[i] == normalized) return true;
  }
  return false;
}

// ================= COIN HANDLER =================
void handleCoinDeposit() {
  if (pulseCount > 0 && (millis() - lastPulseTime > 1200)) {
    int captured = pulseCount;
    pulseCount = 0;
    float added = getCoinValue(captured);
    if (added > 0) {
      currentBalance += added;
      sessionDepositTotal += added;
      updateLog(0, "INSERT COINS...");
      updateLog(1, "COIN: P" + String(added, 2));
      updateLog(2, "SUB:  P" + String(sessionDepositTotal, 2));
      updateLog(3, "D=Done");
    }
  }
}

// ================= PIN CHECK =================
bool checkPIN() {
  String inputPIN = "";
  updateLog(99, "");
  updateLog(0, "ENTER PIN:");
  unsigned long pinTimer = millis();

  while (inputPIN.length() < 4) {
    if (millis() - pinTimer > 15000) {
      updateLog(2, "TIMEOUT!");
      delay(1500);
      return false;
    }

    char key = keypad.getKey();
    if (key) {
      if (key >= '0' && key <= '9') {
        inputPIN += key;
        lcd.setCursor(inputPIN.length() + 6, 1);
        lcd.print("*");
        pinTimer = millis();
      } else if (key == 'D') {
        return false;
      }
    }
    delay(10);
  }

  if (inputPIN == correctPIN) {
    updateLog(2, "PIN CORRECT!");
    delay(1000);
    return true;
  }

  updateLog(2, "WRONG PIN!");
  delay(2000);
  return false;
}

// ================= ENTER AMOUNT =================
float enterAmount(String label) {
  String input = "";
  updateLog(99, "");
  updateLog(0, label);
  updateLog(1, "Amount: P");
  updateLog(3, "A=OK B=DEL D=Cancel");

  while (true) {
    char key = keypad.getKey();
    if (key) {
      if (key >= '0' && key <= '9') {
        if (input.length() < 6) {
          input += key;
          updateLog(1, "Amount: P" + input);
        }
      } else if (key == 'B') {
        if (input.length() > 0) {
          input = input.substring(0, input.length() - 1);
          updateLog(1, "Amount: P" + input);
        }
      } else if (key == 'A') {
        if (input.length() > 0) return input.toFloat();
      } else if (key == 'D') {
        return -1;
      }
    }
    delay(10);
  }
}

// ================= OPEN BOX =================
void openBoxWithClose(String reason) {
  updateLog(99, "");
  updateLog(0, reason);
  updateLog(1, "BOX IS OPEN");
  updateLog(2, "Press A to Close");
  digitalWrite(SOLENOID_PIN, LOW);
  while (true) {
    if (keypad.getKey() == 'A') break;
    delay(200);
  }
  digitalWrite(SOLENOID_PIN, HIGH);
  updateLog(99, "");
  updateLog(1, "BOX LOCKED.");
  delay(1500);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 17);
  lcd.init();
  lcd.backlight();
  SPI.begin();
  mfrc522.PCD_Init();
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), countPulse, FALLING);
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, HIGH);
  connectWiFi();
  refreshAuthorizedUIDs();
  showIdleMessage();
}

// ================= LOOP =================
void loop() {
  ensureWiFiConnected();

  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    sessionUID = getUID();

    if (!isAllowedUID(sessionUID)) {
      updateLog(99, "");
      updateLog(1, "UNKNOWN CARD!");
      updateLog(2, "UID: " + sessionUID);
      delay(2500);
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      showIdleMessage();
      return;
    }

    if (!checkPIN()) {
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      showIdleMessage();
      return;
    }

    updateLog(99, "");
    updateLog(0, "Hi, " + String(ACCOUNT_NAME) + "!");
    updateLog(1, "Loading...");
    currentBalance = getBalanceFromFirebase();

    String token = generateToken(sessionUID);
    updateLog(2, "Opening web session...");
    writeSessionToken(token);
    addTransactionToFirebase("Access", 0, currentBalance);

    bool inSystem = true;
    menuShown = false;

    while (inSystem) {
      if (!menuShown) {
        updateLog(99, "");
        updateLog(0, ACCOUNT_NAME);
        updateLog(1, "A.Trans  B.Balance");
        updateLog(2, "C.OpenBox");
        updateLog(3, "D.Exit");
        menuShown = true;
      }

      handleCoinDeposit();
      char key = keypad.getKey();

      if (key == 'A') {
        bool inTrans = true;
        bool tmShown = false;
        while (inTrans) {
          if (!tmShown) {
            updateLog(99, "");
            updateLog(0, "1.Deposit");
            updateLog(1, "2.Withdraw");
            updateLog(2, "3.Back");
            tmShown = true;
          }

          handleCoinDeposit();
          char tKey = keypad.getKey();
          delay(10);

          if (tKey == '1') {
            sessionDepositTotal = 0.0;
            updateLog(99, "");
            updateLog(0, "INSERT COINS...");
            updateLog(1, "COIN: P0.00");
            updateLog(2, "SUB:  P0.00");
            updateLog(3, "D=Done");

            while (true) {
              handleCoinDeposit();
              if (keypad.getKey() == 'D') break;
              delay(10);
            }

            if (sessionDepositTotal > 0) {
              updateLog(99, "");
              updateLog(1, "Saving...");
              updateBalanceInFirebase(currentBalance);
              addTransactionToFirebase("Deposit", sessionDepositTotal, currentBalance);
              updateLog(1, "Saved! Bal: P" + String(currentBalance, 2));
              delay(1500);
            }
            tmShown = false;
          } else if (tKey == '2') {
            float wa = enterAmount("WITHDRAW AMOUNT:");
            if (wa > 0 && wa <= currentBalance) {
              currentBalance -= wa;
              updateLog(99, "");
              updateLog(1, "Saving...");
              updateBalanceInFirebase(currentBalance);
              addTransactionToFirebase("Withdraw", wa, currentBalance);
              updateLog(1, "Saved!");
              delay(1000);
              openBoxWithClose("TAKE: P" + String(wa, 2));
            } else if (wa > currentBalance) {
              updateLog(2, "INSUFFICIENT!");
              delay(2000);
            }
            tmShown = false;
          } else if (tKey == '3') {
            inTrans = false;
          }
        }
        menuShown = false;
      } else if (key == 'B') {
        updateLog(99, "");
        updateLog(1, "BAL: P" + String(currentBalance, 2));
        delay(3000);
        menuShown = false;
      } else if (key == 'C') {
        openBoxWithClose("BOX OPEN");
        menuShown = false;
      } else if (key == 'D') {
        inSystem = false;
      }
    }

    updateLog(99, "");
    updateLog(1, "Logging out...");
    clearSessionToken();
    delay(800);
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    showIdleMessage();
  }
}
