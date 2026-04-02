#include "button_handler.h"
#include "esp_log.h"

static void button_task(void *arg) {
  int last = 1;

  while (1) {
    int cur = gpio_get_level(BUTTON_GPIO);

        last = cur;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void gpio_init_all() {
  gpio_config_t led = {.pin_bit_mask = 1ULL << LED_GPIO,
                       .mode = GPIO_MODE_OUTPUT};
  gpio_config(&led);

  gpio_config_t btn = {.pin_bit_mask = 1ULL << BUTTON_GPIO,
                       .mode = GPIO_MODE_INPUT,
                       .pull_up_en = GPIO_PULLUP_ENABLE};
  gpio_config(&btn);
  xTaskCreate(button_task, "button", 4096, NULL, 4, NULL);
}

void blink_led(int n, int d) {
  while (n--) {
    gpio_set_level(LED_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(d));
    gpio_set_level(LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(d));
  }
}

void turn_on_off_led(bool on_off) {
  uint32_t level = on_off ? 1 : 0;
  gpio_set_level(LED_GPIO, level);
}
