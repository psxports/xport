#ifndef XPORT_TRACE_H
#define XPORT_TRACE_H

#include "xport.h"
#include "xport.h"

typedef enum XportMemoryRegion
{
    XPORT_MEMORY_NONE = 0,
    XPORT_MEMORY_DRAM = 1,
    XPORT_MEMORY_SCRATCHPAD = 2,
    XPORT_MEMORY_BIOS = 3,
    XPORT_MEMORY_MMIO = 4
} XportMemoryRegion;

typedef enum XportMemoryOperation
{
    XPORT_MEMORY_READ = 1,
    XPORT_MEMORY_WRITE = 2,
    XPORT_MEMORY_COPY = 3,
    XPORT_MEMORY_FILL = 4
} XportMemoryOperation;

typedef enum XportMemoryError
{
    XPORT_MEMORY_ERROR_NONE = 0,
    XPORT_MEMORY_ERROR_RANGE = 1,
    XPORT_MEMORY_ERROR_READONLY = 2,
    XPORT_MEMORY_ERROR_UNBOUND_MMIO = 3,
    XPORT_MEMORY_ERROR_ARGUMENT = 4
} XportMemoryError;

typedef enum XportMemoryUpdate
{
    XPORT_MEMORY_UPDATE_ADD = 1,
    XPORT_MEMORY_UPDATE_SUBTRACT = 2,
    XPORT_MEMORY_UPDATE_MULTIPLY = 3,
    XPORT_MEMORY_UPDATE_DIVIDE = 4,
    XPORT_MEMORY_UPDATE_REMAINDER = 5,
    XPORT_MEMORY_UPDATE_AND = 6,
    XPORT_MEMORY_UPDATE_OR = 7,
    XPORT_MEMORY_UPDATE_XOR = 8,
    XPORT_MEMORY_UPDATE_SHIFT_LEFT = 9,
    XPORT_MEMORY_UPDATE_SHIFT_RIGHT = 10
} XportMemoryUpdate;

typedef sint32 (*XportMemoryMmioRead)(void *user, uint32 physical_address, uint32 width, uint32 *value);
typedef sint32 (*XportMemoryMmioWrite)(void *user, uint32 physical_address, uint32 width, uint32 value, uint32 byte_mask);
typedef void (*XportMemoryObserver)(void *user, XportMemoryOperation operation, XportMemoryRegion region, uint32 offset, size_t width, uint32 before, uint32 after, uint32 byte_mask);

typedef enum XportMemoryRefKind
{
    XPORT_MEMORY_REF_HOST = 1,
    XPORT_MEMORY_REF_GUEST = 2
} XportMemoryRefKind;

typedef struct XportMemoryRef
{
    XportMemoryRefKind kind;
    void *host_pointer;
    uint32 guest_address;
} XportMemoryRef;

typedef enum MEMORY_REGION
{
    MEMORY_REGION_DRAM = 0,
    MEMORY_REGION_SCRATCHPAD = 1
} MEMORY_REGION;

typedef enum MEMORY_EXCEPTION_KIND
{
    MEMORY_EXCEPTION_GUEST_STACK = 0,
    MEMORY_EXCEPTION_FIRMWARE_RUNTIME = 1,
    MEMORY_EXCEPTION_TRUSTED_DEVICE_STATE = 2
} MEMORY_EXCEPTION_KIND;

typedef struct MEMORY_EXCEPTION
{
    uint32 region;
    uint32 offset;
    uint32 size;
    uint32 kind;
} MEMORY_EXCEPTION;

#if defined(__cplusplus)
extern "C"
{
#endif

    void xport_memory_init(uint8 *dram, size_t dram_size, uint8 *scratchpad, size_t scratchpad_size, const uint8 *bios, size_t bios_size);
    void xport_memory_bind_mmio(uint32 first, uint32 size, XportMemoryMmioRead read, XportMemoryMmioWrite write, void *user);
    void xport_memory_bind_bios(const uint8 *bios, size_t bios_size);
    void xport_memory_bind_observer(XportMemoryObserver observer, void *user);
    XportMemoryError xport_memory_error();
    void xport_memory_clear_error();
    sint32 xport_memory_pointer_identity(const void *pointer, size_t length, XportMemoryRegion *region, uint32 *offset);

    void *xport_guest_ptr(uint32 address, size_t length);
    const void *xport_guest_cptr(uint32 address, size_t length);
    sint32 xport_update_u8(uint32 address, XportMemoryUpdate operation, uint32 value);
    sint32 xport_update_u16(uint32 address, XportMemoryUpdate operation, uint32 value);
    sint32 xport_update_u32(uint32 address, XportMemoryUpdate operation, uint32 value);

    uint8 xport_load_u8(const void *pointer);
    uint16 xport_load_le16(const void *pointer);
    uint32 xport_load_le32(const void *pointer);
    sint32 xport_store_u8(void *pointer, uint32 value);
    sint32 xport_store_le16(void *pointer, uint32 value);
    sint32 xport_store_le32(void *pointer, uint32 value);

    XportMemoryRef xport_host_ref(void *pointer);
    XportMemoryRef xport_guest_ref(uint32 address);
    sint32 xport_guest_copy(XportMemoryRef destination, XportMemoryRef source, size_t length);
    sint32 xport_guest_fill(uint32 destination, uint8 value, size_t length);
    extern const MEMORY_EXCEPTION xport_memory_exceptions[];
    extern const uint32 xport_memory_exception_count;
    extern const MEMORY_EXCEPTION game_memory_exceptions[];
    extern const uint32 game_memory_exception_count;

#if defined(__cplusplus)
}
#endif

#if defined(XPORT_MEMORY_DIAGNOSTICS)
#endif
#define XPORT_MEMORY_OBSERVER_MAX_WATCHES 64u
#define XPORT_MEMORY_WRITE_RING_CAPACITY 128u

typedef enum XportMemoryEventFlags
{
    XPORT_MEMORY_EVENT_BEFORE_VALID = 1u,
    XPORT_MEMORY_EVENT_AFTER_VALID = 2u,
    XPORT_MEMORY_EVENT_SOURCE_VALID = 4u,
    XPORT_MEMORY_EVENT_SAME_VALUE = 8u
} XportMemoryEventFlags;

typedef struct XportMemorySourceLocation
{
    const char *file;
    const char *function;
    const char *context;
    uint32 line;
} XportMemorySourceLocation;

typedef struct XportMemoryWatch
{
    XportMemoryRegion region;
    uint32 offset;
    size_t length;
} XportMemoryWatch;

typedef struct XportMemoryEvent
{
    uint64 sequence;
    XportMemoryOperation operation;
    XportMemoryRegion region;
    uint32 offset;
    size_t width;
    uint32 before;
    uint32 after;
    uint32 byte_mask;
    uint32 flags;
    XportMemorySourceLocation source;
} XportMemoryEvent;

typedef struct MEMORY_WRITE_RING
{
    XportMemoryEvent entries[XPORT_MEMORY_WRITE_RING_CAPACITY];
    uint64 next;
    uint64 dropped;
    uint32 count;
} MEMORY_WRITE_RING;

typedef void (*XportMemoryEventSink)(void *user, const XportMemoryEvent *event);

typedef struct XportMemoryObserverState
{
    XportMemoryWatch watches[XPORT_MEMORY_OBSERVER_MAX_WATCHES];
    uint32 watch_count;
    uint64 next_sequence;
    uint64 emitted;
    uint64 filtered;
    XportMemoryEventSink sink;
    void *sink_user;
    XportMemorySourceLocation source;
    uint32 source_depth;
} XportMemoryObserverState;

#if defined(__cplusplus)
extern "C"
{
#endif

    void xport_memory_observer_init(XportMemoryObserverState *state, XportMemoryEventSink sink, void *user);
    sint32 xport_memory_observer_watch(XportMemoryObserverState *state, XportMemoryRegion region, uint32 offset, size_t length);
    void xport_memory_observer_clear_watches(XportMemoryObserverState *state);
    void xport_memory_observer_attach(XportMemoryObserverState *state);
    void xport_memory_observer_detach(XportMemoryObserverState *state);
    void xport_memory_write_ring_init(MEMORY_WRITE_RING *ring);
    void xport_memory_write_ring_capture(void *user, const XportMemoryEvent *event);
    const XportMemoryEvent *xport_memory_write_ring_at(const MEMORY_WRITE_RING *ring, uint32 index);

    sint32 w_u8_at(uint32 address, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 w_u16_at(uint32 address, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 w_u32_at(uint32 address, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 w_masked_u32_at(uint32 address, uint32 value, uint32 byte_mask, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 xport_update_u8_at(uint32 address, XportMemoryUpdate operation, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 xport_update_u16_at(uint32 address, XportMemoryUpdate operation, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 xport_update_u32_at(uint32 address, XportMemoryUpdate operation, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 xport_store_le16_at(void *pointer, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 xport_store_le32_at(void *pointer, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 xport_guest_copy_at(XportMemoryRef destination, XportMemoryRef source, size_t length, const char *file, uint32 line, const char *function, const char *semantic_context);
    sint32 xport_guest_fill_at(uint32 destination, uint8 value, size_t length, const char *file, uint32 line, const char *function, const char *semantic_context);

#if defined(__cplusplus)
}
#endif

#if defined(XPORT_MEMORY_DIAGNOSTICS) && !defined(XPORT_MEMORY_IMPLEMENTATION)
    #undef w_u8
    #undef w_u16
    #undef w_u32
    #undef w_masked_u32
    #define w_u8(address, value) w_u8_at((address), (value), __FILE__, __LINE__, __func__, NULL)
    #define w_u16(address, value) w_u16_at((address), (value), __FILE__, __LINE__, __func__, NULL)
    #define w_u32(address, value) w_u32_at((address), (value), __FILE__, __LINE__, __func__, NULL)
    #define w_masked_u32(address, value, mask) w_masked_u32_at((address), (value), (mask), __FILE__, __LINE__, __func__, NULL)
    #define xport_update_u8(address, operation, value) xport_update_u8_at((address), (operation), (value), __FILE__, __LINE__, __func__, NULL)
    #define xport_update_u16(address, operation, value) xport_update_u16_at((address), (operation), (value), __FILE__, __LINE__, __func__, NULL)
    #define xport_update_u32(address, operation, value) xport_update_u32_at((address), (operation), (value), __FILE__, __LINE__, __func__, NULL)
    #define xport_store_le16(pointer, value) xport_store_le16_at((pointer), (value), __FILE__, __LINE__, __func__, NULL)
    #define xport_store_le32(pointer, value) xport_store_le32_at((pointer), (value), __FILE__, __LINE__, __func__, NULL)
    #define xport_guest_copy(destination, source, length) xport_guest_copy_at((destination), (source), (length), __FILE__, __LINE__, __func__, NULL)
    #define xport_guest_fill(destination, value, length) xport_guest_fill_at((destination), (value), (length), __FILE__, __LINE__, __func__, NULL)
#endif
#include <stdint.h>

typedef enum XportAuditKind
{
    XPORT_AUDIT_MARKER = 1,
    XPORT_AUDIT_EVENT_KIND = 2,
    XPORT_AUDIT_WIP_KIND = 3,
    XPORT_AUDIT_FAILURE_KIND = 4
} XportAuditKind;

typedef struct XportAuditRecord
{
    XportAuditKind kind;
    const char *image;
    uint32_t address;
    const char *name;
    const char *function;
    const char *file;
    int line;
} XportAuditRecord;

typedef void (*XportAuditSink)(void *user, const XportAuditRecord *record);

#if defined(LOCKSTEP_DEBUG)
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define XPORT_AUDIT_RETURN_ADDRESS() ((uintptr_t)_ReturnAddress())
    #else
        #define XPORT_AUDIT_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
    #endif
#endif

#if defined(__cplusplus)
extern "C"
{
#endif
    void xport_audit_bind(XportAuditSink sink, void *user);
    void xport_audit_emit(XportAuditKind kind, const char *image, uint32_t address, const char *name, const char *function, const char *file, int line);
#if defined(LOCKSTEP_DEBUG)
    void xport_lockstep_function_marker(const char *image, uint32_t address, const char *function, const char *file, uint32_t line, uintptr_t host_return_address);
    void xport_lockstep_function_marker_args(const char *image, uint32_t address, const char *function, const char *file, uint32_t line, uint32_t return_kind, uintptr_t host_return_address, uint32_t argument_count, ...);
#endif
#if defined(__cplusplus)
}
#endif

#if defined(XPORT_ENABLE_AUDIT)
    #define XPORT_FUNCTION_MARKER(image, address) xport_audit_emit(XPORT_AUDIT_MARKER, (image), (uint32_t)(address), "function", __func__, __FILE__, __LINE__)
    #define XPORT_AUDIT_EVENT(image, address, name) xport_audit_emit(XPORT_AUDIT_EVENT_KIND, (image), (uint32_t)(address), (name), __func__, __FILE__, __LINE__)
    #define XPORT_WIP(image, address, name) xport_audit_emit(XPORT_AUDIT_WIP_KIND, (image), (uint32_t)(address), (name), __func__, __FILE__, __LINE__)
    #define XPORT_FAILURE(image, address, name) xport_audit_emit(XPORT_AUDIT_FAILURE_KIND, (image), (uint32_t)(address), (name), __func__, __FILE__, __LINE__)
#else
    #define XPORT_FUNCTION_MARKER(image, address) ((void)0)
    #define XPORT_AUDIT_EVENT(image, address, name) ((void)0)
    #define XPORT_WIP(image, address, name) ((void)0)
    #define XPORT_FAILURE(image, address, name) ((void)0)
#endif
#if defined(LOCKSTEP_DEBUG)
    #define FUNCTION_MARKER_ARGS(address, image, return_kind, argument_count, ...) xport_lockstep_function_marker_args((image), (uint32)(address), __func__, __FILE__, __LINE__, (return_kind), XPORT_AUDIT_RETURN_ADDRESS(), (argument_count), __VA_ARGS__)
#else
    #define FUNCTION_MARKER_ARGS(address, image, return_kind, argument_count, ...) FUNCTION_MARKER((address), (image))
#endif
#define FUNCTION_MARKER_ALIAS(address, image) FUNCTION_MARKER((address), (image))

typedef enum XportCallEdge
{
    XPORT_CALL_ENTER = 1,
    XPORT_CALL_EXIT = 2
} XportCallEdge;

typedef enum XportCallValueKind
{
    XPORT_CALL_VALUE_UNSPECIFIED = 0,
    XPORT_CALL_VALUE_SCALAR = 1,
    XPORT_CALL_VALUE_GUEST_POINTER = 2,
    XPORT_CALL_VALUE_HOST_POINTER = 3,
    XPORT_CALL_VALUE_OPAQUE_POINTER = 4,
    XPORT_CALL_VALUE_VOID = 5,
    XPORT_CALL_VALUE_UNSUPPORTED_POINTER = 6
} XportCallValueKind;

typedef struct XportCallValue
{
    XportCallValueKind kind;
    uint64 scalar;
    XportMemoryRegion region;
    uint32 offset;
    size_t bytes;
} XportCallValue;

typedef struct XportCallEvent
{
    uint64 sequence;
    uint64 invocation;
    uint64 parent_invocation;
    uint32 callsite;
    uint32 relation;
    XportCallEdge edge;
    const char *function_id;
    const XportCallValue *values;
    uint32 value_count;
} XportCallEvent;

typedef void (*XportCallTraceSink)(void *user, const XportCallEvent *event);

#if defined(__cplusplus)
extern "C"
{
#endif
    void xport_call_trace_bind(XportCallTraceSink sink, void *user);
    void xport_call_trace_prepare(uint32 callsite, uint32 relation);
    XportCallValue xport_call_scalar(uint64 value);
    XportCallValue xport_call_void(void);
    XportCallValue xport_call_guest_address(uint32 address, size_t bytes);
    XportCallValue xport_call_pointer(const void *pointer, size_t bytes, XportCallValueKind requested);
    void xport_call_trace_enter(const char *function_id, uint32 value_count, ...);
    void xport_call_trace_exit(const char *function_id, XportCallValue value);
#if defined(__cplusplus)
}
#endif

#define XPORT_CALL_SCALAR(value) xport_call_scalar((uint64)(value))
#define XPORT_CALL_GUEST_POINTER(value, bytes) xport_call_guest_address((uint32)(value), (bytes))
#define XPORT_CALL_HOST_POINTER(value, bytes) xport_call_pointer((const void *)(intptr)(value), (bytes), XPORT_CALL_VALUE_HOST_POINTER)
#define XPORT_CALL_OPAQUE_POINTER(value) xport_call_pointer((const void *)(intptr)(value), 0u, XPORT_CALL_VALUE_OPAQUE_POINTER)
#define XPORT_CALL_TRACE_ENTER(function_id, value_count, ...) xport_call_trace_enter((function_id), (value_count), __VA_ARGS__)
#define XPORT_CALL_TRACE_EXIT_SCALAR(function_id, value) xport_call_trace_exit((function_id), xport_call_scalar((uint64)(value)))
#define XPORT_CALL_TRACE_EXIT_VOID(function_id) xport_call_trace_exit((function_id), xport_call_void())

typedef enum XportDevice
{
    XPORT_DEVICE_DMA = 1,
    XPORT_DEVICE_GPU = 2,
    XPORT_DEVICE_SPU = 3
} XportDevice;

typedef enum XportDevicePrimitive
{
    XPORT_DEVICE_RAM_DMA = 1,
    XPORT_DEVICE_GPU_PACKET = 2,
    XPORT_DEVICE_VRAM_UPLOAD = 3,
    XPORT_DEVICE_VRAM_COPY = 4,
    XPORT_DEVICE_VRAM_FILL = 5,
    XPORT_DEVICE_GPU_RASTER = 6,
    XPORT_DEVICE_SPU_UPLOAD = 7,
    XPORT_DEVICE_SPU_DMA = 8,
    XPORT_DEVICE_SPU_CAPTURE = 9,
    XPORT_DEVICE_SPU_REVERB = 10
} XportDevicePrimitive;

typedef enum XportDevicePhase
{
    XPORT_DEVICE_CREATE = 1,
    XPORT_DEVICE_SUBMIT = 2,
    XPORT_DEVICE_FETCH = 3,
    XPORT_DEVICE_WRITE = 4,
    XPORT_DEVICE_RASTER_COMPLETE = 5,
    XPORT_DEVICE_COMPLETE = 6,
    XPORT_DEVICE_GAP = 7
} XportDevicePhase;

typedef enum XportDeviceRegion
{
    XPORT_DEVICE_REGION_NONE = 0,
    XPORT_DEVICE_REGION_RAM = 1,
    XPORT_DEVICE_REGION_VRAM = 2,
    XPORT_DEVICE_REGION_SCRATCHPAD = 3,
    XPORT_DEVICE_REGION_SPU_RAM = 4
} XportDeviceRegion;

typedef enum XportDeviceDirection
{
    XPORT_DEVICE_DIRECTION_NONE = 0,
    XPORT_DEVICE_DIRECTION_TO_DEVICE = 1,
    XPORT_DEVICE_DIRECTION_FROM_DEVICE = 2,
    XPORT_DEVICE_DIRECTION_INTERNAL = 3
} XportDeviceDirection;

typedef enum XportDeviceEvidence
{
    XPORT_DEVICE_EVIDENCE_EXACT = 1,
    XPORT_DEVICE_EVIDENCE_CANDIDATE = 2,
    XPORT_DEVICE_EVIDENCE_UNCOVERED = 3
} XportDeviceEvidence;

typedef struct XportDeviceOperation
{
    uint64 id;
    uint64 parent_id;
    XportDevice device;
    XportDevicePrimitive primitive;
    XportDeviceDirection direction;
    uint32 channel;
    uint32 command;
    uint32 active;
} XportDeviceOperation;

typedef struct XportDeviceEvent
{
    uint64 sequence;
    uint64 operation_id;
    uint64 parent_operation_id;
    uint64 frame;
    uint64 guest_clock;
    XportDevice device;
    XportDevicePrimitive primitive;
    XportDevicePhase phase;
    XportDeviceDirection direction;
    XportDeviceEvidence evidence;
    XportDeviceRegion source_region;
    XportDeviceRegion destination_region;
    uint32 source_offset;
    uint32 destination_offset;
    uint32 byte_count;
    uint32 byte_mask;
    uint32 channel;
    uint32 command;
    sint32 status;
} XportDeviceEvent;

typedef void (*XportDeviceEventSink)(void *user, const XportDeviceEvent *event);

#if defined(__cplusplus)
extern "C"
{
#endif

    void xport_device_trace_bind(XportDeviceEventSink sink, void *user);
    void xport_device_trace_set_time(uint64 frame, uint64 guest_clock);
    sint32 xport_device_trace_begin(XportDeviceOperation *operation, uint64 parent_id, XportDevice device, XportDevicePrimitive primitive, XportDeviceDirection direction, uint32 channel, uint32 command);
    sint32 xport_device_trace_stage(XportDeviceOperation *operation, XportDevicePhase phase, XportDeviceEvidence evidence, XportDeviceRegion source_region, uint32 source_offset, XportDeviceRegion destination_region, uint32 destination_offset, uint32 byte_count, uint32 byte_mask);
    sint32 xport_device_trace_complete(XportDeviceOperation *operation, sint32 status);
    sint32 xport_device_trace_gap(XportDeviceOperation *operation, uint32 reason);
    sint32 xport_device_trace_enabled(void);

#if defined(__cplusplus)
}
#endif

#if defined(LOCKSTEP_DEBUG)
    #if !defined(LOCKSTEP_DEBUG)
        #error xport_trace.h requires LOCKSTEP_DEBUG
    #endif

    #if !defined(_WIN32) || !defined(_M_IX86)
        #error LOCKSTEP_DEBUG requires Windows x86
    #endif

    #include <stddef.h>
    #include "psx.h"

    #define XPORT_LOCKSTEP_RUN_ID_LENGTH 32u
    #define XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY 96u
    #define XPORT_LOCKSTEP_DEFERRED_MAGIC 0x44504C58u
    #define XPORT_LOCKSTEP_DEFERRED_VERSION 12u
    #define XPORT_LOCKSTEP_DEFERRED_MAX_RANGES 511u
    #define XPORT_LOCKSTEP_DEFERRED_MAX_CANONICAL_RANGES 32u
    #define XPORT_LOCKSTEP_SPU_EVENT_CAPACITY 4096u

typedef enum XportLockstepSpuEventKind
{
    XPORT_LOCKSTEP_SPU_KEY_ON_LOW = 1,
    XPORT_LOCKSTEP_SPU_KEY_ON_HIGH = 2,
    XPORT_LOCKSTEP_SPU_KEY_OFF_LOW = 3,
    XPORT_LOCKSTEP_SPU_KEY_OFF_HIGH = 4,
    XPORT_LOCKSTEP_SPU_VOICE_READ = 5
} XportLockstepSpuEventKind;

typedef struct XportLockstepSpuEvent
{
    uint32 kind;
    uint32 address;
    uint32 value;
    uint32 ticks_low;
    uint32 ticks_high;
} XportLockstepSpuEvent;

typedef struct XportLockstepDeferredRange
{
    uint32 region;
    uint32 offset;
    uint32 size;
} XportLockstepDeferredRange;

typedef struct XportLockstepDeferredState
{
    uint32 magic;
    uint32 version;
    volatile uint32 sequence;
    volatile uint32 generation;
    volatile uint32 range_count;
    volatile uint32 boundary_count;
    volatile uint32 catchup_count;
    volatile uint32 open;
    volatile uint32 resolved;
    volatile uint32 canonical_count;
    XportLockstepDeferredRange ready;
    XportLockstepDeferredRange canonical[XPORT_LOCKSTEP_DEFERRED_MAX_CANONICAL_RANGES];
    XportLockstepDeferredRange ranges[XPORT_LOCKSTEP_DEFERRED_MAX_RANGES];
    volatile uint32 device_sequence;
    volatile uint32 device_ticks_low;
    volatile uint32 device_ticks_high;
    volatile uint32 spu_timeline_active;
    volatile uint32 spu_timeline_write;
    volatile uint32 spu_timeline_read;
    volatile uint32 spu_timeline_error;
    XportLockstepSpuEvent spu_events[XPORT_LOCKSTEP_SPU_EVENT_CAPACITY];
    volatile uint32 device_clock_low;
    volatile uint32 device_clock_high;
    volatile uint32 original_frame_boundary;
} XportLockstepDeferredState;

typedef enum XportLockstepMemoryError
{
    XPORT_LOCKSTEP_MEMORY_OK = 0,
    XPORT_LOCKSTEP_MEMORY_MISSING_ENVIRONMENT,
    XPORT_LOCKSTEP_MEMORY_INVALID_RUN_ID,
    XPORT_LOCKSTEP_MEMORY_INVALID_MAPPING_NAME,
    XPORT_LOCKSTEP_MEMORY_MAPPING_COLLISION,
    XPORT_LOCKSTEP_MEMORY_CREATE_FAILED,
    XPORT_LOCKSTEP_MEMORY_MAP_FAILED,
    XPORT_LOCKSTEP_MEMORY_ALREADY_INITIALIZED
} XportLockstepMemoryError;

typedef struct XportLockstepMemoryIdentity
{
    char run_id[XPORT_LOCKSTEP_RUN_ID_LENGTH + 1u];
    char dram_mapping[XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY];
    char scratchpad_mapping[XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY];
    char deferred_mapping[XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY];
    uint32 dram_size;
    uint32 scratchpad_size;
    uint32 deferred_size;
} XportLockstepMemoryIdentity;

    #if defined(__cplusplus)
extern "C"
{
    #endif
    #if defined(_MSC_VER)
    __declspec(dllexport) __declspec(noinline) void xport_lockstep_memory_ready(void);
    #else
__attribute__((visibility("default"), noinline)) void xport_lockstep_memory_ready(void);
    #endif
    sint32 xport_lockstep_memory_init(void);
    void xport_lockstep_memory_shutdown(void);
    XportLockstepMemoryError xport_lockstep_memory_error(void);
    const char *xport_lockstep_memory_error_name(XportLockstepMemoryError error);
    const XportLockstepMemoryIdentity *xport_lockstep_memory_identity(void);
    size_t xport_lockstep_device_checkpoint_size(void);
    sint32 xport_lockstep_device_checkpoint_capture(void *data, size_t size);
    sint32 xport_lockstep_device_checkpoint_restore(const void *data, size_t size);
    sint32 xport_lockstep_device_clock(uint64 *ticks);
    sint32 xport_lockstep_device_frame_boundary(void);
    sint32 xport_lockstep_spu_timeline_active(void);
    sint32 xport_lockstep_spu_event_consume(XportLockstepSpuEventKind kind, uint32 address, uint32 *value, sint32 validate_value, uint64 *ticks);
    sint32 xport_lockstep_deferred_begin(void);
    sint32 xport_lockstep_deferred_active(void);
    sint32 xport_lockstep_deferred_add(XportMemoryRegion region, uint32 offset, uint32 size);
    sint32 xport_lockstep_deferred_ready(XportMemoryRegion region, uint32 offset, uint32 size);
    sint32 xport_lockstep_deferred_canonical(XportMemoryRegion region, uint32 offset, uint32 size);
    void xport_lockstep_deferred_boundary(void);
    uint32 xport_lockstep_deferred_boundary_count(void);
    uint32 xport_lockstep_deferred_range_count(void);
    uint32 xport_lockstep_deferred_take_catchup(void);
    sint32 xport_lockstep_deferred_resolved(void);
    sint32 xport_lockstep_deferred_commit(void);
    #if defined(__cplusplus)
}
    #endif
    #if !defined(LOCKSTEP_DEBUG)
        #error xport_trace.h requires LOCKSTEP_DEBUG
    #endif

    #include <stdint.h>

    #if defined(_MSC_VER)
        #include <intrin.h>
    #endif

    #define XPORT_LOCKSTEP_BOUNDARY_CONTRACT "xport-lockstep-native-boundary-v1"
    #define XPORT_LOCKSTEP_BOUNDARY_MAX_VALUES 16u

typedef enum LOCKSTEP_BOUNDARY_KIND
{
    XPORT_LOCKSTEP_PRE_CALL = 1,
    XPORT_LOCKSTEP_VBLANK = 4,
    XPORT_LOCKSTEP_MODE_TRANSITION = 5
} LOCKSTEP_BOUNDARY_KIND;

typedef enum LOCKSTEP_MODE
{
    LOCKSTEP_MODE_DETAILED = 0,
    LOCKSTEP_MODE_PREFIX_GUARD = 1
} LOCKSTEP_MODE;

typedef struct LOCKSTEP_BOUNDARY_HIT
{
    uint32 schema;
    LOCKSTEP_BOUNDARY_KIND kind;
    uint64 sequence;
    const char *contract;
    const char *image;
    uint32 address;
    const char *function;
    const char *file;
    uint32 line;
    const XportCallValue *arguments;
    uint32 argument_count;
    XportCallValueKind return_kind;
    uintptr_t host_return_address;
    uint32 frame;
    uint32 controller_type[2];
    uint32 controller_buttons[2];
    uint32 controller_analog[2];
    PsxGteSnapshot gte;
} LOCKSTEP_BOUNDARY_HIT;

    #if defined(_MSC_VER)
        #define XPORT_LOCKSTEP_NOINLINE __declspec(noinline)
        #define XPORT_LOCKSTEP_EXPORT __declspec(dllexport)
    #else
        #define XPORT_LOCKSTEP_NOINLINE __attribute__((noinline))
        #define XPORT_LOCKSTEP_EXPORT __attribute__((visibility("default")))
    #endif

    #if defined(__cplusplus)
extern "C"
{
    #endif
    extern XPORT_LOCKSTEP_EXPORT const char xport_lockstep_boundary_contract[];
    extern XPORT_LOCKSTEP_EXPORT const volatile LOCKSTEP_BOUNDARY_HIT *xport_lockstep_last_hit;
    extern XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_mode;
    extern XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_epoch;
    extern XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_target_frame;
    XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_boundary_break(const LOCKSTEP_BOUNDARY_HIT *hit);
    XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_vblank_break(const LOCKSTEP_BOUNDARY_HIT *hit);
    XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_transition_break(const LOCKSTEP_BOUNDARY_HIT *hit);
    void xport_lockstep_function_marker(const char *image, uint32 address, const char *function, const char *file, uint32 line, uintptr_t host_return_address);
    void xport_lockstep_boundary_emit(const char *image, uint32 address, const char *function, const char *file, uint32 line, const XportCallValue *arguments, uint32 argument_count, XportCallValueKind return_kind, uintptr_t host_return_address);
    void xport_lockstep_boundary_frame(const char *image, uint32 frame, const uint32 type[2], const uint32 buttons[2], const uint32 analog[2]);
    void xport_lockstep_boundary_set_frame(uint32 frame);
    void xport_lockstep_boundary_configure(uint32 target_frame);
    void xport_lockstep_boundary_resume_from_checkpoint(uint32 next_frame);
    #if defined(__cplusplus)
}
    #endif

    #if defined(_MSC_VER)
        #define XPORT_LOCKSTEP_RETURN_ADDRESS() ((uintptr_t)_ReturnAddress())
    #else
        #define XPORT_LOCKSTEP_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
    #endif

    #define XPORT_LOCKSTEP_BOUNDARY(address, image, values, value_count, return_kind) xport_lockstep_boundary_emit((image), (uint32)(address), __func__, __FILE__, __LINE__, (values), (value_count), (return_kind), XPORT_LOCKSTEP_RETURN_ADDRESS())
    #if !defined(LOCKSTEP_DEBUG)
        #error xport_trace.h requires LOCKSTEP_DEBUG
    #endif

    #include "xport.h"

    #define XPORT_LOCKSTEP_INPUT_MAGIC "XPLSREC1"
    #define XPORT_LOCKSTEP_INPUT_VERSION 1u
    #define XPORT_LOCKSTEP_INPUT_HEADER_SIZE 64u
    #define XPORT_LOCKSTEP_INPUT_ENTRY_SIZE 28u

    #if defined(__cplusplus)
extern "C"
{
    #endif
    sint32 xport_lockstep_frame_init(const char *image);
    void xport_lockstep_frame_shutdown(void);
    sint32 xport_lockstep_frame_done(void);
    sint32 xport_lockstep_frame_begin_at(uint32 next_frame);
    uint32 xport_lockstep_frame_next(void);
    sint32 xport_lockstep_frame_resume(uint32 next_frame);
    sint32 xport_lockstep_frame_apply_watch_request(void);
    const char *xport_lockstep_frame_error(void);
    #if defined(__cplusplus)
}
    #endif
    #if !defined(LOCKSTEP_DEBUG)
        #error xport_trace.h requires LOCKSTEP_DEBUG
    #endif
    #include <setjmp.h>

    #if defined(__cplusplus)
extern "C"
{
    #endif
    extern XPORT_LOCKSTEP_EXPORT jmp_buf xport_lockstep_checkpoint_context;
    extern XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_checkpoint_valid;
    XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE sint32 xport_lockstep_checkpoint_capture(void);
    XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_checkpoint_jump(void);
    void xport_lockstep_checkpoint_bind(sint32 (*prepare)(void));
    XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_checkpoint_committed_break(void);
    void xport_lockstep_checkpoint_invalidate(void);
    void xport_lockstep_checkpoint_resumed(void);
    sint32 xport_lockstep_checkpoint_save_portable(const char *path);
    sint32 xport_lockstep_checkpoint_load_portable(const char *path, uint32 *next_frame);
    #if defined(__cplusplus)
}
    #endif

#endif

/* Diagnostic streams */
#include <stdio.h>
typedef uint8_t (*XportJournalReadByte)(void *user, uint32_t address);

typedef struct XportJournal
{
    FILE *stream;
    uint64_t sequence;
    int fields;
    int failed;
} XportJournal;

#if defined(__cplusplus)
extern "C"
{
#endif
    int xport_journal_file_sha256(const char *path, char output[65]);
    int xport_journal_open(XportJournal *journal, const char *path, const char *build, const char *executable, int continued);
    int xport_journal_begin(XportJournal *journal, const char *event);
    int xport_journal_string(XportJournal *journal, const char *name, const char *value);
    int xport_journal_u64(XportJournal *journal, const char *name, uint64_t value);
    int xport_journal_u32_hex(XportJournal *journal, const char *name, uint32_t value);
    int xport_journal_boolean(XportJournal *journal, const char *name, int value);
    int xport_journal_u32_hex_array(XportJournal *journal, const char *name, const uint32_t *values, size_t count);
    int xport_journal_hex_bytes(XportJournal *journal, const char *name, uint32_t address, size_t count, XportJournalReadByte read_byte, void *user);
    int xport_journal_end(XportJournal *journal);
    int xport_journal_close(XportJournal *journal);
#if defined(__cplusplus)
}
#endif

typedef struct XportTraceStreams
{
    FILE *phases;
    FILE *calls;
    FILE *actors;
    FILE *world;
    FILE *inputs;
} XportTraceStreams;

#if defined(__cplusplus)
extern "C"
{
#endif
    int xport_trace_env_u32(const char *name, uint32_t maximum, uint32_t *value);
    int xport_trace_open_suffix(FILE **stream, const char *base, const char *suffix, const char *mode);
    int xport_trace_open_streams(XportTraceStreams *streams, const char *phase_path, const char *calls_path, const char *output_base, const char *phase_mode, const char *calls_mode);
    int xport_trace_close_streams(XportTraceStreams *streams);
    int xport_trace_read_context(const char *checkpoint, uint32_t magic, uint32_t pc, uint32_t phase, uint32_t values[12]);
    int xport_trace_write_progress(const char *path, uint32_t sequence, const uint32_t *values, size_t count);
#if defined(__cplusplus)
}
#endif

typedef struct XportMemorySnapshotBoundary
{
    uint64 frame_id;
    uint64 guest_clock;
    uint64 event_cursor;
    uint32 phase_id;
} XportMemorySnapshotBoundary;

typedef struct XportMemorySnapshotWriter
{
    void *implementation;
} XportMemorySnapshotWriter;

#if defined(__cplusplus)
extern "C"
{
#endif
    sint32 xport_memory_snapshot_open(XportMemorySnapshotWriter *writer, const char *path, const char *canonical_manifest, uint32 keyframe_interval);
    sint32 xport_memory_snapshot_register_region(XportMemorySnapshotWriter *writer, uint32 region_id, const void *base, size_t size);
    uint64 xport_memory_snapshot_begin(XportMemorySnapshotWriter *writer, XportMemorySnapshotBoundary boundary);
    sint32 xport_memory_snapshot_region(XportMemorySnapshotWriter *writer, uint64 token, uint32 region_id);
    sint32 xport_memory_snapshot_commit(XportMemorySnapshotWriter *writer, uint64 token);
    sint32 xport_memory_snapshot_finish(XportMemorySnapshotWriter *writer);
    void xport_memory_snapshot_abort(XportMemorySnapshotWriter *writer);
    sint32 xport_memory_snapshot_register_psx_regions(XportMemorySnapshotWriter *writer);
    sint32 xport_memory_snapshot_capture_psx(XportMemorySnapshotWriter *writer, XportMemorySnapshotBoundary boundary);

#if defined(__cplusplus)
}
#endif

#endif
