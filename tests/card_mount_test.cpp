#include "../firmware/rp2040/src/card_mount.h"
#include <assert.h>
#include <stdio.h>
int main() {
    assert(card_mount_status(FR_OK,FS_FAT32)==galaxy::OK);
    assert(card_mount_status(FR_OK,FS_FAT16)==galaxy::BAD_FILESYSTEM);
    assert(card_mount_status(FR_OK,FS_FAT12)==galaxy::BAD_FILESYSTEM);
    assert(card_mount_status(FR_NO_FILESYSTEM,0)==galaxy::BAD_FILESYSTEM);
    assert(card_mount_status(FR_NOT_READY,0)==galaxy::NO_CARD);
    assert(card_mount_status(FR_DISK_ERR,0)==galaxy::IO_ERROR);
    assert(card_mount_status(FR_INT_ERR,0)==galaxy::IO_ERROR);
    puts("Card mount: missing media, unsupported filesystems and disk errors distinguished");
}
