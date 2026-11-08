#include "doorbell_zigbee.h"

#include <Arduino.h>
#include <Zigbee.h>
#include <esp_zigbee_cluster.h>
#include <zcl/esp_zigbee_zcl_command.h>
#include <zcl/esp_zigbee_zcl_time.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstring>
#include <ctime>

#include "config.h"
#include "ota_update.h"

namespace {

constexpr uint32_t kZigbeeEpochOffset = 946684800UL;

class DoorbellEndpoint : public ZigbeeColorDimmableLight {
 public:
  explicit DoorbellEndpoint(uint8_t endpoint) : ZigbeeColorDimmableLight(endpoint) {
    esp_zb_attribute_list_t *onOffClient =
        esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    esp_zb_cluster_list_add_on_off_cluster(_cluster_list, onOffClient,
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
  }
};

DoorbellEndpoint *endpoint = nullptr;
std::atomic<bool> ringRequested{false};
portMUX_TYPE lightMux = portMUX_INITIALIZER_UNLOCKED;
LightSettings lightSettings{};
bool lightChanged = false;

uint32_t baseEpoch = 0;
uint32_t baseMillis = 0;
bool timeSynced = false;
portMUX_TYPE timeMux = portMUX_INITIALIZER_UNLOCKED;

void onLightChangeRgb(bool state, uint8_t red, uint8_t green, uint8_t blue,
                      uint8_t level) {
  portENTER_CRITICAL(&lightMux);
  lightSettings.on = state;
  lightSettings.level = level;
  lightSettings.red = red;
  lightSettings.green = green;
  lightSettings.blue = blue;
  lightChanged = true;
  portEXIT_CRITICAL(&lightMux);
}

void onIdentify(uint16_t time) {
  if (time > 0) {
    ringRequested.store(true, std::memory_order_release);
  }
}

bool sendOnCommand() {
  esp_zb_zcl_on_off_cmd_t command;
  std::memset(&command, 0, sizeof(command));
  command.zcl_basic_cmd.src_endpoint = config::kZigbeeEndpoint;
  command.zcl_basic_cmd.dst_endpoint = 1;
  command.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
  command.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  command.on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_ON_ID;

  esp_zb_lock_acquire(portMAX_DELAY);
  const uint8_t status = esp_zb_zcl_on_off_cmd_req(&command);
  esp_zb_lock_release();

  if (status != ESP_OK) {
    Serial.printf("Zigbee: on command failed: 0x%02x\n", status);
    return false;
  }
  return true;
}

bool setTimeAttribute(uint32_t unixTime) {
  if (unixTime <= kZigbeeEpochOffset) {
    return false;
  }
  uint32_t zigbeeTime = unixTime - kZigbeeEpochOffset;

  esp_zb_lock_acquire(portMAX_DELAY);
  const esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
      config::kZigbeeEndpoint, ESP_ZB_ZCL_CLUSTER_ID_TIME,
      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_TIME_TIME_ID,
      &zigbeeTime, false);
  esp_zb_lock_release();

  if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
    Serial.printf("Zigbee: time attribute failed: 0x%02x\n", status);
    return false;
  }
  return true;
}

bool reportTime(uint32_t unixTime) {
  if (!setTimeAttribute(unixTime)) {
    return false;
  }

  esp_zb_zcl_report_attr_cmd_t report;
  std::memset(&report, 0, sizeof(report));
  report.zcl_basic_cmd.src_endpoint = config::kZigbeeEndpoint;
  report.zcl_basic_cmd.dst_endpoint = 1;
  report.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
  report.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  report.clusterID = ESP_ZB_ZCL_CLUSTER_ID_TIME;
  report.attributeID = ESP_ZB_ZCL_ATTR_TIME_TIME_ID;
  report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report.manuf_specific = 0;
  report.dis_default_resp = 0;

  esp_zb_lock_acquire(portMAX_DELAY);
  const esp_err_t reportStatus = esp_zb_zcl_report_attr_cmd_req(&report);
  esp_zb_lock_release();

  if (reportStatus != ESP_OK) {
    Serial.printf("Zigbee: time report failed: 0x%x\n", reportStatus);
    return false;
  }
  return true;
}

}  // namespace

namespace doorbell_zigbee {

static void timeSyncTask(void *);

bool begin(const LightSettings &initialSettings) {
  setenv("TZ", "UTC0", 1);
  tzset();

  portENTER_CRITICAL(&lightMux);
  lightSettings = initialSettings;
  lightChanged = false;
  portEXIT_CRITICAL(&lightMux);

  endpoint = new DoorbellEndpoint(config::kZigbeeEndpoint);
  if (endpoint == nullptr) {
    Serial.println("Zigbee: endpoint allocation failed");
    return false;
  }

  endpoint->setManufacturerAndModel("DIY", "SmartDoorBell");
  endpoint->setPowerSource(ZB_POWER_SOURCE_MAINS);
  endpoint->addTimeCluster();
  endpoint->setLightColorCapabilities(ZIGBEE_COLOR_CAPABILITY_X_Y);
  endpoint->onLightChangeRgb(onLightChangeRgb);
  endpoint->onIdentify(onIdentify);
  if (!endpoint->addOTAClient(config::kFirmwareVersion,
                              config::kFirmwareVersion + 1,
                              config::kOtaHardwareVersion,
                              config::kOtaManufacturer,
                              config::kOtaImageType,
                              config::kOtaMaxDataSize)) {
    Serial.println("Zigbee: OTA client setup failed");
  }
  if (!Zigbee.addEndpoint(endpoint)) {
    Serial.println("Zigbee: endpoint registration failed");
    return false;
  }

  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    Serial.println("Zigbee: router start failed");
    return false;
  }

  ota_update::begin();

  if (!endpoint->setLight(initialSettings.on, initialSettings.level,
                          initialSettings.red, initialSettings.green,
                          initialSettings.blue)) {
    Serial.println("Zigbee: initial light setup failed");
  }

  Serial.println("Zigbee: router started");
  if (xTaskCreate(timeSyncTask, "time_sync", 6144, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("Zigbee: time sync task failed");
  }
  return true;
}

bool connected() { return Zigbee.connected(); }

bool syncTime() {
  if (endpoint == nullptr || !Zigbee.connected()) {
    return false;
  }

  struct tm timeinfo = endpoint->getTime();
  if (timeinfo.tm_year == 0) {
    Serial.println("Zigbee: time read failed");
    return false;
  }

  time_t unixTime = mktime(&timeinfo);
  if (unixTime <= 0) {
    Serial.println("Zigbee: time conversion failed");
    return false;
  }

  if (timeinfo.tm_year + 1900 < 2000) {
    unixTime += kZigbeeEpochOffset;
  }

  portENTER_CRITICAL(&timeMux);
  baseEpoch = static_cast<uint32_t>(unixTime);
  baseMillis = millis();
  timeSynced = true;
  portEXIT_CRITICAL(&timeMux);

  setTimeAttribute(unixTime);

  struct tm utc;
  gmtime_r(&unixTime, &utc);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &utc);
  Serial.printf("Zigbee: time synced to %s UTC\n", buffer);
  return true;
}

static void timeSyncTask(void *) {
  uint32_t lastOtaQueryMs = 0;
  for (;;) {
    if (!Zigbee.connected()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    const bool ok = syncTime();
    const uint32_t now = millis();
    if (lastOtaQueryMs == 0 ||
        now - lastOtaQueryMs >= config::kOtaQueryIntervalMs) {
      requestOtaUpdate();
      lastOtaQueryMs = now;
    }
    vTaskDelay(pdMS_TO_TICKS(ok ? config::kTimeResyncMs : config::kTimeSyncRetryMs));
  }
}

void requestOtaUpdate() {
  if (endpoint != nullptr) {
    endpoint->requestOTAUpdate();
  }
}

uint32_t unixNow() {
  portENTER_CRITICAL(&timeMux);
  if (!timeSynced) {
    portEXIT_CRITICAL(&timeMux);
    return 0;
  }
  const uint32_t epoch = baseEpoch;
  const uint32_t base = baseMillis;
  portEXIT_CRITICAL(&timeMux);

  return epoch + (millis() - base) / 1000UL;
}

void notifyPress(uint32_t unixTime) {
  if (endpoint == nullptr || !Zigbee.connected()) {
    return;
  }

  sendOnCommand();
  reportTime(unixTime);
}

void reportPressTime(uint32_t unixTime) {
  if (endpoint == nullptr || !Zigbee.connected()) {
    return;
  }

  reportTime(unixTime);
}

bool takeRingRequest() {
  return ringRequested.exchange(false, std::memory_order_acq_rel);
}

bool takeLightChange(LightSettings &settings) {
  bool changed = false;

  portENTER_CRITICAL(&lightMux);
  if (lightChanged) {
    settings = lightSettings;
    lightChanged = false;
    changed = true;
  }
  portEXIT_CRITICAL(&lightMux);

  return changed;
}

void factoryReset() { Zigbee.factoryReset(); }

}  // namespace doorbell_zigbee
