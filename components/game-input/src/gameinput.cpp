// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// gameinput: the GDK input runtime the title loads by ordinal 1.
//
// The title imports GameInputCreate by ordinal (GameInput.dll!#1) and calls
// RegisterDeviceCallback(NULL, kind=0x40000, filter=1, enumeration=2, ...)
// right after, so the only export is GameInputCreate. Everything else is
// internal; a slot this file cannot back is refused with an honest error and a
// log line, never a silent success.
//
// Backing: one always-connected phantom gamepad. It is honest about being a
// phantom -- the family is GameInputFamilyVirtual and the id is a scarab
// marker, not hardware. No real state (sticks, triggers, battery, rumble,
// haptics) is backed; those slots refuse. Upgrade path: XInput hosts (xcl /
// VIctory do), then readings and a reading-callback thread.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string.h>

namespace {

constexpr uint32_t KIND_GAMEPAD = 0x40000;
// connected + input enabled + output enabled + synchronized (documented bits)
constexpr uint32_t STATUS_CONNECTED = 0x1 | 0x2 | 0x4 | 0x40;

const GUID IID_IUNKNOWN = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID IID_IGAMEINPUT = {0x11be2a7e, 0x4254, 0x445a, {0x9c, 0x09, 0xff, 0xc4, 0x0f, 0x00, 0x69, 0x18}};
const GUID IID_IGAMEINPUT_DEVICE = {0x31dd86fb, 0x4c1b, 0x408a, {0x86, 0x8f, 0x43, 0x9b, 0x3c, 0xd4, 0x71, 0x25}};

void log(const char* what, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
    printf("gameinput: %s (a=0x%x b=0x%x c=0x%x)\n", what, a, b, c);
    fflush(stdout);
}

bool guid_eq(const GUID* a, const GUID* b) {
    return a && b && a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3 &&
           memcmp(a->Data4, b->Data4, 8) == 0;
}

uint64_t now_us() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    static uint64_t origin = 0;
    static bool init = false;
    if (!init) { origin = now.QuadPart; init = true; }
    return static_cast<uint64_t>((now.QuadPart - origin) * 1000000ULL / freq.QuadPart);
}

// ---------------------------------------------------------------------------
// the device: one static phantom gamepad.
// ---------------------------------------------------------------------------

struct Device { void** vtable; };

// GameInputDeviceInfo, as far as the title is concerned: it must read the same
// fields xcl/VIctory verified at the same offsets (size, vendorId, productId,
// ..., family at 0x60, capabilities at 0x64, supportedInput at 0x68,
// supportedRumbleMotors at 0x6c, then opaque detail). No field past 0x6c is
// claimed -- that is not a layout this shim knows.
struct DeviceInfo {
    uint32_t infoSize;
    uint16_t vendorId;
    uint16_t productId;
    uint16_t revisionNumber;
    uint8_t interfaceNumber;
    uint8_t collectionNumber;
    uint16_t usagePage;
    uint16_t usageId;
    uint16_t hardwareVersion[4];
    uint16_t firmwareVersion[4];
    uint8_t deviceId[32];
    uint8_t deviceRootId[32];
    int32_t deviceFamily;
    uint32_t capabilities;
    uint32_t supportedInput;
    uint32_t supportedRumbleMotors;
    uint8_t rest[896];
};

static const DeviceInfo& device_info() {
    static const DeviceInfo info = [] {
        DeviceInfo i{};
        i.infoSize = sizeof(DeviceInfo);
        // The honest identity of a phantom: virtual family, no vendor, no
        // real id -- a scarab marker, never a hardware claim.
        memcpy(i.deviceId, "scarab-gamepad", 14);
        memcpy(i.deviceRootId, "scarab-gamepad", 14);
        i.deviceFamily = -1;  // GameInputFamilyVirtual
        i.supportedInput = KIND_GAMEPAD;
        return i;
    }();
    return info;
}

// --- the device's vtable ---------------------------------------------------

HRESULT device_qi(Device* This, const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid && (guid_eq(iid, &IID_IUNKNOWN) || guid_eq(iid, &IID_IGAMEINPUT_DEVICE))) {
        *out = This;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG device_add_ref(Device*) { return 1; }   // the device lives for the process
ULONG device_release(Device*) { return 1; }

const DeviceInfo* device_get_info(Device*) { return &device_info(); }

uint32_t device_get_status(Device*) { return STATUS_CONNECTED; }

// documented: void(state) -- we cannot know a battery; zeros means unknown
void device_get_battery_state(Device*, uint8_t* state) {
    log("IGameInputDevice::GetBatteryState (no battery on a phantom device)");
    if (state) memset(state, 0, 20);
}

HRESULT device_create_force_feedback_effect(Device*, uint32_t motor, const void*, void**) {
    log("IGameInputDevice::CreateForceFeedbackEffect (no force feedback)");
    return E_NOTIMPL;
}

bool device_is_force_feedback_motor_powered_on(Device*, uint32_t motor) {
    log("IGameInputDevice::IsForceFeedbackMotorPoweredOn");
    return false;
}

void device_set_force_feedback_motor_gain(Device*, uint32_t motor, float gain) {
    log("IGameInputDevice::SetForceFeedbackMotorGain (no force feedback)");
}

HRESULT device_set_haptic_motor_state(Device*, uint32_t motor, const void*) {
    log("IGameInputDevice::SetHapticMotorState (no haptics)");
    return E_NOTIMPL;
}

void device_set_rumble_state(Device*, const void* params) {
    // shortcut: no real pad underneath, so rumble cannot be driven at all.
    // Upgrade: XInput State call; then this tracks whatever is real.
    log("IGameInputDevice::SetRumbleState (no real pad, rumble dropped)");
}

void device_set_input_synchronization_state(Device*, bool enabled) {
    log("IGameInputDevice::SetInputSynchronizationState (no input sync)");
}

void device_send_input_synchronization_hint(Device*) {
    log("IGameInputDevice::SendInputSynchronizationHint");
}

void device_power_off(Device*) {
    log("IGameInputDevice::PowerOff (phantom device stays connected)");
}

HRESULT device_create_raw_device_report(Device*, uint32_t id, uint32_t kind, void**) {
    log("IGameInputDevice::CreateRawDeviceReport (no raw device)");
    return E_NOTIMPL;
}

HRESULT device_get_raw_device_feature(Device*, uint32_t id, void**) {
    log("IGameInputDevice::GetRawDeviceFeature (no raw device)");
    return E_NOTIMPL;
}

HRESULT device_set_raw_device_feature(Device*, void*) {
    log("IGameInputDevice::SetRawDeviceFeature (no raw device)");
    return E_NOTIMPL;
}

HRESULT device_send_raw_device_output(Device*, void*) {
    log("IGameInputDevice::SendRawDeviceOutput (no raw device)");
    return E_NOTIMPL;
}

HRESULT device_send_raw_device_output_with_response(Device*, void*, void**) {
    log("IGameInputDevice::SendRawDeviceOutputWithResponse (no raw device)");
    return E_NOTIMPL;
}

HRESULT device_execute_raw_device_io_control(Device*, uint32_t code, size_t in_size, const void*, size_t out_size, void*, size_t* size) {
    log("IGameInputDevice::ExecuteRawDeviceIoControl (no raw device)");
    return E_NOTIMPL;
}

bool device_acquire_exclusive_raw_device_access(Device*, uint64_t timeout) {
    log("IGameInputDevice::AcquireExclusiveRawDeviceAccess (no raw device)");
    return false;
}

void device_release_exclusive_raw_device_access(Device*) {
    log("IGameInputDevice::ReleaseExclusiveRawDeviceAccess");
}

static const void* DEVICE_VTBL[] = {
    (const void*)&device_qi,        // 0  QueryInterface
    (const void*)&device_add_ref,    // 1  AddRef
    (const void*)&device_release,   // 2  Release
    (const void*)&device_get_info,   // 3  GetDeviceInfo
    (const void*)&device_get_status,  // 4  GetDeviceStatus
    (const void*)&device_get_battery_state,
    (const void*)&device_create_force_feedback_effect,
    (const void*)&device_is_force_feedback_motor_powered_on,
    (const void*)&device_set_force_feedback_motor_gain,
    (const void*)&device_set_haptic_motor_state,
    (const void*)&device_set_rumble_state,
    (const void*)&device_set_input_synchronization_state,
    (const void*)&device_send_input_synchronization_hint,
    (const void*)&device_power_off,
    (const void*)&device_create_raw_device_report,
    (const void*)&device_get_raw_device_feature,
    (const void*)&device_set_raw_device_feature,
    (const void*)&device_send_raw_device_output,
    (const void*)&device_send_raw_device_output_with_response,
    (const void*)&device_execute_raw_device_io_control,
    (const void*)&device_acquire_exclusive_raw_device_access,
    (const void*)&device_release_exclusive_raw_device_access,
};

static Device& phantom_device() {
    static Device d{ const_cast<void**>(DEVICE_VTBL) };
    return d;
}

// ---------------------------------------------------------------------------
// the input object: one static instance, alive for the process.
// ---------------------------------------------------------------------------

struct Input { void** vtable; };

typedef void (*DeviceCallback)(uint64_t token, void* context, void* device,
                              uint64_t timestamp, uint32_t current_status, uint32_t previous_status);

static HRESULT input_qi(Input* This, const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid && (guid_eq(iid, &IID_IUNKNOWN) || guid_eq(iid, &IID_IGAMEINPUT))) {
        *out = This;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG input_add_ref(Input*) { return 1; }   // the object lives for the process
static ULONG input_release(Input*) { return 1; }

static uint64_t input_get_current_timestamp(Input*) { return now_us(); }

static HRESULT input_get_current_reading(Input*, uint32_t kind, void*, void**) {
    log("IGameInput::GetCurrentReading", kind);
    return E_NOTIMPL;
}
static HRESULT input_get_next_reading(Input*, void*, uint32_t kind, void*, void**) {
    log("IGameInput::GetNextReading", kind);
    return E_NOTIMPL;
}
static HRESULT input_get_previous_reading(Input*, void*, uint32_t kind, void*, void**) {
    log("IGameInput::GetPreviousReading", kind);
    return E_NOTIMPL;
}
static HRESULT input_get_temporal_reading(Input*, uint64_t timestamp, void*, void**) {
    log("IGameInput::GetTemporalReading");
    return E_NOTIMPL;
}

static uint64_t next_token() {
    static uint64_t tokens = 0;
    return ++tokens;
}

static HRESULT input_register_reading_callback(Input*, void* device, uint32_t kind, float threshold, void* context, DeviceCallback callback, uint64_t* token) {
    log("IGameInput::RegisterReadingCallback", kind);
    if (token) *token = next_token();
    // readings have no backing; never fire a reading callback
    return E_NOTIMPL;
}

static HRESULT input_register_device_callback(Input*, void* device, uint32_t kind,
                                             uint32_t filter, uint32_t enumeration,
                                             void* context, DeviceCallback callback,
                                             uint64_t* token) {
    uint64_t t = next_token();
    if (token) *token = t;
    log("IGameInput::RegisterDeviceCallback", kind, filter, enumeration);
    if (!callback) return E_INVALIDARG;
    if (kind != KIND_GAMEPAD) return E_NOTIMPL;
    if (enumeration == 1) {  // async enumeration: no thread to deliver from
        printf("gameinput: async enumeration is not supported (blocking only)\n");
        fflush(stdout);
        return E_NOTIMPL;
    }
    if (enumeration == 2) {  // blocking: deliver the device now
        if (device != nullptr && device != &phantom_device()) {
            log("IGameInput::RegisterDeviceCallback (unknown device)");
            return E_NOTIMPL;
        }
        (*callback)(t, context, &phantom_device(), now_us(), STATUS_CONNECTED, 0);
        return S_OK;
    }
    // enumeration == 0: register but never enumerate (we report no events)
    return S_OK;
}

static HRESULT input_register_system_button_callback(Input*, void* device, uint32_t filter, void* context, DeviceCallback callback, uint64_t* token) {
    log("IGameInput::RegisterSystemButtonCallback", filter);
    if (token) *token = next_token();
    return E_NOTIMPL;
}

static HRESULT input_register_keyboard_layout_callback(Input*, void* device, void* context, DeviceCallback callback, uint64_t* token) {
    log("IGameInput::RegisterKeyboardLayoutCallback");
    if (token) *token = next_token();
    return E_NOTIMPL;
}

static void input_stop_callback(Input*, uint64_t token) {
    log("IGameInput::StopCallback (nothing pending)");
}

static bool input_unregister_callback(Input*, uint64_t token, uint64_t timeout) {
    log("IGameInput::UnregisterCallback (nothing pending)");
    return true;
}

static HRESULT input_create_dispatcher(Input*, void**) {
    log("IGameInput::CreateDispatcher");
    return E_NOTIMPL;
}

static HRESULT input_create_aggregate_device(Input*, uint32_t kind, void**) {
    log("IGameInput::CreateAggregateDevice", kind);
    return E_NOTIMPL;
}

static HRESULT input_find_device_from_id(Input*, const uint8_t* id, void** deviceOut) {
    if (!deviceOut) return E_POINTER;
    *deviceOut = nullptr;
    if (!id) {
        log("IGameInput::FindDeviceFromId (null id)");
        return E_INVALIDARG;
    }
    if (memcmp(id, device_info().deviceId, 32) == 0) {
        *deviceOut = &phantom_device();
        return S_OK;
    }
    log("IGameInput::FindDeviceFromId (no such device)");
    return (HRESULT)0x838A0002;  // GAMEINPUT_E_DEVICE_NOT_FOUND
}

static HRESULT input_find_device_from_object(Input*, void* value, void**) {
    log("IGameInput::FindDeviceFromObject");
    return E_NOTIMPL;
}

static HRESULT input_find_device_from_platform_handle(Input*, void* value, void**) {
    log("IGameInput::FindDeviceFromPlatformHandle");
    return E_NOTIMPL;
}

static HRESULT input_find_device_from_platform_string(Input*, const wchar_t* value, void**) {
    log("IGameInput::FindDeviceFromPlatformString");
    return E_NOTIMPL;
}

static HRESULT input_enable_oem_device_support(Input*, uint16_t vid, uint16_t pid, uint8_t iface, uint8_t collection) {
    // no OEM features exist on this phantom device; documented no-op
    log("IGameInput::EnableOemDeviceSupport", vid, pid, iface);
    return S_OK;
}

static void input_set_focus_policy(Input*, uint32_t policy) {
    log("IGameInput::SetFocusPolicy", policy);
}

static const void* INPUT_VTBL[] = {
    (const void*)&input_qi,
    (const void*)&input_add_ref,
    (const void*)&input_release,
    (const void*)&input_get_current_timestamp,
    (const void*)&input_get_current_reading,
    (const void*)&input_get_next_reading,
    (const void*)&input_get_previous_reading,
    (const void*)&input_get_temporal_reading,
    (const void*)&input_register_reading_callback,
    (const void*)&input_register_device_callback,
    (const void*)&input_register_system_button_callback,
    (const void*)&input_register_keyboard_layout_callback,
    (const void*)&input_stop_callback,
    (const void*)&input_unregister_callback,
    (const void*)&input_create_dispatcher,
    (const void*)&input_create_aggregate_device,
    (const void*)&input_find_device_from_id,
    (const void*)&input_find_device_from_object,
    (const void*)&input_find_device_from_platform_handle,
    (const void*)&input_find_device_from_platform_string,
    (const void*)&input_enable_oem_device_support,
    (const void*)&input_set_focus_policy,
};

Input& phantom_input() {
    static Input i{ const_cast<void**>(INPUT_VTBL) };
    return i;
}

}  // namespace

extern "C" HRESULT GameInputCreate(Input** out) {
    // shortcut: exports only what the title imports and calls; no readings,
    // no device events beyond the blocking enumeration. Upgrade: reading
    // state, a reading-callback thread, real-device backing.
    if (!out) return E_POINTER;
    printf("gameinput: GameInputCreate -- one always-connected phantom gamepad\n");
    fflush(stdout);
    *out = &phantom_input();
    return S_OK;
}
