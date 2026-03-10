#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"

namespace esphome {
namespace camera_light_sensor {

class CameraLightSensor : public binary_sensor::BinarySensor {
 public:
  void set_sensor_info(std::string name, std::vector<uint32_t> box, std::vector<uint8_t> color) {
    this->name = name;
    memcpy(this->roi, box.data(), sizeof(this->roi));
    memcpy(this->expected_color, color.data(), sizeof(this->expected_color));
  }

  std::string get_name() const { return name; }
  uint32_t* get_roi() { return roi; }
  uint8_t* get_expected_color() { return expected_color; }

  void set_latest_state(bool state) { this->latest_state = state; }
  bool get_latest_state() const { return this->latest_state; }

  void update_state(bool state) { this->publish_state(state); }

 private:
  std::string name;
  uint32_t roi[4];            // Region of Interest: [x1, y1, x2, y2]
  uint8_t expected_color[3];  // Expected color in RGB: [R, G, B]
  std::atomic<bool> latest_state{false};
};

// The Parent Hub (Handles the logic)
class CameraLightSensorHub : public PollingComponent {
 public:
  void add_sensor(CameraLightSensor* s) { sensors.push_back(s); }

  void set_port(uint16_t port) { this->port = port; }

  float get_setup_priority() const override {
    // We need both the camera (DATA priority) AND the Wi-Fi/IP stack (WIFI priority)
    // to be initialized before starting the HTTP server.
    return setup_priority::AFTER_WIFI;
  }

  void setup() override {
    ESP_LOGI("camera_light_sensor", "Setting up Camera Light Sensor Hub on port %d", this->port);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = this->port;
    config.max_uri_handlers = 2;

    httpd_uri_t snapshot_uri = {
        .uri = "/", .method = HTTP_GET, .handler = capture_handler, .user_ctx = this};

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
      httpd_register_uri_handler(camera_httpd, &snapshot_uri);
      ESP_LOGI("camera_light_sensor", "Snapshot server started on port %d", this->port);
    } else {
      ESP_LOGE("camera_light_sensor", "Failed to start HTTP server");
    }

    // Start background task
    xTaskCreatePinnedToCore(CameraLightSensorHub::task_wrapper, "camera_task", 8192, this, 1,
                            &this->task_handle, 1);
  }

  void loop() override {
    // Check if the background task has finished processing a frame
    if (this->data_ready.exchange(false)) {
      for (auto* s : sensors) {
        bool latest = s->get_latest_state();
        if (!s->has_state() || s->state != latest) {
          s->publish_state(latest);
        }
      }
    }
  }

  void update() override {
    // Periodic update call (heartbeat)
    for (auto* s : sensors) {
      s->publish_state(s->get_latest_state());
    }
  }

  static void task_wrapper(void* param) { static_cast<CameraLightSensorHub*>(param)->task_loop(); }

  void task_loop() {
    TickType_t last_wake_time = xTaskGetTickCount();
    while (true) {
      this->process_camera();
      // Signal the main loop that new data is ready
      this->data_ready = true;
      // Adjust polling frequency of the background task here.
      // 500ms is exactly twice per second.
      vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(500));
    }
  }

  void process_camera() {
    if (!esp_camera_sensor_get()) {
      return;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      ESP_LOGE("camera_light_sensor", "Camera fb get failed");
      return;
    }

    bool rgb_allocated = false;
    uint8_t* out_buf = fb->buf;
    if (fb->format != PIXFORMAT_RGB888) {
      rgb_allocated = true;
      out_buf = (uint8_t*)heap_caps_malloc(fb->width * fb->height * 3, MALLOC_CAP_SPIRAM);
      if (!out_buf) {
        ESP_LOGE("camera_light_sensor", "RGB memory allocation failed (PSRAM)");
        esp_camera_fb_return(fb);
        return;
      }
      bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
      if (!converted) {
        ESP_LOGE("camera_light_sensor", "Format to RGB888 failed");
        free(out_buf);
        esp_camera_fb_return(fb);
        return;
      }
    }

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
          // RGB888 order depends on the converter, usually R, G, B.
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

        float dist = sqrt(dr * dr + dg * dg + db * db);
        bool match = dist <= 50.0;

        s->set_latest_state(match);
      }
    }

    if (rgb_allocated) {
      free(out_buf);
    }
    esp_camera_fb_return(fb);
  }

 private:
  std::vector<CameraLightSensor*> sensors;
  uint16_t port = 8080;
  httpd_handle_t camera_httpd = NULL;
  TaskHandle_t task_handle = NULL;
  std::atomic<bool> data_ready{false};

  static esp_err_t capture_handler(httpd_req_t* req) {
    if (!esp_camera_sensor_get()) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      ESP_LOGE("camera_light_sensor", "Camera fb get failed for /snapshot");
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
};

}  // namespace camera_light_sensor
}  // namespace esphome
