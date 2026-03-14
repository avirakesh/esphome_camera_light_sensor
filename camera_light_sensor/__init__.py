import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL
from esphome.core import CORE

# Shared keys
CONF_CAMERA_LIGHT_SENSOR_ID = "camera_light_sensor_id"
CONF_CAMERA_ID = "camera_id"

camera_light_sensor_ns = cg.esphome_ns.namespace("camera_light_sensor")

DEPENDENCIES = ["esp32_camera"]

CameraLightSensorHub = camera_light_sensor_ns.class_(
    "CameraLightSensorHub", cg.PollingComponent
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CameraLightSensorHub),
        cv.Required(CONF_CAMERA_ID): cv.use_id(cg.EntityBase),
    }
).extend(cv.polling_component_schema("10min"))

async def to_code(config):
    hub_var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(hub_var, config)

    camera = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(hub_var.set_camera(camera))

    cg.add(hub_var.set_update_interval_ms(config[CONF_UPDATE_INTERVAL]))

    # Get the idle_framerate from the esp32_camera config.
    # This is used as the sleep time. No point processing frames any faster.
    full_config = CORE.config
    if "esp32_camera" in full_config:
        cam_conf = full_config["esp32_camera"]
        if isinstance(cam_conf, list):
            # returns either the matching block or None if not found
            cam_block = next((c for c in cam_conf if c.get(CONF_ID) == config[CONF_CAMERA_ID]), None)
        else:
            cam_block = cam_conf if cam_conf.get(CONF_ID) == config[CONF_CAMERA_ID] else None

        if cam_block is None:
            raise cv.Invalid(
                "CameraLightSensorHub: No matching esp32_camera block found for "
                f"camera_id: {config[CONF_CAMERA_ID]}"
            )

        if "idle_framerate" not in cam_block:
            idle_fps = 0.1
        else:
            idle_fps = cam_block["idle_framerate"]

        if idle_fps <= 0:
            raise cv.Invalid(
                "CameraLightSensorHub: idle_framerate must be greater than 0. "
                f"Got {idle_fps} in camera block with id: {config[CONF_CAMERA_ID]}"
            )

        idle_ms = int(1000.0 / idle_fps)
        cg.add(hub_var.set_capture_interval_ms(idle_ms))
