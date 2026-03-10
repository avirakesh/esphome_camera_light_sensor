# ESPHome Camera Light Sensor

This component allows you to use an ESP32-CAM (or similar) to monitor specific
regions of its field of view and report whether they match a target color. It
is ideal for detecting status LEDs on appliances like stoves, washing machines,
or servers.

The component runs image processing in a background FreeRTOS task to ensure the
main ESPHome loop remains responsive.

## Configuration

### Hub Configuration

The `camera_light_sensor` hub coordinates the camera and the sensors.

```yaml
camera_light_sensor:
  # The ID for this hub, to be referenced by the binary sensors.
  id: camera_hub
  # Optional: If provided, starts a web server on this port.
  # Visiting http://<device_ip>:<port>/ serves a snapshot for ROI alignment.
  port: 8080
  # Optional: Heartbeat interval for all sensors. State changes are
  # pushed immediately regardless of this. Defaults to 10s.
  update_interval: 10s
  # Optional: The frequency at which the camera captures and analyzes a frame.
  # If set, the ESP32 enters Light Sleep between captures to save power.
  # If not provided, esp32 won't sleep and run the camera at 2fps.
  sensor_refresh_rate: 5s
  ```

  - **`id`** (Required, ID): The ID for this hub, to be referenced by the binary
  sensors.
  - **`port`** (Optional, Port): If provided, starts a web server on this port.
  Visiting `http://<device_ip>:<port>/` will serve a JPEG snapshot from the
  camera. Useful for aligning the regions of interest (ROI).
  - **`update_interval`** (Optional, Time): The heartbeat interval for syncing
  state with Home Assistant. Note: State changes are still pushed immediately
  upon detection if the device is awake. Defaults to `10s`.
  - **`sensor_refresh_rate`** (Optional, Time): The interval at which the camera
  captures and analyzes a frame.
  - If **provided**: The ESP32 will enter **Light Sleep** between captures to
    save power. This is the recommended way to achieve low-power operation.
  - If **not provided**: The capture frequency defaults to `update_interval`
    (or `500ms` if `update_interval` is longer), and the device remains fully
    powered (no sleep).
  - **Note:** Do not provide a `name` for the hub, as it is a coordinator and
  should not be exposed as a separate entity in Home Assistant.
### Binary Sensor Configuration

Define one or more binary sensors to monitor specific areas of the image.

```yaml
binary_sensor:
  - platform: camera_light_sensor
    # The ID of the hub configured above.
    camera_light_sensor_id: camera_hub
    # A list of regions to monitor.
    sensor_roi:
      - name: "Stove On LED"
        # The Region of Interest coordinates [x1, y1, x2, y2].
        box: [927, 292, 930, 295]
        # The target color in RGB [R, G, B].
        expected_color: [255, 0, 0]
```

- **`camera_light_sensor_id`** (Required, ID): The ID of the hub configured
  above.
- **`sensor_roi`** (Required, List): A list of regions to monitor.
  - **`name`** (Required, String): The name of the binary sensor.
  - **`box`** (Required, List of 4 ints): The Region of Interest coordinates
    `[x1, y1, x2, y2]`.
  - **`expected_color`** (Required, List of 3 ints): The target color in RGB
    `[R, G, B]`. This is internally converted to HSV for robust matching.

## How it Works

1. **Background Task:** The hub spawns a FreeRTOS task that captures a camera
   frame every 500ms.
2. **HSV Conversion:** Each pixel in the ROI is converted from RGB to the HSV
   (Hue, Saturation, Value) colorspace.
3. **Circular Averaging:** The Hue values are averaged using vector math
   (sin/cos) to correctly handle the 0/360° wraparound.
4. **Matching:** The average color of the region is compared against the target
   color. A match is found if:
    - The Hue is within ~20 degrees.
    - The Saturation and Value are within reasonable tolerances.
5. **Immediate Push:** If a state change is detected, it is pushed to Home
   Assistant immediately from the main execution loop.

## Calibration Tip

Enable the `port` option during setup. Open the snapshot in a browser and use an
image editor or online tool to find the exact pixel coordinates `[x1, y1, x2,
y2]` for the LED you want to monitor. Once calibrated, you can remove the
`port` configuration to save resources.
