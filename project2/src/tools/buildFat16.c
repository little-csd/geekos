// #include <geekos/pfat16.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <geekos/fat16.h>

#define round_up_block(x) ((x+SECTOR_SIZE-1)&(~(SECTOR_SIZE-1)))

// Valid fat term
#define VALID(x) (x>FIRST_DATA_BLOCK&&x<0xffff)
#define BAD     0xffff
#define EMPTY   0

// int
// read_block(int fd, int blocknum, char* data) {
//     unsigned int offset = blocknum * SECTOR_SIZE;
//     lseek(fd, offset, SEEK_SET);
//     return read(fd, data, SECTOR_SIZE);
// }

// int
// write_block(int fd, int blocknum, char* data) {
//     unsigned int offset = blocknum * SECTOR_SIZE;
//     lseek(fd, offset, SEEK_SET);
//     return write(fd, data, SECTOR_SIZE);
// }

void
copy_real_name(char* dst, char* src) {
    int i;
    int len = strlen(src);
    if (len <= 0 || src[len-1] == '/') {
        printf("Find last slash error: %s\n", src);
        exit(-1);
    }
    for (i = len-1; i >= 0; i--) {
        if (src[i] == '/') break;
    }
    i++;
    if (len - i > MAX_NAME_LEN) {
        printf("File name %s too long.\n", src);
        exit(-1);
    }
    memcpy(dst, src+i, len-i);
}

void
test(char* name) {
    int fd = open(name, O_RDONLY, 0);
    lseek(fd, FIRST_DATA_BLOCK*SECTOR_SIZE, 0);
    char buf[SECTOR_SIZE];
    read(fd, buf, SECTOR_SIZE);
    for (int i = 0; i < 100; i++) {
        if (i % 10 == 0) printf("\n");
        printf("%3d ", buf[i]);
    }
}

int
main(int argc, char** argv) {
    // TODO: Support building boot block
    if (argc <= 1) {
        printf("usage: buildFat16 <diskImage> <files>\n");
        return -1;
    }
    struct stat sbuf;
    Fat16_BootSector bSector;
    DirEntry* dirEntry;
    short* fat;
    unsigned int fat_ptr = FIRST_DATA_BLOCK;
    int rc;

    char* image = argv[1];
    printf("Build Fat16 on image %s\n", image);
    if (stat(image, &sbuf)) {
        printf("stat error\n");
        return -1;
    }
    unsigned int fileCount = argc - 2;
    unsigned int fileSize = sbuf.st_size;
    if (fileSize % SECTOR_SIZE) {
        printf("File size(%d) is not a multiple of %d Bytes",
         fileSize, SECTOR_SIZE);
        return -1;
    }
    if (fileCount > MAX_DIR_COUNT) {
        printf("Files numbers out of range\n");
        return -1;
    }
    memset(&bSector, 0, sizeof(Fat16_BootSector));
    bSector.fatCount = 2;
    bSector.reservedSector = 0;
    bSector.rootEntryCount = fileCount;
    bSector.sectorPerCluster = 1;
    bSector.sectorPerFatTable = SECTOR_PER_FATT;
    bSector.sectorSize = SECTOR_SIZE;
    
    int fd = open(image, O_WRONLY, 0);
    if (fd < 0) {
        printf("Open image failed\n");
        return -1;
    }
    
    dirEntry = (DirEntry*) malloc(sizeof(DirEntry)*MAX_DIR_COUNT);
    fat = (short*) malloc(FAT16_TSIZE);
    if (!fat || !dirEntry) {
        printf("Malloc failed\n");
        return -1;
    }
    memset(dirEntry, 0, sizeof(DirEntry)*MAX_DIR_COUNT);
    memset(fat, 0, FAT16_TSIZE);

    lseek(fd, FIRST_DATA_BLOCK*SECTOR_SIZE, SEEK_SET);
    unsigned int blocks;
    char buf[SECTOR_SIZE];
    char zeros[SECTOR_SIZE];
    memset(zeros, 0, SECTOR_SIZE);
    for (int i = 0; i < fileCount; i++) {
        char* name = argv[i+2];

        DirEntry* dir = dirEntry + i;
        memset(dir, 0, sizeof(DirEntry));
        rc = stat(name, &sbuf);
        if (rc) {
            printf("Stat error %d: %s\n", i, name);
            return -1;
        }
        blocks = round_up_block(sbuf.st_size)/SECTOR_SIZE;
        int fd2 = open(name, O_RDONLY, 0);
        if (!fd2) {
            printf("Open error %d: %s\n", i, name);
            return -1;
        }
        printf("Write %s at first block %d, %d blocks in total.\n",
         name, fat_ptr, blocks);
        
        copy_real_name(dir->name, name);
        dir->firstCluster = fat_ptr;
        dir->size = sbuf.st_size;
        dir->flag = 0;

        for (int j = 0; j < blocks; j++) {
            if (j < blocks-1) {
                fat[fat_ptr] = fat_ptr+1;
            } else {
                fat[fat_ptr] = 0;
            }
            rc = read(fd2, buf, SECTOR_SIZE);
            write(fd, buf, rc);
            if (rc != SECTOR_SIZE) {
                write(fd, zeros, SECTOR_SIZE - rc);
            }
            fat_ptr++;
        }
        close(fd2);
    }
    lseek(fd, 0, SEEK_SET);
    write(fd, &bSector, sizeof(Fat16_BootSector));
    write(fd, zeros, SECTOR_SIZE-sizeof(Fat16_BootSector));
    write(fd, fat, FAT16_TSIZE);
    write(fd, fat, FAT16_TSIZE);
    write(fd, dirEntry, sizeof(DirEntry)*MAX_DIR_COUNT);
    close(fd);
    printf("Build fat16 successfully.\n");
    // test(argv[1]);
    return 0;
}