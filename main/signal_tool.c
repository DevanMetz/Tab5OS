#include "signal_tool.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define SIGNAL_PIN GPIO_NUM_6
#define SIGNAL_TIMER LEDC_TIMER_2
#define SIGNAL_CHANNEL LEDC_CHANNEL_3
#define SIGNAL_TIMEOUT_MS (5 * 60 * 1000)
#define SIGNAL_CLOCK_HZ 40000000U

typedef struct {
    uint32_t frequency_hz;
    ledc_timer_bit_t resolution;
    uint8_t bits;
} frequency_choice_t;

static const frequency_choice_t frequency_choices[] = {
    {50, LEDC_TIMER_14_BIT, 14},
    {100, LEDC_TIMER_14_BIT, 14},
    {1000, LEDC_TIMER_13_BIT, 13},
    {10000, LEDC_TIMER_11_BIT, 11},
    {50000, LEDC_TIMER_9_BIT, 9},
};
static const uint8_t duty_choices[] = {10, 25, 50, 75, 90};
static const uint32_t pulse_choices_us[] = {10, 100, 1000, 10000};

static unsigned frequency_index = 2;
static unsigned duty_index = 2;
static unsigned pulse_index = 1;
static bool pwm_running;
static lv_timer_t *timeout_timer;
static lv_obj_t *frequency_label;
static lv_obj_t *duty_label;
static lv_obj_t *pulse_label;
static lv_obj_t *start_label;
static lv_obj_t *output_label;
static lv_obj_t *status_label;

_Static_assert(SIGNAL_TIMER != LEDC_TIMER_0 && SIGNAL_TIMER != LEDC_TIMER_1,
               "Signal generator must not share the display or Servo timer");
_Static_assert(SIGNAL_CHANNEL != LEDC_CHANNEL_0 && SIGNAL_CHANNEL != LEDC_CHANNEL_1 &&
               SIGNAL_CHANNEL != LEDC_CHANNEL_2,
               "Signal generator must not share the camera, display, or Servo channel");

static uint32_t duty_value(uint8_t bits, uint8_t percent)
{
    return ((uint32_t)1U << bits) * percent / 100U;
}

void signal_tool_self_test(void)
{
    assert(duty_value(14, 10) == 1638);
    assert(duty_value(14, 50) == 8192);
    assert(duty_value(10, 90) == 921);
    assert(frequency_choices[0].frequency_hz == 50);
    assert(frequency_choices[sizeof(frequency_choices) / sizeof(frequency_choices[0]) - 1].frequency_hz == 50000);
    for (size_t i = 0; i < sizeof(frequency_choices) / sizeof(frequency_choices[0]); i++)
        assert((uint64_t)frequency_choices[i].frequency_hz * (1ULL << frequency_choices[i].bits) <=
               SIGNAL_CLOCK_HZ);
    assert(pulse_choices_us[0] == 10 && pulse_choices_us[3] == 10000);
}

static void set_status(const char *format, ...)
{
    if (!status_label) return;
    char text[160];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    lv_label_set_text(status_label, text);
}

static void release_pin(void)
{
    gpio_set_level(SIGNAL_PIN, 0);
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << SIGNAL_PIN,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
}

bool signal_tool_busy(void)
{
    return pwm_running;
}

static void update_controls(void)
{
    const frequency_choice_t *frequency = &frequency_choices[frequency_index];
    if (frequency_label) {
        if (frequency->frequency_hz < 1000)
            lv_label_set_text_fmt(frequency_label, "Frequency\n%lu Hz", (unsigned long)frequency->frequency_hz);
        else
            lv_label_set_text_fmt(frequency_label, "Frequency\n%lu kHz", (unsigned long)(frequency->frequency_hz / 1000));
    }
    if (duty_label) lv_label_set_text_fmt(duty_label, "Duty\n%u%%", duty_choices[duty_index]);
    if (pulse_label) {
        uint32_t width = pulse_choices_us[pulse_index];
        if (width < 1000)
            lv_label_set_text_fmt(pulse_label, "Pulse\n%lu us", (unsigned long)width);
        else
            lv_label_set_text_fmt(pulse_label, "Pulse\n%lu ms", (unsigned long)(width / 1000));
    }
    if (start_label) lv_label_set_text(start_label, pwm_running ? "STOP PWM" : "START PWM");
}

static void stop_pwm(void)
{
    if (timeout_timer) {
        lv_timer_delete(timeout_timer);
        timeout_timer = NULL;
    }
    if (pwm_running) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_stop(LEDC_LOW_SPEED_MODE, SIGNAL_CHANNEL, 0));
        pwm_running = false;
    }
    release_pin();
    update_controls();
}

void signal_tool_stop(void)
{
    stop_pwm();
    frequency_label = NULL;
    duty_label = NULL;
    pulse_label = NULL;
    start_label = NULL;
    output_label = NULL;
    status_label = NULL;
}

static void timeout_elapsed(lv_timer_t *timer)
{
    (void)timer;
    timeout_timer = NULL;
    stop_pwm();
    if (output_label) lv_label_set_text(output_label, "Output: stopped");
    set_status("Five-minute safety timeout reached; G6 released");
}

static esp_err_t start_pwm(void)
{
    const frequency_choice_t *frequency = &frequency_choices[frequency_index];
    esp_err_t error = gpio_set_level(SIGNAL_PIN, 0);
    if (error != ESP_OK) return error;

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = frequency->resolution,
        .timer_num = SIGNAL_TIMER,
        .freq_hz = frequency->frequency_hz,
        .clk_cfg = LEDC_USE_XTAL_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = SIGNAL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = SIGNAL_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SIGNAL_TIMER,
        .duty = duty_value(frequency->bits, duty_choices[duty_index]),
        .hpoint = 0,
    };
    error = ledc_timer_config(&timer);
    if (error == ESP_OK) error = ledc_channel_config(&channel);
    if (error != ESP_OK) {
        release_pin();
        return error;
    }
    pwm_running = true;
    timeout_timer = lv_timer_create(timeout_elapsed, SIGNAL_TIMEOUT_MS, NULL);
    if (!timeout_timer) {
        stop_pwm();
        return ESP_ERR_NO_MEM;
    }
    lv_timer_set_repeat_count(timeout_timer, 1);
    return ESP_OK;
}

static void start_clicked(lv_event_t *event)
{
    (void)event;
    if (pwm_running) {
        stop_pwm();
        lv_label_set_text(output_label, "Output: stopped");
        set_status("PWM stopped; G6 released");
        return;
    }
    esp_err_t error = start_pwm();
    update_controls();
    if (error != ESP_OK) {
        lv_label_set_text(output_label, "Output: stopped");
        set_status("PWM start failed: %s", esp_err_to_name(error));
        return;
    }
    uint32_t actual = ledc_get_freq(LEDC_LOW_SPEED_MODE, SIGNAL_TIMER);
    lv_label_set_text_fmt(output_label, "Output: %lu Hz at %u%% on G6",
                          (unsigned long)actual, duty_choices[duty_index]);
    set_status("PWM active for at most five minutes; press STOP or Home when finished");
}

static void setting_clicked(lv_event_t *event)
{
    if (pwm_running) {
        set_status("Stop PWM before changing output settings");
        return;
    }
    uintptr_t setting = (uintptr_t)lv_event_get_user_data(event);
    if (setting == 0)
        frequency_index = (frequency_index + 1) % (sizeof(frequency_choices) / sizeof(frequency_choices[0]));
    else if (setting == 1)
        duty_index = (duty_index + 1) % (sizeof(duty_choices) / sizeof(duty_choices[0]));
    else
        pulse_index = (pulse_index + 1) % (sizeof(pulse_choices_us) / sizeof(pulse_choices_us[0]));
    update_controls();
    set_status("Settings selected; start PWM or send one pulse");
}

static void pulse_clicked(lv_event_t *event)
{
    (void)event;
    if (pwm_running) {
        set_status("Stop PWM before sending a single pulse");
        return;
    }
    uint32_t width = pulse_choices_us[pulse_index];
    esp_err_t error = gpio_set_level(SIGNAL_PIN, 0);
    if (error == ESP_OK) error = gpio_set_direction(SIGNAL_PIN, GPIO_MODE_OUTPUT);
    if (error == ESP_OK) error = gpio_set_level(SIGNAL_PIN, 1);
    if (error == ESP_OK) {
        esp_rom_delay_us(width);
        error = gpio_set_level(SIGNAL_PIN, 0);
    }
    release_pin();
    if (error == ESP_OK) {
        if (width < 1000)
            set_status("Requested one %lu us high pulse; G6 released", (unsigned long)width);
        else
            set_status("Requested one %lu ms high pulse; G6 released", (unsigned long)(width / 1000));
        lv_label_set_text(output_label, "Output: stopped after one pulse");
    } else {
        set_status("Pulse failed: %s", esp_err_to_name(error));
    }
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, int width,
                              lv_event_cb_t callback, void *user_data, lv_obj_t **label_out)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, width, 76);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return control;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, 640, 80);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return container;
}

void signal_tool_show(lv_obj_t *parent)
{
    lv_obj_set_style_pad_row(parent, 18, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "PWM & Pulse Generator");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *help = lv_label_create(parent);
    lv_obj_set_width(help, 640);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(help, "M5-Bus G6 output, 3.3 V logic only; share ground.\n"
                            "Use a buffer for loads. Output is low before routing and high-impedance while stopped.\n"
                            "Measure the real waveform; single-pulse width is requested timing, not calibrated timing.");

    lv_obj_t *settings = row(parent);
    small_button(settings, "", 200, setting_clicked, (void *)0, &frequency_label);
    small_button(settings, "", 200, setting_clicked, (void *)1, &duty_label);
    small_button(settings, "", 200, setting_clicked, (void *)2, &pulse_label);

    lv_obj_t *actions = row(parent);
    small_button(actions, "", 310, start_clicked, NULL, &start_label);
    small_button(actions, "SINGLE PULSE", 310, pulse_clicked, NULL, NULL);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 640, 300);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    output_label = lv_label_create(card);
    lv_label_set_text(output_label, "Output: stopped");
    lv_obj_set_style_text_font(output_label, &lv_font_montserrat_28, 0);
    lv_obj_t *limits = lv_label_create(card);
    lv_obj_set_width(limits, 560);
    lv_obj_set_style_text_align(limits, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(limits, "PWM automatically stops after five minutes.\n"
                              "Stop before rewiring. Never drive motors, relays, LEDs, or 5 V logic directly.");

    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 640);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "Ready; choose frequency/duty or pulse width");
    update_controls();
}
