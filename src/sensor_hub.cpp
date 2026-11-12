#include "sensor_hub.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>

#include "board_pins.h"
#include "pmic.h"
#include "rtc.h"
#include "sdcard.h"
#include "sysinfo.h"

namespace sensors {
namespace {

constexpr uint32_t kSampleMs = 5000;
constexpr uint32_t kSlotMs = 30u * 60 * 1000;  // one history bar
constexpr const char *kLogDir = "/sdcard/logs";
constexpr const char *kLogFile = "/sdcard/logs/sensors.csv";

// The screen's axes in chip terms, portrait: for screen X (right), Y (up) and Z (out of the
// screen), which chip axis and sign. Verified on hardware (2026-11-12 pose tests): lying flat,
// screen up, chip Z reads -1 g, so out = -Z. Upright in portrait moves chip X and leaning
// right moves chip Y, both with the signs the opposite of a first guess. So right = -Y,
// up = -X, out = -Z: a right-handed frame, as it must be (flipping only one of X/Y couldn't be).
struct AxisMap {
  int chip;
  float sign;
};
constexpr AxisMap kScreenAxes[3] = {{1, -1}, {0, -1}, {2, -1}};

// The board warms the SHTC3. Calibrated 2026-11-12 against a 22.7 °C room reading: the sensor
// read 25.9 °C (33 % RH) with the board on USB, charging (constant-voltage stage), the UI
// idle. The heat varies with what's running (charging at 500 mA, audio, Wi-Fi later), so treat
// it as approximate; on battery it likely reads a little low.
constexpr float kSelfHeatingC = 3.2f;

// Saturation vapour pressure over water, hPa (Magnus formula, Sensirion's coefficients).
float saturation(float c) { return 6.112f * expf(17.62f * c / (243.12f + c)); }

// Ambient temperature and humidity from the sensor's: the same water vapour at the cooler
// room temperature means a higher relative humidity.
shtc3::Reading compensate(shtc3::Reading r) {
  if (!r.ok) return r;
  const float ambient = r.tempC - kSelfHeatingC;
  r.rh = std::min(100.0f, r.rh * saturation(r.tempC) / saturation(ambient));
  r.tempC = ambient;
  return r;
}

void toScreen(float v[3]) {
  const float chip[3] = {v[0], v[1], v[2]};
  for (int i = 0; i < 3; ++i) v[i] = kScreenAxes[i].sign * chip[kScreenAxes[i].chip];
}

// Gyro zero-rate offset, learnt while the board is still (the datasheet allows ±10 °/s; this
// one read about -4 °/s on X at rest). Only the UI task calls motion(), so no lock.
float gyroBias[3] = {};
float lastAcc[3] = {};
bool haveBias = false;

SemaphoreHandle_t mutex = nullptr;
shtc3::Reading latest;
std::vector<float> slots;  // completed half hours, oldest first
double slotSum = 0;
int slotCount = 0;
uint32_t slotStart = 0;
std::vector<uint8_t> scanned;
FILE *logFile = nullptr;
uint32_t lastLogMs = 0;

class Lock {
 public:
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};

// One CSV row: every reading the board has. Called with the lock held.
void appendRow(const shtc3::Reading &env) {
  const qmi8658::Reading m = qmi8658::read();
  const pmic::Detail p = pmic::detail();
  char when[24] = "";
  if (rtc::timeValid()) {
    const time_t now = time(nullptr);
    tm t;
    localtime_r(&now, &t);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &t);
  }
  fprintf(logFile, "%lu,%s,", static_cast<unsigned long>(sysinfo::uptimeSeconds()), when);
  if (env.ok) {
    fprintf(logFile, "%.2f,%.1f,", env.tempC, env.rh);
  } else {
    fputs(",,", logFile);
  }
  if (m.ok) {
    fprintf(logFile, "%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.1f,", m.acc[0], m.acc[1], m.acc[2],
            m.gyro[0], m.gyro[1], m.gyro[2], m.tempC);
  } else {
    fputs(",,,,,,,", logFile);
  }
  fprintf(logFile, "%d,%d,%d\n", p.status.percent, p.vbatMv, p.status.vbus ? 1 : 0);
  fflush(logFile);
}

void sampler(void *) {
  slotStart = millis();
  for (;;) {
    const shtc3::Reading r = compensate(shtc3::read());
    {
      Lock lock;
      if (r.ok) {
        latest = r;
        slotSum += r.tempC;
        ++slotCount;
      }
      if (millis() - slotStart >= kSlotMs) {
        if (slotCount) slots.push_back(static_cast<float>(slotSum / slotCount));
        if (slots.size() > kHistorySlots - 1) slots.erase(slots.begin());
        slotSum = 0;
        slotCount = 0;
        slotStart += kSlotMs;
      }
      if (logFile && millis() - lastLogMs >= kLogEverySeconds * 1000u) {
        lastLogMs = millis();
        appendRow(r);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kSampleMs));
  }
}

}  // namespace

void begin() {
  mutex = xSemaphoreCreateMutex();
  shtc3::begin();
  qmi8658::begin();
  scanBus();
  xTaskCreatePinnedToCore(sampler, "sensors", 6144, nullptr, 1, nullptr, 0);
}

shtc3::Reading env() {
  Lock lock;
  return latest;
}

std::vector<float> history() {
  Lock lock;
  std::vector<float> out = slots;
  if (slotCount) out.push_back(static_cast<float>(slotSum / slotCount));
  return out;
}

qmi8658::Reading motion() {
  qmi8658::Reading m = qmi8658::read();
  if (!m.ok) return m;
  // Still: gravity alone (|a| about 1 g; this chip reads 1.035 g at rest, within the
  // datasheet's ±100 mg offset and ±3 % sensitivity), the accelerometer steady since the last
  // read, and no rotation beyond a plausible offset.
  const float mag = sqrtf(m.acc[0] * m.acc[0] + m.acc[1] * m.acc[1] + m.acc[2] * m.acc[2]);
  bool still = fabsf(mag - 1.0f) < 0.1f;
  for (int i = 0; i < 3; ++i) {
    still = still && fabsf(m.acc[i] - lastAcc[i]) < 0.01f && fabsf(m.gyro[i]) < 10.0f;
    lastAcc[i] = m.acc[i];
  }
  if (still) {
    for (int i = 0; i < 3; ++i) {
      gyroBias[i] = haveBias ? gyroBias[i] + 0.2f * (m.gyro[i] - gyroBias[i]) : m.gyro[i];
    }
    haveBias = true;
  }
  for (int i = 0; i < 3; ++i) m.gyro[i] -= gyroBias[i];
  toScreen(m.acc);
  toScreen(m.gyro);
  return m;
}

void tilt(const qmi8658::Reading &m, float &pitch, float &roll) {
  // At rest the accelerometer reads +1 g straight up, so an axis tipped upwards reads positive.
  constexpr float kDeg = 180.0f / static_cast<float>(M_PI);
  roll = atan2f(m.acc[0], m.acc[2]) * kDeg;
  pitch = atan2f(m.acc[1], m.acc[2]) * kDeg;
}

std::vector<uint8_t> scanBus() {
  std::vector<uint8_t> found;
  for (uint8_t a = 0x03; a <= 0x77; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found.push_back(a);
  }
  Lock lock;
  scanned = found;
  return found;
}

std::vector<uint8_t> lastScan() {
  Lock lock;
  return scanned;
}

int knownFound() {
  int n = 0;
  for (const uint8_t a : lastScan()) n += deviceName(a) != nullptr;
  return n;
}

const char *deviceName(uint8_t address) {
  switch (address) {
    case board::I2C_ADDR_ES8311: return "ES8311";
    case board::I2C_ADDR_PMIC: return "TG28";
    case board::I2C_ADDR_PCF85063: return "PCF85063A";
    case board::I2C_ADDR_QMI8658: return "QMI8658A";
    case board::I2C_ADDR_SHTC3: return "SHTC3";
    default: return nullptr;
  }
}

bool logging() {
  Lock lock;
  return logFile != nullptr;
}

bool setLogging(bool on) {
  Lock lock;
  if (!on) {
    if (logFile) fclose(logFile);
    logFile = nullptr;
    return true;
  }
  if (logFile) return true;
  if (!sdcard::mounted()) return false;
  mkdir(kLogDir, 0777);  // fails harmlessly if it exists
  struct stat st;
  const bool fresh = stat(kLogFile, &st) != 0 || st.st_size == 0;
  logFile = fopen(kLogFile, "a");
  if (!logFile) {
    log_e("sensors: can't open %s", kLogFile);
    return false;
  }
  if (fresh) {
    fputs("uptime_s,time,temp_c,rh_pct,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,imu_temp_c,"
          "battery_pct,vbat_mv,usb\n",
          logFile);
  }
  lastLogMs = millis() - kLogEverySeconds * 1000u;  // first row on the next sample
  log_i("sensors: logging to %s", kLogFile);
  return true;
}

const char *logPath() { return kLogFile; }

}  // namespace sensors
