#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>       // Thêm thư viện SPI
#include <MFRC522.h>   // Thêm thư viện RC522
#include "time.h"

// --- CẤU HÌNH WIFI & SERVER ---
#define WIFI_SSID "iQOO Z10 Turbo"
#define WIFI_PASSWORD "conculo123"
#define SERVER_URL "https://danganhle0623-iot.hf.space" 
#define DEVICE_ID "ESP_PHONG_B202" 

// --- CẤU HÌNH CHÂN CẮM (PHẦN CỨNG) ---
#define RST_PIN      4          
#define SS_PIN       5          
#define BUZZER_PIN   14
#define GREEN_LED    26
#define RED_LED      25
#define RELAY_PIN    27

// --- KHỞI TẠO ĐỐI TƯỢNG NGOẠI VI ---
MFRC522 mfrc522(SS_PIN, RST_PIN); 
LiquidCrystal_I2C lcd(0x27, 16, 2); // Đổi 0x3F nếu LCD không hiện chữ

// --- CẤU TRÚC LƯU TRỮ TRONG RAM ---
struct Student {
  String uid;
  String mssv;
  String name;
};

const int MAX_STUDENTS = 100;
Student students[MAX_STUDENTS];
int currentStudentCount = 0;

// Cấu trúc gói tin gửi vào Hàng đợi (Queue)
struct UploadData {
  char uid[20];
  unsigned long timestamp;
};
QueueHandle_t uploadQueue;

// --- CẤU HÌNH THỜI GIAN NTP ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 25200; // UTC+7
const int   daylightOffset_sec = 0;

String lastUid = "";
unsigned long lastScanMs = 0;
unsigned long lastUpdateListMs = 0; 
const unsigned long UPDATE_LIST_INTERVAL = 30 * 60 * 1000; // 30 phút

// ========================================================
// CÁC HÀM TIỆN ÍCH & XỬ LÝ
// ========================================================

// 1. Lấy Epoch Time (Giờ thực tế)
unsigned long getEpochTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 0;
  time(&now);
  return now;
}

// 2. Chuyển byte mảng UID thành chuỗi String (Dành cho RC522)
String uidToString(byte *buffer, byte bufferSize) {
  String res = "";
  for (byte i = 0; i < bufferSize; i++) {
    res += String(buffer[i] < 0x10 ? "0" : "");
    res += String(buffer[i], HEX);
  }
  res.toUpperCase();
  return res;
}

// 3. Tải danh sách sinh viên từ Server
void fetchStudentList() {
  lcd.clear();
  lcd.print("Tai du lieu...");
  
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String api_url = String(SERVER_URL) + "/api/students/" + String(DEVICE_ID);
    
    Serial.println("Goi API: " + api_url);
    http.begin(api_url);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      
      DynamicJsonDocument doc(8192); // Cấp phát RAM đủ lớn
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print("Loi JSON: "); Serial.println(error.c_str());
        lcd.setCursor(0, 1); lcd.print("Loi du lieu!");
        delay(2000);
        return;
      }

      currentStudentCount = 0;
      JsonArray array = doc.as<JsonArray>();
      
      for (JsonObject repo : array) {
        if (currentStudentCount >= MAX_STUDENTS) break;
        students[currentStudentCount].uid = repo["uid"].as<String>();
        students[currentStudentCount].mssv = repo["mssv"].as<String>();
        students[currentStudentCount].name = repo["name"].as<String>();
        currentStudentCount++;
      }
      
      Serial.printf("Da tai: %d SV.\n", currentStudentCount);
      lcd.setCursor(0, 1); lcd.print("Da tai: " + String(currentStudentCount) + " SV");
      delay(2000);
      
    } else {
      Serial.printf("Loi HTTP: %d\n", httpCode);
      lcd.setCursor(0, 1); lcd.print("Loi may chu!");
      delay(2000);
    }
    http.end();
  }
}

// 4. Luồng chạy ngầm đẩy dữ liệu lên Server (Core 0)
void TaskUpload(void *pvParameters) {
  UploadData data;

  while (true) {
    if (xQueueReceive(uploadQueue, &data, portMAX_DELAY) == pdPASS) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String api_url = String(SERVER_URL) + "/api/attendance";
        
        http.begin(api_url);
        http.addHeader("Content-Type", "application/json");

        StaticJsonDocument<200> doc;
        doc["uid"] = String(data.uid);
        doc["device_id"] = DEVICE_ID;
        doc["time_scan"] = data.timestamp;

        String requestBody;
        serializeJson(doc, requestBody);

        int httpResponseCode = http.POST(requestBody);
        
        if (httpResponseCode > 0) {
          String responseBody = http.getString(); // Bắt lỗi 400 Bad Request
          Serial.printf("Upload HTTP Code: %d\n", httpResponseCode);
          Serial.println("Loi nhan tu Server: " + responseBody);
        } else {
          Serial.printf("Upload Failed - Error: %d\n", httpResponseCode);
        }
        http.end();
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS); // Nhường CPU
  }
}

// 5. Hàm hiển thị LCD nhanh
void displayFast(String mssv, String name) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("OK: " + mssv);
  lcd.setCursor(0, 1);
  if (name.length() > 16) lcd.print(name.substring(0, 16));
  else lcd.print(name);
}

// 6. Xử lý Thẻ Hợp lệ
void accessGranted(const String &uid, const Student &st) {
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RELAY_PIN, HIGH);
  tone(BUZZER_PIN, 2000, 150); // Bíp 1 tiếng thanh
  
  displayFast(st.mssv, st.name);

  UploadData pkg;
  memset(&pkg, 0, sizeof(UploadData));
  strncpy(pkg.uid, uid.c_str(), sizeof(pkg.uid) - 1);
  pkg.timestamp = getEpochTime(); 
  
  if (xQueueSend(uploadQueue, &pkg, 1000 / portTICK_PERIOD_MS) != pdPASS) {
    Serial.println("Loi: Hang doi day, rot goi tin Sinh Vien!");
  }

  delay(1500); 
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RELAY_PIN, LOW);
  
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Ready to Scan...");
  lcd.setCursor(0, 1); lcd.print(DEVICE_ID); 
}

// 7. Xử lý Thẻ Không hợp lệ
void accessDenied(String uid) {
  lcd.clear(); 
  lcd.print("UNKNOWN CARD");
  lcd.setCursor(0, 1); lcd.print(uid);
  
  digitalWrite(RED_LED, HIGH);
  tone(BUZZER_PIN, 500, 800); // Bíp dài cảnh báo

  UploadData pkg;
  memset(&pkg, 0, sizeof(UploadData)); 
  strncpy(pkg.uid, uid.c_str(), sizeof(pkg.uid) - 1);
  pkg.timestamp = getEpochTime();
  
  if (xQueueSend(uploadQueue, &pkg, 1000 / portTICK_PERIOD_MS) != pdPASS) {
    Serial.println("Loi: Hang doi day, rot goi tin Khach!");
  }
  
  delay(1500);
  digitalWrite(RED_LED, LOW);
  lcd.clear(); 
  lcd.print("Ready to Scan...");
  lcd.setCursor(0, 1); lcd.print(DEVICE_ID);
}

// ========================================================
// HÀM SETUP & LOOP CHÍNH
// ========================================================

void setup() {
  Serial.begin(115200);
  
  // Khởi tạo các chuẩn kết nối phần cứng
  Wire.begin(21, 22); // I2C cho màn hình LCD
  SPI.begin();        // SPI cho thẻ RFID
  mfrc522.PCD_Init(); // Khởi tạo RC522
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);

  lcd.init(); 
  lcd.backlight();
  lcd.print("Connecting WiFi");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    lcd.print(".");
  }

// Đồng bộ thời gian qua NTP
  lcd.clear();
  lcd.print("Syncing Time...");
  Serial.print("Dang lay gio tu Internet...");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Ép ESP32 đợi đến khi lấy được giờ chuẩn (Epoch > 1.6 tỷ giây, tương đương từ năm 2020 trở đi)
  while (getEpochTime() < 1600000000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n=> Dong bo thoi gian THANH CONG!");

  uploadQueue = xQueueCreate(20, sizeof(UploadData));

  // Gắn luồng TaskUpload vào Core 0 chạy song song
  xTaskCreatePinnedToCore(TaskUpload, "HttpTask", 12000, NULL, 1, NULL, 0);

  // Lấy dữ liệu lần đầu
  fetchStudentList();
  lastUpdateListMs = millis();

  lcd.clear();
  lcd.print("System Ready!");
  lcd.setCursor(0, 1); lcd.print(DEVICE_ID);
}

void loop() {
  // 1. TỰ ĐỘNG CẬP NHẬT LỊCH MỖI 30 PHÚT
  if (millis() - lastUpdateListMs >= UPDATE_LIST_INTERVAL) {
    Serial.println("Tu dong cap nhat danh sach sau 30 phut...");
    fetchStudentList();
    lastUpdateListMs = millis();
    
    lcd.clear();
    lcd.print("System Ready!");
    lcd.setCursor(0, 1); lcd.print(DEVICE_ID);
  }

  // 2. PHÁT HIỆN VÀ ĐỌC THẺ QUA RC522
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String readUid = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
  
  // 3. CHỐNG DỘI THẺ (SPAM) DƯỚI 2 GIÂY
  if (readUid == lastUid && millis() - lastScanMs < 2000) {
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return; 
  }
  
  lastUid = readUid;
  lastScanMs = millis();
  Serial.println("=> Quet the UID: " + readUid);

  // 4. KIỂM TRA ĐỊNH DANH TRONG RAM
  bool found = false;
  for (int i = 0; i < currentStudentCount; i++) {
    if (readUid.equals(students[i].uid)) {
      accessGranted(readUid, students[i]);
      found = true;
      break;
    }
  }
  
  if (!found) accessDenied(readUid);

  // Giải phóng thẻ khỏi từ trường đầu đọc
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}