import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from . import camera_light_sensor_ns, CONF_CAMERA_LIGHT_SENSOR_ID, CameraLightSensorHub

CONF_SENSOR_ROI = "sensor_roi"
CONF_ROI_NAME = "name"
CONF_ROI_BOX = "box"
CONF_ROI_COLOR = "expected_color"
CONF_ROI_HSV = "expected_hsv"
CONF_THRESHOLD = "threshold"
CONF_MATCH_RADIUS = "match_radius"
CONF_HUE_WEIGHT = "hue_weight"
CONF_SATURATION_WEIGHT = "saturation_weight"
CONF_VALUE_WEIGHT = "value_weight"

CameraLightSensor = camera_light_sensor_ns.class_(
    "CameraLightSensor", binary_sensor.BinarySensor
)

ROI_ITEM_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(CameraLightSensor).extend(
        {
            cv.Required(CONF_ROI_NAME): cv.string,
            cv.Required(CONF_ROI_BOX): cv.All(
                cv.ensure_list(cv.uint32_t), cv.Length(min=4, max=4)
            ),
            cv.Optional(CONF_ROI_COLOR): cv.All(
                cv.ensure_list(cv.uint8_t), cv.Length(min=3, max=3)
            ),
            cv.Optional(CONF_ROI_HSV): cv.All(
                cv.ensure_list(cv.uint8_t), cv.Length(min=3, max=3)
            ),
            cv.Optional(CONF_THRESHOLD, default={}): cv.Schema(
                {
                    cv.Optional(CONF_MATCH_RADIUS, default=50.0): cv.float_,
                    cv.Optional(CONF_HUE_WEIGHT, default=3.0): cv.float_,
                    cv.Optional(CONF_SATURATION_WEIGHT, default=1.0): cv.float_,
                    cv.Optional(CONF_VALUE_WEIGHT, default=1.0): cv.float_,
                }
            ),
        }
    ),
    cv.has_exactly_one_key(CONF_ROI_COLOR, CONF_ROI_HSV),
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
                item[CONF_ROI_NAME], item[CONF_ROI_BOX]
            )
        )

        if CONF_ROI_COLOR in item:
            cg.add(sensor_var.set_expected_rgb(*item[CONF_ROI_COLOR]))
        else:
            cg.add(sensor_var.set_expected_hsv(*item[CONF_ROI_HSV]))

        threshold = item[CONF_THRESHOLD]
        cg.add(sensor_var.set_match_radius(threshold[CONF_MATCH_RADIUS]))
        cg.add(
            sensor_var.set_weights(
                threshold[CONF_HUE_WEIGHT],
                threshold[CONF_SATURATION_WEIGHT],
                threshold[CONF_VALUE_WEIGHT],
            )
        )

        cg.add(hub_var.add_sensor(sensor_var))
