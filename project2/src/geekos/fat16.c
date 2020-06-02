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
#include <geekos/fat16.h>

// #define DEBUG_FAT16

void Debug(char* s) {
#ifdef DEBUG_FAT16
    Print(s);
#endif
}
static void test(Fat16_Fsinfo*);
static void Block_Cache_Init();
static int Block_Read_Cache(struct Block_Device *dev, int blockNum, void *buf);
static int Block_Write_Cache(struct Block_Device *dev, int blockNum, void *buf);

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
uint_t
allocFat(Fat16_Fsinfo* info) {
    uint_t idx = Find_First_Free_Bit(info->fatBitset, info->maxFatBit);
    Set_Bit(info->fatBitset, idx);
    return idx;
}

static
void
freeFatRecursive(Fat16_Fsinfo* info, uint_t idx) {
    short* data = info->fat;
    if (data[idx]) freeFatRecursive(info, data[idx]);
    data[idx] = 0;
    Clear_Bit(info->fatBitset, idx);
}

static
void
initFatRecursive(Fat16_Fsinfo* info, uint_t idx) {
    if (Is_Bit_Set(info->fatBitset, idx)) return;
    Set_Bit(info->fatBitset, idx);
    if (info->fat[idx]) initFatRecursive(info, info->fat[idx]);
}

/***
 * Connect two file name.
 * When path start with '/', ignore prefix
 */
static
char*
connectName(char* prefix, const char* path) {
    if (!path || !prefix) return 0;
    char* buf;
    if (path[0] == '/') {
        uint_t len = strlen(path);
        buf = (char*)Malloc(len+1);
        memcpy(buf, path, len);
        buf[len] = '\0';
        return buf;
    } else {
        uint_t len1 = strlen(prefix);
        uint_t len2 = strlen(path);
        if (prefix[len1-1] == '/') {
            uint_t len = len1 + len2;
            buf = (char*)Malloc(len+1);
            memcpy(buf, prefix, len1);
            memcpy(buf+len1, path, len2);
            buf[len] = '\0';
        } else {
            uint_t len = len1 + len2 + 1;
            buf = (char*)Malloc(len+1);
            memcpy(buf, prefix ,len1);
            buf[len1] = '/';
            memcpy(buf+len1+1, path, len2);
            buf[len] = '\0';
        }
    }
    return buf;
}

static
int
dividePath(char* data, uint_t* div) {
    uint_t idx = 0;
    while (data[idx] && data[idx] != '/') idx++;
    if (!data[idx] || !data[idx+1]) return 0;
    data[idx] = '\0';
    idx++;
    uint_t count = 0;
    while (data[idx]) {
        if (!data[idx-1]) {
            if (count == MAX_DIR_DEEP) {
                return -1;
            }
            div[count++] = idx;
        }
        if (data[idx] == '/') {
            data[idx] = '\0';
        }
        idx++;
    }
    return count;
}

static
void
copyEntry(DirEntry** dst, DirEntry* src) {
    DirEntry* entry = *dst;
    if (!entry) entry = (DirEntry*)Malloc(sizeof(DirEntry));
    memcpy(entry, src, sizeof(DirEntry));
}

/***
 * Look up file in a specific mount point
 * Should be called when info->mutex is locked
 * If fa != NULL, fa and entry should be freed
 * Return
 *  -1: INVALID path
 *  0:  Find fa but no entry
 *  other: Found
 */
static
DirEntry*
File_Lookup(struct Block_Device* dev, Fat16_Fsinfo* info,
    const char* path, DirEntry** fa) {
    
    uint_t len = strlen(path);
    char* name = (char*)Malloc(len+1);
    memcpy(name, path, len);
    name[len] = '\0';
    Debug(name);

    *fa = 0;
    uint_t idxs[MAX_DIR_DEEP];
    int count = dividePath(name, idxs);
    if (count <= 0) {
        Free(name);
        return (DirEntry*)-1;
    }
#ifdef DEBUG_FAT16
    for (int i = 0; i < count; i++) {
        Print("%s\n", name[idxs[i]])
    }
#endif

    DirEntry* entry;
    uint_t idx = idxs[0];
    uint_t i;
    for (i = 0; i < MAX_DIR_COUNT; i++) {
        entry = info->entries + i;
        if (!strcmp(entry->name, name+idx)) {
            break;
        }
    }
    // direct live in root entry
    if (count == 1) {
        Free(name);
        if (i == MAX_DIR_COUNT) return (DirEntry*)-1;
        return entry;
    }
    // dive into directories
    DirEntry* entries = 0;
    for (uint_t t = 1; t < count; t++) {
        copyEntry(fa, entry);
        idx = idxs[t];
        uint_t block = entry->firstCluster;
        if (entries) Free(entries);
        entries = (DirEntry*)Malloc(SECTOR_SIZE);
        Block_Read_Cache(dev, block, entries);
        uint_t num = entry->size / sizeof(DirEntry);
        for (i = 0; i < num; i++) {
            entry = entries + i;
            if (!strcmp(entry->name, name+idx)) {
                break;
            }
        }
        if (i == num) {
            if (t == count-1) goto Lookup_failed;
            else goto Lookup_invalid;
        }
    }
    Free(name);
    DirEntry* res = 0;
    copyEntry(&res, entry);
    if (entries) Free(entries);
    res->reserved2 = 1;
    return res;
Lookup_failed:
    Free(name);
    if (entries) Free(entries);
    return 0;
Lookup_invalid:
    Free(name);
    if (entries) Free(entries);
    return (DirEntry*)-1;
}

/***
 * Read num'th DirEntry in dir
 * Remember to free entry.
 */
static
DirEntry*
Read_DirEntry(struct Block_Device* dev, short* fat, DirEntry* dir, uint_t num) {
    uint_t times = num / DIR_PER_SECTOR;
    short block = dir->firstCluster;
    for (uint_t i = 0; i < times; i++) {
        block = fat[block];
    }
    char buf[SECTOR_SIZE];
    uint_t offset = num % DIR_PER_SECTOR * sizeof(DirEntry);
    int rc = Block_Read_Cache(dev, block, buf);
    if (rc) return 0;
    DirEntry* entry = (DirEntry*)Malloc(sizeof(DirEntry));
    memcpy(entry, buf+offset, sizeof(DirEntry));
    return entry;
}

static
void
copyStat(DirEntry* entry, struct VFS_File_Stat* stat) {
    stat->size = entry->size;
    stat->isDirectory = entry->flag & IS_DIR ? 1:0;
    stat->isSetuid = 0;
    memset(stat->acls, 0, sizeof(stat->acls));
    stat->acls[0].uid = 0;
    if (entry->flag & READ_ONLY) stat->acls[0].permission = O_READ;
    else stat->acls[0].permission = O_READ|O_WRITE;
}

/***
 * File Operations
 */
static
int
FAT16_FStat(struct File *file, struct VFS_File_Stat *stat) {
    Fat16_Fileinfo* fileinfo = (Fat16_Fileinfo*)file->fsData;
    Mutex_Lock(&fileinfo->lock);
    DirEntry* entry = &fileinfo->entry;
    copyStat(entry, stat);
    Mutex_Unlock(&fileinfo->lock);
    return 0;
}

static
int
FAT16_Read(struct File *file, void *buf, ulong_t numBytes) {
    if (numBytes == 0) return 0;
    Fat16_Fileinfo* fileinfo = (Fat16_Fileinfo*)file->fsData;
    Mutex_Lock(&fileinfo->lock);
    Fat16_Fsinfo* fsinfo = (Fat16_Fsinfo*)file->mountPoint->fsData;
    struct Block_Device* dev = file->mountPoint->dev;
    DirEntry* entry = &fileinfo->entry;
    uint_t start = file->filePos;
    uint_t end = start + numBytes;
    if (end > file->endPos) {
        Mutex_Unlock(&fileinfo->lock);
        return -1;
    }
    uint_t startBlock = start / SECTOR_SIZE;
    uint_t endBlock = end / SECTOR_SIZE;
    short block = entry->firstCluster;
    short* fat = fsinfo->fat;
    for (int i = 0; i < startBlock; i++) {
        block = fat[block];
    }
    char data[SECTOR_SIZE];
    uint_t ptr = 0;
    for (int i = startBlock; i <= endBlock; i++) {
        Block_Read_Cache(dev, block, data);
        if (i != startBlock && i != endBlock) {
            memcpy(buf+ptr, data, SECTOR_SIZE);
            ptr += SECTOR_SIZE;
        } else if (i != endBlock) {
            uint_t begin = start % SECTOR_SIZE;
            memcpy(buf, data+begin, SECTOR_SIZE-begin);
            ptr = SECTOR_SIZE - begin;
        } else if (i != startBlock) {
            uint_t last = end % SECTOR_SIZE;
            memcpy(buf+ptr, data, last);
            ptr += last;
        } else {
            uint_t begin = start % SECTOR_SIZE;
            memcpy(buf, data+begin, numBytes);
            ptr = numBytes;
        }
        block = fat[block];
    }
    Mutex_Unlock(&fileinfo->lock);
    file->filePos += numBytes;
    return numBytes;
}

static
int
FAT16_Write(struct File *file, void *data, ulong_t numBytes) {
    if (numBytes == 0) return 0;
    if (!(file->mode & O_WRITE)) return -1;
    Fat16_Fileinfo* fileinfo = (Fat16_Fileinfo*)file->fsData;
    Mutex_Lock(&fileinfo->lock);
    Fat16_Fsinfo* fsinfo = (Fat16_Fsinfo*)file->mountPoint->fsData;
    struct Block_Device* dev = file->mountPoint->dev;
    DirEntry* entry = &fileinfo->entry;
    Mutex_Lock(&fsinfo->mutex);
    
    uint_t stop = entry->size;
    uint_t start = file->filePos;
    uint_t end = start + numBytes;
    uint_t startBlock = start / SECTOR_SIZE;
    uint_t endBlock = end / SECTOR_SIZE;
    uint_t ptr = 0;
    short block = entry->firstCluster;
    short* fat = fsinfo->fat;
    for (int i = 0; i < startBlock; i++) {
        block = fat[block];
    }

    char buf[SECTOR_SIZE];
    // in same block
    if (startBlock == endBlock) {
        Block_Read_Cache(dev, block, buf);
        uint_t offset = start % SECTOR_SIZE;
        memcpy(buf+offset, data, numBytes);
        Block_Write_Cache(dev, block, buf);
        goto Write_End;
    }
    // deal with start block
    Block_Read_Cache(dev, block, buf);
    uint_t offset = start % SECTOR_SIZE;
    memcpy(buf+offset, data, SECTOR_SIZE-offset);
    Block_Write_Cache(dev, block, buf);
    // block = fat[block];
    ptr = SECTOR_SIZE-offset;
    // deal with middle block
    for (int i = startBlock + 1; i < endBlock; i++) {
        if (!fat[block]) {
            block = fat[block] = allocFat(fsinfo);
        }
        Block_Write_Cache(dev, block, data+ptr);
        ptr += SECTOR_SIZE;
    }
    // deal with end block
    if (!fat[block]) {
        block = fat[block] = allocFat(fsinfo);
    }
    memset(buf, 0, SECTOR_SIZE);
    memcpy(buf, data, numBytes-ptr);
    Block_Write_Cache(dev, block, buf);
Write_End:
    Mutex_Unlock(&fsinfo->mutex);
    Mutex_Unlock(&fileinfo->lock);
    if (end > stop) entry->size = end;
    file->endPos = end;
    return 0;
}

static
int
FAT16_Seek(struct File *file, ulong_t pos) {
    Fat16_Fileinfo* fileinfo = (Fat16_Fileinfo*)file->fsData;
    Mutex_Lock(&fileinfo->lock);
    file->filePos = pos;
    Mutex_Unlock(&fileinfo->lock);
    return 0;
}

static
int
FAT16_Close(struct File *file) {
    Free(file->fsData);
    return 0;
}

static
int
FAT16_Read_Entry(struct File *dir, struct VFS_Dir_Entry *entry) {
    TODO("Unsupported\n");
    return 0;
}

static struct File_Ops fat16_FileOps = {
    FAT16_FStat,
    FAT16_Read,
    FAT16_Write,
    FAT16_Seek,
    FAT16_Close,
    FAT16_Read_Entry, /* Read_Entry */
};

/***
 * Mount point operations
 */
static
int
FAT16_Open(struct Mount_Point *mountPoint, const char *path,
    int mode, struct File **pFile) {
    Fat16_Fsinfo* info = (Fat16_Fsinfo*)mountPoint->fsData;
    struct Block_Device* dev = mountPoint->dev;
    KASSERT(dev);
    KASSERT(info);

    Mutex_Lock(&info->mutex);
    char* fullname = connectName(mountPoint->pathPrefix, path);
    uint_t name_len = strlen(fullname);
    DirEntry* father = 0;
    DirEntry* entry = File_Lookup(dev, info, fullname, &father);
    if (entry == (DirEntry*)-1) {
        entry = 0;
        goto Open_Invalid;
    }
    
    char buf[SECTOR_SIZE];
    uint_t block = 0;
    if (mode & O_CREATE) {
        if (entry) goto Open_Invalid;
        if (father) {
            // add in father
            if (!(father->flag & IS_DIR)) goto Open_Invalid;
            uint_t num = father->size / sizeof(DirEntry);
            if (num >= DIR_PER_SECTOR) goto Open_Invalid;
            block = father->firstCluster;
            uint_t rc = Block_Read_Cache(dev, block, buf);
            if (rc) goto Open_Invalid;
            entry = (DirEntry*)(buf + father->size);
        } else {
            // add in root
            uint_t rc = Find_First_Free_Bit(info->entryBitset, info->maxEntryBit);
            if (rc < 0) goto Open_Invalid;
            Set_Bit(info->entryBitset, rc);
            entry = info->entries + rc;
        }
        if (!(mode & O_WRITE)) entry->flag = 1;
        uint_t tmp = name_len;
        while (tmp > 0 && fullname[tmp-1]) {
            tmp--;
        }
        memcpy(entry->name, fullname+tmp, name_len-tmp);
        if (father) Block_Write_Cache(dev, block, buf);
    } else if (!entry) {
        goto Open_Invalid;
    }
    if (entry->flag & IS_DIR) goto Open_Invalid;

    // find entry successfully
    if (fullname) Free(fullname);
    if (father) Free(father);
    Fat16_Fileinfo* fileinfo = (Fat16_Fileinfo*)Malloc(sizeof(Fat16_Fileinfo));
    fileinfo->entry = *entry;
    Mutex_Init(&fileinfo->lock);
    *pFile = Allocate_File(&fat16_FileOps, 0, entry->size, fileinfo,
        mode, mountPoint);
    if (entry->reserved2) Free(entry);
    Mutex_Unlock(&info->mutex);
    return 0;

Open_Invalid:
    if (fullname) Free(fullname);
    if (father) Free(father);
    if (entry && entry->reserved2) Free(entry);
    Mutex_Unlock(&info->mutex);
    return -1;
}

static
int
FAT16_Create_Directory(struct Mount_Point *mountPoint, const char *path) {
   TODO("Unsupported stat\n");
   return 0;
}

static
int
FAT16_Open_Directory(struct Mount_Point *mountPoint,
    const char *path, struct File **pDir) {
    
   TODO("Unsupported stat\n");
   return 0;
}

static
int
FAT16_Stat(struct Mount_Point *mountPoint,
    const char *path, struct VFS_File_Stat *stat) {
    
    Fat16_Fsinfo* info = (Fat16_Fsinfo*)mountPoint->fsData;
    struct Block_Device* dev = mountPoint->dev;
    KASSERT(dev);
    KASSERT(info);

    Mutex_Lock(&info->mutex);
    char* fullname = connectName(mountPoint->pathPrefix, path);
    uint_t name_len = strlen(fullname);
    DirEntry* father = 0;
    DirEntry* entry = File_Lookup(dev, info, fullname, &father);
    if (entry == (DirEntry*)-1 || !entry) {
        entry = 0;
        goto Stat_Invalid;
    }
    Free(fullname);
    if (father) Free(father);
    copyStat(entry, stat);
    if (entry->reserved2) Free(entry);
    Mutex_Unlock(&info->mutex);
    return 0;
Stat_Invalid:
    if (fullname) Free(fullname);
    if (father) Free(father);
    if (entry && entry->reserved2) Free(entry);
    Mutex_Unlock(&info->mutex);
    return -1;
}

static
int
FAT16_Sync(struct Mount_Point *mountPoint) {
    Fat16_Fsinfo* info = (Fat16_Fsinfo*)mountPoint->fsData;
    struct Block_Device* dev = mountPoint->dev;
    Mutex_Lock(&info->mutex);
    Block_Write(dev, 0, &info->bSector);
    char* fat = (char*)info->fat;
    for (int i = 0; i < SECTOR_PER_FATT; i++) {
        Block_Write(dev, i+1, fat+i*SECTOR_SIZE);
    }
    for (int i = 0; i < SECTOR_PER_FATT; i++) {
        Block_Write(dev, i+1+SECTOR_PER_FATT, fat+FAT16_TSIZE+i*SECTOR_SIZE);
    }
    char* ent = (char*)info->entries;
    for (int i = 0; i < DIR_BLOCKS; i++) {
        Block_Write(dev, i+FIRST_DIR_BLOCK, ent+i*SECTOR_SIZE);
    }
    Mutex_Unlock(&info->mutex);
    return 0;
}

static
int
FAT16_Delete(struct Mount_Point *mountPoint, const char *path) {
   TODO("Unsupported stat\n");
   return 0;
}

struct Mount_Point_Ops fat16_mountPointOps = {
    FAT16_Open,
    FAT16_Create_Directory,
    FAT16_Open_Directory,
    FAT16_Stat,
    FAT16_Sync,
    FAT16_Delete
};

static
int
FAT16_Format(struct Block_Device* dev) {
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

// TODO: Support non-root device mount
static
int
FAT16_Mount(struct Mount_Point* mountPoint) {
    Debug("Mounting\n");
    uint_t rc;
    char* fat;
    char* entries;
    Fat16_Fsinfo* info = (Fat16_Fsinfo*)Malloc(sizeof(Fat16_Fsinfo));
    if (!info) goto fail;
    Block_Cache_Init();
    Mutex_Init(&info->mutex);
    
    Debug("Init boot sector\n");
    char buf[SECTOR_SIZE];
    struct Block_Device* dev = mountPoint->dev;
    rc = Block_Read_Cache(dev, 0, buf);
    if (rc) goto fail;
    memcpy(&info->bSector, buf, sizeof(Fat16_BootSector));
    Print("Read: file count: %d\n", info->bSector.rootEntryCount);
    
    Debug("Init FAT table\n");
    fat = (char*)Malloc(FAT16_TSIZE);
    if (!fat) goto fail;
    for (uint_t i = 0; i < SECTOR_PER_FATT; i++) {
        // Print("%d: %p\n", i, dev);
        rc = Block_Read_Cache(dev, i+1, fat+SECTOR_SIZE*i);
        if (rc) goto fail;
    }
    info->fat = (short*)fat;
    info->maxFatBit = MAX_SECTOR;
    info->fatBitset = Create_Bit_Set(MAX_SECTOR);
    // init fat bitset
    for (uint_t i = 0; i < MAX_SECTOR; i++) {
        if (Is_Bit_Set(info->fatBitset, i)) continue;
        if (info->fat[i]) initFatRecursive(info, i);
    }
    
    Debug("Init DirEntry\n");
    entries = (char*)Malloc(DIR_BLOCKS*SECTOR_SIZE);
    if (!entries) goto fail;
    for (uint_t i = 0; i < DIR_BLOCKS; i++) {
        rc = Block_Read_Cache(dev, i+FIRST_DIR_BLOCK, entries+SECTOR_SIZE*i);
        if (rc) goto fail;
    }
    info->entries = (DirEntry*)entries;
    info->maxEntryBit = MAX_DIR_COUNT;
    info->entryBitset = Create_Bit_Set(MAX_DIR_COUNT);
    // init direntry bitset
    for (uint_t i = 0; i < MAX_DIR_COUNT; i++) {
        DirEntry* entry = info->entries + i;
        if (entry->name[0]) Set_Bit(info->entryBitset, i);
    }

    mountPoint->ops = &fat16_mountPointOps;
    mountPoint->fsData = info;
    Print("Mount at fat16 filesystem successfully!\n");
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
    &FAT16_Format,
    &FAT16_Mount,
};

void
Init_Fat16() {
    Register_Filesystem("fat16", &fat16_FilesystemOps);
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
        Print("%d:  name: %s, size: %d\n", i, entry->name, entry->size);
    }
    int* buf = (int*) info->entryBitset;
    for (int i = 0; i < 5; i++) {
        Print("%p\t", buf[i]);
    }
    buf = (int*) info->fatBitset;
    for (int i = 0; i < 30; i++) {
        if (!i%5) Print("\n");
        Print("%p\t", buf[i]);
    }
#endif
}

/**
 * Block Cache
 */
#define MAX_CACHE 200
typedef struct {
    struct Block_Device* dev;
    uint_t blockNum;
    uint_t stamp;
    void* data;
} BlockCache;
static BlockCache blockCache[MAX_CACHE];
static struct Mutex blockCacheLock;

static
void
Block_Cache_Init() {
    if (blockCache[0].data) return;
    for (int i = 0; i < MAX_CACHE; i++) {
        blockCache[i].blockNum = -1;
        blockCache[i].dev = 0;
        blockCache[i].stamp = 0;
        blockCache[i].data = Malloc(SECTOR_SIZE);
    }
    Mutex_Init(&blockCacheLock);
}

static
int
Block_Read_Cache(struct Block_Device *dev, int blockNum, void *buf) {
    Mutex_Lock(&blockCacheLock);
    static uint_t stamp = 0;
    stamp++;
    uint_t min_stamp = 0x7fffffff;
    int idx = 0;
    for (int i = 0; i < MAX_CACHE; i++) {
        if (blockNum == blockCache[i].blockNum && dev == blockCache[i].dev) {
            memcpy(buf, blockCache[i].data, SECTOR_SIZE);
            blockCache[i].stamp = stamp;
            return 0;
        }
        if (blockCache[i].stamp < min_stamp) {
            min_stamp = blockCache[i].stamp;
            idx = i;
        }
    }
    // blockCache[idx].stamp = stamp;
    int rc = Block_Read(dev, blockNum, blockCache[idx].data);
    if (rc) {
        Mutex_Unlock(&blockCache);
        return rc;
    }
    blockCache[idx].dev = dev;
    blockCache[idx].stamp = stamp;
    blockCache[idx].blockNum = blockNum;
    memcpy(buf, blockCache[idx].data, SECTOR_SIZE);
    Mutex_Unlock(&blockCacheLock);
    return 0;
}

static
int
Block_Write_Cache(struct Block_Device *dev, int blockNum, void *buf) {
    Mutex_Lock(&blockCacheLock);
    for (int i = 0; i < MAX_CACHE; i++) {
        if (blockNum == blockCache[i].blockNum && dev == blockCache[i].dev) {
            memcpy(blockCache[i].data, buf, SECTOR_SIZE);
            break;
        }
    }
    Block_Write(dev, blockNum, buf);
    Mutex_Unlock(&blockCacheLock);
    return 0;
}
