#include "gpio_led.h"

#define HPM_CAT_(a, b) a##b
#define HPM_CAT(a, b)  HPM_CAT_(a, b)
#define HPM_FN(name)   HPM_CAT(DEVICE_NAME, HPM_CAT(_, name))
#define HPM_CALL(prefix, method) HPM_CAT(prefix, HPM_CAT(_, method))

int HPM_FN(init)(void) {
    HPM_CALL(pin, init)();
    HPM_CALL(pin, write)(0);
    return 0;
}

void HPM_FN(on)(void) {
    HPM_CALL(pin, write)(1);
}

void HPM_FN(off)(void) {
    HPM_CALL(pin, write)(0);
}

void HPM_FN(toggle)(void) {
    unsigned char value = (unsigned char)HPM_CALL(pin, read)();
    HPM_CALL(pin, write)(value ? 0 : 1);
}
