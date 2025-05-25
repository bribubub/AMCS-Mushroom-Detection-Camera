#include "WiFi.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h" // Needed for converting RGB565 to JPEG
#include "Arduino.h"
#include "soc/soc.h"          // Disable brownout problems
#include "soc/rtc_cntl_reg.h" // Disable brownout problems
#include "driver/rtc_io.h"
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <FS.h> // File System library, also needed for SD
#include <SD_MMC.h> // ESP32-specific SD card library

// Libraries for Sleep and Time
#include "esp_sleep.h"
#include "time.h"
#include "sys/time.h"
#include "esp_sntp.h"

// Libraries for ESP-NOW and JSON
#include <esp_now.h>
#include <ArduinoJson.h> // Required for JSON creation (Install via Library Manager)

// --- Network Credentials ---
const char *ssid = "SkibidiToilet";             // <<<<< CHANGE THIS TO YOUR WIFI SSID
const char *password = "gugugaga"; // <<<<< CHANGE THIS TO YOUR WIFI PASSWORD
const char *ntpServer = "pool.ntp.org";
// Set timezone to WIB (Western Indonesia Time) which is UTC+7
const char *time_zone = "WIB-7"; // Your timezone for accurate scheduling

// --- Server and File Paths ---
AsyncWebServer server(80);
#define FILE_MARKED_PHOTO "/photo.jpg" // Path on SPIFFS for the photo shown on the web page

// --- Camera Configuration ---
// OV2640 camera module pins (CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// Flash LED pin (usually GPIO 4 on ESP32-CAM)
#define FLASH_LED_PIN 4

// --- Analysis Thresholds (within fixed region) ---
#define BRIGHTNESS_THRESHOLD 180    // Pixel brightness to count as white (Try increasing this, e.g., 200, 220)
#define SIZE_THRESHOLD 5000         // White pixel count to consider mushroom large (Pixels within fixed analysis region)
#define BRIGHTNESS_RATIO_MIN 0.05   // Ratio of white pixels to total pixels (within fixed region) - Not currently used directly in final decision
#define CENTER_TOLERANCE 20         // Tolerance in pixels for centering (relative to fixed region center)
#define BBOX_ASPECT_RATIO_MIN 0.5   // Minimum aspect ratio (width/height) of detected object within fixed region
#define BBOX_ASPECT_RATIO_MAX 2.0   // Maximum aspect ratio of detected object within fixed region

// --- Scheduling Parameters (UPDATED FOR 6 AM - 6 PM) ---
// Active hours are [ACTIVE_HOUR_MORNING_START, ACTIVE_HOUR_MORNING_END)
// The afternoon period is effectively disabled by setting its start and end to the same value
#define ACTIVE_HOUR_MORNING_START 6  // 6 AM
#define ACTIVE_HOUR_MORNING_END   18 // 6 PM (Exclusive, so active until 17:59:59 PM)

// Set afternoon hours to be identical so they are never "active"
#define ACTIVE_HOUR_AFTERNOON_START 18 // Using 6 PM (same as end of morning to disable)
#define ACTIVE_HOUR_AFTERNOON_END 18   // Using 6 PM (same as end of morning to disable)


// How often to check the time and potentially go to sleep in loop() (milliseconds)
#define TIME_CHECK_INTERVAL_MS 60000 // Check time every 60 seconds in the active period

// Previous data for movement consistency check
int previousWhiteCount_in_fixed_region = 0;
int confirmedCounter = 0;

// Define the color for the bounding box (Green in RGB565 format)
#define BOX_COLOR_RGB565 0x07E0 // Bright Green

// --- Farm Identifier ---
#define FARM_NAME "Farm 1" // <<<<< CHANGE THIS for each camera
#define FARM_ID "00"       // <<<<< CHANGE THIS for each camera to match receiver rule (00 for mushroom)

// --- ESP-NOW Configuration ---
// MAC address of the ESP-NOW Receiver (MQTT Publisher ESP32)
// As provided: 20:43:A8:65:71:78
uint8_t broadcastAddress[] = {0x20, 0x43, 0xA8, 0x65, 0x71, 0x78};

// Structure to hold analysis results for easier return and JSON conversion
typedef struct AnalysisData {
    int brightPixels;
    int redPixels;
    int yellowPixels;
    int statusPanen; // 0: Not ready, 1: Ready, 2: Ready Soon
    // You can add more fields if needed, like distance, aspect ratio etc.
} AnalysisData;

// HTML page for the web server (Manual LED buttons and JS removed)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { text-align:center; }
.vert { margin-bottom: 10%; }
.hori{ margin-bottom: 0%; }
#analysisResults { margin-top: 20px; text-align: left; border: 1px solid #ccc; padding: 10px; white-space: pre-wrap; }
.button-group button { margin: 5px; }
</style>
</head>
<body>
<div id="container">
<h2>ESP32-CAM Photo and Analysis</h2>
<p>It might take a few seconds to capture and analyze a photo.</p>
<div class="button-group">
<button onclick="rotatePhoto();">ROTATE PHOTO</button>
<button onclick="captureAndAnalyze()">CAPTURE & ANALYZE</button> <button onclick="location.reload();">RELOAD PAGE</button>
</div>
</div>
<div><img src="photo" id="photo" width="70%"></div>
<div id="analysisResults">Analysis results will appear here...</div>
</body>
<script>
var deg = 0;
function captureAndAnalyze() {
var xhr = new XMLHttpRequest();
xhr.open('GET', "/capture", true);
xhr.onload = function () {
if (xhr.status === 200) {
// Update the analysis results area
document.getElementById("analysisResults").innerText = xhr.responseText;
// Refresh the image after capture is complete
document.getElementById("photo").src = "photo?" + new Date().getTime(); // Add timestamp to force refresh
} else {
document.getElementById("analysisResults").innerText = "Error capturing photo: " + xhr.status;
}
};
xhr.send();
// Optionally update status while waiting
document.getElementById("analysisResults").innerText = "Capturing and analyzing...";
}

// Manual LED control functions removed

function rotatePhoto() {
var img = document.getElementById("photo");
deg += 90;
if(isOdd(deg/90)){ document.getElementById("container").className = "vert"; }
else{ document.getElementById("container").className = "hori"; }
img.style.transform = "rotate(" + deg + "deg)";
}
function isOdd(n) { return Math.abs(n % 2) == 1; }

</script>
</html>)rawliteral";

// --- Global Variables ---
camera_config_t config;
bool is_active_period = false; // Flag to indicate if the device should be active (running server)
unsigned long last_time_check_ms = 0; // For periodic time check in loop()
bool sd_card_initialized = false; // Flag to track SD card status
bool time_sync_successful = false; // Flag to track if time sync was successful
bool esp_now_initialized_success = false; // New flag to track successful ESP-NOW init + peer add

// --- Helper Functions ---

// Callback when ESP-NOW data is sent
// MODIFIED to print the exact status code for debugging
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\nESP-NOW Send Status: ");
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Success");
  } else {
    // Print the MAC address and the raw status value
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.printf("Fail to MAC %s, Status Code: %d\n", macStr, status);
  }
}

// Setup NTP time synchronization
void setup_ntp() {
    Serial.println("Setting up NTP");
    configTime(0, 0, ntpServer); // Configure NTP client
    setenv("TZ", time_zone, 1);     // Set timezone
    tzset();

    // Wait for time to be set
    time_t now = time(nullptr);
    int retry_count = 0;
    // Wait up to 20*500ms = 10 seconds for time sync, only if WiFi is connected
    while (now < 100000 && retry_count < 20 && WiFi.status() == WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        retry_count++;
    }
    Serial.println();

    if (now < 100000) {
        Serial.println("Failed to synchronize time from NTP after retries.");
        time_sync_successful = false;
    } else {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        Serial.printf("Time synchronized: %s", asctime(&timeinfo));
        time_sync_successful = true;
    }
}

// Calculates the timestamp for a specific hour, minute, second today or tomorrow
time_t get_target_timestamp(int hour, int minute, int second, time_t now_ts) {
    struct tm tm_now = *localtime(&now_ts);
    struct tm tm_target = tm_now;

    tm_target.tm_hour = hour;
    tm_target.tm_min = minute;
    tm_target.tm_sec = second;
    tm_target.tm_isdst = -1; // Auto-determine DST

    time_t target_ts = mktime(&tm_target);

    // If the target time is in the past relative to now_ts,
    // add one day to get the same time tomorrow.
    // Use mktime again to normalize the date in tm_target
    if (target_ts <= now_ts) {
        tm_target.tm_mday++; // Increment day
        target_ts = mktime(&tm_target); // mktime normalizes the date (handles month/year wrap)
    }

    return target_ts;
}


// Determines the timestamp for the next scheduled wake-up based on the active periods
time_t get_next_wakeup_timestamp(time_t now_ts) {
    struct tm tm_now = *localtime(&now_ts);

    // Calculate start/end times of the defined active periods today
    time_t ts_morning_start_today = get_target_timestamp(ACTIVE_HOUR_MORNING_START, 0, 0, now_ts);
    time_t ts_morning_end_today   = get_target_timestamp(ACTIVE_HOUR_MORNING_END,   0, 0, now_ts);
    time_t ts_afternoon_start_today = get_target_timestamp(ACTIVE_HOUR_AFTERNOON_START, 0, 0, now_ts);
    time_t ts_afternoon_end_today   = get_target_timestamp(ACTIVE_HOUR_AFTERNOON_END,   0, 0, now_ts);


    // --- Determine the next wake-up time ---

    // 1. If currently before the morning active period (e.g., 1 AM when morning starts at 6 AM)
    if (now_ts < ts_morning_start_today) {
        Serial.printf("Current time before %02d:00 AM. Next wakeup: %02d:00 AM today.\n", ACTIVE_HOUR_MORNING_START, ACTIVE_HOUR_MORNING_START);
        return ts_morning_start_today;
    }

    // 2. If currently after the single active period (i.e., after 6 PM today)
    //    This means we go to sleep until the start of the morning period tomorrow.
    Serial.printf("Current time after %02d:00 PM (end of active period). Next wakeup: %02d:00 AM tomorrow.\n", ACTIVE_HOUR_MORNING_END, ACTIVE_HOUR_MORNING_START);
    struct tm tm_tomorrow = tm_now;
    tm_tomorrow.tm_mday++; // Increment day
    tm_tomorrow.tm_hour = ACTIVE_HOUR_MORNING_START;
    tm_tomorrow.tm_min = 0;
    tm_tomorrow.tm_sec = 0;
    tm_tomorrow.tm_isdst = -1; // Auto-determine DST
    return mktime(&tm_tomorrow); // Morning start tomorrow
}


// Enter deep sleep for a given number of seconds
void go_to_sleep(long sleep_duration_sec) {
    if (sleep_duration_sec <= 0) {
      Serial.println("Sleep duration is zero or negative. Sleeping for 60 seconds as a fallback.");
      sleep_duration_sec = 60; // Safety fallback
    }

    Serial.printf("Entering deep sleep for %ld seconds...\n", sleep_duration_sec);
    // Convert seconds to microseconds
    esp_sleep_enable_timer_wakeup(sleep_duration_sec * 1000000ULL); // ULL for unsigned long long
    Serial.flush(); // Ensure serial messages are sent before sleeping
    esp_deep_sleep_start();
}


// Function to perform mushroom analysis within a fixed central region of a camera frame buffer
// Returns AnalysisData struct and updates bounding box coordinates of the detected object within the fixed region
String analyzeMushroom(camera_fb_t *fb, int *out_minX, int *out_minY, int *out_maxX, int *out_maxY, AnalysisData *analysisResult) {
    String results = "===== Analisis Gambar (Fokus pada Area Tengah) =====\n";

    // Initialize bounding box outputs to indicate no object found within the fixed region
    *out_minX = fb->width;
    *out_minY = fb->height;
    *out_maxX = 0;
    *out_maxY = 0;

    analysisResult->brightPixels = 0;
    analysisResult->redPixels = 0;
    analysisResult->yellowPixels = 0;
    analysisResult->statusPanen = 0; // Default to Not ready

    if (!fb) {
        results += "Gagal mengambil gambar untuk analisis.\n";
        return results;
    }

    int width = fb->width;
    int height = fb->height;

    // --- Define the Fixed Analysis Region (e.g., middle half of the image) ---
    int fixedAnalysisMinX = width / 4;
    int fixedAnalysisMinY = height / 4;
    int fixedAnalysisMaxX = width * 3 / 4;
    int fixedAnalysisMaxY = height * 3 / 4;

    // Ensure fixed region is within image bounds
    fixedAnalysisMinX = max(0, fixedAnalysisMinX);
    fixedAnalysisMinY = max(0, fixedAnalysisMinY);
    fixedAnalysisMaxX = min(width - 1, fixedAnalysisMaxX);
    fixedAnalysisMaxY = min(height - 1, fixedAnalysisMaxY);

    int fixedRegionWidth = fixedAnalysisMaxX - fixedAnalysisMinX + 1;
    int fixedRegionHeight = fixedAnalysisMaxY - fixedAnalysisMinY + 1;

    results += "Area Analisis Tetap: (" + String(fixedAnalysisMinX) + ", " + String(fixedAnalysisMinY) + ") to (" + String(fixedAnalysisMaxX) + ", " + String(fixedAnalysisMaxY) + ")\n";

    int whitePixelCount_in_fixed_region = 0;
    int redPixelCount_in_fixed_region = 0;
    int yellowPixelCount_in_fixed_region = 0;
    unsigned long xSum_in_fixed_region = 0;

    uint16_t *pixels = (uint16_t *)fb->buf; // Cast buffer to uint16_t for RGB565 access

    // --- Iterate ONLY within the Fixed Analysis Region ---
    for (int y = fixedAnalysisMinY; y <= fixedAnalysisMaxY; y++) {
        for (int x = fixedAnalysisMinX; x <= fixedAnalysisMaxX; x++) {
            uint16_t pixel = pixels[y * width + x];

            // Extract RGB565 components
            uint8_t r = ((pixel >> 11) & 0x1F) * 255 / 31;
            uint8_t g = ((pixel >> 5) & 0x3F) * 255 / 63;
            uint8_t b = (pixel & 0x1F) * 255 / 31;

            uint8_t brightness = (r + g + b) / 3;

            if (brightness > BRIGHTNESS_THRESHOLD) {
                whitePixelCount_in_fixed_region++;
                xSum_in_fixed_region += x; // Sum x-coordinates within the fixed region

                // Update bounding box of the detected object within the fixed region
                if (x < *out_minX) *out_minX = x;
                if (x > *out_maxX) *out_maxX = x;
                if (y < *out_minY) *out_minY = y;
                if (y > *out_maxY) *out_maxY = y;
            }

            // Color detection within the fixed analysis region
            if (r > 180 && g < 80 && b < 80) { // Increased threshold for 'more red'
                redPixelCount_in_fixed_region++;
            } else if (r > 180 && g > 180 && b < 80) { // Increased threshold for 'more yellow'
                yellowPixelCount_in_fixed_region++;
            }
        }
    }

    results += "Piksel terang (dalam Area Analisis): " + String(whitePixelCount_in_fixed_region) + "\n";
    results += "Piksel merah (dalam Area Analisis): " + String(redPixelCount_in_fixed_region) + "\n";
    results += "Piksel kuning (dalam Area Analisis): " + String(yellowPixelCount_in_fixed_region) + "\n";

    // Store raw pixel counts in the analysisResult struct
    analysisResult->brightPixels = whitePixelCount_in_fixed_region;
    analysisResult->redPixels = redPixelCount_in_fixed_region;
    analysisResult->yellowPixels = yellowPixelCount_in_fixed_region;


    // Calculate the aspect ratio of the detected object within the fixed region
    int detectedObjectWidth_in_fixed_region = (*out_maxX < *out_minX) ? 0 : *out_maxX - *out_minX + 1;
    int detectedObjectHeight_in_fixed_region = (*out_maxY < *out_minY) ? 0 : *out_maxY - *out_minY + 1;

    float aspectRatio = (detectedObjectHeight_in_fixed_region == 0) ? 0 : (float)detectedObjectWidth_in_fixed_region / detectedObjectHeight_in_fixed_region;

    if (whitePixelCount_in_fixed_region > 0 && detectedObjectWidth_in_fixed_region > 0 && detectedObjectHeight_in_fixed_region > 0) {
        results += "Bounding Box Objek Terdeteksi (dalam Area Analisis): (" + String(*out_minX) + ", " + String(*out_minY) + ", " + String(*out_maxX) + ", " + String(*out_maxY) + ")\n";
        results += "Aspect Ratio Objek: " + String(aspectRatio, 2) + "\n";
    } else {
        results += "Bounding Box Objek Terdeteksi: N/A (Tidak ada objek terdeteksi dalam area analisis atau bentuk tidak valid)\n";
        results += "Aspect Ratio Objek: N/A\n";
    }

    // Check for shape consistency (aspect ratio of the detected object within the fixed region)
    if (whitePixelCount_in_fixed_region > 0 && (aspectRatio < BBOX_ASPECT_RATIO_MIN || aspectRatio > BBOX_ASPECT_RATIO_MAX)) {
        results += "❌ Deteksi objek tidak valid (rasio bentuk aneh)\n";
    } else if (whitePixelCount_in_fixed_region == 0) {
        results += "Tidak ada piksel terang terdeteksi dalam area analisis.\n";
    }

    if (whitePixelCount_in_fixed_region > 0) { // Use white count within fixed region for centroid
        float avgX = (float)xSum_in_fixed_region / whitePixelCount_in_fixed_region;
        int fixedRegionCenterX = fixedAnalysisMinX + fixedRegionWidth / 2;
        if (abs(avgX - fixedRegionCenterX) <= CENTER_TOLERANCE) {
            results += "📍 Objek terdeteksi di TENGAH Area Analisis\n";
        } else if (avgX < fixedRegionCenterX) {
            results += "⬅ Objek terdeteksi condong ke KIRI Area Analisis\n";
        } else {
            results += "➡ Objek terdeteksi condong ke KANAN Area Analisis\n";
        }
    } else {
        results += "Posisi: Tidak ada objek terdeteksi dalam area analisis.\n";
    }

    // Movement consistency check
    bool currentDetectionValid = (whitePixelCount_in_fixed_region > 0 && !(aspectRatio < BBOX_ASPECT_RATIO_MIN || aspectRatio > BBOX_ASPECT_RATIO_MAX));
    if (currentDetectionValid && abs(whitePixelCount_in_fixed_region - previousWhiteCount_in_fixed_region) < 500) {
        confirmedCounter++;
    } else {
        confirmedCounter = 0;
    }
    previousWhiteCount_in_fixed_region = whitePixelCount_in_fixed_region;

    results += "Validasi berurutan: " + String(confirmedCounter) + "\n";

    if (confirmedCounter >= 2) {
        results += "✅ Objek dikonfirmasi ...\n";
    } else {
        results += "⏳ Objek BELUM stabil ...\n";
    }

    // Distance estimation
    if (whitePixelCount_in_fixed_region > 8000) {
        results += "📏 Jarak: Dekat (<= 10cm)\n";
    } else if (whitePixelCount_in_fixed_region > 3000) {
        results += "📏 Jarak: Sedang (10cm - 20cm)\n";
    } else {
        results += "📏 Jarak: Jauh (> 20cm)\n";
    }

    // Harvest decision
    results += "--- Keputusan Panen ---\n";
    if (whitePixelCount_in_fixed_region < SIZE_THRESHOLD || detectedObjectWidth_in_fixed_region == 0 || detectedObjectHeight_in_fixed_region == 0) {
        results += "❌ BELUM siap panen (Tidak ada objek terdeteksi dalam area analisis, objek terlalu kecil, atau bentuk aneh)\n";
        analysisResult->statusPanen = 0; // Not ready
    } else {
        if (redPixelCount_in_fixed_region > whitePixelCount_in_fixed_region * 0.1) {
            results += "❌ Mushroom merah/bercak merah: BELUM siap panen\n";
            analysisResult->statusPanen = 0; // Not ready
        } else if (yellowPixelCount_in_fixed_region > whitePixelCount_in_fixed_region * 0.1) {
            results += "⚠ Mushroom kuning/bercak kuning: Siap dipanen segera\n";
            analysisResult->statusPanen = 2; // Ready soon
        } else {
            results += "✅ Mushroom siap panen (Putih dan besar)\n";
            analysisResult->statusPanen = 1; // Ready
        }
    }

    return results;
}

// Function to draw a rectangle on the RGB565 pixel buffer
void drawRectangle(uint16_t *pixels, int width, int height, int minX, int minY, int maxX, int maxY, uint16_t color, int thickness = 1) {
    // Ensure bounding box is within image bounds
    minX = max(0, minX);
    minY = max(0, minY);
    maxX = min(width - 1, maxX);
    maxY = min(height - 1, maxY);

    // Draw top and bottom edges
    for (int y = minY; y <= maxY; y++) {
        if (y >= minY && y < minY + thickness || y <= maxY && y > maxY - thickness) {
            for (int x = minX; x <= maxX; x++) {
                     if (x >= 0 && x < width && y >= 0 && y < height) {
                         pixels[y * width + x] = color;
                    }
            }
        }
    }

    // Draw left and right edges (fill in the gaps missed by horizontal lines)
    for (int x = minX; x <= maxX; x++) {
        if (x >= minX && x < minX + thickness || x <= maxX && x > maxX - thickness) {
             for (int y = minY; y <= maxY; y++) {
                     if (x >= 0 && x < width && y >= 0 && y < height) {
                         pixels[y * width + x] = color;
                    }
            }
        }
    }
}


// Check if photo capture and save were successful (basic check) - SPIFFS version
bool checkPhoto(fs::FS &fs, const char *filename) {
    File f_pic = fs.open(filename);
    unsigned int pic_sz = 0;
    if (f_pic) {
        pic_sz = f_pic.size();
        f_pic.close();
    }
    return (pic_sz > 100); // Check if file size is reasonable (not empty)
}

// Function to send analysis data via ESP-NOW, with a JSON prefix
void sendAnalysisData(const AnalysisData& data, const char* timeStr) {
    // Check if ESP-NOW was successfully initialized and peer added
    if (!esp_now_initialized_success) {
        Serial.println("ESP-NOW skipped: ESP-NOW initialization or peer addition failed in setup.");
        return;
    }

    // Create a StaticJsonDocument with enough capacity for your JSON
    StaticJsonDocument<256> doc; // Should be sufficient for your JSON format

    doc["TIPEDATA"] = "A";
    doc["FARMID"] = FARM_ID;
    doc["timeNow"] = timeStr;
    doc["brightPixels"] = data.brightPixels;
    doc["redPixels"] = data.redPixels;
    doc["yellowPixels"] = data.yellowPixels;
    doc["statusPanen"] = data.statusPanen;

    char jsonOnlyBuffer[256]; // Buffer for JSON string itself
    size_t jsonLen = serializeJson(doc, jsonOnlyBuffer, sizeof(jsonOnlyBuffer));

    if (jsonLen == 0) {
        Serial.println("ERROR: Failed to serialize JSON to buffer.");
        return;
    }

    // Prepare the final message with the "JSON:" prefix
    // Max ESP-NOW payload is 250 bytes. "JSON:" is 5 bytes + null terminator = 6 bytes.
    // So, jsonLen + 6 bytes must be <= 250.
    char fullMessage[250]; // Ensure this buffer doesn't exceed 250 bytes (ESP-NOW limit)
    int bytesWritten = snprintf(fullMessage, sizeof(fullMessage), "JSON:%s", jsonOnlyBuffer);

    Serial.printf("JSON string length: %u\n", jsonLen); // Changed %d to %u for size_t
    Serial.printf("Full message length (including prefix): %u\n", strlen(fullMessage)); // Changed %d to %u

    if (bytesWritten >= sizeof(fullMessage) || bytesWritten < 0) {
        Serial.println("ERROR: ESP-NOW message too long for buffer or snprintf failed!");
        // Consider resizing StaticJsonDocument or reducing data if this triggers often
        return;
    }
    if (strlen(fullMessage) > 250) { // Double check against max payload size
        Serial.printf("ERROR: Actual ESP-NOW payload length (%u) exceeds 250 bytes limit!\n", strlen(fullMessage)); // Changed %d to %u
        return;
    }

    Serial.print("Sending ESP-NOW: ");
    Serial.println(fullMessage);

    // Get the direct return value from esp_now_send()
    esp_err_t sendResult = esp_now_send(broadcastAddress, (uint8_t *)fullMessage, strlen(fullMessage));

    if (sendResult == ESP_OK) {
        Serial.println("ESP-NOW Send initiated successfully.");
        // OnDataSent callback will confirm delivery (Success/Fail)
    } else {
        Serial.printf("ESP-NOW Send failed immediately! Error code: %d\n", sendResult);
        Serial.println("Possible causes: Peer not added, WiFi not ready, or too many pending sends.");
    }
}


void setup() {
    // Serial port for debugging purposes
    Serial.begin(115200);
    Serial.setDebugOutput(false); // Optional: Reduce debug output noise
    Serial.println();
    Serial.println("--- " FARM_NAME " --- ESP32-CAM Starting...");
    Serial.printf("Wakeup cause: %s\n", esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER ? "Timer" : "Other");

    // --- Print ESP32-CAM's MAC Address for debugging ---
    Serial.print("ESP32-CAM MAC Address: ");
    Serial.println(WiFi.macAddress());


    // Turn-off the 'brownout detector'
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    // --- Flash LED Pin Setup ---
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW); // Ensure LED is off initially
    Serial.println("Flash LED pin configured.");

    // --- WiFi Connection ---
    Serial.print("Connecting to WiFi...");
    // Set WiFi to Station Mode EARLY, before begin()
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    int wifi_retry_count = 0;
    // Wait for WiFi to connect
    while (WiFi.status() != WL_CONNECTED && wifi_retry_count < 20) {
        delay(500);
        Serial.print(".");
        wifi_retry_count++;
    }
    Serial.println();

    bool proceed_with_active_mode = false;
    time_sync_successful = false; // Initialize flag before potential NTP call
    esp_now_initialized_success = false; // Initialize the new flag

    if (WiFi.status() != WL_CONNECTED) {
        // --- Scenario 1: WiFi Connection Failed ---
        Serial.println("WiFi connection failed. Sleeping for 5 minutes and retrying.");
        go_to_sleep(300); // Sleep for 5 minutes (300 seconds) as fallback
        return; // Exit setup; device will restart here on wakeup
    } else {
        // --- Scenario 2: WiFi Connected ---
        Serial.println("WiFi connected.");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());

        // --- Time Synchronization (NTP) ---
        // Perform NTP sync here, as it waits for WiFi connection
        setup_ntp(); // This attempts sync and sets the global time_sync_successful flag

        time_t now_ts = time(nullptr); // Get current time after sync attempt
        struct tm tm_now;

        if (time_sync_successful) { // Proceed only if time sync (and thus WiFi stability) is good
            localtime_r(&now_ts, &tm_now);

            // --- Determine if currently in a SCHEDULED active period ---
            bool is_in_morning_active = (tm_now.tm_hour >= ACTIVE_HOUR_MORNING_START && tm_now.tm_hour < ACTIVE_HOUR_MORNING_END);
            bool is_in_afternoon_active = (tm_now.tm_hour >= ACTIVE_HOUR_AFTERNOON_START && tm_now.tm_hour < ACTIVE_HOUR_AFTERNOON_END);

            is_active_period = is_in_morning_active || is_in_afternoon_active;

            Serial.printf("Current time: %02d:%02d:%02d - Is Active Period (Scheduled): %s\n", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, is_active_period ? "YES" : "NO");

            if (is_active_period) {
                proceed_with_active_mode = true;
                Serial.println("Currently in a scheduled active period. Proceeding with initialization.");

                // --- IMPORTANT: Initialize ESP-NOW *AFTER* WiFi is connected, IP acquired, and NTP sync attempt ---
                // This gives the WiFi stack maximum time to settle on its channel.

                // Added small delay AND wait for peer to be added
                Serial.println("Attempting ESP-NOW initialization and peer add...");

                // Get current WiFi channel of this ESP32-CAM (the sender)
                int current_wifi_channel = WiFi.channel();
                Serial.printf("Current WiFi Channel (Sender): %d\n", current_wifi_channel);


                // Add a loop to retry ESP-NOW init and peer add
                int espnow_retry_count = 0;
                const int MAX_ESPNOW_RETRIES = 5; // Allow a few retries
                while (!esp_now_initialized_success && espnow_retry_count < MAX_ESPNOW_RETRIES) {
                    espnow_retry_count++;
                    Serial.printf("  [ESPNOW] Init/Peer add attempt %d...\n", espnow_retry_count);
                    
                    // Always de-init before re-init to ensure a clean state
                    if (esp_now_deinit() == ESP_OK) {
                        Serial.println("  [ESPNOW] Previous ESP-NOW de-initialized successfully.");
                    } else {
                        Serial.println("  [ESPNOW] Warning: ESP-NOW de-initialization failed or not initialized.");
                    }
                    delay(100); // Short delay after de-init

                    if (esp_now_init() == ESP_OK) {
                        Serial.println("  [ESPNOW] ESP-NOW initialized.");
                        esp_now_peer_info_t peerInfo;
                        memcpy(peerInfo.peer_addr, broadcastAddress, 6);
                        peerInfo.channel = current_wifi_channel; // Set the peer's channel to the current WiFi channel
                        peerInfo.encrypt = false; // No encryption for simplicity
                        peerInfo.ifidx = WIFI_IF_STA; // Explicitly set interface to STA

                        esp_err_t addPeerErr = esp_now_add_peer(&peerInfo);
                        if (addPeerErr == ESP_OK) {
                            Serial.println("✅ [ESPNOW] Peer added successfully.");
                            esp_now_register_send_cb(OnDataSent); // Register send callback
                            esp_now_initialized_success = true; // Set flag only on full success
                        } else {
                            Serial.printf("❌ [ESPNOW] Failed to add peer! Error code: %d. Retrying...\n", addPeerErr);
                            // If adding peer fails, delay before next retry
                            delay(500);
                        }
                    } else {
                        Serial.printf("❌ [ESPNOW] Error initializing ESP-NOW! Error code: %d. Retrying...\n", esp_now_init());
                        delay(500); // If init fails, just delay and retry
                    }
                }

                if (!esp_now_initialized_success) {
                    Serial.println("❌ [FATAL] ESP-NOW failed to initialize/add peer after multiple retries. ESP-NOW messages will NOT be sent.");
                }
                // --- END OF ESP-NOW Initialization ---

            } else {
                time_t next_wakeup_ts = get_next_wakeup_timestamp(now_ts);
                long sleep_duration_sec = next_wakeup_ts - now_ts;

                Serial.printf("Not in a scheduled active period. Sleeping until next scheduled wakeup at %s", ctime(&next_wakeup_ts));
                go_to_sleep(sleep_duration_sec);
                return;
            }

        } else {
            Serial.println("Time sync failed. Running in always-awake mode (scheduling disabled) & ESP-NOW may not work.");
            is_active_period = true;
            proceed_with_active_mode = true;
            // If time sync failed, we cannot be sure of WiFi stability or channel,
            // so we don't even attempt ESP-NOW init here.
            // esp_now_initialized_success will remain false.
        }
    }

    // --- Initialize peripherals (SPIFFS, SD, Camera) and start server ONLY if proceed_with_active_mode is true ---
    if (proceed_with_active_mode) {
        // --- SPIFFS Initialization ---
        if (!SPIFFS.begin(true)) {
            Serial.println("An Error has occurred while mounting SPIFFS");
        } else {
            delay(500);
            Serial.println("SPIFFS mounted successfully");
        }

        // --- SD Card Initialization (SD_MMC) ---
        Serial.print("Initializing SD_MMC...");
        if (!SD_MMC.begin()) {
            Serial.println("SD_MMC Card Mount Failed");
            sd_card_initialized = false;
        } else {
            uint32_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Serial.printf("SD_MMC Card mounted. Type: %u, Size: %luMB\n", SD_MMC.cardType(), cardSize);
            sd_card_initialized = true;
        }

        // --- Camera Configuration ---
        config.ledc_channel = LEDC_CHANNEL_0;
        config.ledc_timer = LEDC_TIMER_0;
        config.pin_d0 = Y2_GPIO_NUM;
        config.pin_d1 = Y3_GPIO_NUM;
        config.pin_d2 = Y4_GPIO_NUM;
        config.pin_d3 = Y5_GPIO_NUM;
        config.pin_d4 = Y6_GPIO_NUM;
        config.pin_d5 = Y7_GPIO_NUM;
        config.pin_d6 = Y8_GPIO_NUM;
        config.pin_d7 = Y9_GPIO_NUM;
        config.pin_xclk = XCLK_GPIO_NUM;
        config.pin_pclk = PCLK_GPIO_NUM;
        config.pin_vsync = VSYNC_GPIO_NUM;
        config.pin_href = HREF_GPIO_NUM;
        config.pin_sccb_sda = SIOD_GPIO_NUM;
        config.pin_sccb_scl = SIOC_GPIO_NUM;
        config.pin_pwdn = PWDN_GPIO_NUM;
        config.pin_reset = RESET_GPIO_NUM;
        config.xclk_freq_hz = 20000000;
        config.pixel_format = PIXFORMAT_RGB565;

        if (psramFound()) {
            config.frame_size = FRAMESIZE_QVGA;
            config.fb_count = 2;
        } else {
            config.frame_size = FRAMESIZE_QQVGA;
            config.fb_count = 1;
        }
        config.jpeg_quality = 80;

        esp_err_t err = esp_camera_init(&config);
        if (err != ESP_OK) {
            Serial.printf("Camera init failed with error 0x%x\n", err);
        } else {
            Serial.println("Camera initialized successfully.");
            camera_fb_t *test_fb = esp_camera_fb_get();
            if (test_fb) {
              Serial.printf("Initial camera test capture successful. Size: %u bytes\n", test_fb->len);
              esp_camera_fb_return(test_fb);
            } else {
              Serial.println("Initial camera test capture failed.");
            }
        }
        
        // --- Web Server Routes ---
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send(200, "text/html", index_html);
        });

        server.on("/capture", HTTP_GET, [&](AsyncWebServerRequest *request) {
            Serial.println("\n--- " FARM_NAME " --- Capture and Analysis Triggered ---");

            time_t now_capture = time(nullptr);
            struct tm timeinfo_capture;
            if (time_sync_successful) {
                localtime_r(&now_capture, &timeinfo_capture);
            }

            char time_str[64];
            String currentAnalysisResults = "";

            currentAnalysisResults += "Farm: " + String(FARM_NAME) + "\n";
            Serial.printf("Farm: %s\n", FARM_NAME);

            if (time_sync_successful) {
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo_capture);
                currentAnalysisResults += "Capture Time (Synced): " + String(time_str) + "\n";
                Serial.printf("Capture Time (Synced): %s\n", time_str);
            } else {
                unsigned long current_millis = millis();
                snprintf(time_str, sizeof(time_str), "Millis: %lu", current_millis);
                currentAnalysisResults += "Capture Time (Unsynced): " + String(time_str) + "\n";
                currentAnalysisResults += "WARNING: Time sync failed. Scheduling disabled & SD filenames may be incorrect.\n";
                Serial.printf("Capture Time (Unsynced): %s\n", time_str);
                Serial.println("WARNING: Time sync failed.");
            }

            currentAnalysisResults += "----------------------------------------\n";
            Serial.println("----------------------------------------");

            bool sd_save_success = false;
            int detectedObjectMinX, detectedObjectMinY, detectedObjectMaxX, detectedObjectMaxY;
            AnalysisData analysisResult; // Declare the struct to hold analysis data

            digitalWrite(FLASH_LED_PIN, HIGH);
            delay(200);

            camera_fb_t *fb = esp_camera_fb_get();

            digitalWrite(FLASH_LED_PIN, LOW);

            if (!fb) {
                Serial.println("Camera capture failed.");
                currentAnalysisResults += "ERROR: Camera capture failed.";
            } else {
                Serial.println("Camera capture successful. Saving to SD and running analysis...");

                // --- Save Photo to MicroSD Card ---
                if (sd_card_initialized) {
                    char filename[64];
                    if (time_sync_successful) {
                        strftime(filename, sizeof(filename), "/%Y%m%d_%H%M%S.jpg", &timeinfo_capture);
                        currentAnalysisResults += "SD Save: Using timestamp filename.\n";
                    } else {
                        snprintf(filename, sizeof(filename), "/photo_%lu.jpg", millis());
                        currentAnalysisResults += "SD Save: Using generic filename (Time sync failed).\n";
                    }

                    Serial.printf("Saving image to SD: %s\n", filename);

                    File file_sd = SD_MMC.open(filename, FILE_WRITE);
                    if (!file_sd) {
                        Serial.println("Failed to open file on SD card for writing");
                        currentAnalysisResults += "SD Save: Failed to open file.\n";
                    } else {
                        size_t jpg_buf_len_sd = 0;
                        uint8_t *jpg_buf_sd = NULL;
                        if (fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 90, &jpg_buf_sd, &jpg_buf_len_sd)) {
                            size_t bytes_written = file_sd.write(jpg_buf_sd, jpg_buf_len_sd);
                            if (bytes_written == jpg_buf_len_sd) {
                                Serial.printf("Successfully wrote %u bytes to SD card.\n", jpg_buf_len_sd);
                                currentAnalysisResults += "SD Save: OK\n";
                                sd_save_success = true;
                            } else {
                                Serial.printf("Failed to write all bytes to SD card. Wrote %u of %u\n", bytes_written, jpg_buf_len_sd);
                                currentAnalysisResults += "SD Save: Write incomplete (" + String(bytes_written) + "/" + String(jpg_buf_len_sd) + " bytes)\n";
                            }
                            free(jpg_buf_sd);
                        } else {
                            Serial.println("RGB565 to JPEG conversion failed for SD card save");
                            currentAnalysisResults += "SD Save: JPEG conversion failed.\n";
                        }
                        file_sd.close();
                    }
                } else {
                    Serial.println("SD card not initialized. Skipping save to SD.");
                    currentAnalysisResults += "SD Save: Skipped (SD card not ready).\n";
                }

                // --- Perform Analysis and get Bounding Box & AnalysisData ---
                Serial.println("Running analysis...");
                currentAnalysisResults += analyzeMushroom(fb, &detectedObjectMinX, &detectedObjectMinY, &detectedObjectMaxX, &detectedObjectMaxY, &analysisResult);

                Serial.println("Analysis complete.");
                Serial.println(currentAnalysisResults);

                // --- Send Analysis Data via ESP-NOW ---
                // Only send ESP-NOW if ESP-NOW was successfully initialized earlier in setup
                if (esp_now_initialized_success) {
                    sendAnalysisData(analysisResult, time_str); // This function now adds "JSON:" prefix
                } else {
                    Serial.println("ESP-NOW skipped: Initialization/peer add failed or time sync failed.");
                    currentAnalysisResults += "ESP-NOW: Skipped (Init failed).\n";
                }


                // --- Draw Bounding Box on the image buffer (for Web Image) ---
                if (detectedObjectMinX < detectedObjectMaxX && detectedObjectMinY < detectedObjectMaxY) {
                    Serial.printf("Drawing box around detected object: (%d, %d) to (%d, %d)\n", detectedObjectMinX, detectedObjectMinY, detectedObjectMaxX, detectedObjectMaxY);
                    drawRectangle((uint16_t *)fb->buf, fb->width, fb->height, detectedObjectMinX, detectedObjectMinY, detectedObjectMaxX, detectedObjectMaxY, BOX_COLOR_RGB565, 2);
                } else {
                    Serial.println("No valid object bounding box detected within the fixed region, skipping drawing object box for web image.");
                }

                // --- Convert the Modified RGB565 frame to JPEG for SPIFFS/Web and Save ---
                size_t jpg_buf_len_web = 0;
                uint8_t *jpg_buf_web = NULL;
                if (fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, config.jpeg_quality, &jpg_buf_web, &jpg_buf_len_web)) {
                    File file_spiffs = SPIFFS.open(FILE_MARKED_PHOTO, FILE_WRITE);
                    if (!file_spiffs) {
                        Serial.println("Failed to open file for marked JPEG save (SPIFFS)");
                        currentAnalysisResults += "SPIFFS Save: Failed to open file.\n";
                    } else {
                        file_spiffs.write(jpg_buf_web, jpg_buf_len_web);
                        file_spiffs.close();
                        Serial.printf("Marked JPEG saved to SPIFFS %s - Size: %u bytes\n", FILE_MARKED_PHOTO, jpg_buf_len_web);
                        currentAnalysisResults += "SPIFFS Save: OK\n";
                    }
                    free(jpg_buf_web);
                } else {
                    Serial.println("RGB565 to JPEG conversion failed after marking (for web)");
                    currentAnalysisResults += "SPIFFS Save: JPEG conversion failed.\n";
                }

                esp_camera_fb_return(fb);
            }

            request->send(200, "text/plain", currentAnalysisResults);
            Serial.println("--- Capture and Analysis Complete ---");
        });

        server.on("/photo", HTTP_GET, [](AsyncWebServerRequest *request) {
            if (SPIFFS.exists(FILE_MARKED_PHOTO)) {
                request->send(SPIFFS, FILE_MARKED_PHOTO, "image/jpg", false);
            } else {
                request->send(404, "text/plain", "Photo not found. Capture one first!");
            }
        });

        // Start server
        server.begin();
        Serial.println("HTTP server started.");
        last_time_check_ms = millis();
    }
}


void loop() {
    // This loop only runs if the device started the server in setup()
    // and is in an active period or if time sync failed.

    if (is_active_period) {
        // If time sync was successful, we check the time to potentially go to sleep.
        if (time_sync_successful && millis() - last_time_check_ms > TIME_CHECK_INTERVAL_MS) {
            last_time_check_ms = millis(); // Reset timer

            time_t now_ts = time(nullptr); // Get current time
            struct tm tm_now = *localtime(&now_ts);

            // Check if current hour is within the morning period (6-18)
            bool is_in_morning_active = (tm_now.tm_hour >= ACTIVE_HOUR_MORNING_START && tm_now.tm_hour < ACTIVE_HOUR_MORNING_END);

            // The afternoon period (ACTIVE_HOUR_AFTERNOON_START = ACTIVE_HOUR_AFTERNOON_END)
            // will always result in false for `should_still_be_active`,
            // effectively disabling it.
            bool is_in_afternoon_active = (tm_now.tm_hour >= ACTIVE_HOUR_AFTERNOON_START && tm_now.tm_hour < ACTIVE_HOUR_AFTERNOON_END);

            bool should_still_be_active = is_in_morning_active || is_in_afternoon_active;


            Serial.printf("Periodic time check: %02d:%02d:%02d - Should stay active (based on schedule): %s\n", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, should_still_be_active ? "YES" : "NO");

            if (!should_still_be_active) {
                Serial.println("Scheduled active period ended. Preparing to sleep.");

                // Stop the web server gracefully (optional but good practice)
                server.end();
                Serial.println("Web server stopped.");

                // Calculate the sleep duration until the next active window starts
                time_t now_ts_sleep_end = time(nullptr); // Get fresh time before sleeping
                time_t next_wakeup_ts = get_next_wakeup_timestamp(now_ts_sleep_end);
                long sleep_duration_sec = next_wakeup_ts - now_ts_sleep_end;

                Serial.printf("Calculated next wakeup at %s", ctime(&next_wakeup_ts)); // ctime includes newline
                go_to_sleep(sleep_duration_sec);
                // The code execution stops here until timer wakeup, then restarts from setup()
            }
        } else if (!time_sync_successful) {
            // If time sync failed, we stay awake but print a periodic warning
            if (millis() - last_time_check_ms > TIME_CHECK_INTERVAL_MS) {
                last_time_check_ms = millis(); // Reset timer for warning messages
                Serial.println("WARNING: Time sync failed. Running in always-awake mode. Scheduled sleep disabled.");
            }
        }

        // Keep the loop running to allow AsyncWebServer to process requests
        // A small delay is often needed for the server to function correctly
        delay(1);
    }
    // If not in active period (determined in setup), loop() is not continuously executed
    // because setup() calls go_to_sleep(), which stops code execution until wakeup.
}