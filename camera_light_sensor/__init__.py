import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT, CONF_UPDATE_INTERVAL

# Shared keys
CONF_CAMERA_LIGHT_SENSOR_ID = "camera_light_sensor_id"
CONF_SENSOR_REFRESH_RATE = "sensor_refresh_rate"

# Define the C++ namespace for your component
camera_light_sensor_ns = cg.esphome_ns.namespace("camera_light_sensor")

DEPENDENCIES = ["esp32_camera"]

CameraLightSensorHub = camera_light_sensor_ns.class_(
    "CameraLightSensorHub", cg.PollingComponent
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CameraLightSensorHub),
        cv.Optional(CONF_PORT): cv.port,
        cv.Optional(CONF_UPDATE_INTERVAL, default="500ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_SENSOR_REFRESH_RATE): cv.positive_time_period_milliseconds,
    }
).extend(cv.polling_component_schema("10s"))

async def to_code(config):
    hub_var = cg.new_Pvariable(config[cv.CONF_ID])
    await cg.register_component(hub_var, config)
    
    if CONF_PORT in config:
        cg.add(hub_var.set_port(config[CONF_PORT]))
    
    cg.add(hub_var.set_update_interval_ms(config[CONF_UPDATE_INTERVAL]))
    
    if CONF_SENSOR_REFRESH_RATE in config:
        cg.add(hub_var.set_sensor_refresh_rate_ms(config[CONF_SENSOR_REFRESH_RATE]))
