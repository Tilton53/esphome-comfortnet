#pragma once

#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "../comfortnet.h"

namespace comfortnet {

class ComfortnetTextSensor : public esphome::text_sensor::TextSensor, public esphome::Component, public ComfortnetClient {
 public:
  void setup() override;
  void dump_config() override;
  void set_sensor_key(const std::string &sensor_key) { this->sensor_key_ = sensor_key; }
  void set_sensor_target_device_type(uint8_t device_type) { this->device_type_ = static_cast<NodeType>(device_type); }

 protected:
  std::string sensor_key_{""};
  NodeType device_type_{NodeType::ANY};
};

}  // namespace comfortnet
