#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

#define LED_GPIO 13
// #define LED_GPIO GPIO_NUM_2
#define BUTTON_GPIO GPIO_NUM_0

void turn_on_off_led(bool on_off);
void blink_led(int n, int d);

void gpio_init_all();