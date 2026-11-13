// Libraries
#include <WiFi.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "dashboard.h"

// CYD 2.8-inch touchscreen pins
static constexpr int CYD_TOUCH_CS   = 33;
static constexpr int CYD_TOUCH_IRQ  = 36;
static constexpr int CYD_TOUCH_CLK  = 25;
static constexpr int CYD_TOUCH_MISO = 39;
static constexpr int CYD_TOUCH_MOSI = 32;

// CYD TFT backlight
#define CYD_TFT_BACKLIGHT 21

// Limiters
static constexpr size_t MAX_WIFI_RECORDS = 20;
static constexpr size_t MAX_BLE_RECORDS = 25;
static constexpr size_t MAX_EVENTS = 20;

// Timing
static constexpr uint32_t BLE_WINDOW_MS = 5000;
static constexpr uint32_t WIFI_INTERVAL_MS = 15000;
static constexpr uint32_t WIFI_TIMEOUT_MS = 12000;
static constexpr uint32_t UI_REFRESH_MS = 1000;

// Colors
#define C_BG      TFT_WHITE
#define C_PANEL   0x18E3
#define C_BLUE    0x5D9F
#define C_WHITE   TFT_BLACK
#define C_GREY    0x8410
#define C_AMBER   0xFD20
#define C_GREEN   0x07E0
#define C_RED     0xF800

// Main Objects
TFT_eSPI tft;
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(CYD_TOUCH_CS, CYD_TOUCH_IRQ);
WebServer server(80);

NimBLEScan* bleScanner = nullptr;

// Structure
struct WifiRecord {
    String ssid;
    String bssid;
    int32_t rssi = 0;
    int channel = 0;
    uint8_t encryption = 0;
    uint32_t firstSeen = 0;
    uint32_t lastSeen = 0;
    uint32_t sightings = 0;
};

struct BleRecord {
    String address;
    String name;
    String serviceUuids;
    String manufacturerSummary;
    int rssi = 0;
    uint32_t firstSeen = 0;
    uint32_t lastSeen = 0;
    uint32_t sightings = 0;
};

struct EventRecord {
    String message;
    uint32_t uptime = 0;
};

// An array to store discovered Wi-Fi network records
WifiRecord wifiRecords[MAX_WIFI_RECORDS];
// An array to store discovered Bluetooth Low Energy BLE device records
BleRecord bleRecords[MAX_BLE_RECORDS];
// An array to store system events, such as device discoveries and scanner activity
EventRecord events[MAX_EVENTS];
// Allows other parts of the firmware to notify the display that an update is needed
void requestUIRefresh();
// Tracks the current number of Wi-Fi network records stored in wifiRecords
size_t wifiCount = 0;
// Tracks the current number of BLE devices
size_t bleCount = 0;
// Tracks the secuirty events
size_t eventCount = 0;
// Indicates whether the user interface needs to be refreshed
volatile bool uiDirty = true;
// Defines the available scanning modes for the ESP32
enum ScanMode {
    MODE_BLE,
    MODE_WIFI,
    MODE_IDLE
};
// Defines the available pages within the ESP32 touchscreen user interface
enum ScreenPage {
    PAGE_HOME,
    PAGE_WIFI,
    PAGE_BLE,
    PAGE_EVENTS
};
// Sets the scanner's initial operating mode to IDLE
ScanMode scanMode = MODE_IDLE;
// Sets the initial touchscreen page to HOME
ScreenPage currentPage = PAGE_HOME;
// Tracks whether a Bluetooth Low Energy BLE scan is currently active
bool bleScanRunning = false;
// Tracks whether a Wifi Access Point scan is currently active
bool wifiScanRunning = false;
// Indicates whether the ESP32 wireless access point (AP) has been successfully started
bool apStarted = false;
// Stores the timestamp when BLE scans begins
uint32_t bleStartedAt = 0;
// Stores the time timestamp when Wifi scans begins
uint32_t wifiStartedAt = 0;\
// Store the most recent Wifi scan
uint32_t lastWifiStart = 0;
// Store the last refresh of the UI
uint32_t lastUIRefreshed = 0;
// Store when the last touch was
uint32_t lastTouchAt = 0;
// Store the total amount of Wifi sightings
uint32_t wifiTotalSightings = 0;
// Store  total amount od BLE sightings
uint32_t bleTotalSightings = 0;
// Stores the total amount of completed scans
uint32_t completedWifiScans = 0;
// Stored total failed wifi scans
uint32_t failedWifiScans = 0;
// Stores the SSID of the acces point
String apSSID;
// Stores the most recent scanner message
String lastScanMessage = "Starting";
// Converts elapsed time from milliseconds into a readable uptime format
String uptimeString(uint32_t milliseconds){
  // Converts the supplied millisecond value into total elapsed seconds
    uint32_t sec = milliseconds / 1000;
  // Allocates a character buffer to store the formatted uptime string
    char out[20];
  // Formats the elapsed time into hours, minutes, and seconds
    snprintf(out, sizeof(out), "%02lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60), (unsigned long)(sec % 60));
// Converts the formatted character buffer into an Arduino String object
    return String(out);
}
// Sanitizes an input string by replacing newline characters with spaces
String safeText(String value, size_t maxLength = 48){
  // Creates an empty String object to store the cleaned text
    String cleaned;
  // Reserves memory for the expected maximum string length
    cleaned.reserve(maxLength);
  // Iterates through the original string whilecCharacters remain in the input string and the cleaned string has not reached the maximum allowed length
    for(size_t i = 0; i < value.length() && cleaned.length() < maxLength; i++){
      // Retrieves the current character from the original string
        char c = value[i];
      // Checks whether the current character is a carriage return or newline
        if( c == '\r' || c == '\n'){
          // Replaces the line break character with a regular space.
            c = ' ';
        }
      // Appends the processed character to the cleaned string
        cleaned += c;
    }
  // Returns the cleaned string after processing all permitted characters
    return cleaned;
}
// Adds a new event to the system event log
void addEvent(const String& message){
  // Checks whether the event array has reached its maximum storage capacity
    if (eventCount == MAX_EVENTS){
      // Shifts all existing events one position toward the beginning of the array
        for (size_t i = 1; i < MAX_EVENTS; i++){
          // Copies the next event into the current array position
            events[i - 1] = events[i];
        }
      // Decreases the event counter because the oldest event was discarded
        eventCount--;
    }
  // Stores the new event message after sanitizing it
    events[eventCount].message = safeText(message, 100);
  // Records the current ESP32 uptime in milliseconds
    events[eventCount].uptime = millis();
  // Increments the event counter to account for the newly stored event
    eventCount++;
  // Debug print the message
    Serial.printf("[EVENT] %s\n", message.c_str());
}
// Converts a Wi-Fi authentication/encryption value into a readable security name
String encryptionName(uint8_t value){
  // Checks the supplied authentication value against supported Wi-Fi security types
    switch (value){
      // Identifies networks that do not require a password or encryption
        case WIFI_AUTH_OPEN: return "Open";
      // Identifies networks using the older Wired Equivalent Privacy WEP protocol
        case WIFI_AUTH_WEP: return "WEP";
      // Identifies networks using Wi-Fi Protected Access WPA
        case WIFI_AUTH_WPA_PSK: return "WPA";
      // Identifies networks using Wi-Fi Protected Access 2 WPA2
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
      // Identifies networks supporting both WPA and WPA2 authentication
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
      // Identifies WPA2 Enterprise networks, commonly used in business environments
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 Enterprise";
      // Identifies networks using Wi-Fi Protected Access 3 WPA3
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
      // Identifies networks supporting both WPA2 and WPA3 authentication
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
      // Handles authentication values that do not match any listed security type
        default: return "Other/Unknown";
    }
}
// Finds the index of the record with the oldest lastSeen timestamp
template <typename T, size_t N>
size_t findOldestRecord(const T (&records)[N]){
  // Initializes the oldest record index to the first array position
    size_t oldestIndex = 0;
  // Iterates through the remaining records, starting at index 1
    for (size_t i = 1; i < N; i++){
      // Checks whether the current record was last seen earlier than the record currently identified as the oldest
        if (records[i].lastSeen < records[oldestIndex].lastSeen){
          // Updates oldestIndex to the position of the newly identified
            oldestIndex = i;
        }
    }
  // Returns the array index of the record with the oldest lastSeen timestamp
    return oldestIndex;
}
// Wi-Fi record handling
void recordWifi(const String& ssid, const String& bssid, int32_t rssi, int channel, uint8_t encryption){
  // Captures the current ESP32 uptime in milliseconds
    uint32_t now = millis();
    // Searches through all currently stored Wi-Fi records
    for (size_t i = 0; i < wifiCount; i++) {
      // Compares the discovered BSSID against previously stored BSSIDs
        if (wifiRecords[i].bssid == bssid){
          // Updates the stored SSID with the latest network name
            wifiRecords[i].ssid = safeText(ssid, 32);
          // Updates the received signal strength indicator RSSI
            wifiRecords[i].rssi = rssi;
          // Updates the Wi-Fi channel currently used by the access point.
            wifiRecords[i].channel = channel;
          // Updates the access point's Wi-Fi authentication/encryption type
            wifiRecords[i].encryption = encryption;
          // Updates the timestamp of the most recent detection
            wifiRecords[i].lastSeen = now;
          // Increments the number of times this access point has been detected
            wifiRecords[i].sightings++;
          // Increments the global counter tracking total Wi-Fi sightings
            wifiTotalSightings++;
          // Flags the user interface for a refresh so updated information
            requestUIRefresh();
          // Exits the function because the existing access point record
            return;
        }
    }
    // Declares an index variable to identify where the Wi-Fi record will be stored
    size_t index;

    // Checks whether the Wi-Fi record array still has available storage capacity
    if (wifiCount < MAX_WIFI_RECORDS){
      // Uses the next available position in the Wi-Fi record array
        index = wifiCount;
      // Increments the record counter to account for the new access point
        wifiCount++;
    } else {

        // Replace the oldest Wi-Fi record when the array is full.
        index = findOldestRecord(wifiRecords);
    }

    wifiRecords[index].ssid = safeText(ssid, 32);
    wifiRecords[index].bssid = bssid;
    wifiRecords[index].rssi = rssi;
    wifiRecords[index].channel = channel;
    wifiRecords[index].encryption = encryption;

    wifiRecords[index].firstSeen = now;
    wifiRecords[index].lastSeen = now;
    wifiRecords[index].sightings = 1;

    wifiTotalSightings++;

    addEvent(
        "Wi-Fi AP observed: " +
        (ssid.length() ? ssid : String("(hidden SSID)"))
    );

    requestUIRefresh();
}
