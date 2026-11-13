#include <assert.h>
#include "../main/ble_address.h"
int main(void) {
    uint8_t a[6] = {0xc0, 1, 2, 3, 4, 0x00};
    assert(rfs_ble_classify(0, a) == RFS_BLE_PUBLIC);
    assert(rfs_ble_classify(2, a) == RFS_BLE_PUBLIC);
    assert(rfs_ble_classify(1, a) == RFS_BLE_NRPA);
    a[5]=0x40; assert(rfs_ble_classify(1,a)==RFS_BLE_RPA);
    a[5]=0xc0; assert(rfs_ble_classify(1,a)==RFS_BLE_STATIC);
    assert(rfs_ble_classify(3,a)==RFS_BLE_STATIC);
    a[5]=0x80; assert(rfs_ble_classify(1,a)==RFS_BLE_UNKNOWN);
    assert(rfs_ble_classify(9,a)==RFS_BLE_UNKNOWN);
    assert(rfs_ble_filter_allows(RFS_BLE_FILTER_ALL,RFS_BLE_NRPA));
    assert(!rfs_ble_filter_allows(RFS_BLE_FILTER_STABLE,RFS_BLE_RPA));
    assert(rfs_ble_filter_allows(RFS_BLE_FILTER_STABLE,RFS_BLE_STATIC));
    assert(!rfs_ble_filter_allows(RFS_BLE_FILTER_PUBLIC,RFS_BLE_STATIC));
    assert(rfs_ble_filter_allows(RFS_BLE_FILTER_PUBLIC,RFS_BLE_PUBLIC));
    return 0;
}
