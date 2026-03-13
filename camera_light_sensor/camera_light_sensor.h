#pragma once

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"

namespace esphome {

namespace esp32_camera {
class ESP32Camera;
}

namespace camera_light_sensor {

/**
 * @struct HSV
 * @brief Simple 8-bit HSV color structure.
 */
struct HSV {
  uint8_t h;  ///< Hue (0-255 map to 0-360°)
  uint8_t s;  ///< Saturation (0-255)
  uint8_t v;  ///< Value (0-255)
};

/**
 * @brief Individual sensor within the camera light sensor hub.
 *
 * Each sensor represents a Region of Interest (ROI) and an expected color.
 */
class CameraLightSensor : public binary_sensor::BinarySensor {
 public:
  /**
   * @brief Configures the sensor's name and region of interest.
   *
   * @param name The display name of the sensor.
   * @param box Region of Interest: [x1, y1, x2, y2].
   */
  void set_sensor_info(std::string name, std::vector<uint32_t> box);

  /**
   * @brief Sets the expected target color using RGB.
   * @param r Red component (0-255).
   * @param g Green component (0-255).
   * @param b Blue component (0-255).
   */
  void set_expected_rgb(uint8_t r, uint8_t g, uint8_t b);

  /**
   * @brief Sets the expected target color using HSV.
   * @param h Hue component (0-255).
   * @param s Saturation component (0-255).
   * @param v Value component (0-255).
   */
  void set_expected_hsv(uint8_t h, uint8_t s, uint8_t v);

  /**
   * @brief Sets the matching radius for the HSV space distance.
   * @param radius Euclidean distance radius in HSV space.
   */
  void set_match_radius(float radius);

  /**
   * @brief Sets the weights for the distance calculation.
   * @param h Hue weight.
   * @param s Saturation weight.
   * @param v Value weight.
   */
  void set_weights(float h, float s, float v) {
    this->hue_weight = h;
    this->saturation_weight = s;
    this->value_weight = v;
  }

  /// @return The sensor's name.
  std::string get_name() const { return name; }
  /// @return Pointer to the ROI array [x1, y1, x2, y2].
  uint32_t* get_roi() { return roi; }
  /// @return The expected HSV target.
  HSV get_expected_hsv() const { return expected_hsv; }

  /// @return Match radius in HSV space.
  float get_match_radius() const { return match_radius; }

  /// Cached values for optimized matching
  float get_expected_v_f() const { return expected_v_f; }
  float get_expected_s_f() const { return expected_s_f; }
  float get_expected_h_angle() const { return expected_h_angle; }
  float get_match_radius_sq() const { return match_radius_sq; }

  /// Weight accessors
  float get_hue_weight() const { return hue_weight; }
  float get_saturation_weight() const { return saturation_weight; }
  float get_value_weight() const { return value_weight; }

  /**
   * @brief Updates the most recently calculated state from the background task.
   * @param state True if the target color matches the ROI.
   */
  void set_latest_state(bool state) { this->latest_state = state; }
  /// @return The most recently calculated state.
  bool get_latest_state() const { return this->latest_state; }

  /**
   * @brief Publishes the provided state to the ESPHome framework.
   * @param state The state to publish.
   */
  void update_state(bool state) { this->publish_state(state); }

 private:
  std::string name;                       ///< Display name of the sensor.
  uint32_t roi[4];                        ///< Region of Interest: [x1, y1, x2, y2].
  HSV expected_hsv;                       ///< Expected color in HSV.
  float match_radius = 50.0f;             ///< Euclidean distance radius in HSV space.

  // Weights for distance components
  float hue_weight = 3.0f;
  float saturation_weight = 1.0f;
  float value_weight = 1.0f;

  // Cached for performance
  float expected_v_f = 0.0f;
  float expected_s_f = 0.0f;
  float expected_h_angle = 0.0f;
  float match_radius_sq = 2500.0f;

  std::atomic<bool> latest_state{false};  ///< Thread-safe storage for the latest calculated state.
};

/**
 * @brief Hub component that handles camera frame capture and coordinates sensors.
 *
 * This component runs a background FreeRTOS task to process images twice per second,
 * ensuring the main execution loop remains responsive.
 */
class CameraLightSensorHub : public PollingComponent {
 public:
  /**
   * @brief Registers a sensor with the hub.
   * @param s Pointer to the sensor to add.
   */
  void add_sensor(CameraLightSensor* s) { sensors.push_back(s); }

  /**
   * @brief Sets the camera component to use for captures.
   * @param camera Pointer to the ESP32 camera instance.
   */
  void set_camera(esp32_camera::ESP32Camera* camera) { this->camera = camera; }

  /**
   * @brief Sets the HTTP server port for snapshots.
   * @param port The port to listen on.
   */
  void set_port(uint16_t port) { this->port = port; }

  /**
   * @brief Sets the processing frequency for the background task.
   * @param ms The interval in milliseconds.
   */
  void set_update_interval_ms(uint32_t ms) { this->update_interval_ms = ms; }

  /**
   * @brief Sets the duration for camera captures.
   * @param ms The interval in milliseconds.
   */
  void set_capture_interval_ms(uint32_t ms) { this->capture_interval_ms = ms; }

  /// @return Setup priority; ensures Wi-Fi is up before starting HTTP server.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Initializes the HTTP server and background processing task.
  void setup() override;
  /// Checks for new data from the background task and pushes updates.
  void loop() override;
  /// Periodic heartbeat update to ensure Home Assistant state remains synced.
  void update() override;

  /**
   * @brief Wrapper for the FreeRTOS task entry point.
   * @param param Pointer to the CameraLightSensorHub instance.
   */
  static void task_wrapper(void* param) { static_cast<CameraLightSensorHub*>(param)->task_loop(); }

  /// Main loop for the background FreeRTOS task.
  void task_loop();

  /// Captures a frame, converts format if needed, and analyzes all ROIs.
  void process_camera();

  /**
   * @brief Fast integer RGB to HSV conversion.
   * @param r Red component (0-255).
   * @param g Green component (0-255).
   * @param b Blue component (0-255).
   * @return HSV structure with components mapped to 0-255.
   */
  static HSV rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b);

 private:
  std::vector<CameraLightSensor*> sensors;  ///< List of managed sensors.
  esp32_camera::ESP32Camera* camera{nullptr}; ///< Pointer to the ESP32 camera component.
  uint16_t port = 0;                        ///< Snapshot HTTP server port (0 = disabled).
  uint32_t update_interval_ms = 500;        ///< Interval between captures in ms.
  uint32_t capture_interval_ms = 0;         ///< Interval between captures (auto-derived).
  uint8_t* rgb_buffer = nullptr;            ///< Persistent RGB888 buffer in PSRAM.
  size_t rgb_buffer_capacity = 0;           ///< Current capacity of the RGB buffer.
  httpd_handle_t camera_httpd = NULL;       ///< Handle for the snapshot server.
  TaskHandle_t task_handle = NULL;          ///< Handle for the background task.
  std::atomic<bool> data_ready{false};      ///< Flag signaling new processing results are ready.

  /**
   * @brief HTTP handler for capturing and serving a JPEG snapshot.
   * @param req The HTTP request object.
   * @return esp_err_t result.
   */
  static esp_err_t capture_handler(httpd_req_t* req);
};

}  // namespace camera_light_sensor
}  // namespace esphome
