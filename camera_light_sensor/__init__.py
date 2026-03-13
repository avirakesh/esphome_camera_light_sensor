import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT, CONF_UPDATE_INTERVAL
from esphome.core import CORE

# Shared keys
CONF_CAMERA_LIGHT_SENSOR_ID = "camera_light_sensor_id"
CONF_CAMERA_ID = "camera_id"

# Define the C++ namespace for your component
camera_light_sensor_ns = cg.esphome_ns.namespace("camera_light_sensor")

DEPENDENCIES = ["esp32_camera"]

CameraLightSensorHub = camera_light_sensor_ns.class_(
    "CameraLightSensorHub", cg.PollingComponent
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CameraLightSensorHub),
        cv.Required(CONF_CAMERA_ID): cv.use_id(cg.EntityBase),
        cv.Optional(CONF_PORT): cv.port,
    }
).extend(cv.polling_component_schema("10min"))

async def to_code(config):
    hub_var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(hub_var, config)

    camera = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(hub_var.set_camera(camera))

    if CONF_PORT in config:
        cg.add(hub_var.set_port(config[CONF_PORT]))

    cg.add(hub_var.set_update_interval_ms(config[CONF_UPDATE_INTERVAL]))

    # Try to find the idle_update_interval from the camera component's config
    # to maintain the same frequency as the camera frame rate.
    full_config = CORE.config
    if "esp32_camera" in full_config:
        cam_conf = full_config["esp32_camera"]
        if isinstance(cam_conf, list):
            cam_block = next((c for c in cam_conf if c.get(CONF_ID) == config[CONF_CAMERA_ID]), None)
        else:
            cam_block = cam_conf if cam_conf.get(CONF_ID) == config[CONF_CAMERA_ID] else None

        if cam_block and "idle_framerate" in cam_block:
            idle_fps = cam_block["idle_framerate"]
            if idle_fps > 0:
                idle_ms = int(1000.0 / idle_fps)
                cg.add(hub_var.set_capture_interval_ms(idle_ms))
