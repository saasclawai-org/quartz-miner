/**
 * quartz_agent.c — Autonomous Device Agent Implementation
 *
 * A deterministic rule engine that lets the ESP32 act as an autonomous
 * economic agent. Rules are trigger→action pairs evaluated in a loop.
 *
 * This is NOT artificial intelligence. It's a thermostat with a wallet.
 * The novelty is that the thermostat has a PUF identity, mining income,
 * and physical actuation — making it a self-governing economic entity
 * embedded in real infrastructure.
 */

#include "quartz_agent.h"
#include "quartz_pay.h"
#include "quartz_wifi.h"
#include "quartz_display.h"
#include "quartz.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#define ESP_LOGD(tag, fmt, ...)
#define vTaskDelay(x)
#endif

static const char *TAG = "AGENT";

/* NVS namespace for agent rules */
#define QZ_AGENT_NVS_NAMESPACE "qz_agent"
#define QZ_AGENT_NVS_KEY       "rules"
#define QZ_AGENT_NVS_VERSION   2

/* === Sensor GPIO mapping === */
#define QZ_SENSOR_VIBRATION_ADC_CHANNEL  ADC1_CHANNEL_0  /* GPIO36 - input only */
#define QZ_SENSOR_LIGHT_ADC_CHANNEL      ADC1_CHANNEL_3  /* GPIO39 - input only */
#define QZ_SENSOR_VOLTAGE_ADC_CHANNEL    ADC1_CHANNEL_7  /* GPIO35 - input only */
#define QZ_SENSOR_CURRENT_ADC_CHANNEL    ADC1_CHANNEL_6  /* GPIO34 - input only */
#define QZ_SENSOR_DIGITAL_GPIO           36              /* GPIO36 also digital (safe, input-only) */

/* === Static state === */
static qz_agent_state_t s_state;
static char s_wallet_addr[QZ_AGENT_ADDR_MAX];

/* === Forward declarations === */
static int  load_rules_nvs(void);
static int  save_rules_nvs(void);
static void read_sensors_hw(void);
static void sync_chain_state(void);
static int  execute_action_hw(const qz_action_t *action);
static bool check_condition(const qz_condition_t *cond);
static uint32_t now_sec(void);

/*=========================================================================
 * Public API
 *=======================================================================*/

int quartz_agent_init(const char *wallet_address) {
    memset(&s_state, 0, sizeof(s_state));

    if (wallet_address) {
        strncpy(s_wallet_addr, wallet_address, sizeof(s_wallet_addr) - 1);
    }

#ifdef ESP_PLATFORM
    /* Configure ADC for sensors */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(QZ_SENSOR_VIBRATION_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(QZ_SENSOR_LIGHT_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(QZ_SENSOR_VOLTAGE_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(QZ_SENSOR_CURRENT_ADC_CHANNEL, ADC_ATTEN_DB_11);

    /* Configure digital GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << QZ_SENSOR_DIGITAL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
#endif

    /* Load rules from NVS */
    int rc = load_rules_nvs();
    if (rc != 0 || s_state.rule_count == 0) {
        ESP_LOGI(TAG, "No rules in NVS — setting defaults");
        quartz_agent_set_defaults();
        save_rules_nvs();
    }

    s_state.initialized = true;
    s_state.mining_active = true;

    ESP_LOGI(TAG, "Agent initialized: %d rules loaded", s_state.rule_count);

    /* Print rules */
    for (int i = 0; i < s_state.rule_count; i++) {
        char desc[256];
        quartz_agent_rule_describe(&s_state.rules[i], desc, sizeof(desc));
        ESP_LOGI(TAG, "  Rule %d: %s", i, desc);
    }

    return 0;
}

void quartz_agent_step(uint32_t delta_sec) {
    if (!s_state.initialized) return;

    s_state.last_eval_sec = now_sec();
    s_state.eval_count++;

    /* Update sensor readings */
    quartz_agent_read_sensors();

    /* Sync chain state (if WiFi connected) */
    if (quartz_wifi_is_connected()) {
        quartz_agent_sync_chain();
    }

    /* LLM-backed decision (every s_llm_interval_sec) */
    quartz_agent_llm_poll();

    /* Evaluate each rule */
    for (int i = 0; i < s_state.rule_count; i++) {
        qz_rule_t *rule = &s_state.rules[i];

        if (!rule->enabled) continue;

        /* Check cooldown */
        uint32_t elapsed = s_state.last_eval_sec - rule->last_fired_sec;
        if (rule->last_fired_sec > 0 && elapsed < rule->cooldown_sec) {
            continue;
        }

        /* Check all conditions (AND) */
        bool conditions_met = quartz_agent_check_conditions(rule);
        if (!conditions_met) continue;

        /* Execute actions */
        ESP_LOGI(TAG, "Rule '%s' triggered — executing %d action(s)",
                 rule->label, rule->action_count);

        for (int j = 0; j < rule->action_count; j++) {
            int rc = quartz_agent_execute_action(&rule->actions[j]);
            if (rc != 0) {
                ESP_LOGW(TAG, "  Action %d failed (rc=%d)", j, rc);
            }
        }

        rule->last_fired_sec = s_state.last_eval_sec;
        rule->fire_count++;
    }
}

int quartz_agent_load_rules(void) {
    return load_rules_nvs();
}

int quartz_agent_save_rules(void) {
    return save_rules_nvs();
}

int quartz_agent_add_rule(const qz_rule_t *rule) {
    if (!rule) return -1;
    if (s_state.rule_count >= QZ_AGENT_MAX_RULES) return -2;

    s_state.rules[s_state.rule_count++] = *rule;
    save_rules_nvs();
    return s_state.rule_count - 1;
}

int quartz_agent_remove_rule(int index) {
    if (index < 0 || index >= s_state.rule_count) return -1;

    /* Shift remaining rules */
    for (int i = index; i < s_state.rule_count - 1; i++) {
        s_state.rules[i] = s_state.rules[i + 1];
    }
    s_state.rule_count--;
    memset(&s_state.rules[s_state.rule_count], 0, sizeof(qz_rule_t));

    save_rules_nvs();
    return 0;
}

const qz_rule_t *quartz_agent_get_rule(int index) {
    if (index < 0 || index >= s_state.rule_count) return NULL;
    return &s_state.rules[index];
}

const qz_agent_state_t *quartz_agent_get_state(void) {
    return &s_state;
}

void quartz_agent_read_sensors(void) {
    read_sensors_hw();
}

void quartz_agent_sync_chain(void) {
    sync_chain_state();
}

int quartz_agent_execute_action(const qz_action_t *action) {
    if (!action) return -1;
    return execute_action_hw(action);
}

bool quartz_agent_check_conditions(const qz_rule_t *rule) {
    if (!rule || rule->condition_count == 0) return true; /* no conditions = always */

    for (int i = 0; i < rule->condition_count; i++) {
        if (!check_condition(&rule->conditions[i])) {
            return false;
        }
    }
    return true;
}

int quartz_agent_rule_describe(const qz_rule_t *rule, char *buf, int buf_len) {
    if (!rule || !buf || buf_len <= 0) return -1;

    int len = snprintf(buf, buf_len, "[%s] ", rule->label);
    if (rule->condition_count == 0) {
        len += snprintf(buf + len, buf_len - len, "always");
    } else {
        for (int i = 0; i < rule->condition_count && len < buf_len; i++) {
            const qz_condition_t *c = &rule->conditions[i];
            if (i > 0) len += snprintf(buf + len, buf_len - len, " AND ");
            switch (c->type) {
                case QZ_TRIGGER_PAYMENT_RECEIVED:
                    len += snprintf(buf + len, buf_len - len, "payment_received");
                    break;
                case QZ_TRIGGER_BALANCE_ABOVE:
                    len += snprintf(buf + len, buf_len - len, "balance>%lu QZ-sat",
                                    (unsigned long)c->threshold);
                    break;
                case QZ_TRIGGER_BALANCE_BELOW:
                    len += snprintf(buf + len, buf_len - len, "balance<%lu QZ-sat",
                                    (unsigned long)c->threshold);
                    break;
                case QZ_TRIGGER_BLOCK_HEIGHT:
                    len += snprintf(buf + len, buf_len - len, "block>=%lu",
                                    (unsigned long)c->threshold);
                    break;
                case QZ_TRIGGER_MESSAGE_RECEIVED:
                    len += snprintf(buf + len, buf_len - len, "message_received");
                    break;
                case QZ_TRIGGER_SENSOR_ABOVE:
                    len += snprintf(buf + len, buf_len - len, "sensor%d>%lu",
                                    c->sensor, (unsigned long)c->threshold);
                    break;
                case QZ_TRIGGER_SENSOR_BELOW:
                    len += snprintf(buf + len, buf_len - len, "sensor%d<%lu",
                                    c->sensor, (unsigned long)c->threshold);
                    break;
                case QZ_TRIGGER_TIMER_INTERVAL:
                    len += snprintf(buf + len, buf_len - len, "every%lus",
                                    (unsigned long)c->interval_s);
                    break;
                default:
                    len += snprintf(buf + len, buf_len - len, "?");
                    break;
            }
        }
    }

    len += snprintf(buf + len, buf_len - len, " → ");
    for (int i = 0; i < rule->action_count && len < buf_len; i++) {
        const qz_action_t *a = &rule->actions[i];
        if (i > 0) len += snprintf(buf + len, buf_len - len, " + ");
        switch (a->type) {
            case QZ_ACTION_PAY:
                len += snprintf(buf + len, buf_len - len, "pay(%llu sats)",
                                (unsigned long long)a->amount_satoshis);
                break;
            case QZ_ACTION_TRIGGER_RELAY:
                len += snprintf(buf + len, buf_len - len, "relay(%lums)",
                                (unsigned long)a->relay_duration_ms);
                break;
            case QZ_ACTION_SEND_MESSAGE:
                len += snprintf(buf + len, buf_len - len, "msg(\"%s\")",
                                a->message);
                break;
            case QZ_ACTION_SET_MINING_RATE:
                len += snprintf(buf + len, buf_len - len, "mine@%d%%",
                                a->mining_rate_pct);
                break;
            case QZ_ACTION_WITHDRAW_QUARRY:
                len += snprintf(buf + len, buf_len - len, "withdraw");
                break;
            case QZ_ACTION_DISPLAY_ALERT:
                len += snprintf(buf + len, buf_len - len, "alert(\"%s\")",
                                a->message);
                break;
            case QZ_ACTION_STOP_MINING:
                len += snprintf(buf + len, buf_len - len, "stop_mining");
                break;
            case QZ_ACTION_RESTART_MINING:
                len += snprintf(buf + len, buf_len - len, "restart_mining");
                break;
            default:
                len += snprintf(buf + len, buf_len - len, "?");
                break;
        }
    }

    return len;
}

void quartz_agent_set_defaults(void) {
    memset(s_state.rules, 0, sizeof(s_state.rules));
    s_state.rule_count = 0;

    /* Rule 0: Low voltage alert — stop mining if voltage drops below 3.0V */
    qz_rule_t *r0 = &s_state.rules[0];
    r0->enabled = false;  /* disabled by default — needs external voltage sensor on GPIO35 */
    strncpy(r0->label, "low_voltage_protect", sizeof(r0->label));
    r0->condition_count = 1;
    r0->conditions[0].type = QZ_TRIGGER_SENSOR_BELOW;
    r0->conditions[0].sensor = QZ_SENSOR_VOLTAGE_ADC;
    r0->conditions[0].threshold = 3000;  /* 3.0V (in millivolts approx) */
    r0->action_count = 2;
    r0->actions[0].type = QZ_ACTION_STOP_MINING;
    r0->actions[1].type = QZ_ACTION_SEND_MESSAGE;
    strncpy(r0->actions[1].message, "LOW VOLTAGE — mining stopped",
            sizeof(r0->actions[1].message) - 1);
    r0->cooldown_sec = 60;  /* don't spam messages */
    s_state.rule_count++;

    /* Rule 1: Overtemperature protection — stop at 85°C */
    qz_rule_t *r1 = &s_state.rules[1];
    r1->enabled = true;
    strncpy(r1->label, "overtemp_protect", sizeof(r1->label));
    r1->condition_count = 1;
    r1->conditions[0].type = QZ_TRIGGER_SENSOR_ABOVE;
    r1->conditions[0].sensor = QZ_SENSOR_TEMP_INTERNAL;
    r1->conditions[0].threshold = 85;  /* 85°C */
    r1->action_count = 2;
    r1->actions[0].type = QZ_ACTION_STOP_MINING;
    r1->actions[1].type = QZ_ACTION_DISPLAY_ALERT;
    strncpy(r1->actions[1].message, "OVERTEMP — mining stopped",
            sizeof(r1->actions[1].message) - 1);
    r1->cooldown_sec = 30;
    s_state.rule_count++;

    /* Rule 2: High vibration detection (pothole/tamper) */
    qz_rule_t *r2 = &s_state.rules[2];
    r2->enabled = false;  /* disabled by default — needs calibration */
    strncpy(r2->label, "vibration_alert", sizeof(r2->label));
    r2->condition_count = 1;
    r2->conditions[0].type = QZ_TRIGGER_SENSOR_ABOVE;
    r2->conditions[0].sensor = QZ_SENSOR_VIBRATION_ADC;
    r2->conditions[0].threshold = 2500;  /* raw ADC threshold */
    r2->action_count = 1;
    r2->actions[0].type = QZ_ACTION_SEND_MESSAGE;
    strncpy(r2->actions[0].message, "High vibration detected — check site",
            sizeof(r2->actions[0].message) - 1);
    r2->cooldown_sec = 300;  /* 5 min cooldown */
    s_state.rule_count++;

    ESP_LOGI(TAG, "Default rules set: %d rules (low_volt, overtemp, vibration)",
             s_state.rule_count);
}

/*=========================================================================
 * Internal: Hardware / Platform
 *=======================================================================*/

static uint32_t now_sec(void) {
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000000);
#else
    return 0;
#endif
}

static void read_sensors_hw(void) {
#ifdef ESP_PLATFORM
    /* Read vibration (piezo/accelerometer on ADC) */
    s_state.sensor_vibration = adc1_get_raw(QZ_SENSOR_VIBRATION_ADC_CHANNEL);

    /* Read light level */
    s_state.sensor_light = adc1_get_raw(QZ_SENSOR_LIGHT_ADC_CHANNEL);

    /* Read voltage (via voltage divider on ADC) */
    int raw_v = adc1_get_raw(QZ_SENSOR_VOLTAGE_ADC_CHANNEL);
    /* Convert 12-bit ADC (0-4095) to millivolts with voltage divider
     * Assumes divider ratio set by hardware. Rough approximation:
     * Vbat = raw / 4095 * 3.3V * divider_ratio
     * For a 2:1 divider: raw/4095*6600
     */
    s_state.sensor_voltage_mv = (uint32_t)((raw_v * 6600ULL) / 4095);

    /* Read current sensor (ACS712 or similar) */
    int raw_c = adc1_get_raw(QZ_SENSOR_CURRENT_ADC_CHANNEL);
    s_state.sensor_current_ma = (uint32_t)((raw_c * 3300ULL) / 4095);

    /* Read digital GPIO */
    s_state.sensor_gpio_val = gpio_get_level(QZ_SENSOR_DIGITAL_GPIO);

    /* Internal temperature (ESP32 — rough estimate from hall sensor / internal) */
    /* The ESP32 doesn't have a precise internal temp sensor exposed in IDF v5.3
     * We use a placeholder: read from hall sensor if available, else 40°C */
    s_state.sensor_temp_c = 40;  /* placeholder — replace with real sensor */

    ESP_LOGD(TAG, "Sensors: vib=%lu light=%lu volt=%lumV curr=%lumA gpio=%d temp=%d",
             (unsigned long)s_state.sensor_vibration,
             (unsigned long)s_state.sensor_light,
             (unsigned long)s_state.sensor_voltage_mv,
             (unsigned long)s_state.sensor_current_ma,
             s_state.sensor_gpio_val,
             s_state.sensor_temp_c);
#else
    /* Non-ESP32: fill with zeros */
    s_state.sensor_vibration = 0;
    s_state.sensor_light = 0;
    s_state.sensor_voltage_mv = 3300;
    s_state.sensor_current_ma = 0;
    s_state.sensor_gpio_val = 1;
    s_state.sensor_temp_c = 25;
#endif
}

static void sync_chain_state(void) {
    /* Fetch balance and block height from node via HTTP
     * We reuse the WiFi HTTP client from quartz_wifi.c
     *
     * In production, this would call quartz_wifi_http_get() or similar.
     * For now, we sync via the existing mining work fetch which updates
     * block height, and we do a lightweight balance check.
     */

    /* The block template already gives us block height */
    /* For balance, we'd need a /api/v1/address/<addr> call.
     * This is a placeholder that will be wired to the HTTP client. */

    /* TODO: Wire to actual HTTP calls when node API supports lightweight polling
     * For now, balance and block_height are updated opportunistically
     * by the mining task which already fetches work templates. */
}

static int execute_action_hw(const qz_action_t *action) {
    if (!action) return -1;

    switch (action->type) {
    case QZ_ACTION_PAY: {
        ESP_LOGI(TAG, "ACTION: Pay %llu sats to %s",
                 (unsigned long long)action->amount_satoshis,
                 action->address);
#ifdef ESP_PLATFORM
        /* Use the existing message/payment infrastructure */
        /* For on-chain payment, we need to construct and sign a transaction.
         * This requires the wallet's private key and node connectivity.
         * Deferred to full wallet integration. */
        /* TODO: quartz_wallet_send(action->address, action->amount_satoshis); */
        ESP_LOGW(TAG, "  PAY action not yet wired to wallet (stub)");
#endif
        return 0;
    }

    case QZ_ACTION_TRIGGER_RELAY: {
        uint32_t duration = action->relay_duration_ms;
        if (duration == 0) duration = 3000;  /* default 3s */
        ESP_LOGI(TAG, "ACTION: Trigger relay for %lu ms", (unsigned long)duration);
#ifdef ESP_PLATFORM
        quartz_pay_trigger_relay(duration);
#endif
        return 0;
    }

    case QZ_ACTION_SEND_MESSAGE: {
        ESP_LOGI(TAG, "ACTION: Send message: \"%s\"", action->message);
#ifdef ESP_PLATFORM
        if (quartz_wifi_is_connected()) {
            quartz_messages_send("agent", "", action->message);
        }
#endif
        return 0;
    }

    case QZ_ACTION_SET_MINING_RATE: {
        ESP_LOGI(TAG, "ACTION: Set mining rate to %d%%", action->mining_rate_pct);
        /* Adjust mining intensity by inserting delays in the mining loop.
         * 100% = no delay, 50% = delay every other hash, 0% = stop mining.
         * Implementation: set a global that the mining task reads. */
        s_state.mining_active = (action->mining_rate_pct > 0);
        /* TODO: expose g_mining_rate from main.c */
        return 0;
    }

    case QZ_ACTION_WITHDRAW_QUARRY: {
        ESP_LOGI(TAG, "ACTION: Withdraw quarry to %s",
                 action->address[0] ? action->address : s_wallet_addr);
        /* Quarry withdrawal = sign a transaction moving up to 15% of balance.
         * Requires wallet + node sync.
         * TODO: wire to wallet when quarry system is implemented on-chain. */
        ESP_LOGW(TAG, "  WITHDRAW action not yet wired (stub)");
        return 0;
    }

    case QZ_ACTION_DISPLAY_ALERT: {
        ESP_LOGI(TAG, "ACTION: Display alert: \"%s\"", action->message);
#ifdef ESP_PLATFORM
        /* Show alert on screen */
        quartz_display_clear(0x0843);  /* dark blue background */
        quartz_display_fill_rect(0, 0, 320, 22, 0xF800);  /* red header */
        quartz_display_draw_text(8, 3, "⚠ AGENT ALERT", 0xFFFF, 0xF800);
        quartz_display_draw_text(8, 40, action->message, 0xFFFF, 0x0843);
        /* Return to mining screen after 5 seconds (non-blocking here) */
#endif
        return 0;
    }

    case QZ_ACTION_STOP_MINING: {
        ESP_LOGI(TAG, "ACTION: Stop mining");
        s_state.mining_active = false;
        /* TODO: expose a global flag in main.c to actually halt mining */
        return 0;
    }

    case QZ_ACTION_RESTART_MINING: {
        ESP_LOGI(TAG, "ACTION: Restart mining");
        s_state.mining_active = true;
        return 0;
    }

    default:
        ESP_LOGW(TAG, "Unknown action type %d", action->type);
        return -2;
    }
}

static bool check_condition(const qz_condition_t *cond) {
    if (!cond || cond->type == QZ_TRIGGER_NONE) return true;

    switch (cond->type) {
    case QZ_TRIGGER_PAYMENT_RECEIVED:
        /* Check if we received a payment since last evaluation */
        /* TODO: wire to actual payment polling */
        return false;

    case QZ_TRIGGER_BALANCE_ABOVE:
        return s_state.balance_satoshis > (uint64_t)cond->threshold;

    case QZ_TRIGGER_BALANCE_BELOW:
        return s_state.balance_satoshis < (uint64_t)cond->threshold;

    case QZ_TRIGGER_BLOCK_HEIGHT:
        return s_state.block_height >= cond->threshold;

    case QZ_TRIGGER_MESSAGE_RECEIVED:
        /* Check if a new message arrived since last eval */
        /* TODO: track message serial/timestamp */
        return false;

    case QZ_TRIGGER_SENSOR_ABOVE: {
        uint32_t val = 0;
        switch (cond->sensor) {
        case QZ_SENSOR_VIBRATION_ADC: val = s_state.sensor_vibration; break;
        case QZ_SENSOR_TEMP_INTERNAL: val = (uint32_t)(s_state.sensor_temp_c); break;
        case QZ_SENSOR_LIGHT_ADC:     val = s_state.sensor_light; break;
        case QZ_SENSOR_VOLTAGE_ADC:   val = s_state.sensor_voltage_mv; break;
        case QZ_SENSOR_CURRENT_ADC:   val = s_state.sensor_current_ma; break;
        case QZ_SENSOR_GPIO_DIGITAL:  val = (uint32_t)s_state.sensor_gpio_val; break;
        default: break;
        }
        return val > cond->threshold;
    }

    case QZ_TRIGGER_SENSOR_BELOW: {
        uint32_t val = 0;
        switch (cond->sensor) {
        case QZ_SENSOR_VIBRATION_ADC: val = s_state.sensor_vibration; break;
        case QZ_SENSOR_TEMP_INTERNAL: val = (uint32_t)(s_state.sensor_temp_c); break;
        case QZ_SENSOR_LIGHT_ADC:     val = s_state.sensor_light; break;
        case QZ_SENSOR_VOLTAGE_ADC:   val = s_state.sensor_voltage_mv; break;
        case QZ_SENSOR_CURRENT_ADC:   val = s_state.sensor_current_ma; break;
        case QZ_SENSOR_GPIO_DIGITAL:  val = (uint32_t)s_state.sensor_gpio_val; break;
        default: break;
        }
        return val < cond->threshold;
    }

    case QZ_TRIGGER_TIMER_INTERVAL: {
        /* Fire if enough time has passed since last fire (or boot) */
        /* Uses eval_count as a proxy: if delta_sec * eval_count >= interval */
        /* This is approximate — the caller should ensure agent_step is called
         * at roughly the interval they want */
        static uint32_t s_timer_accum = 0;
        s_timer_accum += 1;  /* incremented each eval */
        if (s_timer_accum >= cond->interval_s) {
            s_timer_accum = 0;
            return true;
        }
        return false;
    }

    case QZ_TRIGGER_GOVERNANCE_PROPOSAL:
        /* Not yet implemented — governance system needs on-chain proposals */
        return false;

    default:
        return false;
    }
}

/*=========================================================================
 * LLM-backed agent
 *=======================================================================*/

#define QZ_LLM_INTERVAL_DEFAULT_SEC  0   /* disabled by default — opt-in */
#define QZ_LLM_RESPONSE_MAX          1024
#define QZ_LLM_PATH                  "/api/v1/agent/decide"

static uint32_t s_llm_interval_sec = QZ_LLM_INTERVAL_DEFAULT_SEC;
static uint32_t s_llm_last_poll_sec = 0;

void quartz_agent_llm_set_interval(uint32_t seconds) {
    s_llm_interval_sec = seconds;
}

bool quartz_agent_llm_enabled(void) {
    return s_llm_interval_sec > 0;
}

/* Simple JSON string extraction: finds "key":"value" and copies value */
static bool json_extract_string(const char *json, const char *key, char *out, size_t out_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;  /* skip escape */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

int quartz_agent_llm_poll(void) {
    if (!s_llm_interval_sec) return -1;  /* disabled */
    if (!quartz_wifi_is_connected()) return -1;

    uint32_t now = now_sec();
    if (s_llm_last_poll_sec > 0 && (now - s_llm_last_poll_sec) < s_llm_interval_sec) {
        return -1;  /* too soon */
    }
    s_llm_last_poll_sec = now;

    /* Build JSON body with device state */
    char body[768];
    snprintf(body, sizeof(body),
        "{"
        "\"device_id\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"balance\":%llu.%02u,"
        "\"block_height\":%lu,"
        "\"hashrate\":%lu,"
        "\"temperature\":%d,"
        "\"voltage_mv\":%lu,"
        "\"vibration\":%lu,"
        "\"light\":%lu,"
        "\"mining_active\":%s,"
        "\"uptime_sec\":%lu,"
        "\"blocks_found\":%lu,"
        "\"relay_state\":\"idle\""
        "}",
        (unsigned char)s_wallet_addr[0],  /* placeholder: use MAC */
        0, 0, 0, 0, 0,
        (unsigned long long)(s_state.balance_satoshis / 100000000ULL),
        (unsigned)((s_state.balance_satoshis % 100000000ULL) / 1000000ULL),
        (unsigned long)s_state.block_height,
        (unsigned long)0,  /* hashrate — TODO: pass from main */
        (int)s_state.sensor_temp_c,
        (unsigned long)s_state.sensor_voltage_mv,
        (unsigned long)s_state.sensor_vibration,
        (unsigned long)s_state.sensor_light,
        s_state.mining_active ? "true" : "false",
        (unsigned long)now,
        (unsigned long)0,  /* blocks_found — TODO: pass from main */
        0
    );

    /* Make HTTP request */
    static char response[QZ_LLM_RESPONSE_MAX];
    int n = quartz_http_request("POST", QZ_LLM_PATH, body, response, sizeof(response));
    if (n <= 0) {
        ESP_LOGW(TAG, "LLM poll failed (HTTP error %d)", n);
        return -1;
    }

    ESP_LOGI(TAG, "LLM response: %.*s", n > 200 ? 200 : n, response);

    /* Parse action from JSON response */
    char action_str[32];
    char reasoning[256];
    char params_text[QZ_AGENT_MSG_MAX];

    if (!json_extract_string(response, "action", action_str, sizeof(action_str))) {
        ESP_LOGW(TAG, "LLM response missing 'action' field");
        return -1;
    }

    json_extract_string(response, "reasoning", reasoning, sizeof(reasoning));
    json_extract_string(response, "text", params_text, sizeof(params_text));

    ESP_LOGI(TAG, "LLM decision: action='%s' reasoning='%s'", action_str, reasoning);

    /* Execute the action */
    qz_action_t action;
    memset(&action, 0, sizeof(action));

    if (strcmp(action_str, "relay") == 0) {
        action.type = QZ_ACTION_TRIGGER_RELAY;
        action.relay_duration_ms = 3000;  /* default */
        /* TODO: parse duration_ms from params */
    } else if (strcmp(action_str, "message") == 0) {
        action.type = QZ_ACTION_SEND_MESSAGE;
        strncpy(action.message, params_text, sizeof(action.message) - 1);
    } else if (strcmp(action_str, "alert") == 0) {
        action.type = QZ_ACTION_DISPLAY_ALERT;
        strncpy(action.message, params_text, sizeof(action.message) - 1);
    } else if (strcmp(action_str, "stop_mining") == 0) {
        action.type = QZ_ACTION_STOP_MINING;
    } else if (strcmp(action_str, "restart_mining") == 0) {
        action.type = QZ_ACTION_RESTART_MINING;
    } else {
        /* action == "none" or unknown */
        ESP_LOGI(TAG, "LLM says: do nothing (keep mining)");
        return 0;
    }

    return quartz_agent_execute_action(&action);
}

/*=========================================================================
 * Internal: NVS Persistence
 *=======================================================================*/

static int load_rules_nvs(void) {
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    esp_err_t err = nvs_open(QZ_AGENT_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS open failed: %s — first boot?", esp_err_to_name(err));
        return -1;
    }

    /* Read version */
    uint8_t version = 0;
    nvs_get_u8(h, "ver", &version);
    if (version != QZ_AGENT_NVS_VERSION) {
        ESP_LOGW(TAG, "Agent NVS version mismatch (%d != %d) — using defaults",
                 version, QZ_AGENT_NVS_VERSION);
        nvs_close(h);
        return -1;
    }

    /* Read rule count */
    int16_t count = 0;
    size_t required = sizeof(count);
    nvs_get_blob(h, "count", &count, &required);
    if (count < 0 || count > QZ_AGENT_MAX_RULES) {
        ESP_LOGE(TAG, "Invalid rule count %d in NVS", count);
        nvs_close(h);
        return -1;
    }

    /* Read each rule */
    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rule%d", i);
        size_t sz = sizeof(qz_rule_t);
        err = nvs_get_blob(h, key, &s_state.rules[i], &sz);
        if (err != ESP_OK || sz != sizeof(qz_rule_t)) {
            ESP_LOGW(TAG, "Failed to load rule %d (%s)", i, esp_err_to_name(err));
            memset(&s_state.rules[i], 0, sizeof(qz_rule_t));
            continue;
        }
    }

    s_state.rule_count = count;
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d rules from NVS", count);
    return 0;
#else
    return -1;
#endif
}

static int save_rules_nvs(void) {
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    esp_err_t err = nvs_open(QZ_AGENT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Write version */
    nvs_set_u8(h, "ver", QZ_AGENT_NVS_VERSION);

    /* Write rule count */
    int16_t count = (int16_t)s_state.rule_count;
    nvs_set_blob(h, "count", &count, sizeof(count));

    /* Write each rule */
    for (int i = 0; i < s_state.rule_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rule%d", i);
        err = nvs_set_blob(h, key, &s_state.rules[i], sizeof(qz_rule_t));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save rule %d (%s)", i, esp_err_to_name(err));
        }
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d rules to NVS", s_state.rule_count);
    return 0;
#else
    return 0;
#endif
}
