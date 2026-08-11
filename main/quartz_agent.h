/**
 * quartz_agent.h — Autonomous Device Agent
 *
 * Gives the ESP32 a deterministic rule engine so it can act as an
 * autonomous economic agent: respond to on-chain events, sensor
 * readings, and timers with payments, relay triggers, and messages.
 *
 * "A thermostat with a wallet."
 *
 * Rules are owner-defined trigger→action pairs stored in NVS.
 * The device evaluates rules in a periodic loop and executes
 * matching actions autonomously.
 *
 * No LLM. No cloud. No server. The chip IS the backend.
 */

#ifndef QUARTZ_AGENT_H
#define QUARTZ_AGENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Limits === */

#define QZ_AGENT_MAX_RULES          16    /* max rules stored in NVS */
#define QZ_AGENT_MAX_CONDITIONS      4     /* max conditions per rule (AND'd) */
#define QZ_AGENT_MAX_ACTIONS         3     /* max actions per rule */
#define QZ_AGENT_LABEL_MAX           32
#define QZ_AGENT_MSG_MAX             160
#define QZ_AGENT_ADDR_MAX            65

/* === Trigger Sources === */

typedef enum {
    QZ_TRIGGER_NONE = 0,
    QZ_TRIGGER_PAYMENT_RECEIVED,   /* QZ received at our address */
    QZ_TRIGGER_BALANCE_ABOVE,      /* balance exceeds threshold */
    QZ_TRIGGER_BALANCE_BELOW,      /* balance drops below threshold */
    QZ_TRIGGER_BLOCK_HEIGHT,       /* specific block height reached */
    QZ_TRIGGER_MESSAGE_RECEIVED,   /* on-chain message received */
    QZ_TRIGGER_SENSOR_ABOVE,       /* sensor reading above threshold */
    QZ_TRIGGER_SENSOR_BELOW,       /* sensor reading below threshold */
    QZ_TRIGGER_TIMER_INTERVAL,     /* every N seconds */
    QZ_TRIGGER_GOVERNANCE_PROPOSAL,/* governance proposal affecting quarry */
} qz_trigger_type_t;

/* === Sensor IDs (maps to ADC/GPIO) === */

typedef enum {
    QZ_SENSOR_NONE = 0,
    QZ_SENSOR_VIBRATION_ADC,    /* ADC1_CH0 (GPIO36) — piezo/vibration */
    QZ_SENSOR_TEMP_INTERNAL,    /* ESP32 internal temperature sensor */
    QZ_SENSOR_LIGHT_ADC,        /* ADC1_CH3 (GPIO39) — photoresistor */
    QZ_SENSOR_VOLTAGE_ADC,      /* ADC1_CH7 (GPIO35) — battery/solar voltage */
    QZ_SENSOR_CURRENT_ADC,      /* ADC1_CH4 (GPIO32) — current sensor */
    QZ_SENSOR_GPIO_DIGITAL,     /* any GPIO — digital 0/1 */
} qz_sensor_id_t;

/* === Action Types === */

typedef enum {
    QZ_ACTION_NONE = 0,
    QZ_ACTION_PAY,              /* send QZ to an address */
    QZ_ACTION_TRIGGER_RELAY,    /* pulse GPIO relay */
    QZ_ACTION_SEND_MESSAGE,     /* broadcast on-chain message */
    QZ_ACTION_SET_MINING_RATE,  /* adjust mining intensity (0-100%) */
    QZ_ACTION_WITHDRAW_QUARRY,  /* trigger quarry withdrawal to owner address */
    QZ_ACTION_DISPLAY_ALERT,    /* show alert on screen */
    QZ_ACTION_STOP_MINING,      /* halt mining (emergency) */
    QZ_ACTION_RESTART_MINING,   /* resume mining after stop */
} qz_action_type_t;

/* === Condition (part of a rule's trigger) === */

typedef struct {
    qz_trigger_type_t type;
    qz_sensor_id_t    sensor;       /* for SENSOR_* triggers */
    uint32_t          threshold;    /* for balance/sensor/block triggers */
    uint32_t          interval_s;   /* for TIMER_INTERVAL (seconds) */
} qz_condition_t;

/* === Action === */

typedef struct {
    qz_action_type_t type;
    uint64_t         amount_satoshis;  /* for PAY (in quartz-sats) */
    char             address[QZ_AGENT_ADDR_MAX]; /* for PAY/WITHDRAW */
    uint32_t         relay_duration_ms;           /* for TRIGGER_RELAY */
    char             message[QZ_AGENT_MSG_MAX];   /* for SEND_MESSAGE/DISPLAY_ALERT */
    uint8_t          mining_rate_pct;             /* for SET_MINING_RATE (0-100) */
} qz_action_t;

/* === Rule === */

typedef struct {
    bool             enabled;
    char             label[QZ_AGENT_LABEL_MAX];   /* human-readable name */
    qz_condition_t   conditions[QZ_AGENT_MAX_CONDITIONS];
    int              condition_count;             /* conditions are AND'd */
    qz_action_t      actions[QZ_AGENT_MAX_ACTIONS];
    int              action_count;
    /* Runtime state */
    uint32_t         last_fired_sec;   /* last time this rule triggered */
    uint32_t         fire_count;       /* total times fired */
    uint32_t         cooldown_sec;     /* min seconds between fires */
} qz_rule_t;

/* === Agent State === */

typedef struct {
    bool       initialized;
    bool       mining_active;
    int        rule_count;
    qz_rule_t  rules[QZ_AGENT_MAX_RULES];
    uint32_t   last_eval_sec;     /* last loop evaluation time */
    uint32_t   eval_count;        /* total evaluations performed */
    /* Cached sensor readings */
    uint32_t   sensor_vibration;
    int8_t     sensor_temp_c;
    uint32_t   sensor_light;
    uint32_t   sensor_voltage_mv;
    uint32_t   sensor_current_ma;
    int        sensor_gpio_val;
    /* Cached chain state */
    uint64_t   balance_satoshis;
    uint32_t   block_height;
    uint32_t   last_payment_time;
    char       last_message_from[32];
    char       last_message_text[QZ_AGENT_MSG_MAX];
} qz_agent_state_t;

/* === API === */

/**
 * Initialize the agent subsystem.
 * Loads rules from NVS (or defaults if first boot).
 * Sets up sensor GPIO.
 */
int quartz_agent_init(const char *wallet_address);

/**
 * Main agent loop — call periodically from the mining task.
 * Evaluates all rules against current sensor + chain state.
 * Executes matching actions.
 *
 * @param delta_sec  Seconds since last call
 */
void quartz_agent_step(uint32_t delta_sec);

/**
 * Load rules from NVS.
 */
int quartz_agent_load_rules(void);

/**
 * Save rules to NVS.
 */
int quartz_agent_save_rules(void);

/**
 * Add a rule. Returns rule index or -1 on failure.
 */
int quartz_agent_add_rule(const qz_rule_t *rule);

/**
 * Remove a rule by index.
 */
int quartz_agent_remove_rule(int index);

/**
 * Get rule by index (or NULL).
 */
const qz_rule_t *quartz_agent_get_rule(int index);

/**
 * Get current agent state (for display/debug).
 */
const qz_agent_state_t *quartz_agent_get_state(void);

/**
 * Update cached sensor readings.
 * Called automatically by agent_step, or manually for testing.
 */
void quartz_agent_read_sensors(void);

/**
 * Update cached chain state from node.
 * Called automatically by agent_step when WiFi is connected.
 */
void quartz_agent_sync_chain(void);

/**
 * Execute a single action directly (bypass rules).
 * Useful for testing and manual triggers.
 */
int quartz_agent_execute_action(const qz_action_t *action);

/**
 * Check if a rule's conditions are met.
 * Returns true if all conditions are satisfied.
 */
bool quartz_agent_check_conditions(const qz_rule_t *rule);

/**
 * Get a human-readable description of a rule.
 * For display on screen or serial output.
 */
int quartz_agent_rule_describe(const qz_rule_t *rule, char *buf, int buf_len);

/**
 * Set default rules (called on first boot if no rules in NVS).
 * Ships with: auto-withdraw at 15% cap, low-voltage alert.
 */
void quartz_agent_set_defaults(void);

/* === LLM-backed agent === */

/**
 * Poll the LLM endpoint for an autonomous decision.
 * Sends device state to /api/v1/agent/decide, receives action.
 * Called automatically by the agent loop at the configured interval.
 *
 * @return 0 on success (action executed), -1 on error/no action
 */
int quartz_agent_llm_poll(void);

/**
 * Set the LLM polling interval (seconds).
 * Default: 60s. Set to 0 to disable LLM polling.
 */
void quartz_agent_llm_set_interval(uint32_t seconds);

/**
 * Check if LLM agent mode is enabled.
 */
bool quartz_agent_llm_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* QUARTZ_AGENT_H */
