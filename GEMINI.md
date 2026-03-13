# ESPHome Camera Light Sensor - Core Guidelines

This project is a custom ESPHome component for the ESP32-CAM that performs
real-time color detection within specific Regions of Interest (ROI) using
HSV-based matching.

## Core Mandates

- **MANDATORY Documentation Updates:** `GEMINI.md` is a live document. You SHALL
  update it immediately upon making changes to architecture, configuration,
  standards, or workflows.
- **Empirical Verification:** After reading `GEMINI.md`, you SHALL read the
  relevant source code to verify the documentation's accuracy. The **source
  code** is the ultimate source of truth in all discrepancies.
- **Strict File Reading:** You SHALL always read the target file immediately
  before applying any edits to prevent reverting uncommitted user changes.
- **Atomic & Focused Commits:** Each commit SHALL be atomic, focused on a single
  logical change, and leave the project in a compilable state.
- **Imperative Commit Messages:** Use the imperative mood (e.g., "Add ROI
  validation"). You SHALL verify the commit message against the `git diff`
  before execution.

## Architectural Principles

- **Non-Blocking Execution:** All image processing MUST occur in the
  background FreeRTOS task (`camera_task`). The main ESPHome `loop()` SHALL
  only check for results and publish state changes.
- **Core Affinity:** The background task is pinned to **Core 1** to prevent
  interference with the main loop and Wi-Fi stack.
- **Thread Safety:** State communication between the background task and the
  main loop MUST use `std::atomic` variables.
- **Memory Management:** High-resolution frame buffers and RGB conversions MUST
  use **PSRAM** allocation via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
- **Power Efficiency:**
  - **Buffer Reuse:** A persistent RGB buffer in PSRAM MUST be used to avoid
    frequent allocation/deallocation overhead.
  - **Capture Frequency:** The background task frequency is automatically
    synchronized with the camera's `idle_framerate` via the mandatory
    `camera_id` link.

## Implementation Details

### Color Space & Matching
- **HSV Mapping:** RGB to HSV conversion maps Hue to **0-255** (0-360°).
- **Dual Input Support:** Expected color can be defined using either `expected_color` (RGB) or `expected_hsv` (HSV).
- **Circular Averaging:** You MUST use vector math (averaging `sin` and `cos`
  components) for Hue to correctly handle the 0/255 wraparound.
- **Weighted Euclidean Matching:** Color matching uses Euclidean distance in the HSV
  cylindrical space with configurable component weights.
- **Configurable Radius & Weights (Nested under `threshold`):**
  - `match_radius`: $\leq 50.0$ by default.
  - `hue_weight`: $3.0$ by default (stricter matching).
  - `saturation_weight`: $1.0$ by default.
  - `value_weight`: $1.0$ by default.
  - All parameters are configurable per sensor via YAML under the `threshold` key.
  - Detailed tuning examples are provided in `README.md`.

### Component Structure
- **Hub (`CameraLightSensorHub`):** Manages the camera, background task, and
  optional HTTP snapshot server.
- **Sensor (`CameraLightSensor`):** Inherits from
  `binary_sensor::BinarySensor`. Represents a single ROI.

## Coding Standards

- **Namespaces:** All C++ code MUST reside within `esphome::camera_light_sensor`.
- **Logging:** Use `ESP_LOG*` macros with the defined `TAG`. Minimize logging
  within the `process_camera()` loop.
- **Types:** Use fixed-width types (`uint8_t`, `uint32_t`) for hardware logic.

## Testing & Validation

1. **Compilation:** Verify changes via `esphome compile camera_light_sensor.yaml`.
2. **ROI Alignment:** Use the HTTP snapshot server (port 8080) for coordinates.
3. **Log Monitoring:** Use `DEBUG` or higher to observe sensor calculation times, and `VERY_VERBOSE` for detailed live HSV results.

## Common Pitfalls
- **Hub Configuration:** NEVER provide a `name` for the hub in YAML.
- **Resolution:** `1024x768` is the optimized default; increasing it SHALL be
  avoided unless absolutely necessary due to memory/CPU constraints.
