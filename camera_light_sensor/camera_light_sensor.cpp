#include "camera_light_sensor.h"
#include <cmath>
#include "esphome/core/log.h"

namespace esphome {
namespace camera_light_sensor {

static const char* const TAG = "camera_light_sensor";

/**
 * @brief Configures sensor metadata and targets.
 */
void CameraLightSensor::set_sensor_info(std::string name,
                                        std::vector<uint32_t> box,
                                        std::vector<uint8_t> color) {
  this->name = name;
  memcpy(this->roi, box.data(), sizeof(this->roi));
  memcpy(this->expected_color, color.data(), sizeof(this->expected_color));
}

/**
 * @brief Sets up the snapshot server and background task.
 */
void CameraLightSensorHub::setup() {
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
 *
 * Runs at a fixed frequency of 500ms (twice per second).
 */
void CameraLightSensorHub::task_loop() {
  TickType_t last_wake_time = xTaskGetTickCount();
  while (true) {
    this->process_camera();
    this->data_ready = true;
    // Precisely timed delay to maintain 2Hz processing frequency
    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(500));
  }
}

/**
 * @brief Performs image capture and color analysis.
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

  bool rgb_allocated = false;
  uint8_t* out_buf = fb->buf;

  // Convert non-RGB888 formats to RGB888 for analysis
  if (fb->format != PIXFORMAT_RGB888) {
    rgb_allocated = true;
    out_buf = (uint8_t*)heap_caps_malloc(fb->width * fb->height * 3, MALLOC_CAP_SPIRAM);
    if (!out_buf) {
      ESP_LOGE(TAG, "RGB memory allocation failed (PSRAM)");
      esp_camera_fb_return(fb);
      return;
    }
    bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    if (!converted) {
      ESP_LOGE(TAG, "Format to RGB888 failed");
      free(out_buf);
      esp_camera_fb_return(fb);
      return;
    }
  }

  // Analyze each ROI for its average color and compare to target
  for (auto* s : sensors) {
    uint32_t* roi = s->get_roi();
    uint8_t* expected = s->get_expected_color();

    uint32_t x1 = std::max((uint32_t)0, roi[0]);
    uint32_t y1 = std::max((uint32_t)0, roi[1]);
    uint32_t x2 = std::min((uint32_t)fb->width, roi[2]);
    uint32_t y2 = std::min((uint32_t)fb->height, roi[3]);

    uint64_t r_sum = 0, g_sum = 0, b_sum = 0;
    uint32_t count = 0;

    for (uint32_t y = y1; y < y2; y++) {
      for (uint32_t x = x1; x < x2; x++) {
        uint32_t idx = (y * fb->width + x) * 3;
        r_sum += out_buf[idx + 2];  // R
        g_sum += out_buf[idx + 1];  // G
        b_sum += out_buf[idx + 0];  // B
        count++;
      }
    }

    if (count > 0) {
      uint8_t avg_r = r_sum / count;
      uint8_t avg_g = g_sum / count;
      uint8_t avg_b = b_sum / count;

      int dr = avg_r - expected[0];
      int dg = avg_g - expected[1];
      int db = avg_b - expected[2];

      // Euclidean distance in RGB space to determine color match
      float dist = std::sqrt(dr * dr + dg * dg + db * db);
      bool match = dist <= 50.0;  // Distance threshold of 50.0

      s->set_latest_state(match);
    }
  }

  // Cleanup buffers
  if (rgb_allocated) {
    free(out_buf);
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
