import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from . import camera_light_sensor_ns, CONF_CAMERA_LIGHT_SENSOR_ID, CameraLightSensorHub

CONF_SENSOR_ROI = "sensor_roi"
CONF_ROI_NAME = "name"
CONF_ROI_BOX = "box"
CONF_ROI_COLOR = "expected_color"

CameraLightSensor = camera_light_sensor_ns.class_(
    "CameraLightSensor", binary_sensor.BinarySensor
)

ROI_ITEM_SCHEMA = binary_sensor.binary_sensor_schema(CameraLightSensor).extend(
    {
        cv.Required(CONF_ROI_NAME): cv.string,
        cv.Required(CONF_ROI_BOX): cv.All(
            cv.ensure_list(cv.uint32_t), cv.Length(min=4, max=4)
        ),
        cv.Required(CONF_ROI_COLOR): cv.All(
            cv.ensure_list(cv.uint8_t), cv.Length(min=3, max=3)
        ),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_CAMERA_LIGHT_SENSOR_ID): cv.use_id(CameraLightSensorHub),
        cv.Required(CONF_SENSOR_ROI): cv.ensure_list(ROI_ITEM_SCHEMA),
    }
)

async def to_code(config):
    hub_var = await cg.get_variable(config[CONF_CAMERA_LIGHT_SENSOR_ID])

    for item in config[CONF_SENSOR_ROI]:
        sensor_var = await binary_sensor.new_binary_sensor(item)

        cg.add(
            sensor_var.set_sensor_info(
                item[CONF_ROI_NAME], item[CONF_ROI_BOX], item[CONF_ROI_COLOR]
            )
        )

        cg.add(hub_var.add_sensor(sensor_var))
