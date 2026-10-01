import esphome.codegen as cg
from esphome.components import climate_ir
import esphome.config_validation as cv

AUTO_LOAD = ["climate_ir"]
CODEOWNERS = ["@defl"]

CONF_FAHRENHEIT_DISPLAY = "fahrenheit_display"

midea_b2_ns = cg.esphome_ns.namespace("midea_b2")
MideaB2Climate = midea_b2_ns.class_("MideaB2Climate", climate_ir.ClimateIR)

CONFIG_SCHEMA = climate_ir.climate_ir_with_receiver_schema(MideaB2Climate).extend(
    {
        # What the unit's own display shows. The wire is Celsius either way; this only sets
        # the unit bit in the trailer frame.
        cv.Optional(CONF_FAHRENHEIT_DISPLAY, default=True): cv.boolean,
    }
)


async def to_code(config):
    var = await climate_ir.new_climate_ir(config)
    cg.add(var.set_fahrenheit_display(config[CONF_FAHRENHEIT_DISPLAY]))
