#include <8051.h>
#include "driver.h"

// 约几毫秒的忙等，让 P1.0 的翻转在调试事件流里可见
static void delay(void) {
    for (volatile unsigned int i = 0; i < 5000; ++i) {
    }
}

void main(void) {
    ch552g_init();

    P1 = 0xFF;
    while (1) {
        P1_0 = !P1_0;
        delay();
    }
}
