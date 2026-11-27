#include "esphome/core/log.h"
#include "comfortnet_text_sensor.h"

namespace comfortnet {

static const char *const TAG = "comfortnet.text_sensor";

void ComfortnetTextSensor::setup() {
  this->parent_->register_listener(this->sensor_key_, [this](ComfortnetData data) {
    if (data.device_type != NodeType::ANY && this->device_type_ != NodeType::ANY &&
        data.device_type != this->device_type_) {
      return;
    }
    if (data.type == ComfortnetData::DataType::STRING) {
      this->publish_state(std::get<std::string>(data.data));
    }
  });
}

void ComfortnetTextSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "ComfortNet Text Sensor:");
  ESP_LOGCONFIG(TAG, "  Data key: %s", this->sensor_key_.c_str());
}

}  // namespace comfortnet
