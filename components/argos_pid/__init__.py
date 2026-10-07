import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_KILOGRAM,
    UNIT_KILOWATT,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_SECOND,
)

CODEOWNERS = []
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]

argos_pid_ns = cg.esphome_ns.namespace("argos_pid")
ArgosPidComponent = argos_pid_ns.class_("ArgosPidComponent", cg.Component)

CONF_BIND_IP = "bind_ip"
CONF_EXPECTED_DEVICE_IP = "expected_device_ip"
CONF_ONLINE_TIMEOUT = "online_timeout"
CONF_RESPONSE_A35 = "response_a35"
CONF_RESPONSE_A36 = "response_a36"
CONF_RESPONSE_A37 = "response_a37"
CONF_ONLINE = "online"

# v0.3 proxy / diagnostics
CONF_MODE = "mode"
CONF_UPSTREAM_HOST = "upstream_host"
CONF_UPSTREAM_IP = "upstream_ip"
CONF_UPSTREAM_PORT = "upstream_port"
CONF_PROXY_TIMEOUT = "proxy_timeout"
CONF_PROXY_FALLBACK_LOCAL = "proxy_fallback_local"
CONF_CLOUD_ONLINE = "cloud_online"
CONF_CLOUD_LATENCY_MS = "cloud_latency_ms"
CONF_CLOUD_STATUS_CODE = "cloud_status_code"
CONF_CLOUD_RESPONSE_FIELDS = "cloud_response_fields"
CONF_PROXY_REQUEST_COUNT = "proxy_request_count"
CONF_CLOUD_RESPONSE_TYPE = "cloud_response_type"
CONF_CLOUD_LAST_SUMMARY = "cloud_last_summary"
CONF_RAW_LAST_CHANGE = "raw_last_change"
CONF_ALARM_ACTIVE = "alarm_active"
CONF_ALARM_CODE = "alarm_code"
CONF_ALARM_MESSAGE = "alarm_message"

# Telemetria
CONF_TEMPERATURE_BOILER = "temperature_boiler"
CONF_TEMPERATURE_DHW = "temperature_dhw"
CONF_TEMPERATURE_FEEDER = "temperature_feeder"
CONF_TEMPERATURE_VALVE = "temperature_valve"
CONF_FUEL_KG = "fuel_kg"
CONF_POWER_PERCENT = "power_percent"
CONF_EXHAUST_TEMPERATURE_PROVISIONAL = "exhaust_temperature_provisional"
CONF_CURRENT_POWER_KW_PROVISIONAL = "current_power_kw_provisional"

# Kocioł / palnik / podtrzymanie
CONF_BOILER_NIGHT_SETPOINT = "boiler_night_setpoint"
CONF_BOILER_DAY_SETPOINT = "boiler_day_setpoint"
CONF_BURNER_FEED_TIME = "burner_feed_time"
CONF_BURNER_PAUSE_TIME = "burner_pause_time"
CONF_BURNER_FAN_MAX = "burner_fan_max"
CONF_BURNER_FAN_MIN = "burner_fan_min"
CONF_BURNER_POWER_MIN = "burner_power_min"
CONF_HOLD_WORK_TIME = "hold_work_time"
CONF_HOLD_PAUSE_TIME = "hold_pause_time"
CONF_HOLD_FAN = "hold_fan"
CONF_HOLD_FEED_EVERY_CYCLES = "hold_feed_every_cycles"
CONF_HOLD_FEED_MULTIPLIER = "hold_feed_multiplier"

# Pompy / CWU / zawór
CONF_CO_PUMP_MODE = "co_pump_mode"
CONF_CO_PUMP_START_TEMP = "co_pump_start_temp"
CONF_CO_PUMP_HYSTERESIS = "co_pump_hysteresis"
CONF_DHW_MODE = "dhw_mode"
CONF_DHW_SETPOINT = "dhw_setpoint"
CONF_DHW_HYSTERESIS = "dhw_hysteresis"
CONF_DHW_PRIORITY = "dhw_priority"
CONF_CIRCULATION_MODE = "circulation_mode"
CONF_CIRCULATION_CO_TEMP = "circulation_co_temp"
CONF_CIRCULATION_DHW_TEMP = "circulation_dhw_temp"
CONF_CIRCULATION_HYSTERESIS = "circulation_hysteresis"
CONF_CIRCULATION_WORK_MIN = "circulation_work_min"
CONF_CIRCULATION_PAUSE_MIN = "circulation_pause_min"
CONF_VALVE_MODE = "valve_mode"
CONF_VALVE_DAY_SETPOINT = "valve_day_setpoint"
CONF_VALVE_NIGHT_SETPOINT = "valve_night_setpoint"


def temp_sensor_schema(decimals=1):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=decimals,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def numeric_sensor_schema(unit=None, decimals=0):
    kwargs = {"accuracy_decimals": decimals, "state_class": STATE_CLASS_MEASUREMENT}
    if unit is not None:
        kwargs["unit_of_measurement"] = unit
    return sensor.sensor_schema(**kwargs)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ArgosPidComponent),
        cv.Optional(CONF_BIND_IP, default="10.10.15.1"): cv.string_strict,
        cv.Optional(CONF_EXPECTED_DEVICE_IP, default="10.10.15.11"): cv.string_strict,
        cv.Optional(CONF_ONLINE_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RESPONSE_A35, default="8"): cv.string_strict,
        cv.Optional(CONF_RESPONSE_A36, default="0"): cv.string_strict,
        cv.Optional(CONF_RESPONSE_A37, default="48"): cv.string_strict,
        cv.Optional(CONF_MODE, default="local"): cv.one_of("local", "proxy", lower=True),
        cv.Optional(CONF_UPSTREAM_HOST, default="www.ekotlownia.pl"): cv.string_strict,
        cv.Optional(CONF_UPSTREAM_IP, default="109.95.148.70"): cv.string_strict,
        cv.Optional(CONF_UPSTREAM_PORT, default=80): cv.port,
        cv.Optional(CONF_PROXY_TIMEOUT, default="2500ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PROXY_FALLBACK_LOCAL, default=True): cv.boolean,
        cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_CONNECTIVITY),
        cv.Optional(CONF_CLOUD_ONLINE): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_CONNECTIVITY),
        cv.Optional(CONF_CLOUD_RESPONSE_TYPE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CLOUD_LAST_SUMMARY): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_RAW_LAST_CHANGE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_ALARM_ACTIVE): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
        cv.Optional(CONF_ALARM_CODE): sensor.sensor_schema(accuracy_decimals=0),
        cv.Optional(CONF_ALARM_MESSAGE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CLOUD_LATENCY_MS): numeric_sensor_schema("ms", 0),
        cv.Optional(CONF_CLOUD_STATUS_CODE): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_CLOUD_RESPONSE_FIELDS): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_PROXY_REQUEST_COUNT): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_TEMPERATURE_BOILER): temp_sensor_schema(1),
        cv.Optional(CONF_TEMPERATURE_DHW): temp_sensor_schema(1),
        cv.Optional(CONF_TEMPERATURE_FEEDER): temp_sensor_schema(1),
        cv.Optional(CONF_TEMPERATURE_VALVE): temp_sensor_schema(1),
        cv.Optional(CONF_FUEL_KG): numeric_sensor_schema(UNIT_KILOGRAM, 0),
        cv.Optional(CONF_POWER_PERCENT): numeric_sensor_schema(UNIT_PERCENT, 0),
        cv.Optional(CONF_EXHAUST_TEMPERATURE_PROVISIONAL): temp_sensor_schema(1),
        cv.Optional(CONF_CURRENT_POWER_KW_PROVISIONAL): sensor.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BOILER_NIGHT_SETPOINT): temp_sensor_schema(0),
        cv.Optional(CONF_BOILER_DAY_SETPOINT): temp_sensor_schema(0),
        cv.Optional(CONF_BURNER_FEED_TIME): numeric_sensor_schema(UNIT_SECOND, 0),
        cv.Optional(CONF_BURNER_PAUSE_TIME): numeric_sensor_schema(UNIT_SECOND, 0),
        cv.Optional(CONF_BURNER_FAN_MAX): numeric_sensor_schema(UNIT_PERCENT, 0),
        cv.Optional(CONF_BURNER_FAN_MIN): numeric_sensor_schema(UNIT_PERCENT, 0),
        cv.Optional(CONF_BURNER_POWER_MIN): numeric_sensor_schema(UNIT_PERCENT, 0),
        cv.Optional(CONF_HOLD_WORK_TIME): numeric_sensor_schema(UNIT_SECOND, 0),
        cv.Optional(CONF_HOLD_PAUSE_TIME): numeric_sensor_schema(UNIT_SECOND, 0),
        cv.Optional(CONF_HOLD_FAN): numeric_sensor_schema(UNIT_PERCENT, 0),
        cv.Optional(CONF_HOLD_FEED_EVERY_CYCLES): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_HOLD_FEED_MULTIPLIER): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_CO_PUMP_MODE): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_CO_PUMP_START_TEMP): temp_sensor_schema(0),
        cv.Optional(CONF_CO_PUMP_HYSTERESIS): temp_sensor_schema(0),
        cv.Optional(CONF_DHW_MODE): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_DHW_SETPOINT): temp_sensor_schema(0),
        cv.Optional(CONF_DHW_HYSTERESIS): temp_sensor_schema(0),
        cv.Optional(CONF_DHW_PRIORITY): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_CIRCULATION_MODE): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_CIRCULATION_CO_TEMP): temp_sensor_schema(0),
        cv.Optional(CONF_CIRCULATION_DHW_TEMP): temp_sensor_schema(0),
        cv.Optional(CONF_CIRCULATION_HYSTERESIS): temp_sensor_schema(0),
        cv.Optional(CONF_CIRCULATION_WORK_MIN): numeric_sensor_schema(UNIT_MINUTE, 0),
        cv.Optional(CONF_CIRCULATION_PAUSE_MIN): numeric_sensor_schema(UNIT_MINUTE, 0),
        cv.Optional(CONF_VALVE_MODE): numeric_sensor_schema(None, 0),
        cv.Optional(CONF_VALVE_DAY_SETPOINT): temp_sensor_schema(0),
        cv.Optional(CONF_VALVE_NIGHT_SETPOINT): temp_sensor_schema(0),
    }
).extend(cv.COMPONENT_SCHEMA)


_SENSOR_SETTERS = {
    CONF_TEMPERATURE_BOILER: "set_temperature_boiler_sensor",
    CONF_TEMPERATURE_DHW: "set_temperature_dhw_sensor",
    CONF_TEMPERATURE_FEEDER: "set_temperature_feeder_sensor",
    CONF_TEMPERATURE_VALVE: "set_temperature_valve_sensor",
    CONF_FUEL_KG: "set_fuel_kg_sensor",
    CONF_POWER_PERCENT: "set_power_percent_sensor",
    CONF_EXHAUST_TEMPERATURE_PROVISIONAL: "set_exhaust_temperature_provisional_sensor",
    CONF_CURRENT_POWER_KW_PROVISIONAL: "set_current_power_kw_provisional_sensor",
    CONF_BOILER_NIGHT_SETPOINT: "set_boiler_night_setpoint_sensor",
    CONF_BOILER_DAY_SETPOINT: "set_boiler_day_setpoint_sensor",
    CONF_BURNER_FEED_TIME: "set_burner_feed_time_sensor",
    CONF_BURNER_PAUSE_TIME: "set_burner_pause_time_sensor",
    CONF_BURNER_FAN_MAX: "set_burner_fan_max_sensor",
    CONF_BURNER_FAN_MIN: "set_burner_fan_min_sensor",
    CONF_BURNER_POWER_MIN: "set_burner_power_min_sensor",
    CONF_HOLD_WORK_TIME: "set_hold_work_time_sensor",
    CONF_HOLD_PAUSE_TIME: "set_hold_pause_time_sensor",
    CONF_HOLD_FAN: "set_hold_fan_sensor",
    CONF_HOLD_FEED_EVERY_CYCLES: "set_hold_feed_every_cycles_sensor",
    CONF_HOLD_FEED_MULTIPLIER: "set_hold_feed_multiplier_sensor",
    CONF_CO_PUMP_MODE: "set_co_pump_mode_sensor",
    CONF_CO_PUMP_START_TEMP: "set_co_pump_start_temp_sensor",
    CONF_CO_PUMP_HYSTERESIS: "set_co_pump_hysteresis_sensor",
    CONF_DHW_MODE: "set_dhw_mode_sensor",
    CONF_DHW_SETPOINT: "set_dhw_setpoint_sensor",
    CONF_DHW_HYSTERESIS: "set_dhw_hysteresis_sensor",
    CONF_CIRCULATION_MODE: "set_circulation_mode_sensor",
    CONF_CIRCULATION_CO_TEMP: "set_circulation_co_temp_sensor",
    CONF_CIRCULATION_DHW_TEMP: "set_circulation_dhw_temp_sensor",
    CONF_CIRCULATION_HYSTERESIS: "set_circulation_hysteresis_sensor",
    CONF_CIRCULATION_WORK_MIN: "set_circulation_work_min_sensor",
    CONF_CIRCULATION_PAUSE_MIN: "set_circulation_pause_min_sensor",
    CONF_VALVE_MODE: "set_valve_mode_sensor",
    CONF_VALVE_DAY_SETPOINT: "set_valve_day_setpoint_sensor",
    CONF_VALVE_NIGHT_SETPOINT: "set_valve_night_setpoint_sensor",
    CONF_CLOUD_LATENCY_MS: "set_cloud_latency_ms_sensor",
    CONF_CLOUD_STATUS_CODE: "set_cloud_status_code_sensor",
    CONF_CLOUD_RESPONSE_FIELDS: "set_cloud_response_fields_sensor",
    CONF_PROXY_REQUEST_COUNT: "set_proxy_request_count_sensor",
    CONF_ALARM_CODE: "set_alarm_code_sensor",
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_bind_ip(config[CONF_BIND_IP]))
    cg.add(var.set_expected_device_ip(config[CONF_EXPECTED_DEVICE_IP]))
    cg.add(var.set_online_timeout_ms(config[CONF_ONLINE_TIMEOUT].total_milliseconds))
    cg.add(var.set_response_a35(config[CONF_RESPONSE_A35]))
    cg.add(var.set_response_a36(config[CONF_RESPONSE_A36]))
    cg.add(var.set_response_a37(config[CONF_RESPONSE_A37]))
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_upstream_host(config[CONF_UPSTREAM_HOST]))
    cg.add(var.set_upstream_ip(config[CONF_UPSTREAM_IP]))
    cg.add(var.set_upstream_port(config[CONF_UPSTREAM_PORT]))
    cg.add(var.set_proxy_timeout_ms(config[CONF_PROXY_TIMEOUT].total_milliseconds))
    cg.add(var.set_proxy_fallback_local(config[CONF_PROXY_FALLBACK_LOCAL]))

    if CONF_ONLINE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_ONLINE])
        cg.add(var.set_online_binary_sensor(bs))
    if CONF_CLOUD_ONLINE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_CLOUD_ONLINE])
        cg.add(var.set_cloud_online_binary_sensor(bs))
    if CONF_DHW_PRIORITY in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_DHW_PRIORITY])
        cg.add(var.set_dhw_priority_binary_sensor(bs))
    if CONF_CLOUD_RESPONSE_TYPE in config:
        ts = await text_sensor.new_text_sensor(config[CONF_CLOUD_RESPONSE_TYPE])
        cg.add(var.set_cloud_response_type_text_sensor(ts))
    if CONF_CLOUD_LAST_SUMMARY in config:
        ts = await text_sensor.new_text_sensor(config[CONF_CLOUD_LAST_SUMMARY])
        cg.add(var.set_cloud_last_summary_text_sensor(ts))
    if CONF_RAW_LAST_CHANGE in config:
        ts = await text_sensor.new_text_sensor(config[CONF_RAW_LAST_CHANGE])
        cg.add(var.set_raw_last_change_text_sensor(ts))
    if CONF_ALARM_ACTIVE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_ALARM_ACTIVE])
        cg.add(var.set_alarm_active_binary_sensor(bs))
    if CONF_ALARM_MESSAGE in config:
        ts = await text_sensor.new_text_sensor(config[CONF_ALARM_MESSAGE])
        cg.add(var.set_alarm_message_text_sensor(ts))

    for key, setter_name in _SENSOR_SETTERS.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter_name)(sens))
