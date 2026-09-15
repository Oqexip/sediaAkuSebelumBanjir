#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <PubSubClient.h>

// --- PIN ---
#define DHTPIN       4
#define DHTTYPE      DHT22
#define TRIG_PIN     5
#define ECHO_PIN     18
#define SERVO_PIN    19
#define POT_PIN      34
#define BUZZER_PIN   14
#define BUTTON_PIN   13

// --- WIFI & MQTT ---
const char* WIFI_SSID = "GalangHero";
const char* WIFI_PASSWORD = "noldelapankali";

const char* MQTT_SERVER = "192.168.110.156";
const int MQTT_PORT = 1883;

const char* TOPIC_STATUS = "rumah/esp32/alarm/status";
const char* TOPIC_MUTE = "rumah/esp32/alarm/cmd/mute";

// --- OBJEK ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHTPIN, DHTTYPE);
Servo myservo;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// --- VARIABEL ---
bool buzzerMuted = false;
float suhu = 0;
float kelembapan = 0;
float jarak = 0;

unsigned long prevSensorMillis = 0;
unsigned long prevDhtMillis = 0;
unsigned long prevMqttMillis = 0;
unsigned long prevMqttTry = 0;
unsigned long prevWifiTry = 0;

const long intervalSensor = 500;
const long intervalDht = 2000;
const long intervalMqtt = 2000;

// --- BACA JARAK ---
float bacaJarak() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) return 400;
  return duration * 0.034 / 2;
}

// --- PERINTAH DARI MQTT ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_MUTE) != 0) return;

  // Payload ON = mute; OFF = nyalakan alarm kembali
  if (length == 2 && payload[0] == 'O' && payload[1] == 'N') {
    buzzerMuted = true;
    Serial.println("Buzzer di-mute dari MQTT");
  }

  if (length == 3 &&
      payload[0] == 'O' &&
      payload[1] == 'F' &&
      payload[2] == 'F') {
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

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH); // OFF untuk modul low-trigger

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ESP32PWM::allocateTimer(0);
  myservo.setPeriodHertz(50);
  myservo.attach(SERVO_PIN, 500, 2400);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  } else {
    connectMQTT();
    mqtt.loop();
  }

  // Tombol mute
  if (digitalRead(BUTTON_PIN) == LOW) {
    buzzerMuted = true;
    digitalWrite(BUZZER_PIN, HIGH);
  }

  // Baca jarak dan perbarui LCD setiap 500 ms
  if (currentMillis - prevSensorMillis >= intervalSensor) {
    prevSensorMillis = currentMillis;

    jarak = bacaJarak();

    // DHT22 sebaiknya dibaca tiap 2 detik
    if (currentMillis - prevDhtMillis >= intervalDht) {
      prevDhtMillis = currentMillis;

      float t = dht.readTemperature();
      float h = dht.readHumidity();

      if (!isnan(t)) suhu = t;
      if (!isnan(h)) kelembapan = h;
    }

    if (jarak >= 20.0) {
      buzzerMuted = false;
    }

    bool alarm = jarak < 20.0 && jarak > 0;

    char line1[17];
    char line2[17];

    snprintf(line1, sizeof(line1), "Jrk:%3dcm %s",
             (int)jarak, alarm ? "ALARM!" : "AMAN");

    snprintf(line2, sizeof(line2), "T:%2d%cC H:%2d%%",
             (int)suhu, (char)223, (int)kelembapan);

    lcd.setCursor(0, 0);
    lcd.print("                ");
    lcd.setCursor(0, 0);
    lcd.print(line1);

    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  // Kontrol servo dan buzzer
  bool alarm = jarak < 20.0 && jarak > 0;

  if (alarm) {
    myservo.write(90);

    if (!buzzerMuted) {
      digitalWrite(BUZZER_PIN, LOW);
    } else {
      digitalWrite(BUZZER_PIN, HIGH);
    }
  } else {
    digitalWrite(BUZZER_PIN, HIGH);

    int potVal = analogRead(POT_PIN);
    int servoAngle = map(potVal, 0, 4095, 0, 180);
    myservo.write(servoAngle);
  }

  // Kirim status ke MQTT setiap 2 detik
  if (mqtt.connected() &&
      currentMillis - prevMqttMillis >= intervalMqtt) {

    prevMqttMillis = currentMillis;

    char payload[160];

    snprintf(payload, sizeof(payload),
             "{\"suhu\":%.1f,\"kelembapan\":%.1f,"
             "\"jarak\":%.1f,\"alarm\":%s,\"mute\":%s}",
             suhu,
             kelembapan,
             jarak,
             alarm ? "true" : "false",
             buzzerMuted ? "true" : "false");

    mqtt.publish(TOPIC_STATUS, payload);
    Serial.println(payload);
  }

  delay(10);
}