import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID
from . import camera_light_sensor_ns

CameraLightSensor = camera_light_sensor_ns.class_(
    "CameraLightSensor", cg.PollingComponent, sensor.Sensor
)

CONFIG_SCHEMA = sensor.sensor_schema(CameraLightSensor).extend(
    cv.polling_component_schema("60s")
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
