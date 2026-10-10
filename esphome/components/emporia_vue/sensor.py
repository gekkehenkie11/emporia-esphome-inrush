from esphome.components import sensor, i2c
from esphome import automation
import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.const import (
    CONF_CALIBRATION,
    CONF_CURRENT,
    CONF_ID,
    CONF_INPUT,
    CONF_POWER,
    CONF_TRIGGER_ID,
    CONF_VOLTAGE,
    CONF_FREQUENCY,
    CONF_PHASE_ANGLE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_PERCENT,
    UNIT_SECOND,
    UNIT_WATT,
    UNIT_VOLT,
    UNIT_HERTZ,
    UNIT_DEGREES,
)

CONF_CT_CLAMPS = "ct_clamps"
CONF_PHASES = "phases"
CONF_PHASE_ID = "phase_id"
CONF_VARIANT = "variant"

CONF_ON_UPDATE = "on_update"

CONF_CYCLE_CAPTURE = "cycle_capture"
CONF_POLL_INTERVAL = "poll_interval"
CONF_LIVE_INTERVAL = "live_interval"
CONF_TRIGGER_CURRENT = "trigger_current"
CONF_REARM_CURRENT = "rearm_current"
CONF_PRE_TRIGGER = "pre_trigger"
CONF_POST_TRIGGER = "post_trigger"
CONF_SETTLE_FACTOR = "settle_factor"
CONF_MAINS_CURRENT_MULTIPLIER = "mains_current_multiplier"
CONF_AUX_CURRENT_MULTIPLIER = "aux_current_multiplier"
CONF_AUX_INPUTS = "aux_inputs"
CONF_LOG_TRACE = "log_trace"
CONF_ON_START_CAPTURED = "on_start_captured"
CONF_FRAME_RATE = "frame_rate"

CODEOWNERS = ["@flaviut", "@Maelstrom96", "@krconv"]
ESP_PLATFORMS = ["esp-idf"]
DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor"]

emporia_vue_ns = cg.esphome_ns.namespace("emporia_vue")
EmporiaVueComponent = emporia_vue_ns.class_(
    "EmporiaVueComponent", cg.PollingComponent, i2c.I2CDevice
)
PhaseConfig = emporia_vue_ns.class_("PhaseConfig")
CTClampConfig = emporia_vue_ns.class_("CTClampConfig")

# Trigger for after statistics sensors are updated
EmporiaVueUpdateTrigger = emporia_vue_ns.class_(
    "EmporiaVueUpdateTrigger", automation.Trigger.template()
)
CycleCaptureConfig = emporia_vue_ns.class_("CycleCaptureConfig")
EmporiaVueStartCapturedTrigger = emporia_vue_ns.class_(
    "EmporiaVueStartCapturedTrigger",
    automation.Trigger.template(cg.std_string, cg.std_string),
)


PhaseInputWire = emporia_vue_ns.enum("PhaseInputWire")
PHASE_INPUT = {
    "BLACK": PhaseInputWire.BLACK,
    "RED": PhaseInputWire.RED,
    "BLUE": PhaseInputWire.BLUE,
}

CTInputPort = emporia_vue_ns.enum("CTInputPort")
CT_INPUT = {
    "A": CTInputPort.A,
    "B": CTInputPort.B,
    "C": CTInputPort.C,
    "1": CTInputPort.ONE,
    "2": CTInputPort.TWO,
    "3": CTInputPort.THREE,
    "4": CTInputPort.FOUR,
    "5": CTInputPort.FIVE,
    "6": CTInputPort.SIX,
    "7": CTInputPort.SEVEN,
    "8": CTInputPort.EIGHT,
    "9": CTInputPort.NINE,
    "10": CTInputPort.TEN,
    "11": CTInputPort.ELEVEN,
    "12": CTInputPort.TWELVE,
    "13": CTInputPort.THIRTEEN,
    "14": CTInputPort.FOURTEEN,
    "15": CTInputPort.FIFTEEN,
    "16": CTInputPort.SIXTEEN,
}

SCHEMA_CT_CLAMP = {
    cv.GenerateID(): cv.declare_id(CTClampConfig),
    cv.Required(CONF_PHASE_ID): cv.use_id(PhaseConfig),
    cv.Required(CONF_INPUT): cv.enum(CT_INPUT),
    cv.Optional(CONF_POWER): sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT,
        device_class=DEVICE_CLASS_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    cv.Optional(CONF_CURRENT): sensor.sensor_schema(
        unit_of_measurement=UNIT_AMPERE,
        device_class=DEVICE_CLASS_CURRENT,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=2,
    ),
}


def validate_phases(val):
    base_validated = cv.Schema(
        cv.ensure_list(
            {
                cv.Required(CONF_ID): cv.declare_id(PhaseConfig),
                cv.Required(CONF_INPUT): cv.enum(PHASE_INPUT),
                cv.Optional(CONF_CALIBRATION, default=0.022): cv.zero_to_one_float,
                cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
                    unit_of_measurement=UNIT_VOLT,
                    device_class=DEVICE_CLASS_VOLTAGE,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=1,
                ),
                cv.Optional(CONF_FREQUENCY): sensor.sensor_schema(
                    unit_of_measurement=UNIT_HERTZ,
                    device_class=DEVICE_CLASS_FREQUENCY,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=1,
                ),
                cv.Optional(CONF_PHASE_ANGLE): sensor.sensor_schema(
                    unit_of_measurement=UNIT_DEGREES,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=0,
                ),
            }
        )
    )(val)

    if len(base_validated) > 3:
        raise cv.Invalid("No more than 3 phases are supported")

    inputs = [phase[CONF_INPUT] for phase in base_validated]
    if len(inputs) != len(set(inputs)):
        raise cv.Invalid("Only one entry per input color is allowed")

    for i, phase in enumerate(base_validated):
        input_wire = phase[CONF_INPUT]
        if input_wire == "BLACK":
            if CONF_PHASE_ANGLE in phase:
                raise cv.Invalid(
                    "Phase angle is not supported for the black wire, only for the "
                    "red and blue wires",
                    path=[i, CONF_PHASE_ANGLE],
                )
        elif input_wire in {"RED", "BLUE"}:
            if CONF_FREQUENCY in phase:
                raise cv.Invalid(
                    "Frequency is not supported for the red and blue wires, only for "
                    "the black wire",
                    path=[i, CONF_FREQUENCY],
                )

    return base_validated


def _current_sensor(decimals=1):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_AMPERE,
        device_class=DEVICE_CLASS_CURRENT,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=decimals,
    )


def _voltage_sensor():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    )


def _frequency_sensor():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_HERTZ,
        device_class=DEVICE_CLASS_FREQUENCY,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=2,
    )


# key -> (setter, schema factory)
CYCLE_CAPTURE_SENSORS = {
    "max_current": ("set_max_current_sensor", _current_sensor),
    "min_voltage": ("set_min_voltage_sensor", _voltage_sensor),
    "min_frequency": ("set_min_frequency_sensor", _frequency_sensor),
    "max_frequency": ("set_max_frequency_sensor", _frequency_sensor),
    "start_peak_current": ("set_start_peak_current_sensor", _current_sensor),
    "start_inrush_100ms": ("set_start_inrush_100ms_sensor", _current_sensor),
    "start_peak_instant": ("set_start_peak_instant_sensor", _current_sensor),
    "start_running_current": ("set_start_running_current_sensor", _current_sensor),
    "start_duration": (
        "set_start_duration_sensor",
        lambda: sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            device_class=DEVICE_CLASS_DURATION,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=2,
        ),
    ),
    "start_voltage_min": ("set_start_voltage_min_sensor", _voltage_sensor),
    "start_voltage_sag": (
        "set_start_voltage_sag_sensor",
        lambda: sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=1,
        ),
    ),
    "start_clipped": (
        "set_start_clipped_sensor",
        lambda: sensor.sensor_schema(accuracy_decimals=0),
    ),
    "start_count": (
        "set_start_count_sensor",
        lambda: sensor.sensor_schema(
            state_class=STATE_CLASS_TOTAL_INCREASING, accuracy_decimals=0
        ),
    ),
}

CYCLE_CAPTURE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CycleCaptureConfig),
        cv.Optional(CONF_POLL_INTERVAL, default="50ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(max=cv.TimePeriod(milliseconds=400)),
        ),
        cv.Optional(CONF_LIVE_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_TRIGGER_CURRENT, default=100.0): cv.positive_float,
        cv.Optional(CONF_REARM_CURRENT): cv.positive_float,
        cv.Optional(CONF_PRE_TRIGGER, default="500ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_POST_TRIGGER, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_SETTLE_FACTOR, default=1.3): cv.float_range(min=1.0, max=5.0),
        cv.Optional(CONF_MAINS_CURRENT_MULTIPLIER, default=1.0): cv.positive_float,
        cv.Optional(CONF_AUX_CURRENT_MULTIPLIER, default=1.0): cv.positive_float,
        cv.Optional(CONF_AUX_INPUTS, default=[]): cv.All(
            cv.ensure_list(cv.int_range(min=1, max=16)), cv.Length(max=2)
        ),
        cv.Optional(CONF_LOG_TRACE, default=True): cv.boolean,
        # SAMD09 ADC frames per second. Leave out to measure it against the ESP32 clock.
        cv.Optional(CONF_FRAME_RATE): cv.float_range(min=20000, max=32000),
        cv.Optional(CONF_ON_START_CAPTURED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                    EmporiaVueStartCapturedTrigger
                ),
            }
        ),
        **{cv.Optional(key): factory() for key, (_, factory) in CYCLE_CAPTURE_SENSORS.items()},
    }
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmporiaVueComponent),
            cv.Required(CONF_PHASES): validate_phases,
            cv.Required(CONF_CT_CLAMPS): cv.ensure_list(SCHEMA_CT_CLAMP),
            cv.Optional(CONF_VARIANT, default="vue2"): cv.one_of("vue2", "vue3", lower=True),
            cv.Optional(CONF_CYCLE_CAPTURE): CYCLE_CAPTURE_SCHEMA,
            cv.Optional(CONF_ON_UPDATE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        EmporiaVueUpdateTrigger
                    ),
                }
            ),
        },
    )
    .extend(cv.polling_component_schema("200ms"))
    .extend(i2c.i2c_device_schema(0x64)),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if config.get(CONF_VARIANT, "vue2") == "vue3":
        cg.add_define("EMPORIA_VUE_VARIANT_VUE3")
    else:
        cg.add_define("EMPORIA_VUE_VARIANT_VUE2")

    phases = []
    for phase_config in config[CONF_PHASES]:
        phase_var = cg.new_Pvariable(phase_config[CONF_ID], PhaseConfig())
        cg.add(phase_var.set_input_wire(phase_config[CONF_INPUT]))
        cg.add(phase_var.set_calibration(phase_config[CONF_CALIBRATION]))

        if CONF_VOLTAGE in phase_config:
            voltage_sensor = await sensor.new_sensor(phase_config[CONF_VOLTAGE])
            cg.add(phase_var.set_voltage_sensor(voltage_sensor))

        if CONF_FREQUENCY in phase_config:
            frequency_sensor = await sensor.new_sensor(phase_config[CONF_FREQUENCY])
            cg.add(phase_var.set_frequency_sensor(frequency_sensor))

        if CONF_PHASE_ANGLE in phase_config:
            phase_angle_sensor = await sensor.new_sensor(phase_config[CONF_PHASE_ANGLE])
            cg.add(phase_var.set_phase_angle_sensor(phase_angle_sensor))

        phases.append(phase_var)
    cg.add(var.set_phases(phases))

    ct_clamps = []
    for ct_config in config[CONF_CT_CLAMPS]:
        ct_clamp_var = cg.new_Pvariable(ct_config[CONF_ID], CTClampConfig())
        phase_var = await cg.get_variable(ct_config[CONF_PHASE_ID])
        cg.add(ct_clamp_var.set_phase(phase_var))
        cg.add(ct_clamp_var.set_input_port(ct_config[CONF_INPUT]))

        if CONF_POWER in ct_config:
            power_sensor = await sensor.new_sensor(ct_config[CONF_POWER])
            cg.add(ct_clamp_var.set_power_sensor(power_sensor))

        if CONF_CURRENT in ct_config:
            current_sensor = await sensor.new_sensor(ct_config[CONF_CURRENT])
            cg.add(ct_clamp_var.set_current_sensor(current_sensor))

        ct_clamps.append(ct_clamp_var)
    cg.add(var.set_ct_clamps(ct_clamps))

    for trigger_conf in config.get(CONF_ON_UPDATE, []):
        trigger = cg.new_Pvariable(trigger_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], trigger_conf )

    if CONF_CYCLE_CAPTURE in config:
        cc_conf = config[CONF_CYCLE_CAPTURE]
        cc = cg.new_Pvariable(cc_conf[CONF_ID])
        cg.add(cc.set_poll_interval(cc_conf[CONF_POLL_INTERVAL]))
        cg.add(cc.set_live_interval(cc_conf[CONF_LIVE_INTERVAL]))
        cg.add(cc.set_trigger_current(cc_conf[CONF_TRIGGER_CURRENT]))
        if CONF_REARM_CURRENT in cc_conf:
            cg.add(cc.set_rearm_current(cc_conf[CONF_REARM_CURRENT]))
        cg.add(cc.set_pre_trigger(cc_conf[CONF_PRE_TRIGGER]))
        cg.add(cc.set_post_trigger(cc_conf[CONF_POST_TRIGGER]))
        cg.add(cc.set_settle_factor(cc_conf[CONF_SETTLE_FACTOR]))
        cg.add(cc.set_mains_current_multiplier(cc_conf[CONF_MAINS_CURRENT_MULTIPLIER]))
        cg.add(cc.set_aux_current_multiplier(cc_conf[CONF_AUX_CURRENT_MULTIPLIER]))
        for i, aux in enumerate(cc_conf[CONF_AUX_INPUTS]):
            # input "1".."16" is index 3..18 in the SAMD09 packet
            cg.add(cc.set_aux_input(i, aux + 2))
        cg.add(cc.set_log_trace(cc_conf[CONF_LOG_TRACE]))
        if CONF_FRAME_RATE in cc_conf:
            cg.add(cc.set_frame_rate(cc_conf[CONF_FRAME_RATE]))
        for key, (setter, _) in CYCLE_CAPTURE_SENSORS.items():
            if key in cc_conf:
                sens = await sensor.new_sensor(cc_conf[key])
                cg.add(getattr(cc, setter)(sens))
        for trigger_conf in cc_conf.get(CONF_ON_START_CAPTURED, []):
            trigger = cg.new_Pvariable(trigger_conf[CONF_TRIGGER_ID], cc)
            await automation.build_automation(
                trigger,
                [(cg.std_string, "summary"), (cg.std_string, "trace")],
                trigger_conf,
            )
        cg.add(var.set_cycle_capture(cc))
