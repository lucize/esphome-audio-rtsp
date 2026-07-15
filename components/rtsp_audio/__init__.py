import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@local"]
DEPENDENCIES = ["wifi"]

rtsp_audio_ns = cg.esphome_ns.namespace("rtsp_audio")
RTSPAudioComponent = rtsp_audio_ns.class_("RTSPAudioComponent", cg.Component)

CONF_BCLK_PIN = "bclk_pin"
CONF_LRCLK_PIN = "lrclk_pin"
CONF_DIN_PIN = "din_pin"
CONF_I2S_BCLK_PIN = "i2s_bclk_pin"
CONF_I2S_LRCLK_PIN = "i2s_lrclk_pin"
CONF_I2S_DIN_PIN = "i2s_din_pin"
CONF_PORT = "port"
CONF_SAMPLE_RATE = "sample_rate"
CONF_CHANNEL = "channel"
CONF_I2S_PORT = "i2s_port"
CONF_I2S_BITS_PER_SAMPLE = "i2s_bits_per_sample"
CONF_BITS_PER_SAMPLE = "bits_per_sample"
CONF_SAMPLE_SHIFT = "sample_shift"
CONF_GAIN = "gain"
CONF_USE_APLL = "use_apll"
CONF_RTP_PAYLOAD_TYPE = "rtp_payload_type"
CONF_PACKET_MS = "packet_ms"
CONF_DEBUG = "debug"
CONF_STATUS_INTERVAL = "status_interval"
CONF_ADC_TYPE = "adc_type"
CONF_PDM = "pdm"
CONF_USE_STEREO_SLOT = "use_stereo_slot"


def _bits(value):
    if isinstance(value, str):
        value = value.lower().replace(" ", "")
        if value.endswith("bit"):
            value = value[:-3]
    return cv.one_of(16, 24, 32, int=True)(value)


def _validate(config):
    # Allow either the original short names or ESPHome-like i2s_audio/microphone names.
    if CONF_BCLK_PIN not in config and CONF_I2S_BCLK_PIN not in config:
        raise cv.Invalid("Either bclk_pin or i2s_bclk_pin is required")
    if CONF_LRCLK_PIN not in config and CONF_I2S_LRCLK_PIN not in config:
        raise cv.Invalid("Either lrclk_pin or i2s_lrclk_pin is required")
    if CONF_DIN_PIN not in config and CONF_I2S_DIN_PIN not in config:
        raise cv.Invalid("Either din_pin or i2s_din_pin is required")
    if CONF_BCLK_PIN in config and CONF_I2S_BCLK_PIN in config:
        raise cv.Invalid("Use only one of bclk_pin or i2s_bclk_pin")
    if CONF_LRCLK_PIN in config and CONF_I2S_LRCLK_PIN in config:
        raise cv.Invalid("Use only one of lrclk_pin or i2s_lrclk_pin")
    if CONF_DIN_PIN in config and CONF_I2S_DIN_PIN in config:
        raise cv.Invalid("Use only one of din_pin or i2s_din_pin")
    if CONF_I2S_BITS_PER_SAMPLE in config and CONF_BITS_PER_SAMPLE in config:
        raise cv.Invalid("Use only one of i2s_bits_per_sample or bits_per_sample")
    if config.get(CONF_PDM, False):
        raise cv.Invalid("pdm: true is not supported by this minimal RTSP component; use pdm: false for INMP441")
    if config.get(CONF_ADC_TYPE, "external") != "external":
        raise cv.Invalid("Only adc_type: external is supported")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RTSPAudioComponent),
            cv.Optional(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_DIN_PIN): pins.internal_gpio_input_pin_number,
            # ESPHome-like aliases, so the block can look close to i2s_audio + microphone YAML.
            cv.Optional(CONF_I2S_BCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_I2S_LRCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_I2S_DIN_PIN): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_ADC_TYPE, default="external"): cv.one_of("external", lower=True),
            cv.Optional(CONF_PDM, default=False): cv.boolean,
            cv.Optional(CONF_PORT, default=8554): cv.port,
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=8000, max=48000),
            cv.Optional(CONF_CHANNEL, default="left"): cv.one_of("left", "right", lower=True),
            cv.Optional(CONF_I2S_PORT, default=0): cv.int_range(min=0, max=1),
            cv.Optional(CONF_I2S_BITS_PER_SAMPLE): _bits,
            cv.Optional(CONF_BITS_PER_SAMPLE): _bits,
            cv.Optional(CONF_SAMPLE_SHIFT, default=14): cv.int_range(min=0, max=24),
            cv.Optional(CONF_GAIN, default=1.0): cv.float_range(min=0.01, max=64.0),
            cv.Optional(CONF_USE_APLL, default=False): cv.boolean,
            cv.Optional(CONF_RTP_PAYLOAD_TYPE, default=96): cv.int_range(min=96, max=127),
            cv.Optional(CONF_PACKET_MS, default=20): cv.int_range(min=10, max=100),
            cv.Optional(CONF_USE_STEREO_SLOT, default=False): cv.boolean,
            cv.Optional(CONF_DEBUG, default=False): cv.boolean,
            cv.Optional(CONF_STATUS_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


def _validate_framework(config):
    if not CORE.is_esp32:
        raise cv.Invalid("rtsp_audio native component currently supports ESP32 only")
    # ESPHome 2026.6 does not expose CORE.using_esp_idf. The safest check is to reject Arduino-only builds.
    if getattr(CORE, "using_arduino", False):
        raise cv.Invalid("rtsp_audio native component requires esp32.framework.type: esp-idf, not arduino")
    return config


FINAL_VALIDATE_SCHEMA = _validate_framework


async def to_code(config):
    # ESPHome 2026.2+ excludes many built-in ESP-IDF components by default.
    # The ESP-IDF 5 I2S STD driver lives in esp_driver_i2s, otherwise
    # #include "driver/i2s_std.h" fails with "No such file or directory".
    try:
        from esphome.components.esp32 import include_builtin_idf_component
        include_builtin_idf_component("esp_driver_i2s")
    except ImportError:
        # Older ESPHome versions included all IDF components by default.
        pass

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    bclk = config.get(CONF_BCLK_PIN, config.get(CONF_I2S_BCLK_PIN))
    lrclk = config.get(CONF_LRCLK_PIN, config.get(CONF_I2S_LRCLK_PIN))
    din = config.get(CONF_DIN_PIN, config.get(CONF_I2S_DIN_PIN))
    bits = config.get(CONF_I2S_BITS_PER_SAMPLE, config.get(CONF_BITS_PER_SAMPLE, 32))

    cg.add(var.set_i2s_pins(bclk, lrclk, din))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_audio_config(config[CONF_SAMPLE_RATE], config[CONF_CHANNEL] == "right"))
    cg.add(var.set_i2s_config(config[CONF_I2S_PORT], bits, config[CONF_SAMPLE_SHIFT], config[CONF_USE_APLL]))
    cg.add(var.set_use_stereo_slot(config[CONF_USE_STEREO_SLOT]))
    cg.add(var.set_rtp_config(config[CONF_RTP_PAYLOAD_TYPE], config[CONF_PACKET_MS]))
    cg.add(var.set_gain(config[CONF_GAIN]))
    cg.add(var.set_debug(config[CONF_DEBUG]))
    cg.add(var.set_status_interval(config[CONF_STATUS_INTERVAL].total_milliseconds))
