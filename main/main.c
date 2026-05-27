#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "TRAFFIC_LIGHT";

// Визначення пінів
#define LED_RED_PIN      6
#define LED_YELLOW_PIN   7
#define LED_GREEN_PIN   16
#define BUTTON_PIN       18

// Налаштування часу (в мілісекундах)

// Тривалості фаз світлофора
#define TIME_RED          5000
#define TIME_RED_YELLOW   2000
#define TIME_GREEN        4000
#define TIME_GREEN_BLINK  3000
#define TIME_YELLOW       2000
#define TIME_BLINK_FREQ   500

typedef enum {
    MODE_NORMAL,
    MODE_BLINKING_YELLOW
} system_mode_t;

typedef enum {
    STATE_RED,
    STATE_RED_YELLOW,
    STATE_GREEN,
    STATE_GREEN_BLINKING,
    STATE_YELLOW
} light_state_t;

// Глобальні змінні стану
static system_mode_t current_mode = MODE_NORMAL;
static light_state_t current_light_state = STATE_RED;
static bool blink_toggle = false;
static int blink_counter = 0;

// Хендли таймерів
static esp_timer_handle_t light_timer;

// Змінні для відстеження кнопки
#define BUTTON_POLL_INTERVAL_MS 10
#define BUTTON_DEBOUNCE_MS 50

typedef enum {
    BUTTON_RELEASED,
    BUTTON_DEBOUNCE_PRESS,
    BUTTON_PRESSED,
    BUTTON_DEBOUNCE_RELEASE
} button_state_t;

static button_state_t button_state = BUTTON_RELEASED;
// Останній стабільний (дебаунснений) рівень кнопки: 1 = released, 0 = pressed
static int last_stable_level = 1;

// Прототипи функцій
static void update_leds(void);
static void light_timer_callback(void *arg);
static void button_poll_task(void *arg);
static void toggle_mode_on_press(void);

static void set_leds(bool red, bool yellow, bool green) {
    gpio_set_level(LED_RED_PIN, red);
    gpio_set_level(LED_YELLOW_PIN, yellow);
    gpio_set_level(LED_GREEN_PIN, green);
}
static void toggle_mode_on_press(void) {
    esp_timer_stop(light_timer);
    if (current_mode == MODE_NORMAL) {
        ESP_LOGI(TAG, "Кнопка: перехід у постійний мигаючий жовтий режим.");
        current_mode = MODE_BLINKING_YELLOW;
        blink_toggle = true;
    } else {
        ESP_LOGI(TAG, "Кнопка: повернення до звичного режиму.");
        current_mode = MODE_NORMAL;
        current_light_state = STATE_RED;
    }
    update_leds();
}

static void button_poll_task(void *arg) {
    int raw_level = 1;
    int64_t now = 0;
    int64_t debounce_start = 0;

    while (true) {
        raw_level = gpio_get_level(BUTTON_PIN);
        now = esp_timer_get_time();

        /* Якщо сирий рівень кнопки та останній стабільний стан однакові,
         * немає зміни — пропускаємо обробку до наступного опитування. */
        if (raw_level == last_stable_level) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
            continue;
        }

        switch (button_state) {
            case BUTTON_RELEASED:
                if (raw_level == 0) {
                    button_state = BUTTON_DEBOUNCE_PRESS;
                    debounce_start = now;
                }
                break;

            case BUTTON_DEBOUNCE_PRESS:
                if (raw_level == 0) {
                    if (now - debounce_start >= (BUTTON_DEBOUNCE_MS * 1000LL)) {
                        button_state = BUTTON_PRESSED;
                        /* Зафіксували натискання — дія один раз тут */
                        last_stable_level = 0;
                        toggle_mode_on_press();
                    }
                } else {
                    button_state = BUTTON_RELEASED;
                }
                break;

            case BUTTON_PRESSED:
                if (raw_level == 0) {
                    /* Залишаємо стан натискання — не виконувати дію до відпускання */
                } else {
                    button_state = BUTTON_DEBOUNCE_RELEASE;
                    debounce_start = now;
                }
                break;

            case BUTTON_DEBOUNCE_RELEASE:
                if (raw_level == 1) {
                    if (now - debounce_start >= (BUTTON_DEBOUNCE_MS * 1000LL)) {
                        /* Підтверджене відпускання — просто повертаємо стан */
                        button_state = BUTTON_RELEASED;
                        last_stable_level = 1;
                    }
                } else {
                    button_state = BUTTON_PRESSED;
                }
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
    }
}

// 3. Скінченний автомат світлофора (State Machine)
static void update_leds(void) {
    if (current_mode == MODE_BLINKING_YELLOW) {
        set_leds(false, blink_toggle, false);
        blink_toggle = !blink_toggle;
        esp_timer_start_once(light_timer, TIME_BLINK_FREQ * 1000);
        return;
    }

    switch (current_light_state) {
        case STATE_RED:
            set_leds(true, false, false);
            current_light_state = STATE_RED_YELLOW;
            esp_timer_start_once(light_timer, TIME_RED * 1000);
            break;

        case STATE_RED_YELLOW:
            set_leds(true, true, false);
            current_light_state = STATE_GREEN;
            esp_timer_start_once(light_timer, TIME_RED_YELLOW * 1000);
            break;

        case STATE_GREEN:
            set_leds(false, false, true);
            current_light_state = STATE_GREEN_BLINKING;
            blink_counter = 0;
            blink_toggle = true;
            esp_timer_start_once(light_timer, TIME_GREEN * 1000);
            break;

        case STATE_GREEN_BLINKING:
            set_leds(false, false, blink_toggle);
            blink_toggle = !blink_toggle;
            blink_counter += TIME_BLINK_FREQ;

            if (blink_counter >= TIME_GREEN_BLINK) {
                current_light_state = STATE_YELLOW;
                esp_timer_start_once(light_timer, TIME_BLINK_FREQ * 1000);
            } else {
                esp_timer_start_once(light_timer, TIME_BLINK_FREQ * 1000);
            }
            break;

        case STATE_YELLOW:
            set_leds(false, true, false);
            current_light_state = STATE_RED;
            esp_timer_start_once(light_timer, TIME_YELLOW * 1000);
            break;
    }
}

static void light_timer_callback(void *arg) {
    update_leds();
}

void app_main(void) {
    // Налаштування LED пінів
    gpio_config_t io_conf = {
        .pin_bit_mask = ((1ULL << LED_RED_PIN) | (1ULL << LED_YELLOW_PIN) | (1ULL << LED_GREEN_PIN)),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // Налаштування кнопки в режимі input з підтягуванням
    io_conf.pin_bit_mask = (1ULL << BUTTON_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    // Створення таймера для світлофора
    const esp_timer_create_args_t light_timer_args = {
        .callback = &light_timer_callback,
        .name = "light_timer"
    };
    esp_timer_create(&light_timer_args, &light_timer);

    xTaskCreate(button_poll_task, "button_poll", 2048, NULL, 5, NULL);

    // Старт програми
    ESP_LOGI(TAG, "Світлофор запущено");
    update_leds();
}