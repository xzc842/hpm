/*
 * gpio_led 驱动虚拟化程序。
 *
 * 设备：LED，输出设备。
 * 通道：power（bool, out, readonly）。
 */
#include <hpm/debug/plugin.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const HpmDebugHostApi* api;
    char name[64];
    char pin[64];
    int power;
} LedCtx;

static const char* find_key(const char* json, const char* key) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') return NULL;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static int get_string(const char* json, const char* key, char* out, size_t cap) {
    const char* p = find_key(json, key);
    if (!p || *p != '"') return -1;
    ++p;

    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) ++p;
        if (i + 1 < cap) out[i++] = *p;
        ++p;
    }
    out[i] = '\0';
    return 0;
}


static void emit_ui(LedCtx* ctx) {
    char json[768];
    snprintf(json, sizeof(json),
             "{\"$ui\":{\"schema\":\"hpm-ui/1\","
             "\"role\":\"output\",\"title\":\"LED\","
             "\"description\":\"CH552 GPIO LED\","
             "\"channels\":[{\"id\":\"power\",\"name\":\"Power\","
             "\"direction\":\"out\",\"type\":\"bool\",\"widget\":\"lamp\","
             "\"readonly\":true}],"
             "\"state\":{\"power\":%s},\"pins\":[\"%s\"],\"order\":10}}",
             ctx->power ? "true" : "false", ctx->pin);
    ctx->api->emit_json(ctx->api->host, json);
}

static void emit_state(LedCtx* ctx) {
    char json[128];
    snprintf(json, sizeof(json), "{\"$state\":{\"power\":%s}}",
             ctx->power ? "true" : "false");
    ctx->api->emit_json(ctx->api->host, json);
}

static void emit_error(LedCtx* ctx, const char* code, const char* message,
                       const char* channel, int fatal) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"$error\":{\"code\":\"%s\",\"message\":\"%s\","
             "\"channel\":\"%s\",\"fatal\":%s}}",
             code, message, channel, fatal ? "true" : "false");
    ctx->api->emit_json(ctx->api->host, json);
}

static void handle_json(LedCtx* ctx, const char* text) {
    char cmd[32] = {0};
    if (get_string(text, "cmd", cmd, sizeof(cmd)) != 0) return;

    if (strcmp(cmd, "describe") == 0) {
        emit_ui(ctx);
        return;
    }

    if (strcmp(cmd, "get") == 0) {
        emit_state(ctx);
        return;
    }

    if (strcmp(cmd, "set") == 0) {
        char channel[64] = {0};
        get_string(text, "channel", channel, sizeof(channel));
        if (strcmp(channel, "power") == 0) {
            emit_error(ctx, "readonly", "LED is output-only", "power", 0);
        } else {
            emit_error(ctx, "unknown_channel", "unknown channel", channel, 0);
        }
        return;
    }

    if (strcmp(cmd, "invoke") == 0) {
        char channel[64] = {0};
        get_string(text, "channel", channel, sizeof(channel));
        emit_error(ctx, "unsupported", "LED has no actions", channel, 0);
        return;
    }
}

uint32_t hpm_debug_abi_version(void) {
    return HPM_DEBUG_ABI_VERSION;
}

void* hpm_debug_create(const HpmDebugHostApi* api,
                       const HpmDebugDeviceInfo* info) {
    if (!api || !info) return NULL;

    LedCtx* ctx = (LedCtx*)calloc(1, sizeof(LedCtx));
    if (!ctx) return NULL;

    ctx->api = api;
    snprintf(ctx->name, sizeof(ctx->name), "%s",
             info->device ? info->device : "led");
    ctx->pin[0] = '\0';

    for (size_t i = 0; i < info->binding_count; ++i) {
        const char* key = info->bindings[i].key;
        const char* value = info->bindings[i].value;
        if (!key || !value) continue;
        if (strcmp(key, "pin") == 0 || strcmp(key, "led") == 0) {
            snprintf(ctx->pin, sizeof(ctx->pin), "%s", value);
            break;
        }
    }

    if (ctx->pin[0]) {
        uint32_t value = 0, function = 0, direction = 0;
        if (api->get_pin(api->host, ctx->pin, &value, &function,
                         &direction) == 0) {
            ctx->power = value ? 1 : 0;
        }
    }

    emit_ui(ctx);
    emit_state(ctx);
    return ctx;
}

void hpm_debug_destroy(void* handle) {
    free(handle);
}

int hpm_debug_event(void* handle, const HpmDebugEvent* ev) {
    LedCtx* ctx = (LedCtx*)handle;
    if (!ctx || !ev) return -1;

    switch (ev->type) {
        case HPM_DEBUG_EVENT_PIN:
            if (ctx->pin[0] && ev->u.pin.pin &&
                strcmp(ev->u.pin.pin, ctx->pin) == 0) {
                const int power = ev->u.pin.value ? 1 : 0;
                if (power != ctx->power) {
                    ctx->power = power;
                    ctx->api->log(ctx->api->host, HPM_DEBUG_LOG_INFO,
                                  power ? "power=on" : "power=off");
                    emit_state(ctx);
                }
            }
            break;

        case HPM_DEBUG_EVENT_JSON:
            if (ev->u.json) handle_json(ctx, ev->u.json);
            break;

        default:
            break;
    }
    return 0;
}
