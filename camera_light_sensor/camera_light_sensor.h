#pragma once

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome {
namespace camera_light_sensor {

class CameraLightSensor : public PollingComponent, public sensor::Sensor {
 public:
  void setup() override {
    // This will be called by App.setup()
    ESP_LOGI("camera_light_sensor", "Setting up Camera Light Sensor");
  }

  void update() override {
    // This will be called every "update_interval" milliseconds.
    // You can fetch new data from the sensor here and publish it.
    ESP_LOGI("camera_light_sensor", "Updating Camera Light Sensor");

    // Example: Publish a dummy value (replace with actual sensor reading)
    this->publish_state(42.0);
  }
};

}  // namespace camera_light_sensor
}  // namespace esphome
