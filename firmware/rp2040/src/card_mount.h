#pragma once
#include "ff.h"
#include "protocol.h"

inline galaxy::Status card_mount_status(FRESULT result,BYTE filesystem) {
    if(result==FR_OK) return filesystem==FS_FAT32?galaxy::OK:galaxy::BAD_FILESYSTEM;
    if(result==FR_NO_FILESYSTEM) return galaxy::BAD_FILESYSTEM;
    if(result==FR_NOT_READY) return galaxy::NO_CARD;
    return galaxy::IO_ERROR;
}
