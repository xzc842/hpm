/*
 * Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * HPM 设备虚拟化插件 ABI。
 *
 * 驱动包里的 debug/debug.c 用 C 编写，导出：
 *   uint32_t hpm_debug_abi_version(void);
 *   void*    hpm_debug_create(const HpmDebugHostApi*,
 *                             const HpmDebugDeviceInfo*);
 *   void     hpm_debug_destroy(void* ctx);
 *   int      hpm_debug_event(void* ctx, const HpmDebugEvent*);
 *
 * 插件只依赖本头文件，不链接 hpm/mcus 的库。
 */

#ifndef HPM_DEBUG_PLUGIN_H
#define HPM_DEBUG_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPM_DEBUG_ABI_VERSION 3

typedef struct {
    const char* key;
    const char* value;
} HpmDebugKv;

typedef struct {
    const char* device;        /* 用户设备名，如 "led" */
    const char* driver;        /* 驱动名 */
    const HpmDebugKv* config;  /* 驱动 config 打平后的键值 */
    size_t config_count;
    const HpmDebugKv* bindings; /* 资源 key -> 引脚/实例，已展开 */
    size_t binding_count;
} HpmDebugDeviceInfo;

typedef enum {
    HPM_DEBUG_LOG_TRACE = 0,
    HPM_DEBUG_LOG_DEBUG = 1,
    HPM_DEBUG_LOG_INFO = 2,
    HPM_DEBUG_LOG_WARN = 3,
    HPM_DEBUG_LOG_ERROR = 4
} HpmDebugLogLevel;

typedef enum {
    HPM_DEBUG_EVENT_PIN = 0,
    HPM_DEBUG_EVENT_MMIO = 1,
    HPM_DEBUG_EVENT_BUS = 2,
    HPM_DEBUG_EVENT_IRQ = 3,
    HPM_DEBUG_EVENT_JSON = 4,
    HPM_DEBUG_EVENT_USB = 5
} HpmDebugEventType;

typedef struct {
    uint64_t time;
    uint32_t pin_id;
    const char* pin;      /* 归一化后的引脚名，如 "P1.1" */
    uint32_t value;
    uint32_t function;
    uint32_t direction;
    uint32_t changed;     /* 1=value, 2=function, 4=direction */
} HpmDebugPinEvent;

typedef struct {
    uint64_t time;
    const char* space;
    uint32_t addr;
    uint32_t value;
    uint8_t width;
    uint8_t access;       /* 0=read, 1=write */
} HpmDebugMmioEvent;

typedef struct {
    uint64_t time;
    uint32_t bus_id;
    uint32_t xfer_id;
    uint8_t kind;         /* BusEventKind */
    uint8_t direction;    /* 0=to host, 1=to mcu */
    uint8_t flags;
    uint16_t len;
    const uint8_t* data;
} HpmDebugBusEvent;

typedef struct {
    uint64_t time;
    uint32_t xfer_id;
    uint16_t len;
    uint8_t ep;
    uint8_t token;
    uint8_t handshake;
    const uint8_t* data;
} HpmDebugUsbEvent;

typedef struct {
    uint64_t time;
    uint32_t number;
    uint32_t pending;
} HpmDebugIrqEvent;

typedef struct {
    uint32_t type;        /* HpmDebugEventType */
    union {
        HpmDebugPinEvent pin;
        HpmDebugMmioEvent mmio;
        HpmDebugBusEvent bus;
        HpmDebugUsbEvent usb;
        HpmDebugIrqEvent irq;
        const char* json; /* HPM_DEBUG_EVENT_JSON：外部输入 JSON 字符串 */
    } u;
} HpmDebugEvent;

typedef struct HpmDebugHostApi {
    uint32_t abi_version;
    void* host;

    /* 调试日志。CLI 下打印 [device] msg；JSON 模式下转为 log 事件。 */
    void (*log)(void* host, int level, const char* message);

    /* JSON 输出。必须是 JSON object 字符串。 */
    void (*emit_json)(void* host, const char* json);

    /* 读引脚当前快照。只允许读该设备绑定过的引脚。 */
    int (*get_pin)(void* host, const char* pin,
                   uint32_t* value, uint32_t* function,
                   uint32_t* direction);

    /* 驱动芯片输入引脚。 */
    int (*set_pin)(void* host, const char* pin, uint32_t value);

    /* 向芯片注入 MMIO 写。 */
    int (*inject_mmio)(void* host, const char* space,
                       uint32_t addr, uint32_t value, uint8_t width);

    /* 触发中断。 */
    int (*raise_irq)(void* host, uint32_t irq);

    /* 总线响应。xfer_id 对应收到的 SPI BUS 事件。 */
    int (*bus_write)(void* host, uint32_t xfer_id,
                     const uint8_t* data, size_t len, uint32_t flags);

    /* 通用总线发送：UART RX、SPI 响应、I2C 等。
     * direction: 0=ToHost, 1=ToMcu；kind 见 mcus::BusEventKind。 */
    int (*bus_send)(void* host, uint32_t bus_id, uint32_t xfer_id,
                    uint8_t kind, uint8_t direction,
                    const uint8_t* data, size_t len, uint8_t flags);

    /* USB 响应/请求。 */
    int (*usb_send)(void* host, uint32_t xfer_id, uint8_t ep,
                    uint8_t token, uint8_t handshake,
                    const uint8_t* data, size_t len);

    /* 在芯片输入引脚上产生一个脉冲。
     * host 会暂停 CPU，按 width_ns 换算虚拟时间步进，设置 active，
     * 到达目标后设置 idle，再恢复原运行状态。
     * 精度为一条指令边界：实际宽度会向上取整到 cycles_per_instruction。
     * 返回 0 成功，-1 引脚不存在/模拟器未就绪。 */
    int (*pulse_pin)(void* host, const char* pin,
                     uint32_t active_value, uint32_t idle_value,
                     uint64_t width_ns);
} HpmDebugHostApi;

uint32_t hpm_debug_abi_version(void);
void* hpm_debug_create(const HpmDebugHostApi* api,
                       const HpmDebugDeviceInfo* info);
void hpm_debug_destroy(void* ctx);
int hpm_debug_event(void* ctx, const HpmDebugEvent* ev);

typedef uint32_t (*HpmDebugAbiVersionFn)(void);
typedef void* (*HpmDebugCreateFn)(const HpmDebugHostApi*,
                                  const HpmDebugDeviceInfo*);
typedef void (*HpmDebugDestroyFn)(void*);
typedef int (*HpmDebugEventFn)(void*, const HpmDebugEvent*);

#ifdef __cplusplus
}
#endif

#endif /* HPM_DEBUG_PLUGIN_H */
