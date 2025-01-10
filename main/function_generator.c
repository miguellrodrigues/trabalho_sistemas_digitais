#include <sys/cdefs.h>
#include <freertos/FreeRTOS.h>
#include <math.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include "driver/mcpwm_timer.h"
#include "driver/mcpwm_gen.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_cmpr.h"
#include "esp_timer.h"
#include <i2c_device.h>
#include <lcd.h>

#define MEGA 1000000

/***************** Pwm Timer Definitions *****************/

#define GPIO_PWM_OUT 38
#define PWM_TIMER_RESOLUTION (80 * MEGA)
#define PWM_TIMER_PERIOD_TICKS 400

uint32_t pwm_timer_frequency = (uint32_t) PWM_TIMER_RESOLUTION / PWM_TIMER_PERIOD_TICKS;

/***************** Signal Timer Definitions *****************/

#define SIGNAL_TIMER_RESOLUTION (20 * MEGA)
#define SIGNAL_TIMER_PERIOD_TICKS 150

uint32_t signal_timer_frequency = (uint32_t) SIGNAL_TIMER_RESOLUTION / SIGNAL_TIMER_PERIOD_TICKS;

/***************** Table(s) Definitions *****************/

typedef enum {
    SINE, SQUARE, SAWTOOTH, TRIANGULAR
} waveform_t;

#define TABLE_RESOLUTION 128  // this need to be a power of 2

uint16_t sine_table[TABLE_RESOLUTION];
uint16_t square_table[TABLE_RESOLUTION];
uint16_t sawtooth_table[TABLE_RESOLUTION];
uint16_t triangular_table[TABLE_RESOLUTION];

uint16_t *waveforms_table[4] = {sine_table, square_table, sawtooth_table, triangular_table};
uint16_t *selected_table = NULL;

/***************** Vars *****************/

#define TAG "SYS"

uint8_t debounce_delay_ms = 50;

uint16_t cycles_to_pop = 1;
volatile uint8_t cycles_counter = 0;
volatile uint16_t table_counter = 0;
double amplitude = 1.0f;

mcpwm_cmpr_handle_t pwm_cmpr_a, pwm_cmpr_b;
uint32_t comparator_ticks = 1, modulated_comparator_ticks = 1;

I2CD_t lcd;

bool on_full_callback(mcpwm_timer_handle_t timer, const mcpwm_timer_event_data_t *edata, void *user_ctx) {
    cycles_counter += 1;

    if (cycles_counter >= cycles_to_pop) {
        table_counter = (table_counter + 1) % TABLE_RESOLUTION;

        comparator_ticks = *(selected_table + table_counter);
        modulated_comparator_ticks = (uint32_t) (comparator_ticks * amplitude);

        mcpwm_comparator_set_compare_value(pwm_cmpr_b, modulated_comparator_ticks);
        cycles_counter = 0;
    }

    return false;
}

void setup_signal_timer() {
    mcpwm_timer_config_t config = {.count_mode = MCPWM_TIMER_COUNT_MODE_UP, .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT, .group_id = 0, .resolution_hz =
    SIGNAL_TIMER_RESOLUTION, .period_ticks = SIGNAL_TIMER_PERIOD_TICKS};

    mcpwm_timer_handle_t timerHandle;
    mcpwm_new_timer(&config, &timerHandle);

    mcpwm_timer_event_callbacks_t callbacks = {.on_full = on_full_callback};
    mcpwm_timer_register_event_callbacks(timerHandle, &callbacks, NULL);

    mcpwm_timer_enable(timerHandle);
    mcpwm_timer_start_stop(timerHandle, MCPWM_TIMER_START_NO_STOP);

    ESP_LOGI(TAG, "Signal Timer setup successfully");
    ESP_LOGI(TAG, "Signal Timer Frequency: %lu Hz", signal_timer_frequency);
}

void setup_pwm_timer() {
    mcpwm_timer_config_t config = {.count_mode = MCPWM_TIMER_COUNT_MODE_UP, .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT, .group_id = 0, .intr_priority = 0, .resolution_hz =
    PWM_TIMER_RESOLUTION, .period_ticks = PWM_TIMER_PERIOD_TICKS};

    mcpwm_generator_config_t generatorConfig = {.gen_gpio_num = GPIO_PWM_OUT,};

    mcpwm_oper_handle_t operatorHandle;

    mcpwm_operator_config_t operatorConfig = {.group_id = 0, .intr_priority = 1};

    mcpwm_new_operator(&operatorConfig, &operatorHandle);

    mcpwm_gen_handle_t genHandle;
    mcpwm_new_generator(operatorHandle, &generatorConfig, &genHandle);

    mcpwm_timer_handle_t timerHandle;
    mcpwm_new_timer(&config, &timerHandle);

    mcpwm_operator_connect_timer(operatorHandle, timerHandle);

    mcpwm_comparator_config_t cmprConfig = {.intr_priority = 0};

    mcpwm_new_comparator(operatorHandle, &cmprConfig, &pwm_cmpr_a);
    mcpwm_new_comparator(operatorHandle, &cmprConfig, &pwm_cmpr_b);

    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genHandle, MCPWM_GEN_COMPARE_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_UP, pwm_cmpr_a, MCPWM_GEN_ACTION_HIGH), MCPWM_GEN_COMPARE_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_DOWN, pwm_cmpr_a, MCPWM_GEN_ACTION_LOW), MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genHandle, MCPWM_GEN_COMPARE_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_UP, pwm_cmpr_b, MCPWM_GEN_ACTION_LOW), MCPWM_GEN_COMPARE_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_DOWN, pwm_cmpr_b, MCPWM_GEN_ACTION_HIGH), MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

//    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genHandle, MCPWM_GEN_COMPARE_EVENT_ACTION(
//            MCPWM_TIMER_DIRECTION_UP, pwm_cmpr_a, MCPWM_GEN_ACTION_TOGGLE), MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

    mcpwm_timer_enable(timerHandle);
    mcpwm_timer_start_stop(timerHandle, MCPWM_TIMER_START_NO_STOP);

    ESP_LOGI(TAG, "PWM Timer setup successfully");
    ESP_LOGI(TAG, "PWM Timer Frequency: %lu Hz", pwm_timer_frequency);
}

void setup_pulldown_pin(uint8_t pin, gpio_mode_t mode) {
    gpio_config_t io_conf = {.pin_bit_mask = (1ULL
            << pin), .mode = mode, .pull_down_en = GPIO_PULLDOWN_ENABLE, .pull_up_en = GPIO_PULLUP_DISABLE, .intr_type = GPIO_INTR_DISABLE};

    gpio_config(&io_conf);
}

float counts2frequency(uint32_t c2p) {
    return signal_timer_frequency / (TABLE_RESOLUTION * c2p);
}

bool debounce_gpio(uint8_t pin, bool *last_state) {
    static uint32_t last_time[GPIO_NUM_MAX] = {0};

    uint32_t current_time = esp_timer_get_time();
    bool current_state = gpio_get_level(pin);

    if (current_state != *last_state && (current_time - last_time[pin]) > 1000 * debounce_delay_ms) {
        *last_state = current_state;
        last_time[pin] = current_time;
        return true;
    }

    return false;
}

void setup_gpio() {
    setup_pulldown_pin(GPIO_PWM_OUT, GPIO_MODE_OUTPUT);
    setup_pulldown_pin(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    setup_pulldown_pin(GPIO_NUM_1, GPIO_MODE_OUTPUT);
    setup_pulldown_pin(GPIO_NUM_13, GPIO_MODE_OUTPUT);

    setup_pulldown_pin(GPIO_NUM_14, GPIO_MODE_INPUT);
    setup_pulldown_pin(GPIO_NUM_21, GPIO_MODE_INPUT);
    setup_pulldown_pin(GPIO_NUM_48, GPIO_MODE_INPUT);
    setup_pulldown_pin(GPIO_NUM_47, GPIO_MODE_INPUT);

    ESP_LOGI(TAG, "GPIOs setup successfully");
}

void populate_tables() {
    uint16_t w = PWM_TIMER_PERIOD_TICKS >> 1;

    for (int i = 0; i < TABLE_RESOLUTION; i++) {
        sine_table[i] = (uint16_t) ((double) w * (1 + sin(2 * M_PI * i / TABLE_RESOLUTION)));
        uint8_t sign = sine_table[i] > w ? 1 : 0;

        square_table[i] = 2 * w * sign;
        sawtooth_table[i] = 2 * w * i / TABLE_RESOLUTION;
        triangular_table[i] = 2 * w * (i < TABLE_RESOLUTION / 2 ? i : TABLE_RESOLUTION - i) / (TABLE_RESOLUTION / 2);
    }

    selected_table = waveforms_table[SINE]; // Default waveform

    ESP_LOGI(TAG, "Tables populated successfully");
    ESP_LOGI(TAG, "Default waveform: SINE");

    ESP_LOGI(TAG, "Frequency %f | C2P %hu", counts2frequency(cycles_to_pop), cycles_to_pop);
}

void handle_gpio_input() {
    static bool last_io_level_14 = false, last_io_level_21 = false, last_io_level_48 = false, last_io_level_47 = false;
    static bool io_14_state = false, io_21_state = false, io_48_state = false, io_47_state = false;

    static char freq_str[16], amp_str[16];

    static uint8_t selected_waveform = SINE;

    if (debounce_gpio(GPIO_NUM_14, &last_io_level_14)) {
        io_14_state = !io_14_state;
    }
    if (debounce_gpio(GPIO_NUM_21, &last_io_level_21)) {
        io_21_state = !io_21_state;
    }
    if (debounce_gpio(GPIO_NUM_48, &last_io_level_48)) {
        io_48_state = !io_48_state;
    }
    if (debounce_gpio(GPIO_NUM_47, &last_io_level_47)) {
        io_47_state = !io_47_state;
    }

    if (io_21_state) {
        cycles_to_pop = cycles_to_pop - 1 > 1 ? cycles_to_pop - 1 : 1;
        ESP_LOGI(TAG, "Frequency %f | C2P %hu", counts2frequency(cycles_to_pop), cycles_to_pop);

        lcd_clear(lcd);

        sprintf(freq_str, "Freq: %0.2f", counts2frequency(cycles_to_pop));

        lcd_put_cur(lcd, 0, 0);
        lcd_send_string(lcd, freq_str);
    }

    if (io_14_state) {
        cycles_to_pop = cycles_to_pop + 1 < 200 ? cycles_to_pop + 1 : 200;
        ESP_LOGI(TAG, "Frequency %f | C2P %hu", counts2frequency(cycles_to_pop), cycles_to_pop);

        lcd_clear(lcd);

        sprintf(freq_str, "Freq: %0.2f", counts2frequency(cycles_to_pop));

        lcd_put_cur(lcd, 0, 0);
        lcd_send_string(lcd, freq_str);
    }

    if (io_48_state) {
        ESP_LOGI(TAG, "Amplitude %f", amplitude);
        amplitude = amplitude + 0.01f < 1.0f ? amplitude + 0.01f : 1.0f;
    }

    if (io_47_state) {
        ESP_LOGI(TAG, "Amplitude %f", amplitude);
        amplitude = amplitude - 0.01f > 0.01f ? amplitude - 0.01f : 0.01f;
    }

    if (io_47_state && io_48_state) {
        selected_waveform = (selected_waveform + 1) % 4;

        ESP_LOGI(TAG, "Selected waveform: %d", selected_waveform);
        selected_table = waveforms_table[selected_waveform];
    }
}

void init_i2c() {
    i2c_master_bus_config_t i2c_mst_config = {.clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = -1, .scl_io_num = GPIO_NUM_5, .sda_io_num = GPIO_NUM_4, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};

    i2c_master_bus_handle_t bus_handle;
    esp_err_t i2c_er = i2c_new_master_bus(&i2c_mst_config, &bus_handle);

    if (i2c_er != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus");
        return;
    }

    ESP_LOGI(TAG, "I2C bus initialized");

    lcd = init_device(LCD_ADDRESS, bus_handle);
}

_Noreturn void app_main() {
    populate_tables();

    setup_gpio();
    setup_pwm_timer();
    setup_signal_timer();

    init_i2c();

    lcd_init(lcd);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        handle_gpio_input();
    }
}