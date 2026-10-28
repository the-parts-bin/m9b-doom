//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O functions.
//

#include <stdio.h>
#ifdef M9B_RAWWAD
#include <fcntl.h>
#include <unistd.h>
#endif

#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

typedef struct
{
    wad_file_t wad;
#ifdef M9B_RAWWAD
    int fd;
#else
    FILE *fstream;
#endif
} stdc_wad_file_t;

extern wad_file_class_t stdc_wad_file;

#ifdef M9B_RAWWAD
// m9b: open/lseek/read directly (port/syscalls.c: FatFs), without stdio,
// whose fread copies a byte at a time through its buffer. FatFs reads the
// whole sectors of a lump straight into Doom's buffer.
static wad_file_t *W_StdC_OpenFile(char *path)
{
    stdc_wad_file_t *result;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
    {
        return NULL;
    }
    result = Z_Malloc(sizeof(stdc_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = lseek(fd, 0, SEEK_END);
    result->fd = fd;
    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad)
{
    close(((stdc_wad_file_t *) wad)->fd);
    Z_Free(wad);
}

size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                   void *buffer, size_t buffer_len)
{
    stdc_wad_file_t *stdc_wad = (stdc_wad_file_t *) wad;
    ssize_t result;

    if (lseek(stdc_wad->fd, offset, SEEK_SET) < 0)
    {
        return 0;
    }
    result = read(stdc_wad->fd, buffer, buffer_len);
    return result < 0 ? 0 : (size_t) result;
}
#else
static wad_file_t *W_StdC_OpenFile(char *path)
{
    stdc_wad_file_t *result;
    FILE *fstream;

    fstream = fopen(path, "rb");

    if (fstream == NULL)
    {
        return NULL;
    }

    // Create a new stdc_wad_file_t to hold the file handle.

    result = Z_Malloc(sizeof(stdc_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = M_FileLength(fstream);
    result->fstream = fstream;

    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad)
{
    stdc_wad_file_t *stdc_wad;

    stdc_wad = (stdc_wad_file_t *) wad;

    fclose(stdc_wad->fstream);
    Z_Free(stdc_wad);
}

// Read data from the specified position in the file into the 
// provided buffer.  Returns the number of bytes read.

size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                   void *buffer, size_t buffer_len)
{
    stdc_wad_file_t *stdc_wad;
    size_t result;

    stdc_wad = (stdc_wad_file_t *) wad;

    // Jump to the specified position in the file.

    fseek(stdc_wad->fstream, offset, SEEK_SET);

    // Read into the buffer.

    result = fread(buffer, 1, buffer_len, stdc_wad->fstream);

    return result;
}
#endif


wad_file_class_t stdc_wad_file = 
{
    W_StdC_OpenFile,
    W_StdC_CloseFile,
    W_StdC_Read,
};


