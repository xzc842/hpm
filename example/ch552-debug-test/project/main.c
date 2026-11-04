#include "driver.h"

static void delay(void) {
    for (volatile unsigned int i = 0; i < 5000; ++i) {
    }
}

void main(void) {
    hpm_init_all();

    while (1) {
        led_on();
        delay();
        led_off();
        delay();
    }
}
