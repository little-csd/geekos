#ifndef GEEKOS_FAT16_H
#define GEEKOS_FAT16_H

#define SECTOR_SIZE 512
#define MAX_SECTOR (1<<16)
#define FAT16_TSIZE (MAX_SECTOR*sizeof(short))
#define SECTOR_PER_FATT (FAT16_TSIZE/SECTOR_SIZE)
#define FIRST_DIR_BLOCK (1+2*SECTOR_PER_FATT)
#define DIR_BLOCKS 32
#define MAX_DIR_COUNT (DIR_BLOCKS*SECTOR_SIZE/sizeof(DirEntry))

/**
 * FAT16 structure
 * 1. Boot sector (512 Bytes)
 * 2. FAT Table1 (65536 * 2 Bytes)
 * 3. FAT Table2 (same as Table1)
 * 4. Root DirEntry Area (DIR_BLOCKS * SECTOR_SIZE Bytes)
 * 5. Files
 * 6. Unused Area
 */

// ignore some unimportant params
typedef struct {
    short sectorSize;       // equal to SECTOR_SIZE
    char sectorPerCluster;  // equal to 4
    short reservedSector;   // always 0
    char fatCount;          // unused
    short rootEntryCount;   // DirEntries in root
    short unknown;
    char mediaDesc;         // unused
    short sectorPerFatTable;// unused
} __attribute__ ((packed)) Fat16_BootSector;

typedef struct {
    char name[8+3];
    char flag;
    unsigned long reserved1; // Reserved words
    unsigned short reseverd2; // Reserved words
    short modifiedTime;
    short modifiedDate;
    short firstCluster;
    int size; // unit: cluster
} __attribute__ ((packed)) DirEntry;

typedef struct {
    Fat16_BootSector bSector;
    DirEntry* entries;
    void* entryBitset;
    uint_t maxEntryBit;
    short* fat;
    void* fatBitset;
    uint_t maxFatBit;
    struct Mutex mutex;
} Fat16_Fsinfo;

// dirEntry: flag
#define READ_ONLY   (1)
#define HIDDEN      (1<<1) // unused
#define SYSTEM      (1<<2) // unused
#define DISK_SIG    (1<<3) // unused
#define IS_DIR      (1<<4)
#define FILED       (1<<5) // unused

void Init_Fat16(void);

#endif