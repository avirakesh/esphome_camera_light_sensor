#include "camera_light_sensor.h"
#include <cmath>
#include "esphome/core/log.h"

namespace esphome {
namespace camera_light_sensor {

static const char* const TAG = "camera_light_sensor";

/**
 * @brief Fast integer RGB to HSV conversion.
 * 
 * Maps Hue to 0-255 (instead of 0-360°) to fit into a single byte.
 */
HSV CameraLightSensorHub::rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b) {
  HSV hsv;
  uint8_t min_val = std::min({r, g, b});
  uint8_t max_val = std::max({r, g, b});
  uint8_t delta = max_val - min_val;

  hsv.v = max_val;
  if (max_val == 0) {
    hsv.s = 0;
    hsv.h = 0;
    return hsv;
  }

  hsv.s = (255UL * delta) / max_val;
  if (delta == 0) {
    hsv.h = 0;
    return hsv;
  }

  int32_t h;
  if (max_val == r) {
    h = 0 + 43 * (g - b) / delta;
  } else if (max_val == g) {
    h = 85 + 43 * (b - r) / delta;
  } else {
    h = 171 + 43 * (r - g) / delta;
  }

  hsv.h = static_cast<uint8_t>(h);
  return hsv;
}

/**
 * @brief Configures sensor metadata and targets, converting target RGB to HSV.
 */
void CameraLightSensor::set_sensor_info(std::string name,
                                        std::vector<uint32_t> box,
                                        std::vector<uint8_t> color) {
  this->name = name;
  memcpy(this->roi, box.data(), sizeof(this->roi));
  this->expected_hsv = CameraLightSensorHub::rgb_to_hsv(color[0], color[1], color[2]);
  
  ESP_LOGI(TAG, "Sensor '%s' configured with Target HSV(%d, %d, %d)", 
           this->name.c_str(), this->expected_hsv.h, this->expected_hsv.s, this->expected_hsv.v);
}

/**
 * @brief Sets up the snapshot server and background task.
 */
void CameraLightSensorHub::setup() {
  if (this->port > 0) {
    ESP_LOGI(TAG, "Setting up Camera Light Sensor Hub on port %d", this->port);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = this->port;
    config.max_uri_handlers = 2;

    // URI handler for the root path to serve snapshots
    httpd_uri_t snapshot_uri = {
        .uri = "/", .method = HTTP_GET, .handler = capture_handler, .user_ctx = this};

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
      httpd_register_uri_handler(camera_httpd, &snapshot_uri);
      ESP_LOGI(TAG, "Snapshot server started on port %d", this->port);
    } else {
      ESP_LOGE(TAG, "Failed to start HTTP server");
    }
  } else {
    ESP_LOGI(TAG, "Snapshot server disabled (no port configured)");
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
    s->publish_state(s->get_latest_state());
  }
}

/**
 * @brief Background task execution loop.
 */
void CameraLightSensorHub::task_loop() {
  TickType_t last_wake_time = xTaskGetTickCount();
  while (true) {
    this->process_camera();
    this->data_ready = true;
    // Precisely timed delay to maintain the configured processing frequency
    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(this->update_interval_ms));
  }
}

/**
 * @brief Performs image capture and color analysis in HSV space.
 */
void CameraLightSensorHub::process_camera() {
  if (!esp_camera_sensor_get()) {
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
    size_t required_size = fb->width * fb->height * 3;
    
    // Manage persistent buffer in PSRAM
    if (this->rgb_buffer == nullptr || this->rgb_buffer_capacity < required_size) {
      if (this->rgb_buffer != nullptr) {
        free(this->rgb_buffer);
      }
      this->rgb_buffer = (uint8_t*)heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM);
      if (this->rgb_buffer == nullptr) {
        ESP_LOGE(TAG, "RGB memory allocation failed (PSRAM)");
        this->rgb_buffer_capacity = 0;
        esp_camera_fb_return(fb);
        return;
      }
      this->rgb_buffer_capacity = required_size;
    }
    
    out_buf = this->rgb_buffer;
    bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    if (!converted) {
      ESP_LOGE(TAG, "Format to RGB888 failed");
      esp_camera_fb_return(fb);
      return;
    }
  }

  // Analyze each ROI for its average color in HSV space
  for (auto* s : sensors) {
    uint32_t* roi = s->get_roi();
    HSV expected = s->get_expected_hsv();

    uint32_t x1 = std::max((uint32_t)0, roi[0]);
    uint32_t y1 = std::max((uint32_t)0, roi[1]);
    uint32_t x2 = std::min((uint32_t)fb->width, roi[2]);
    uint32_t y2 = std::min((uint32_t)fb->height, roi[3]);

    float sin_h_sum = 0, cos_h_sum = 0;
    uint32_t s_sum = 0, v_sum = 0;
    uint32_t count = 0;

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
        count++;
      }
    }

    if (count > 0) {
      // Reconstruct average Hue from vector components
      float avg_h_angle = std::atan2(sin_h_sum / count, cos_h_sum / count);
      if (avg_h_angle < 0) avg_h_angle += 2.0f * M_PI;
      uint8_t avg_h = static_cast<uint8_t>(avg_h_angle * (256.0f / (2.0f * M_PI)));
      
      uint8_t avg_s = s_sum / count;
      uint8_t avg_v = v_sum / count;

      // Match logic:
      // 1. Hue distance (circular)
      int h_dist = std::abs((int)avg_h - (int)expected.h);
      if (h_dist > 128) h_dist = 256 - h_dist;
      
      // 2. Saturation and Value distance
      int s_dist = std::abs((int)avg_s - (int)expected.s);
      int v_dist = std::abs((int)avg_v - (int)expected.v);

      // Thresholds (scaled to 0-255):
      // Hue threshold ~20 degrees (256 * 20 / 360 = 14)
      bool hue_match = h_dist <= 14;
      // Saturation/Value threshold (flexible to allow for lighting changes)
      bool sv_match = s_dist <= 40 && v_dist <= 60;

      s->set_latest_state(hue_match && sv_match);
    }
  }

  esp_camera_fb_return(fb);
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
