#include "camera_light_sensor.h"
#include <esp_timer.h>
#include <cmath>
#include "esphome/components/esp32_camera/esp32_camera.h"
#include "esphome/core/log.h"

namespace esphome {
namespace camera_light_sensor {

static const char* const TAG = "camera_light_sensor";

/**
 * @brief High-precision RGB to HSV conversion using floating point.
 *
 * Maps Hue to 0-255 (instead of 0-360°) to fit into a single byte.
 *
 * There is a fast integer approximation that avoids floating point, but I didn't notice too much
 * latency difference when processing small patches.
 */
HSV CameraLightSensorHub::rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b) {
  HSV hsv;
  float rf = r / 255.0f;
  float gf = g / 255.0f;
  float bf = b / 255.0f;

  float max_val = std::max({rf, gf, bf});
  float min_val = std::min({rf, gf, bf});
  float delta = max_val - min_val;

  hsv.v = static_cast<uint8_t>(std::round(max_val * 255.0f));

  if (max_val == 0.0f) {
    hsv.s = 0;
    hsv.h = 0;
    return hsv;
  }

  hsv.s = static_cast<uint8_t>(std::round((delta / max_val) * 255.0f));

  if (delta == 0.0f) {
    hsv.h = 0;
    return hsv;
  }

  float h;
  if (max_val == rf) {
    h = (gf - bf) / delta;
  } else if (max_val == gf) {
    h = 2.0f + (bf - rf) / delta;
  } else {
    h = 4.0f + (rf - gf) / delta;
  }

  h *= 60.0f;
  // Fix negative angles by adding 360° to bring them into the [0, 360) range.
  if (h < 0.0f)
    h += 360.0f;

  // Map 0-360° to 0-255. Use 256.0f as the divisor for circular wrapping (360° -> 256 -> 0).
  float h_mapped = std::round(h * 256.0f / 360.0f);
  hsv.h = static_cast<uint8_t>(static_cast<uint32_t>(h_mapped) % 256);

  return hsv;
}

/**
 * @brief Configures sensor metadata and ROI.
 */
void CameraLightSensor::set_sensor_info(std::string name, std::vector<uint32_t> box) {
  this->name = name;
  memcpy(this->roi, box.data(), sizeof(this->roi));
}

/**
 * @brief Sets the expected target color using RGB and converts it to HSV.
 */
void CameraLightSensor::set_expected_rgb(uint8_t r, uint8_t g, uint8_t b) {
  ESP_LOGV(TAG, "Sensor '%s' configured with Target RGB(%d, %d, %d). Converting to HSV...",
           this->name.c_str(), r, g, b);

  HSV epected_hsv = CameraLightSensorHub::rgb_to_hsv(r, g, b);
  this->set_expected_hsv(epected_hsv.h, epected_hsv.s, epected_hsv.v);
}

/**
 * @brief Sets the expected target color directly using HSV.
 */
void CameraLightSensor::set_expected_hsv(uint8_t h, uint8_t s, uint8_t v) {
  this->expected_hsv = {h, s, v};

  // Update cached values
  this->expected_v_f = (float)v;
  this->expected_s_f = (float)s;
  this->expected_h_angle = h * (2.0f * M_PI / 256.0f);

  ESP_LOGV(TAG, "Sensor '%s' configured with Target HSV(%d, %d, %d)", this->name.c_str(), h, s, v);
}

/**
 * @brief Sets the match radius and updates the cached squared value.
 */
void CameraLightSensor::set_match_radius(float radius) {
  this->match_radius = radius;
  this->match_radius_sq = radius * radius;
}

/**
 * @brief Sets up the snapshot server and background task.
 */
void CameraLightSensorHub::setup() {
  ESP_LOGV(TAG, "Setting up Camera Light Sensor Hub. Heartbeat: %ums, Capture Interval: %ums",
           this->update_interval_ms, this->capture_interval_ms);

  if (this->port > 0) {
    ESP_LOGV(TAG, "Setting up Camera Light Sensor Hub on port %d", this->port);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = this->port;
    config.max_uri_handlers = 2;

    // URI handler for the root path to serve snapshots
    httpd_uri_t snapshot_uri = {
        .uri = "/", .method = HTTP_GET, .handler = capture_handler, .user_ctx = this};

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
      httpd_register_uri_handler(camera_httpd, &snapshot_uri);
      ESP_LOGV(TAG, "Snapshot server started on port %d", this->port);
    } else {
      ESP_LOGE(TAG, "Failed to start HTTP server");
    }
  } else {
    ESP_LOGV(TAG, "Snapshot server disabled (no port configured)");
  }

  // Create the background task pinned to Core 1 to avoid interference with the main ESPHome loop
  xTaskCreatePinnedToCore(CameraLightSensorHub::task_wrapper, "camera_task", 8192, this, 1,
                          &this->task_handle, 1);
}

/**
 * @brief Main ESPHome loop execution.
 *
 * Pushes updates only when the background task signals that new data is available.
 */
void CameraLightSensorHub::loop() {
  if (this->data_ready.exchange(false)) {
    for (auto* s : sensors) {
      bool latest = s->get_latest_state();
      // Only publish if the state has changed or hasn't been set yet
      if (!s->has_state() || s->state != latest) {
        ESP_LOGD(TAG, "Sensor '%s' state changed to %s", s->get_name().c_str(), ONOFF(latest));
        s->publish_state(latest);
      }
    }
  }
}

/**
 * @brief Periodic heartbeat update.
 */
void CameraLightSensorHub::update() {
  for (auto* s : sensors) {
    bool latest = s->get_latest_state();
    ESP_LOGV(TAG, "Sensor '%s' heartbeat push: %s", s->get_name().c_str(), ONOFF(latest));
    s->publish_state(latest);
  }
}

/**
 * @brief Background task execution loop, responsible for camera capture and processing.
 */
void CameraLightSensorHub::task_loop() {
  TickType_t last_wake_time = xTaskGetTickCount();
  while (true) {
    this->process_camera();
    uint32_t interval = this->capture_interval_ms > 0 ? this->capture_interval_ms : this->update_interval_ms;

    // Precisely timed delay to maintain the configured processing frequency
    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(interval));
  }
}

/**
 * @brief Performs image capture and color analysis in HSV space.
 */
void CameraLightSensorHub::process_camera() {
  if (this->camera == nullptr || !esp_camera_sensor_get()) {
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    ESP_LOGE(TAG, "Camera fb get failed");
    return;
  }

  uint8_t* out_buf = fb->buf;

  // Convert non-RGB888 formats to RGB888 for analysis
  if (fb->format != PIXFORMAT_RGB888) {
    if (this->rgb_buffer == nullptr) {
      ESP_LOGV(TAG, "Allocating initial RGB buffer in PSRAM");
      usize_t frame_size = fb->width * fb->height * 3;
      this->rgb_buffer = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM);
      if (this->rgb_buffer == nullptr) {
        ESP_LOGE(TAG, "Initial RGB memory allocation failed (PSRAM)");
        esp_camera_fb_return(fb);
        return
      }
    }

    out_buf = this->rgb_buffer;
    bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    if (!converted) {
      ESP_LOGE(TAG, "Format to RGB888 failed");
      esp_camera_fb_return(fb);
      return;
    }
  }

  bool data_changed = false;
  // Analyze each ROI for its average color in HSV space
  for (auto* s : sensors) {
    bool sensor_state = this->calculate_sensor_value(s, fb, out_buf);
    data_changed |= s->state != sensor_state;
    s->set_latest_state(sensor_state);
  }

  this->data_ready = data_changed;

  esp_camera_fb_return(fb);
}

/**
 * @brief processes a single ROI for color analysis and updates the corresponding sensor.
 */
bool CameraLightSensorHub::calculate_sensor_value(CameraLightSensor* s,
                                                  camera_fb_t* fb,
                                                  uint8_t* out_buf) {
  uint32_t start_time = esp_timer_get_time();
  uint32_t* roi = s->get_roi();

  uint32_t x1 = roi[0];
  uint32_t y1 = roi[1];
  uint32_t x2 = std::min((uint32_t)fb->width, roi[2]);
  uint32_t y2 = std::min((uint32_t)fb->height, roi[3]);

  if (x1 >= fb->width || x2 >= fb->width || y1 >= fb->height || y2 >= fb->height) {
    ESP_LOGE(TAG, "ROI out of bounds for sensor '%s': [%d, %d, %d, %d], image size: [%d, %d]",
             s->get_name().c_str(), x1, y1, x2, y2, fb->width, fb->height);
    return false;
  }

  float sin_h_sum = 0, cos_h_sum = 0;
  uint32_t s_sum = 0, v_sum = 0;
  uint32_t num_pixels = 0;

  // No bounds checking here because the config does that for us.
  for (uint32_t y = y1; y < y2; y++) {
    for (uint32_t x = x1; x < x2; x++) {
      uint32_t idx = (y * fb->width + x) * 3;
      // Map BGR to RGB for the conversion
      HSV hsv = rgb_to_hsv(out_buf[idx + 2], out_buf[idx + 1], out_buf[idx + 0]);

      // Circular averaging for Hue using vector components
      float angle = hsv.h * (2.0f * M_PI / 256.0f);
      sin_h_sum += std::sin(angle);
      cos_h_sum += std::cos(angle);

      s_sum += hsv.s;
      v_sum += hsv.v;
      num_pixels++;
    }
  }

  // Reconstruct average Hue from vector components
  float avg_h_angle = std::atan2(sin_h_sum / num_pixels, cos_h_sum / num_pixels);
  if (avg_h_angle < 0)
    avg_h_angle += 2.0f * M_PI;
  uint8_t avg_h = static_cast<uint8_t>(avg_h_angle * (256.0f / (2.0f * M_PI)));

  float avg_s = (float)s_sum / num_pixels;
  float avg_v = (float)v_sum / num_pixels;

  // Weighted match logic in cylindrical HSV space
  float delta_v = avg_v - s->get_expected_v_f();
  float delta_s = avg_s - s->get_expected_s_f();

  float s1 = avg_s;
  float s2 = s->get_expected_s_f();
  float delta_h_term = 2.0f * s1 * s2 * (1.0f - std::cos(avg_h_angle - s->get_expected_h_angle()));

  // dist_sq = w_v * (V1-V2)^2 + w_s * (S1-S2)^2 + w_h * (2*S1*S2*(1-cos(H1-H2)))
  float dist_sq = s->get_value_weight() * (delta_v * delta_v) +
                  s->get_saturation_weight() * (delta_s * delta_s) +
                  s->get_hue_weight() * delta_h_term;

  float dist = std::sqrt(std::max(0.0f, dist_sq));

  uint32_t end_time = esp_timer_get_time();
  ESP_LOGD(TAG,
           "Sensor '%s':\n"
           "    State: %s,\n"
           "    Current HSV(%d, %.1f, %.1f),\n"
           "    Target HSV(%d, %.1f, %.1f),\n"
           "    Weighted Dist: %.2f,\n"
           "    Radius: %.2f\n"
           "    Processed %d pixels in %u us.",
           s->get_name().c_str(), ONOFF(dist_sq <= s->get_match_radius_sq()), avg_h, avg_s, avg_v,
           s->get_expected_hsv().h, s2, s->get_expected_v_f(), dist, s->get_match_radius(),
           num_pixels, (uint32_t)(end_time - start_time));

  return dist_sq <= s->get_match_radius_sq();
}

/**
 * @brief Serves a JPEG snapshot via HTTP.
 */
esp_err_t CameraLightSensorHub::capture_handler(httpd_req_t* req) {
  if (!esp_camera_sensor_get()) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    ESP_LOGE(TAG, "Camera fb get failed for /snapshot");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

  esp_err_t res = ESP_OK;
  if (fb->format == PIXFORMAT_JPEG) {
    res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  } else {
    uint8_t* jpeg_buf = NULL;
    size_t jpeg_len = 0;
    // Convert frame buffer to JPEG for serving over HTTP
    if (frame2jpg(fb, 80, &jpeg_buf, &jpeg_len)) {
      res = httpd_resp_send(req, (const char*)jpeg_buf, jpeg_len);
      free(jpeg_buf);
    } else {
      res = ESP_FAIL;
      httpd_resp_send_500(req);
    }
  }
  esp_camera_fb_return(fb);
  return res;
}

}  // namespace camera_light_sensor
}  // namespace esphome
