#pragma once

#include <stdbool.h>
#include <stdint.h>

/* NimBLE addresses are stored least significant octet first. Values 2 and 3
   denote resolved public and random identity addresses respectively. */
typedef enum {
    RFS_BLE_UNKNOWN = 0, RFS_BLE_PUBLIC, RFS_BLE_STATIC,
    RFS_BLE_RPA, RFS_BLE_NRPA
} rfs_ble_address_t;

typedef enum {
    RFS_BLE_FILTER_ALL = 0, RFS_BLE_FILTER_STABLE = 1,
    RFS_BLE_FILTER_PUBLIC = 2
} rfs_ble_filter_t;

static inline rfs_ble_address_t rfs_ble_classify(uint8_t type, const uint8_t addr[6]) {
    if (type == 0 || type == 2) return RFS_BLE_PUBLIC;
    if (type != 1 && type != 3) return RFS_BLE_UNKNOWN;
    switch (addr[5] & 0xc0) {
        case 0xc0: return RFS_BLE_STATIC;
        case 0x40: return type == 1 ? RFS_BLE_RPA : RFS_BLE_UNKNOWN;
        case 0x00: return type == 1 ? RFS_BLE_NRPA : RFS_BLE_UNKNOWN;
        default: return RFS_BLE_UNKNOWN;
    }
}

static inline bool rfs_ble_filter_allows(uint8_t filter, rfs_ble_address_t type) {
    if (filter == RFS_BLE_FILTER_ALL) return true;
    if (filter == RFS_BLE_FILTER_PUBLIC) return type == RFS_BLE_PUBLIC;
    return type == RFS_BLE_PUBLIC || type == RFS_BLE_STATIC;
}
