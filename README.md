# ESPHome Camera Light Sensor

This component allows you to use an ESP32-CAM (or similar) to monitor specific
regions of its field of view and report whether they match a target color. It
is ideal for detecting status LEDs on appliances like stoves, or servers, or
whatever else can be detected by changing colors.

## Configuration

The component is broken into two parts:
1. `binary_sensor`: This is a list of binary sensors that should be exposed.
2. `camera_light_sensor`: This is the coordinator (or hub) responsible for the
    business logic of converting camera frames to sensor values.

### Camera Light Sensor Configuration

The `camera_light_sensor` hub coordinates the camera and the sensors.

```yaml
camera_light_sensor:
  # The ID for this hub, to be referenced by the binary sensors.
  id: camera_hub
  # The ID of the camera component to use.
  camera_id: my_camera
  # Optional: Heartbeat interval for all sensors. State changes are
  # pushed immediately but this serves as a backup. Defaults to 10s.
  update_interval: 10s
```

  - **`id`** (Required, ID): The ID for this hub, to be referenced by the binary
    sensors.
  - **`camera_id`** (Required, ID): The ID of the `esp32_camera` component. The
    hub automatically synchronizes its capture frequency with the camera's
    `idle_framerate`.
  - **`update_interval`** (Optional, Time): The heartbeat interval for syncing
    state with Home Assistant. Note: State changes are pushed immediately
    upon detection. This is simply a fail safe. Defaults to `10min`.
  - **Note:** Do not provide a `name` for the hub, as it is meant to coordinate
    the sensors and should not be exposed as a separate entity in Home
    Assistant.

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
        # Define the target color using EITHER RGB or HSV.
        # Regardless of input, matching ALWAYS occurs in the HSV color space.
        expected_color: [255, 0, 0] # RGB [R, G, B]
        # OR
        # expected_hsv: [0, 255, 255] # HSV [H, S, V] (Hue mapped to 0-255)

        # Optional: Custom thresholds for color matching.
        threshold:
          # This is the Euclidean distance in the HSV cone.
          match_radius: 50.0       # Default: 50.0
          # Weights for matching components.
          # Higher weights make the component more strict.
          hue_weight: 3.0          # Default: 3.0
          saturation_weight: 1.0   # Default: 1.0
          value_weight: 1.0        # Default: 1.0
```

- **`camera_light_sensor_id`** (Required, ID): The ID of the hub configured
  above.
- **`sensor_roi`** (Required, List): A list of regions to monitor.
  - **`name`** (Required, String): The name of the binary sensor.
  - **`box`** (Required, List of 4 ints): The Region of Interest coordinates
    `[x1, y1, x2, y2]`.
  - **`expected_color`** (Exclusive, List of 3 ints): The target color in RGB
    `[R, G, B]`. This is internally converted to HSV for matching.
  - **`expected_hsv`** (Exclusive, List of 3 ints): The target color in HSV
    `[H, S, V]`. Useful for matching values observed in debug logs.
  - **Note:** All matching logic is performed in the **HSV color space**.
    Providing RGB is a convenience; it is converted to HSV during
    initialization.
  - **`threshold`** (Optional, Object): Matching tolerance settings.
    - **`match_radius`** (Optional, float): The matching tolerance as a radius
      in the HSV cylindrical space. Defaults to `50.0`.
    - **`hue_weight`** (Optional, float): Weight for Hue strictness.
      Defaults to `3.0`.
    - **`saturation_weight`** (Optional, float): Weight for Saturation
      strictness. Defaults to `1.0`.
    - **`value_weight`** (Optional, float): Weight for Value strictness.
      Defaults to `1.0`.

## Weighting HSV Components

The component uses a weighted Euclidean distance in the HSV cylindrical space
to determine if the captured color matches the target. Adjusting the weights
allows you to tune the sensor's sensitivity to different aspects of the color.

### The Matching Formula

The distance $d$ between the captured color $(H_1, S_1, V_1)$ and the target
color $(H_2, S_2, V_2)$ is calculated as:

$$d = \sqrt{w_v(V_1 - V_2)^2 + w_s(S_1 - S_2)^2 + w_h(2S_1S_2(1 - \cos(H_1 - H_2)))}$$

Where:
- $w_h, w_s, w_v$ are the configured weights for Hue, Saturation, and Value.
- $H$ is the Hue angle (converted from the 0-255 range to 0-2π).
- A match is confirmed if $d \leq \text{match\_radius}$.

### Calculation Examples

Assume a target of **Pure Red** (HSV: 0, 255, 255) and default weights
($w_h=3, w_s=1, w_v=1$):

1.  **Slightly Dimmer Red** (HSV: 0, 255, 200):
    - $V$ difference is 55. $d = \sqrt{1 \cdot (55)^2} = 55.0$.
    - With a `match_radius` of 50, this is a **No Match**.
    - *Tuning Fix:* Lower `value_weight` to 0.1 → $d = \sqrt{0.1 \cdot 3025}
      \approx 17.4$ (**Match**).

2.  **Slightly Orange Shift** (HSV: 10, 255, 255):
    - $H$ difference is ~14°. $d = \sqrt{3 \cdot (2 \cdot 255^2 \cdot (1 -
      \cos(14^\circ)))} \approx 108.5$.
    - This is a **No Match**.
    - *Tuning Fix:* Increase `match_radius` to 110 OR lower `hue_weight` to 0.5.

- **`hue_weight`**: Controls sensitivity to color changes (e.g., Red vs.
  Orange). A higher weight makes the sensor more selective about the exact
  color hue.
- **`saturation_weight`**: Controls sensitivity to color purity (e.g., Red vs.
  Pink/White). Useful for distinguishing a colored LED from white ambient light.
- **`value_weight`**: Controls sensitivity to brightness. Lowering this weight
  makes the sensor more robust against shadows or changes in ambient lighting
  that affect brightness but not the color itself.

### Example Scenarios

| Scenario | Recommended Tuning | Why? |
| :--- | :--- | :--- |
| **Differentiating Red vs. Orange** | High `hue_weight` (e.g., 5.0) | Ensures the sensor only triggers for the exact hue, even if brightness is similar. |
| **Variable Ambient Lighting** | Low `value_weight` (e.g., 0.2) | Makes the sensor ignore changes in brightness caused by external light or shadows. |
| **Faint LED in Bright Room** | High `saturation_weight` (e.g., 3.0) | Helps distinguish the saturated color of the LED from the desaturated white/grey background. |
| **Night Monitoring** | Balanced `value_weight` | In very dark environments, the "Value" (brightness) is often the most reliable indicator of an LED being ON. |

## How it Works

1. **Background Task:** The hub spawns a FreeRTOS task that captures and
   analyzes a camera frame at the frequency defined by the camera's
   `idle_framerate`.
2. **HSV Conversion:** Each pixel in the ROI is converted from RGB to the HSV
   (Hue, Saturation, Value) colorspace.
3. **Circular Averaging:** The Hue values are averaged using vector math to
   correctly handle the 0/360° wraparound.
4. **Matching:** The average color of the region is compared against the target
   color using a weighted Euclidean distance in the HSV cylindrical space. A
   match is found if the distance is less than or equal to the `match_radius`.
   The Hue component is weighted more heavily by default to ensure precise color
   matching.
5. **Immediate Push:** If a state change is detected, it is pushed to Home
   Assistant immediately from the main execution loop.

## Calibration Tip

To figure out the ROI bounds, simply give the `esp32_camera` component a name
and Home Assistant should show you a live camera stream. This is resource
intensive though, so remember to remove the name from `esp32_camera` to hide
it from Home Assistant again.

---

**Disclaimer:** This project was developed and refined with the assistance of AI.
