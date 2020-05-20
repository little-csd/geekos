/*
 * ELF executable loading
 * Copyright (c) 2003, Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 * Copyright (c) 2003, David H. Hovemeyer <daveho@cs.umd.edu>
 * $Revision: 1.29 $
 * 
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <geekos/errno.h>
#include <geekos/kassert.h>
#include <geekos/ktypes.h>
#include <geekos/screen.h>  /* for debug Print() statements */
#include <geekos/pfat.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/elf.h>


/**
 * From the data of an ELF executable, determine how its segments
 * need to be loaded into memory.
 * @param exeFileData buffer containing the executable file
 * @param exeFileLength length of the executable file in bytes
 * @param exeFormat structure describing the executable's segments
 *   and entry address; to be filled in
 * @return 0 if successful, < 0 on error
 */
int Parse_ELF_Executable(char *exeFileData, ulong_t exeFileLength,
    struct Exe_Format *exeFormat)
{
    elfHeader* header = (elfHeader*) exeFileData;
    exeFormat->entryAddr = header->entry;
    programHeader* ph = (programHeader*) (exeFileData + header->phoff);
    int num = header->phnum;
    exeFormat->numSegments = num;
    struct Exe_Segment* es = &(exeFormat->segmentList);
    for (int i = 0; i < num; i++) {
        es->offsetInFile = ph->offset;
        es->lengthInFile = ph->fileSize;
        es->startAddress = ph->vaddr;
        es->sizeInMemory = ph->memSize;
        es->protFlags = ph->flags;
        Print("File off: %x, Size: %x, Start: %x\n", es->offsetInFile, es->sizeInMemory, es->startAddress);
        es++;
        ph++;
    }
    return 0;
}

