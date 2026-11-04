/*
 * HPM debug 示例插件：监听 P1.0，输出日志和 JSON。
 */
#include <hpm/debug/plugin.h>
#include <stdio.h>
#include <string.h>

static const HpmDebugHostApi* g_api;
static char g_name[64];

uint32_t hpm_debug_abi_version(void) {
    return HPM_DEBUG_ABI_VERSION;
}

void* hpm_debug_create(const HpmDebugHostApi* api,
                       const HpmDebugDeviceInfo* info) {
    g_api = api;
    strncpy(g_name, info->device, sizeof(g_name) - 1);
    g_name[sizeof(g_name) - 1] = '\0';
    return g_name;
}

void hpm_debug_destroy(void* ctx) {
    (void)ctx;
}

int hpm_debug_event(void* ctx, const HpmDebugEvent* ev) {
    (void)ctx;
    if (ev->type == HPM_DEBUG_EVENT_PIN) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s=%u", ev->u.pin.pin, ev->u.pin.value);
        g_api->log(g_api->host, HPM_DEBUG_LOG_INFO, msg);

        char js[160];
        snprintf(js, sizeof(js), "{\"pin\":\"%s\",\"value\":%u}",
                 ev->u.pin.pin, ev->u.pin.value);
        g_api->emit_json(g_api->host, js);
    } else if (ev->type == HPM_DEBUG_EVENT_JSON) {
        char msg[256];
        snprintf(msg, sizeof(msg), "input: %s", ev->u.json);
        g_api->log(g_api->host, HPM_DEBUG_LOG_INFO, msg);
    }
    return 0;
}
