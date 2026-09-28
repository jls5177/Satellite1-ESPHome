import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import microphone, speaker
from esphome.components import esp32, psram
from esphome.const import CONF_ID, CONF_URL, CONF_TRIGGER_ID

CODEOWNERS = ["@maxmaxme"]
DEPENDENCIES = ["network", "microphone", "speaker", "psram"]

CONF_MICROPHONE = "microphone"
CONF_MIC_CHANNEL = "mic_channel"
CONF_MIC_GAIN = "mic_gain"
CONF_SPEAKER = "speaker"
CONF_BARGE_IN = "barge_in"
CONF_BARGE_IN_HOLDOFF_MS = "barge_in_holdoff_ms"
CONF_ON_PHASE = "on_phase"
CONF_ON_REPEATED_FAILURE = "on_repeated_failure"
CONF_ON_FOLLOWUP_OPENED = "on_followup_opened"
CONF_ON_TIMER_STARTED = "on_timer_started"
CONF_ON_TIMER_FINISHED = "on_timer_finished"
CONF_ON_TIMER_CANCELLED = "on_timer_cancelled"
CONF_ON_TIMER_TICK = "on_timer_tick"
CONF_TOKEN = "token"
CONF_ON_ANNOUNCEMENT_REQUEST = "on_announcement_request"

va_client_ns = cg.esphome_ns.namespace("va_client")
VaClient = va_client_ns.class_("VaClient", cg.Component)
OnPhaseTrigger = va_client_ns.class_(
    "OnPhaseTrigger", automation.Trigger.template(cg.std_string)
)
OnRepeatedFailureTrigger = va_client_ns.class_(
    "OnRepeatedFailureTrigger", automation.Trigger.template()
)
OnFollowupOpenedTrigger = va_client_ns.class_(
    "OnFollowupOpenedTrigger", automation.Trigger.template()
)
OnTimerStartedTrigger = va_client_ns.class_(
    "OnTimerStartedTrigger", automation.Trigger.template()
)
OnTimerFinishedTrigger = va_client_ns.class_(
    "OnTimerFinishedTrigger", automation.Trigger.template(cg.std_string)
)
OnTimerCancelledTrigger = va_client_ns.class_(
    "OnTimerCancelledTrigger", automation.Trigger.template()
)
OnTimerTickTrigger = va_client_ns.class_(
    "OnTimerTickTrigger", automation.Trigger.template()
)
OnAnnouncementRequestTrigger = va_client_ns.class_(
    "OnAnnouncementRequestTrigger", automation.Trigger.template()
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VaClient),
        cv.Required(CONF_URL): cv.string,
        cv.Optional(CONF_TOKEN, default=""): cv.string,
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_MIC_CHANNEL, default=0): cv.int_range(min=0, max=1),
        cv.Optional(CONF_MIC_GAIN, default=1.0): cv.float_range(min=0.25, max=16.0),
        cv.Optional(CONF_BARGE_IN, default=True): cv.boolean,
        cv.Optional(CONF_BARGE_IN_HOLDOFF_MS, default=400): cv.int_range(
            min=0, max=2000
        ),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_ON_PHASE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnPhaseTrigger),
            }
        ),
        cv.Optional(CONF_ON_REPEATED_FAILURE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnRepeatedFailureTrigger),
            }
        ),
        cv.Optional(CONF_ON_FOLLOWUP_OPENED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnFollowupOpenedTrigger),
            }
        ),
        cv.Optional(CONF_ON_TIMER_STARTED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnTimerStartedTrigger)}
        ),
        cv.Optional(CONF_ON_TIMER_FINISHED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnTimerFinishedTrigger)}
        ),
        cv.Optional(CONF_ON_TIMER_CANCELLED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnTimerCancelledTrigger)}
        ),
        cv.Optional(CONF_ON_TIMER_TICK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnTimerTickTrigger)}
        ),
        cv.Optional(CONF_ON_ANNOUNCEMENT_REQUEST): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnAnnouncementRequestTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # esp-idf managed component providing esp_websocket_client.
    esp32.add_idf_component(
        name="espressif/esp_websocket_client",
        ref="1.7.0",
    )

    # va_client task stacks are allocated in PSRAM.
    psram.request_external_task_stack()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_mic_channel(config[CONF_MIC_CHANNEL]))
    cg.add(var.set_mic_gain(config[CONF_MIC_GAIN]))
    cg.add(var.set_barge_in(config[CONF_BARGE_IN]))
    cg.add(var.set_barge_in_holdoff_ms(config[CONF_BARGE_IN_HOLDOFF_MS]))

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))

    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))

    for conf in config.get(CONF_ON_PHASE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "phase")], conf)

    for conf in config.get(CONF_ON_REPEATED_FAILURE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_FOLLOWUP_OPENED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_ANNOUNCEMENT_REQUEST, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for key, args in (
        (CONF_ON_TIMER_STARTED, []),
        (CONF_ON_TIMER_FINISHED, [(cg.std_string, "name")]),
        (CONF_ON_TIMER_CANCELLED, []),
        (CONF_ON_TIMER_TICK, []),
    ):
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, args, conf)
