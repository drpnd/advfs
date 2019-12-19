/*_
 * Copyright (c) 2019 Hirochika Asai <asai@jar.jp>
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _ADVFS_H
#define _ADVFS_H

#define FUSE_USE_VERSION  28

#include "config.h"
#include <stdint.h>

/* OpenSSL */
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#define ADVFS_NAME_MAX          255
#define ADVFS_NUM_ENTRIES       100
#define ADVFS_MAX_CHILDREN      128
#define ADVFS_BLOCK_SIZE        4096
#define ADVFS_BLOCK_NUM         10240
#define ADVFS_INODE_NUM         128
#define ADVFS_INODE_BLOCKPTR    16

/*
 * type
 */
typedef enum {
    ADVFS_UNUSED,
    ADVFS_REGULAR_FILE,
    ADVFS_DIR,
} advfs_entry_type_t;

/*
 * free list
 */
typedef struct {
    uint64_t next;
} advfs_free_list_t;

/*
 * Block management
 */
typedef struct {
    /* Hash */
    unsigned char hash[SHA384_DIGEST_LENGTH];
    /* Reference counter */
    uint64_t ref;
    /* Left */
    uint64_t left;
    /* Right */
    uint64_t right;
} __attribute__ ((packed, aligned(128))) advfs_block_mgt_t;

/*
 * inode attribute
 */
typedef struct {
    uint64_t type;
    uint64_t mode;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t size;
    uint64_t n_blocks;
} __attribute__ ((packed, aligned(128))) advfs_inode_attr_t;

/*
 * inode
 */
typedef struct {
    /* Attributes: 128 bytes */
    advfs_inode_attr_t attr;
    /* Name: 256 bytes */
    char name[ADVFS_NAME_MAX + 1];
    /* Blocks 128 bytes */
    uint64_t blocks[ADVFS_INODE_BLOCKPTR];
} __attribute__ ((packed, aligned(512))) advfs_inode_t;

/*
 * advfs superblock
 */
typedef struct {
    /* pointers (in block) */
    uint64_t ptr_inode;
    uint64_t ptr_block_mgt;
    uint64_t ptr_block;
    /* # of inodes */
    uint64_t n_inodes;
    uint64_t n_inode_used;
    /* Block management root */
    uint64_t block_mgt_root;
    /* # of blocks */
    uint64_t n_blocks;
    uint64_t n_block_used;
    uint64_t freelist;
    /* Root inode */
    advfs_inode_t root;
} __attribute__ ((packed, aligned(ADVFS_BLOCK_SIZE))) advfs_superblock_t;

/*
 * advfs data structure
 */
typedef struct {
    advfs_superblock_t *superblock;
} advfs_t;

#ifdef __cplusplus
extern "C" {
#endif

    /* init.c */
    int advfs_init(advfs_t *);

    /* ramblock.c */
    int advfs_read_block(advfs_t *, advfs_inode_t *, void *, uint64_t);
    int advfs_write_block(advfs_t *, advfs_inode_t *, void *, uint64_t *);

    /* main.c */
    uint64_t advfs_alloc_block(advfs_t *);
    void advfs_free_block(advfs_t *, uint64_t);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
