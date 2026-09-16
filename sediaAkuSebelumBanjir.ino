#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// --- PIN ---
#define DHTPIN        4
#define DHTTYPE      DHT22
#define TRIG_PIN      5
#define ECHO_PIN     18

// 2 Servo & 1 Potensio
#define SERVO1_PIN   19
#define SERVO2_PIN   16  // GPIO 16 (Label pin RX2)
#define POT_PIN      34  // Analog Input Only

#define BUZZER_PIN   14
#define BUTTON_PIN   13

// --- WIFI, MQTT & TELEGRAM CONFIG ---
const char* WIFI_SSID = "UGM-Hotspot";
const char* WIFI_PASSWORD = ""; // Jaringan terbuka / tanpa password

const char* MQTT_SERVER = "192.168.110.156";
const int MQTT_PORT = 1883;
const char* TOPIC_STATUS = "rumah/esp32/alarm/status";
const char* TOPIC_MUTE = "rumah/esp32/alarm/cmd/mute";

// Kredensial Bot Telegram (@SediaAkuSebelumBanjir_bot)
#define BOT_TOKEN "8821676777:AAGdqVN2Y9lpsvF2Xb-LnwxWIr3ALwtpFRs"
#define CHAT_ID   "7668523620"

// --- OBJEK ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHTPIN, DHTTYPE);
Servo myservo1;
Servo myservo2;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// --- VARIABEL UTAMA ---
bool buzzerMuted = false;
float suhu = 0;
float kelembapan = 0;
float jarak = 400; // Default aman
bool lastAlarmState = false;

// Variabel Waktu (Non-Blocking)
unsigned long prevSensorMillis = 0;
unsigned long prevDhtMillis = 0;
unsigned long prevMqttMillis = 0;
unsigned long prevMqttTry = 0;
unsigned long prevWifiTry = 0;
unsigned long prevSirenMillis = 0;
unsigned long prevTelegramMillis = 0;

const long intervalSensor = 200;
const long intervalDht = 2000;
const long intervalMqtt = 2000;
const long intervalTelegram = 1000; // Cek pesan Telegram tiap 1 detik

// Variabel Debounce Tombol Physical
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;

// --- VARIABEL SIRINE EVAKUASI AMBULAN / BENCANA (SWEEP WAIL) ---
int sirenFreq = 500;             // Frekuensi awal
bool sirenRising = true;         // Arah gelombang suara (naik/turun)
const int SIREN_MIN = 500;       // Nada terendah (Hz)
const int SIREN_MAX = 1500;      // Nada tertinggi (Hz)
const int SIREN_STEP = 15;       // Kehalusan kenaikan nada
const long SIREN_SPEED = 8;      // Kecepatan sweep (makin kecil makin cepat meliuk)

// --- BACA JARAK ULTRASONIK ---
float bacaJarak() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) return 400.0;
  return duration * 0.034 / 2.0;
}

// --- FUNGSI SIRINE EVAKUASI AMBULAN (GELOMBANG WAIL) ---
void updateSiren() {
  unsigned long currentMillis = millis();
  
  if (currentMillis - prevSirenMillis >= SIREN_SPEED) {
    prevSirenMillis = currentMillis;

    ledcWriteTone(BUZZER_PIN, sirenFreq);

    if (sirenRising) {
      sirenFreq += SIREN_STEP;
      if (sirenFreq >= SIREN_MAX) {
        sirenFreq = SIREN_MAX;
        sirenRising = false; // Capai puncak, lalu turun
      }
    } else {
      sirenFreq -= SIREN_STEP;
      if (sirenFreq <= SIREN_MIN) {
        sirenFreq = SIREN_MIN;
        sirenRising = true;  // Capai dasar, lalu naik lagi
      }
    }
  }
}

void stopSiren() {
  ledcWriteTone(BUZZER_PIN, 0); // Matikan frekuensi
  sirenFreq = SIREN_MIN;
  sirenRising = true;
}

// --- FUNGSI PESAN MASUK TELEGRAM ---
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;

    if (text == "/start" || text == "/help") {
      String msg = "🤖 *Bot Sedia Aku Sebelum Banjir*\n\n";
      msg += "Perintah yang tersedia:\n";
      msg += "/status - Cek kondisi sensor saat ini\n";
      msg += "/mute - Matikan bunyi sirine alarm\n";
      msg += "/unmute - Aktifkan kembali sirine alarm\n";
      bot.sendMessage(chat_id, msg, "Markdown");
    }
    else if (text == "/status") {
      bool isAlarm = (jarak < 20.0 && jarak > 0);
      String msg = "📊 *STATUS SISTEM SUNGAI*\n\n";
      msg += "🌊 Ketinggian Air: " + String(jarak, 1) + " cm\n";
      msg += "🌡️ Suhu Lingkungan: " + String(suhu, 1) + " °C\n";
      msg += "💧 Kelembapan: " + String(kelembapan, 1) + " %\n";
      msg += "🚨 Status Alarm: " + String(isAlarm ? " BAHAYA / ALARM!" : "✅ AMAN") + "\n";
      msg += "🔇 Mute Status: " + String(buzzerMuted ? "YA" : "TIDAK") + "\n";
      bot.sendMessage(chat_id, msg, "Markdown");
    }
    else if (text == "/mute") {
      buzzerMuted = true;
      stopSiren();
      bot.sendMessage(chat_id, "🔇 Sirine alarm berhasil di-mute via Telegram.", "");
    }
    else if (text == "/unmute") {
      buzzerMuted = false;
      bot.sendMessage(chat_id, "🔊 Mute sirine dilepas via Telegram.", "");
    }
  }
}

// --- PERINTAH DARI MQTT ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_MUTE) != 0) return;

  if (length == 2 && payload[0] == 'O' && payload[1] == 'N') {
    buzzerMuted = true;
    stopSiren();
    Serial.println("Buzzer di-mute dari MQTT");
  }

  if (length == 3 && payload[0] == 'O' && payload[1] == 'F' && payload[2] == 'F') {
    buzzerMuted = false;
    Serial.println("Mute buzzer dilepas dari MQTT");
  }
}

// --- HUBUNGKAN MQTT ---
void connectMQTT() {
  if (mqtt.connected()) return;

  if (millis() - prevMqttTry < 5000) return;
  prevMqttTry = millis();

  String clientId = "esp32-alarm-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("Menghubungkan MQTT... ");

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("berhasil");
    mqtt.subscribe(TOPIC_MUTE);
  } else {
    Serial.print("gagal, kode: ");
    Serial.println(mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.print("System Starting");

  dht.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Setup Buzzer PWM (API ESP32 Core v3.0+)
  ledcAttach(BUZZER_PIN, 2000, 8);
  stopSiren();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Setup Timer & Attach Dual Servo
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  
  myservo1.setPeriodHertz(50);
  myservo1.attach(SERVO1_PIN, 500, 2400);

  myservo2.setPeriodHertz(50);
  myservo2.attach(SERVO2_PIN, 500, 2400);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID); // Koneksi tanpa password

  // Abaikan verifikasi sertifikat SSL Telegram agar koneksi lebih cepat
  secured_client.setInsecure();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  delay(1000);
  lcd.clear();
}

void loop() {
  unsigned long currentMillis = millis();

  // Menjaga koneksi Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    if (currentMillis - prevWifiTry > 10000) {
      prevWifiTry = currentMillis;
      Serial.println("Mencoba WiFi kembali...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID);
    }
  } else {
    connectMQTT();
    mqtt.loop();

    // Polling Pesan Telegram Masuk
    if (currentMillis - prevTelegramMillis >= intervalTelegram) {
      prevTelegramMillis = currentMillis;
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while (numNewMessages) {
        handleNewMessages(numNewMessages);
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
    }
  }

  // Tombol Mute Manual dengan Debounce & Latching
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = currentMillis;
  }
  if ((currentMillis - lastDebounceTime) > 50) {
    if (reading == LOW && !buzzerMuted) {
      buzzerMuted = true;
      stopSiren();
      Serial.println("Buzzer di-mute via Tombol Physical!");
    }
  }
  lastButtonState = reading;

  // Baca jarak & sensor
  if (currentMillis - prevSensorMillis >= intervalSensor) {
    prevSensorMillis = currentMillis;

    jarak = bacaJarak();

    if (currentMillis - prevDhtMillis >= intervalDht) {
      prevDhtMillis = currentMillis;

      float t = dht.readTemperature();
      float h = dht.readHumidity();

      if (!isnan(t)) suhu = t;
      if (!isnan(h)) kelembapan = h;
    }

    bool alarm = (jarak < 20.0 && jarak > 0);

    // Reset status mute HANYA ketika kondisi alarm sudah kembali aman
    if (!alarm) {
      buzzerMuted = false;
    }

    // NOTIFIKASI OTOMATIS TELEGRAM (Push Alert saat status berubah)
    if (alarm && !lastAlarmState) {
      String alertMsg = "🚨 *PERINGATAN BAHAYA BANJIR!*\n";
      alertMsg += "Jarak terdeteksi: " + String(jarak, 1) + " cm!\n";
      alertMsg += "Servo telah terkunci di 90° dan Sirine Evakuasi aktif.";
      bot.sendMessage(CHAT_ID, alertMsg, "Markdown");
    } else if (!alarm && lastAlarmState) {
      String safeMsg = "✅ *KONDISI KEMBALI AMAN*\n";
      safeMsg += "Jarak terdeteksi: " + String(jarak, 1) + " cm.\n";
      safeMsg += "Kontrol Servo manual diaktifkan kembali.";
      bot.sendMessage(CHAT_ID, safeMsg, "Markdown");
    }
    lastAlarmState = alarm;

    // Display LCD
    char line1[17];
    char line2[17];

    snprintf(line1, sizeof(line1), "Jrk:%3dcm %s", (int)jarak, alarm ? "ALARM!" : "AMAN");
    snprintf(line2, sizeof(line2), "T:%2d%cC H:%2d%%", (int)suhu, (char)223, (int)kelembapan);

    lcd.setCursor(0, 0);
    lcd.print(line1);

    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  // Evaluasi Kondisi Alarm & Dual Servo
  bool alarm = (jarak < 20.0 && jarak > 0);

  if (alarm) {
    // Kedua Servo dikunci ke 90 derajat saat Alarm aktif
    myservo1.write(90);
    myservo2.write(90);

    if (!buzzerMuted) {
      updateSiren(); // Sirine evakuasi bergelombang (Wail Siren)
    } else {
      stopSiren();
    }
  } else {
    stopSiren();

    // Kontrol Kedua Servo Serentak dari 1 Potensiometer (GPIO 34)
    int potVal = analogRead(POT_PIN);
    int servoAngle = map(potVal, 0, 4095, 0, 180);

    myservo1.write(servoAngle);
    myservo2.write(servoAngle);
  }

  // Kirim status ke MQTT
  if (mqtt.connected() && currentMillis - prevMqttMillis >= intervalMqtt) {
    prevMqttMillis = currentMillis;

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"suhu\":%.1f,\"kelembapan\":%.1f,\"jarak\":%.1f,\"alarm\":%s,\"mute\":%s}",
             suhu, kelembapan, jarak, alarm ? "true" : "false", buzzerMuted ? "true" : "false");

    mqtt.publish(TOPIC_STATUS, payload);
  }

  delay(1);
}
