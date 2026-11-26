import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from .. import (
    CONF_COMFORTNET_ID,
    CONF_SENSOR_KEY,
    CONF_TARGET_DEVICE_TYPE,
    COMFORTNET_CLIENT_SCHEMA,
    ComfortnetClient,
    comfortnet_ns,
)

DEPENDENCIES = ["comfortnet"]

ComfortnetTextSensor = comfortnet_ns.class_(
    "ComfortnetTextSensor", text_sensor.TextSensor, cg.Component, ComfortnetClient
)

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema()
    .extend({cv.GenerateID(): cv.declare_id(ComfortnetTextSensor)})
    .extend(COMFORTNET_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_COMFORTNET_ID])
    cg.add(var.set_comfortnet_parent(parent))
    cg.add(var.set_sensor_key(config[CONF_SENSOR_KEY]))
    cg.add(var.set_sensor_target_device_type(config[CONF_TARGET_DEVICE_TYPE]))
