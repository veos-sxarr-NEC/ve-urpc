/**
 * Copyright (C) 2025 NEC Corporation
 * This file is part of the VE Offload.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>

#define MAX_CORE_NUM 18
#define BUFF_LEN_PER_THREADS (4 * 1024 * 1024 * 2)

int main(int argc, char *argv[]) {
    key_t key = 0;
    size_t size = 0;
    int flags = (SHM_HUGETLB | S_IRWXU);
    int segid = -1;
    char *ptr;

    /* Check key_t and size_t are compatible with expected platform types */
    static_assert(sizeof(int) == sizeof(key_t),
                "key_t must be the same size as int");
    static_assert(sizeof(unsigned long) == sizeof(size_t),
                "size_t must be the same size as unsigned long");

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <key> <size>\n", argv[0]);
        return -1;
    }

    key = (key_t)strtol(argv[1], &ptr, 0);
    if (*ptr != '\0') {
        fprintf(stderr, "Invalid key\n");
        return -1;
    }
    size = strtoul(argv[2], &ptr, 0);
    if (*ptr != '\0') {
        fprintf(stderr, "Invalid size\n");
        return -1;
    }
    if ((size > (MAX_CORE_NUM * BUFF_LEN_PER_THREADS)) || (size == 0)) {
        fprintf(stderr, "Invalid argument: size = %zu\n", size);
        return -1;
    }
    segid = shmget(key, size, flags);
    if (segid == -1) {
        fprintf(stderr, "shmget failed: %s (key=%d size=%zu)\n",
            strerror(errno), key, size);
        return -1;
    }
    printf("%d\n", segid);
    return 0;
}