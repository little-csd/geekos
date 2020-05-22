#include <limits.h>
#include <geekos/errno.h>
#include <geekos/screen.h>
#include <geekos/string.h>
#include <geekos/malloc.h>
#include <geekos/ide.h>
#include <geekos/blockdev.h>
#include <geekos/bitset.h>
#include <geekos/vfs.h>
#include <geekos/list.h>
#include <geekos/synch.h>
#include <geekos/pfat16.h>

// #define DEBUG_FAT16

void Debug(char* s) {
#ifdef DEBUG_FAT16
    print(s);
#endif
}
static void test(Fat16_Fsinfo*);

/**
 * Fat16 table function
 */
static inline
short
getFatNext(Fat16_Fsinfo* info, uint_t idx) {
    return info->fat[idx];
}

static inline
void
setFatNext(Fat16_Fsinfo* info, uint_t idx, short nxt) {
    info->fat[idx] = nxt;
    Set_Bit(info->fatBitset, idx);
}

static inline
void
allocFat(Fat16_Fsinfo* info) {
    uint_t idx = Find_First_Free_Bit(info->fatBitset, info->maxFatBit);
    Set_Bit(info->fatBitset, idx);
    return idx;
}

static
void
freeFatRecursive(Fat16_Fsinfo* info, uint_t idx) {
    short* data = info->fat;
    if (data[idx]) removeFatRecursive(info, data[idx]);
    data[idx] = 0;
    Clear_Bit(info->fatBitset, idx);
}

/***
 * Mount point function
 */

struct Mount_Point_Ops* fat16_mountPointOps = {
    0,
    0,
    0,
    0,
    0,
    0
};

static
int
Fat16_Format(struct Block_Device* dev) {
    Debug("Formatting...\n");
    char buf[SECTOR_SIZE];
    memset(buf, 0, SECTOR_SIZE);
    Fat16_BootSector bSector;
    memset(&bSector, 0, sizeof(bSector));

    // write fat table
    for (int i = 1; i < FIRST_DIR_BLOCK; i++) {
        Block_Write(dev, i, buf);
    }

    bSector.sectorPerCluster = 1;
    bSector.fatCount = 2;
    bSector.sectorPerFatTable = SECTOR_PER_FATT;
    bSector.sectorSize = SECTOR_SIZE;
    memcpy(buf, &bSector, sizeof(bSector));
    Block_Write(dev, 0, buf);
    return 0;
}

static
int
Fat16_Mount(struct Mount_Point* mountPoint) {
    Debug("Mounting\n");
    uint_t rc;
    Fat16_Fsinfo* info = (Fat16_Fsinfo*)Malloc(sizeof(Fat16_Fsinfo));
    if (!info) goto fail;
    Mutex_Init(&info->mutex);
    
    Debug("Init boot sector\n");
    rc = Block_Read(mountPoint->dev, 0, info->bSector);
    if (rc) goto fail;
    
    Debug("Init FAT table\n");
    char* fat = (char*)Malloc(FAT16_TSIZE);
    if (!fat) goto fail;
    for (int i = 0; i < SECTOR_PER_FATT; i++) {
        rc = Block_Read(mountPoint->dev, i+1, fat+SECTOR_SIZE*i);
        if (rc) goto fail;
    }
    info->fat = (short*)fat;
    info->maxFatBit = MAX_SECTOR;
    info->fatBitset = Create_Bit_Set(MAX_SECTOR);
    
    Debug("Init DirEntry\n");
    char* entries = (char*)Malloc(DIR_BLOCKS*SECTOR_SIZE);
    if (!entries) goto fail;
    for (int i = 0; i < DIR_BLOCKS; i++) {
        rc = Block_Read(mountPoint->dev, i+FIRST_DIR_BLOCK, entries+SECTOR_SIZE*i);
        if (rc) goto fail;
    }
    info->entries = (DirEntry*)entries;
    info->maxEntryBit = MAX_DIR_COUNT;
    info->entryBitset = Create_Bit_Set(MAX_DIR_COUNT);
    
    mountPoint->ops = fat16_mountPointOps;
    mountPoint->fsData = info;
    test(info);
    return 0;
fail:
    if (info) Free(info);
    if (fat) Free(fat);
    if (entries) Free(entries);
    return -1;
}

/**
 * public function
*/

static
struct Filesystem_Ops fat16_FilesystemOps = {
    &Fat16_Format,
    &Fat16_Mount,
};

void
Init_Fat16() {
    Register_FileSystem("fat16", &fat16_FilesystemOps);
}

/**
 * Test Function
 */
static void test(Fat16_Fsinfo* info) {
#ifdef DEBUG_FAT16
    DirEntry* entries = info->entries;
    uint_t count = info->bSector.rootEntryCount;
    for (int i = 0; i < count; i++) {
        DirEntry* entry = entries + i;
        Print("%d:  name: %s, size: %s\n", i, entry->name, entry->size);
    }
#endif
}