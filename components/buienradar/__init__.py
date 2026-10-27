"""Buienradar: the ESPHome side. smart_display.register_plugin() reads the id, version, tile memory and the screen's
texts from the manifest and translations/ beside this folder (docs/FIRMWARE_API.md). The two selects of plugin.yaml are
the top bar item's settings; this component reads them."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select, smart_display
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display"]

CONF_BAR_WHEN = "bar_when"
CONF_BAR_WORDS = "bar_words"

buienradar_ns = cg.esphome_ns.namespace("buienradar")
Buienradar = buienradar_ns.class_("Buienradar", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Buienradar),
        cv.Optional(CONF_BAR_WHEN): cv.use_id(select.Select),
        cv.Optional(CONF_BAR_WORDS): cv.use_id(select.Select),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    if CONF_BAR_WHEN in config:
        cg.add(var.set_bar_when(await cg.get_variable(config[CONF_BAR_WHEN])))
    if CONF_BAR_WORDS in config:
        cg.add(var.set_bar_words(await cg.get_variable(config[CONF_BAR_WORDS])))
