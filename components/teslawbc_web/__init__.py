"""TeslaWBC web page and REST API under /api/.

Serves the Status, Settings and Setup wizard page on / and a JSON API under
/api/, protected by a device password. Keeps the settings that must be
changeable without a firmware build (device password, MQTT broker and
login, Wall Connector address) in flash.
"""

import gzip
from pathlib import Path

import esphome.codegen as cg
from esphome.components import binary_sensor, button, number, select, sensor, text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@R3stFu1"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["web_server_base", "json", "sha256"]

CONF_MODE = "mode"
CONF_TARGET = "target"
CONF_HEARTBEAT = "heartbeat"
CONF_SETTINGS = "settings"
CONF_SENSORS = "sensors"
CONF_BINARY_SENSORS = "binary_sensors"
CONF_TEXT_SENSORS = "text_sensors"
CONF_DEFAULT_MQTT_PORT = "default_mqtt_port"

teslawbc_web_ns = cg.esphome_ns.namespace("teslawbc_web")
TeslaWbcWeb = teslawbc_web_ns.class_("TeslaWbcWeb", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TeslaWbcWeb),
        cv.Required(CONF_MODE): cv.use_id(select.Select),
        cv.Required(CONF_TARGET): cv.use_id(number.Number),
        cv.Required(CONF_HEARTBEAT): cv.use_id(button.Button),
        cv.Optional(CONF_SETTINGS, default=[]): cv.ensure_list(cv.use_id(number.Number)),
        cv.Optional(CONF_SENSORS, default=[]): cv.ensure_list(cv.use_id(sensor.Sensor)),
        cv.Optional(CONF_BINARY_SENSORS, default=[]): cv.ensure_list(
            cv.use_id(binary_sensor.BinarySensor)
        ),
        cv.Optional(CONF_TEXT_SENSORS, default=[]): cv.ensure_list(
            cv.use_id(text_sensor.TextSensor)
        ),
        cv.Optional(CONF_DEFAULT_MQTT_PORT, default=1883): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


def _page_array() -> str:
    """The web page, gzipped, as a C array."""
    html = (Path(__file__).parent / "index.html").read_bytes()
    data = gzip.compress(html, compresslevel=9, mtime=0)
    body = ",".join(str(b) for b in data)
    return (
        f"extern const uint8_t TESLAWBC_INDEX_HTML[] = {{{body}}};\n"
        f"extern const size_t TESLAWBC_INDEX_HTML_SIZE = {len(data)};"
    )


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_global(cg.RawStatement(_page_array()))

    cg.add(var.set_mode(await cg.get_variable(config[CONF_MODE])))
    cg.add(var.set_target(await cg.get_variable(config[CONF_TARGET])))
    cg.add(var.set_heartbeat(await cg.get_variable(config[CONF_HEARTBEAT])))
    for conf in config[CONF_SETTINGS]:
        cg.add(var.add_setting(await cg.get_variable(conf)))
    for conf in config[CONF_SENSORS]:
        cg.add(var.add_sensor(await cg.get_variable(conf)))
    for conf in config[CONF_BINARY_SENSORS]:
        cg.add(var.add_binary_sensor(await cg.get_variable(conf)))
    for conf in config[CONF_TEXT_SENSORS]:
        cg.add(var.add_text_sensor(await cg.get_variable(conf)))
    cg.add(var.set_default_mqtt_port(config[CONF_DEFAULT_MQTT_PORT]))
    if "mqtt" in CORE.config:
        cg.add_define("TESLAWBC_USE_MQTT")
