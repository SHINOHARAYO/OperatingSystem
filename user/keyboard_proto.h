#pragma once

#define KEYBOARD_REQ_BIND_MANAGER 2ULL
#define KEYBOARD_REQ_SET_FOCUS    3ULL
#define KEYBOARD_REQ_READ         4ULL

#define KEYBOARD_EVENT_KEY 1ULL

#define KEYBOARD_KEY_RELEASE 0ULL
#define KEYBOARD_KEY_PRESS   1ULL
#define KEYBOARD_KEY_REPEAT  2ULL

#define KEYBOARD_MOD_SHIFT (1ULL << 0)
#define KEYBOARD_MOD_CTRL  (1ULL << 1)
#define KEYBOARD_MOD_ALT   (1ULL << 2)

#define KEYBOARD_EVENT_PACK(type, modifiers, value, code, character) \
    ((((uint64_t)(type) & 0xFFULL) << 56) | \
     (((uint64_t)(modifiers) & 0xFFULL) << 32) | \
     (((uint64_t)(value) & 0xFFULL) << 24) | \
     (((uint64_t)(code) & 0xFFFFULL) << 8) | \
     ((uint64_t)(uint8_t)(character)))

#define KEYBOARD_EVENT_TYPE(event) (((event) >> 56) & 0xFFULL)
#define KEYBOARD_EVENT_MODIFIERS(event) (((event) >> 32) & 0xFFULL)
#define KEYBOARD_EVENT_VALUE(event) (((event) >> 24) & 0xFFULL)
#define KEYBOARD_EVENT_CODE(event) (((event) >> 8) & 0xFFFFULL)
#define KEYBOARD_EVENT_CHAR(event) ((char)((event) & 0xFFULL))
