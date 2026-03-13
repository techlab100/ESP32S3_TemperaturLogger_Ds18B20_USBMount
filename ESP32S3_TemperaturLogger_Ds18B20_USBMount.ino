#ifndef ARDUINO_USB_MODE
#error This ESP32 SoC has no Native USB interface
#elif ARDUINO_USB_MODE == 1
#warning This sketch should be used when USB is in OTG mode
void setup() {}
void loop() {}
#else

#include "USB.h"
#include "USBMSC.h"
#include <OneWire.h>
#include <DallasTemperature.h>

USBMSC MSC;

#define ONE_WIRE_BUS 4

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

#define FAT_U8(v)          ((v) & 0xFF)
#define FAT_U16(v)         FAT_U8(v), FAT_U8((v) >> 8)
#define FAT_U32(v)         FAT_U8(v), FAT_U8((v) >> 8), FAT_U8((v) >> 16), FAT_U8((v) >> 24)
#define FAT_MS2B(s, ms)    FAT_U8(((((s) & 0x1) * 1000) + (ms)) / 10)
#define FAT_HMS2B(h, m, s) FAT_U8(((s) >> 1) | (((m) & 0x7) << 5)), FAT_U8((((m) >> 3) & 0x7) | ((h) << 3))
#define FAT_YMD2B(y, m, d) FAT_U8(((d) & 0x1F) | (((m) & 0x7) << 5)), FAT_U8((((m) >> 3) & 0x1) | ((((y) - 1980) & 0x7F) << 1))
#define FAT_TBL2B(l, h)    FAT_U8(l), FAT_U8(((l >> 8) & 0xF) | ((h << 4) & 0xF0)), FAT_U8(h >> 4)

static const uint32_t DISK_SECTOR_COUNT = 2 * 8;   // 8KB
static const uint16_t DISK_SECTOR_SIZE  = 512;
static const uint16_t DISC_SECTORS_PER_TABLE = 1;

static uint8_t msc_disk[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE] = {0};

static const uint8_t DATA_START_BLOCK = 3;                   // boot, FAT, root dir
static const uint8_t FIRST_FILE_CLUSTER = 2;                 // cluster 2 -> block 3
static const uint8_t FILE_CLUSTER_COUNT = DISK_SECTOR_COUNT - DATA_START_BLOCK; // 13
static const uint32_t FILE_MAX_SIZE = FILE_CLUSTER_COUNT * DISK_SECTOR_SIZE;

static uint32_t logFileSize = 0;
static unsigned long lastLog = 0;
static const unsigned long intervalMs = 2000;
static bool loggingEnabled = true;

// ---------- FAT12 helpers ----------
static uint16_t fat12Offset(uint16_t cluster) {
  return (cluster * 3) / 2;
}

static void fat12SetEntry(uint16_t cluster, uint16_t value) {
  uint8_t *fat = msc_disk[1];
  uint16_t off = fat12Offset(cluster);

  if (cluster & 1) {
    fat[off]     = (fat[off] & 0x0F) | ((value << 4) & 0xF0);
    fat[off + 1] = (value >> 4) & 0xFF;
  } else {
    fat[off]     = value & 0xFF;
    fat[off + 1] = (fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F);
  }
}

static void setFileSize(uint32_t size) {
  // second root entry starts at byte 32
  uint8_t *entry = &msc_disk[2][32];
  entry[28] = (uint8_t)(size & 0xFF);
  entry[29] = (uint8_t)((size >> 8) & 0xFF);
  entry[30] = (uint8_t)((size >> 16) & 0xFF);
  entry[31] = (uint8_t)((size >> 24) & 0xFF);
}

static void clearLogFile() {
  for (uint8_t b = DATA_START_BLOCK; b < DISK_SECTOR_COUNT; b++) {
    memset(msc_disk[b], 0, DISK_SECTOR_SIZE);
  }
  logFileSize = 0;
  setFileSize(0);
  Serial.println("LOG.TXT CLEARED");
}

static void initDisk() {
  memset(msc_disk, 0, sizeof(msc_disk));

  // -------- Block 0: Boot Sector --------
  uint8_t boot[512] = {
    0xEB, 0x3C, 0x90,
    'M', 'S', 'D', 'O', 'S', '5', '.', '0',
    FAT_U16(DISK_SECTOR_SIZE),
    FAT_U8(1),
    FAT_U16(1),
    FAT_U8(1),
    FAT_U16(16),
    FAT_U16(DISK_SECTOR_COUNT),
    0xF8,
    FAT_U16(DISC_SECTORS_PER_TABLE),
    FAT_U16(1),
    FAT_U16(1),
    FAT_U32(0),
    FAT_U32(0),
    0x00,
    0x00,
    0x29,
    FAT_U32(0x1234),
    'T', 'i', 'n', 'y', 'U', 'S', 'B', ' ', 'M', 'S', 'C',
    'F', 'A', 'T', '1', '2', ' ', ' ', ' '
  };
  memcpy(msc_disk[0], boot, sizeof(boot));
  msc_disk[0][510] = 0x55;
  msc_disk[0][511] = 0xAA;

  // -------- Block 1: FAT12 --------
  // reserved FAT entries
  fat12SetEntry(0, 0xFF8);
  fat12SetEntry(1, 0xFFF);

  // chain file clusters 2 -> 3 -> 4 -> ... -> end
  for (uint16_t c = FIRST_FILE_CLUSTER; c < FIRST_FILE_CLUSTER + FILE_CLUSTER_COUNT - 1; c++) {
    fat12SetEntry(c, c + 1);
  }
  fat12SetEntry(FIRST_FILE_CLUSTER + FILE_CLUSTER_COUNT - 1, 0xFFF);

  // -------- Block 2: Root Directory --------
  // volume label entry
  uint8_t *dir = msc_disk[2];
  memcpy(&dir[0], "ESP32S3 MSC", 11);
  dir[11] = 0x08;

  // second entry = LOG.TXT
  uint8_t *file = &dir[32];
  memcpy(&file[0], "LOG     TXT", 11);
  file[11] = 0x20;
  file[13] = FAT_MS2B(0, 0);
  file[14] = FAT_HMS2B(12, 0, 0);
  file[16] = FAT_YMD2B(2026, 3, 13);
  file[18] = FAT_YMD2B(2026, 3, 13);
  file[22] = FAT_HMS2B(12, 0, 0);
  file[24] = FAT_YMD2B(2026, 3, 13);
  file[26] = FIRST_FILE_CLUSTER & 0xFF;
  file[27] = (FIRST_FILE_CLUSTER >> 8) & 0xFF;
  setFileSize(0);

  clearLogFile();
}

static void appendLine(const String &line) {
  if (logFileSize >= FILE_MAX_SIZE) {
    Serial.println("WARN: LOG.TXT FULL");
    loggingEnabled = false;
    return;
  }

  uint32_t remaining = FILE_MAX_SIZE - logFileSize;
  uint32_t n = min((uint32_t)line.length(), remaining);

  for (uint32_t i = 0; i < n; i++) {
    uint32_t pos = logFileSize + i;
    uint32_t block  = DATA_START_BLOCK + (pos / DISK_SECTOR_SIZE);
    uint32_t offset = pos % DISK_SECTOR_SIZE;
    msc_disk[block][offset] = (uint8_t)line[i];
  }

  logFileSize += n;
  setFileSize(logFileSize);

  Serial.print("LOG: ");
  Serial.print(line);
}

// ---------- USB MSC callbacks ----------
static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  Serial.printf("MSC WRITE: lba: %lu, offset: %lu, bufsize: %lu\n", lba, offset, bufsize);
  memcpy(msc_disk[lba] + offset, buffer, bufsize);
  return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  Serial.printf("MSC READ: lba: %lu, offset: %lu, bufsize: %lu\n", lba, offset, bufsize);
  memcpy(buffer, msc_disk[lba] + offset, bufsize);
  return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  Serial.printf("MSC START/STOP: power: %u, start: %u, eject: %u\n", power_condition, start, load_eject);
  return true;
}

static void usbEventCallback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == ARDUINO_USB_EVENTS) {
    arduino_usb_event_data_t *data = (arduino_usb_event_data_t *)event_data;
    switch (event_id) {
      case ARDUINO_USB_STARTED_EVENT: Serial.println("USB PLUGGED"); break;
      case ARDUINO_USB_STOPPED_EVENT: Serial.println("USB UNPLUGGED"); break;
      case ARDUINO_USB_SUSPEND_EVENT: Serial.printf("USB SUSPENDED: remote_wakeup_en: %u\n", data->suspend.remote_wakeup_en); break;
      case ARDUINO_USB_RESUME_EVENT:  Serial.println("USB RESUMED"); break;
      default: break;
    }
  }
}

// ---------- Serial commands ----------
static void handleCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == 's' || c == 'S') {
      loggingEnabled = false;
      Serial.println("LOGGER STOPPED");
    } else if (c == 'r' || c == 'R') {
      loggingEnabled = true;
      Serial.println("LOGGER RUNNING");
    } else if (c == 'c' || c == 'C') {
      clearLogFile();
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(1500);

  Serial.println("ESP32-S3 USBMSC LOGGER");
  Serial.println("Version 1.0");
  Serial.println("Commands: s=stop, r=run, c=clear");

  sensors.begin();
  initDisk();

  USB.onEvent(usbEventCallback);

  MSC.vendorID("ESP32");
  MSC.productID("USB_LOGGER");
  MSC.productRevision("1.0");
  MSC.onStartStop(onStartStop);
  MSC.onRead(onRead);
  MSC.onWrite(onWrite);

  MSC.mediaPresent(true);
  MSC.isWritable(false);  // read-only on PC = more robust
  MSC.begin(DISK_SECTOR_COUNT, DISK_SECTOR_SIZE);

  USB.begin();

  Serial.println("USBMSC STARTED");
  Serial.println("File: LOG.TXT");
}

void loop() {
  handleCommands();

  if (!loggingEnabled) return;
  if (millis() - lastLog < intervalMs) return;

  lastLog = millis();

  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  uint32_t ts = millis() / 1000;

  if (t == DEVICE_DISCONNECTED_C || t < -55 || t > 125) {
    appendLine(String(ts) + "||ERROR\r\n");
  } else {
    appendLine(String(ts) + "|" + String(t, 2) + "|OK\r\n");
  }
}

#endif /* ARDUINO_USB_MODE */
