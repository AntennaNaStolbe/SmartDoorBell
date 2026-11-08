#include "ota_update.h"

#include <Arduino.h>
#include <Zigbee.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <zcl/esp_zigbee_zcl_command.h>

#include <algorithm>
#include <cstring>

#include "config.h"

namespace {

constexpr uint32_t kOtaFileMagic = 0x0BEEF11E;
constexpr uint16_t kOtaHeaderVersion = 0x0100;
constexpr size_t kOtaMandatoryHeaderSize = 56;
constexpr size_t kOtaMaxHeaderSize = 128;
constexpr size_t kOtaElementHeaderSize = 6;
constexpr uint16_t kOtaImageTag = 0x0000;

constexpr uint16_t kFieldControlCredentialVersion = 1U << 0;
constexpr uint16_t kFieldControlDeviceSpecific = 1U << 1;
constexpr uint16_t kFieldControlHardwareVersions = 1U << 2;

enum class ParserState : uint8_t {
  Idle,
  Header,
  ElementHeader,
  ElementData,
  Ready,
};

struct OtaParser {
  ParserState state = ParserState::Idle;
  uint8_t header[kOtaMaxHeaderSize] = {};
  size_t headerSize = 0;
  size_t headerLength = 0;
  uint8_t elementHeader[kOtaElementHeaderSize] = {};
  size_t elementHeaderSize = 0;
  uint32_t totalSize = 0;
  uint32_t received = 0;
  uint32_t imageWritten = 0;
  uint32_t elementLength = 0;
  uint32_t elementRemaining = 0;
  bool imageSeen = false;
};

OtaParser parser;
const esp_partition_t *updatePartition = nullptr;
esp_ota_handle_t otaHandle = 0;
bool otaActive = false;
bool otaFailed = false;
uint32_t otaStartedMs = 0;

uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

bool failOta(const char *reason) {
  Serial.printf("OTA: %s\n", reason);
  otaFailed = true;
  if (otaActive) {
    esp_ota_abort(otaHandle);
    otaActive = false;
  }
  parser.state = ParserState::Idle;
  return false;
}

bool validateOptionalHardware(const uint8_t *header, size_t headerLength,
                              uint16_t fieldControl) {
  size_t offset = kOtaMandatoryHeaderSize;
  if ((fieldControl & kFieldControlCredentialVersion) != 0) {
    offset += 1;
  }
  if ((fieldControl & kFieldControlDeviceSpecific) != 0) {
    offset += 8;
  }
  if ((fieldControl & kFieldControlHardwareVersions) != 0) {
    if (offset + 4 > headerLength) {
      return false;
    }
    const uint16_t minimum = readLe16(header + offset);
    const uint16_t maximum = readLe16(header + offset + 2);
    if (config::kOtaHardwareVersion < minimum ||
        config::kOtaHardwareVersion > maximum) {
      Serial.printf("OTA: hardware %u outside %u..%u\n",
                    config::kOtaHardwareVersion, minimum, maximum);
      return false;
    }
    offset += 4;
  }
  return offset <= headerLength;
}

bool validateHeader() {
  if (parser.headerLength < kOtaMandatoryHeaderSize ||
      parser.headerLength > kOtaMaxHeaderSize) {
    return failOta("invalid header length");
  }
  if (readLe32(parser.header) != kOtaFileMagic) {
    return failOta("invalid file magic");
  }
  if (readLe16(parser.header + 4) != kOtaHeaderVersion) {
    return failOta("unsupported header version");
  }

  const uint16_t fieldControl = readLe16(parser.header + 8);
  const uint16_t manufacturer = readLe16(parser.header + 10);
  const uint16_t imageType = readLe16(parser.header + 12);
  const uint32_t fileVersion = readLe32(parser.header + 14);
  const uint32_t imageSize = readLe32(parser.header + 52);

  if (manufacturer != config::kOtaManufacturer) {
    Serial.printf("OTA: manufacturer 0x%04x, expected 0x%04x\n", manufacturer,
                  config::kOtaManufacturer);
    return failOta("manufacturer mismatch");
  }
  if (imageType != config::kOtaImageType) {
    Serial.printf("OTA: image type 0x%04x, expected 0x%04x\n", imageType,
                  config::kOtaImageType);
    return failOta("image type mismatch");
  }
  if (imageSize != parser.totalSize) {
    Serial.printf("OTA: image size %lu, expected %lu\n",
                  static_cast<unsigned long>(imageSize),
                  static_cast<unsigned long>(parser.totalSize));
    return failOta("image size mismatch");
  }
  if (!validateOptionalHardware(parser.header, parser.headerLength,
                                fieldControl)) {
    return failOta("unsupported hardware version");
  }

  Serial.printf("OTA: image version 0x%08lx, %lu bytes\n",
                static_cast<unsigned long>(fileVersion),
                static_cast<unsigned long>(parser.totalSize));
  parser.state = ParserState::ElementHeader;
  return true;
}

bool startOta(const esp_zb_zcl_ota_upgrade_value_message_t *message) {
  if (otaActive) {
    esp_ota_abort(otaHandle);
    otaActive = false;
  }

  updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    return failOta("no OTA partition");
  }

  parser = OtaParser{};
  parser.totalSize = message->ota_header.image_size;
  parser.state = ParserState::Header;

  if (parser.totalSize == 0 || parser.totalSize > updatePartition->size) {
    return failOta("image does not fit OTA partition");
  }

  const esp_err_t status = esp_ota_begin(updatePartition, 0, &otaHandle);
  if (status != ESP_OK) {
    Serial.printf("OTA: begin failed: %s\n", esp_err_to_name(status));
    return false;
  }

  otaActive = true;
  otaFailed = false;
  otaStartedMs = millis();
  Serial.printf("OTA: started, partition at 0x%08lx, capacity %lu bytes\n",
                static_cast<unsigned long>(updatePartition->address),
                static_cast<unsigned long>(updatePartition->size));
  return true;
}

bool feedOta(const uint8_t *data, size_t size) {
  size_t offset = 0;

  while (offset < size && !otaFailed) {
    if (parser.state == ParserState::Header) {
      if (parser.headerSize < 6) {
        const size_t count = std::min(size - offset, 6 - parser.headerSize);
        std::memcpy(parser.header + parser.headerSize, data + offset, count);
        parser.headerSize += count;
        parser.received += count;
        offset += count;

        if (parser.headerSize == 6) {
          parser.headerLength = readLe16(parser.header + 6);
          if (parser.headerLength < kOtaMandatoryHeaderSize ||
              parser.headerLength > kOtaMaxHeaderSize) {
            return failOta("invalid header length");
          }
        }
        continue;
      }

      if (parser.headerSize < parser.headerLength) {
        const size_t count =
            std::min(size - offset, parser.headerLength - parser.headerSize);
        std::memcpy(parser.header + parser.headerSize, data + offset, count);
        parser.headerSize += count;
        parser.received += count;
        offset += count;
      }

      if (parser.headerSize == parser.headerLength && !validateHeader()) {
        return false;
      }
      continue;
    }

    if (parser.state == ParserState::ElementHeader) {
      const size_t count = std::min(
          size - offset, kOtaElementHeaderSize - parser.elementHeaderSize);
      std::memcpy(parser.elementHeader + parser.elementHeaderSize,
                  data + offset, count);
      parser.elementHeaderSize += count;
      parser.received += count;
      offset += count;

      if (parser.elementHeaderSize == kOtaElementHeaderSize) {
        const uint16_t tag = readLe16(parser.elementHeader);
        parser.elementLength = readLe32(parser.elementHeader + 2);
        if (tag != kOtaImageTag) {
          return failOta("unexpected OTA element");
        }
        if (parser.imageSeen || parser.elementLength == 0) {
          return failOta("invalid OTA image element");
        }
        parser.imageSeen = true;
        parser.elementRemaining = parser.elementLength;
        parser.state = ParserState::ElementData;
      }
      continue;
    }

    if (parser.state == ParserState::ElementData) {
      const size_t count =
          std::min<size_t>(size - offset, parser.elementRemaining);
      const esp_err_t status =
          esp_ota_write(otaHandle, data + offset, count);
      if (status != ESP_OK) {
        Serial.printf("OTA: write failed: %s\n", esp_err_to_name(status));
        return failOta("write failed");
      }

      parser.elementRemaining -= count;
      parser.imageWritten += count;
      parser.received += count;
      offset += count;

      if (parser.elementRemaining == 0) {
        parser.state = ParserState::ElementHeader;
        parser.elementHeaderSize = 0;
      }
      continue;
    }

    return failOta("unexpected OTA data");
  }

  if (parser.received > parser.totalSize) {
    return failOta("received more data than expected");
  }
  return !otaFailed;
}

bool checkOta() {
  if (otaFailed || !parser.imageSeen || parser.elementRemaining != 0 ||
      parser.received != parser.totalSize ||
      parser.imageWritten != parser.elementLength) {
    return failOta("incomplete image");
  }
  Serial.printf("OTA: image complete, %lu bytes\n",
                static_cast<unsigned long>(parser.imageWritten));
  parser.state = ParserState::Ready;
  return true;
}

bool finishOta() {
  if (otaFailed || parser.state != ParserState::Ready) {
    return failOta("finish without valid image");
  }

  esp_err_t status = esp_ota_end(otaHandle);
  otaActive = false;
  if (status != ESP_OK) {
    Serial.printf("OTA: end failed: %s\n", esp_err_to_name(status));
    return false;
  }

  status = esp_ota_set_boot_partition(updatePartition);
  if (status != ESP_OK) {
    Serial.printf("OTA: set boot partition failed: %s\n",
                  esp_err_to_name(status));
    return false;
  }

  Serial.printf("OTA: finished in %lu ms, restarting\n",
                static_cast<unsigned long>(millis() - otaStartedMs));
  delay(200);
  esp_restart();
  return true;
}

esp_err_t handleOtaValue(
    const esp_zb_zcl_ota_upgrade_value_message_t *message) {
  if (message == nullptr) {
    return ESP_FAIL;
  }

  switch (message->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
      return startOta(message) ? ESP_OK : ESP_FAIL;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
      return feedOta(message->payload, message->payload_size) ? ESP_OK
                                                              : ESP_FAIL;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
      return checkOta() ? ESP_OK : ESP_FAIL;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
      Serial.println("OTA: apply");
      return otaFailed ? ESP_FAIL : ESP_OK;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
      return finishOta() ? ESP_OK : ESP_FAIL;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
      return failOta("aborted") ? ESP_OK : ESP_FAIL;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK:
      Serial.println("OTA: upgrade confirmed");
      return ESP_OK;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
      Serial.println("OTA: server reported error");
      return failOta("server error") ? ESP_OK : ESP_FAIL;
    default:
      Serial.printf("OTA: status %u\n", message->upgrade_status);
      return ESP_OK;
  }
}

esp_err_t handleSetAttribute(
    const esp_zb_zcl_set_attr_value_message_t *message) {
  if (message == nullptr) {
    return ESP_FAIL;
  }
  for (ZigbeeEP *ep : Zigbee.ep_objects) {
    if (message->info.dst_endpoint != ep->getEndpoint()) {
      continue;
    }
    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY) {
      ep->zbIdentify(message);
    } else {
      ep->zbAttributeSet(message);
    }
  }
  return ESP_OK;
}

esp_err_t handleReportAttribute(
    const esp_zb_zcl_report_attr_message_t *message) {
  if (message == nullptr) {
    return ESP_FAIL;
  }
  for (ZigbeeEP *ep : Zigbee.ep_objects) {
    if (message->dst_endpoint == ep->getEndpoint()) {
      ep->zbAttributeRead(message->cluster, &message->attribute,
                          message->src_endpoint, message->src_address);
    }
  }
  return ESP_OK;
}

esp_err_t handleReadAttributeResponse(
    const esp_zb_zcl_cmd_read_attr_resp_message_t *message) {
  if (message == nullptr) {
    return ESP_FAIL;
  }
  for (ZigbeeEP *ep : Zigbee.ep_objects) {
    if (message->info.dst_endpoint != ep->getEndpoint()) {
      continue;
    }
    esp_zb_zcl_read_attr_resp_variable_t *variable = message->variables;
    while (variable != nullptr) {
      if (variable->status == ESP_ZB_ZCL_STATUS_SUCCESS) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BASIC) {
          ep->zbReadBasicCluster(&variable->attribute);
        } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_TIME) {
          ep->zbReadTimeCluster(&variable->attribute);
        } else {
          ep->zbAttributeRead(message->info.cluster, &variable->attribute,
                              message->info.src_endpoint,
                              message->info.src_address);
        }
      }
      variable = variable->next;
    }
  }
  return ESP_OK;
}

esp_err_t handleWriteAttributeResponse(
    const esp_zb_zcl_cmd_write_attr_resp_message_t *message) {
  if (message == nullptr) {
    return ESP_FAIL;
  }
  for (ZigbeeEP *ep : Zigbee.ep_objects) {
    if (message->info.dst_endpoint != ep->getEndpoint()) {
      continue;
    }
    esp_zb_zcl_write_attr_resp_variable_t *variable = message->variables;
    while (variable != nullptr) {
      ep->zbWriteAttributeResponse(message->info.cluster,
                                   variable->attribute_id, variable->status,
                                   message->info.src_endpoint,
                                   message->info.src_address);
      variable = variable->next;
    }
  }
  return ESP_OK;
}

esp_err_t handleDefaultResponse(
    const esp_zb_zcl_cmd_default_resp_message_t *message) {
  if (message == nullptr) {
    return ESP_FAIL;
  }
  Zigbee.callDefaultResponseCallback(
      static_cast<zb_cmd_type_t>(message->resp_to_cmd), message->status_code,
      message->info.dst_endpoint, message->info.cluster);
  for (ZigbeeEP *ep : Zigbee.ep_objects) {
    if (message->info.dst_endpoint == ep->getEndpoint()) {
      ep->zbDefaultResponse(message);
    }
  }
  return ESP_OK;
}

esp_err_t handleQueryImageResponse(
    const esp_zb_zcl_ota_upgrade_query_image_resp_message_t *message) {
  if (message == nullptr) {
    return ESP_FAIL;
  }
  if (message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS) {
    Serial.printf(
        "OTA: server 0x%04x endpoint %u, version 0x%08lx, size %lu\n",
        message->server_addr.u.short_addr, message->server_endpoint,
        static_cast<unsigned long>(message->file_version),
        static_cast<unsigned long>(message->image_size));
    if (message->image_size == 0 || message->file_version == 0) {
      Serial.println("OTA: image rejected");
      return ESP_FAIL;
    }
  } else {
    Serial.printf("OTA: query status 0x%02x\n", message->info.status);
  }
  return ESP_OK;
}

esp_err_t actionHandler(esp_zb_core_action_callback_id_t callbackId,
                        const void *message) {
  switch (callbackId) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
      return handleSetAttribute(
          static_cast<const esp_zb_zcl_set_attr_value_message_t *>(message));
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
      return handleReportAttribute(
          static_cast<const esp_zb_zcl_report_attr_message_t *>(message));
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
      return handleReadAttributeResponse(
          static_cast<const esp_zb_zcl_cmd_read_attr_resp_message_t *>(
              message));
    case ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID:
      return handleWriteAttributeResponse(
          static_cast<const esp_zb_zcl_cmd_write_attr_resp_message_t *>(
              message));
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
      return handleDefaultResponse(
          static_cast<const esp_zb_zcl_cmd_default_resp_message_t *>(message));
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
      return handleOtaValue(
          static_cast<const esp_zb_zcl_ota_upgrade_value_message_t *>(message));
    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
      return handleQueryImageResponse(
          static_cast<const esp_zb_zcl_ota_upgrade_query_image_resp_message_t *>(
              message));
    default:
      return ESP_OK;
  }
}

}  // namespace

namespace ota_update {

void begin() { esp_zb_core_action_handler_register(actionHandler); }

}  // namespace ota_update
