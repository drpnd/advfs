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

#define FUSE_USE_VERSION  28

#include "config.h"
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <sys/time.h>
#include <assert.h>

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
    uint64_t ptr_inode;
    uint64_t ptr_block_mgt;
    uint64_t ptr_block;
    /* # of inodes */
    uint64_t n_inodes;
    uint64_t n_inode_used;
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

/* Prototype declarations */
static advfs_inode_t *
_path2inode_rec(advfs_t *, advfs_inode_t *, const char *, int);


/*
 * Resolve the block corresponding to the block number b
 */
static void *
_get_block(advfs_t *advfs, uint64_t b)
{
    return (void *)advfs->superblock + ADVFS_BLOCK_SIZE * b;
}

/*
 * Resolve the block management data structure for the block number b
 */
static advfs_block_mgt_t *
_get_block_mgt(advfs_t *advfs, uint64_t b)
{
    uint64_t off;
    advfs_block_mgt_t *mgt;

    off = advfs->superblock->ptr_block_mgt;
    mgt = (void *)advfs->superblock + ADVFS_BLOCK_SIZE * off;

    return &mgt[b];
}

/*
 * Allocate a new block
 */
static uint64_t
_alloc_block(advfs_t *advfs)
{
    uint64_t b;
    advfs_free_list_t *fl;

    b = advfs->superblock->freelist;
    if ( 0 == b ) {
        return 0;
    }
    fl = _get_block(advfs, b);
    advfs->superblock->freelist = fl->next;

    advfs->superblock->n_block_used++;

    return b;
}

/*
 * Release a block
 */
static void
_free_block(advfs_t *advfs, uint64_t b)
{
    advfs_free_list_t *fl;

    fl = _get_block(advfs, b);
    fl->next = advfs->superblock->freelist;
    advfs->superblock->freelist = b;

    advfs->superblock->n_block_used--;
}

/*
 * Get the inode corresponding to the inode number nr
 */
static advfs_inode_t *
_get_inode(advfs_t *advfs, uint64_t nr)
{
    uint64_t b;
    advfs_inode_t *inodes;

    b = advfs->superblock->ptr_inode;
    inodes = (void *)advfs->superblock + ADVFS_BLOCK_SIZE * b;

    return &inodes[nr];
}

/*
 * Increase the block
 */
static int
_increase_block(advfs_t *advfs, advfs_inode_t *e, uint64_t nb)
{
    uint64_t b1;
    uint64_t b2;
    uint64_t pos;
    uint64_t *block;
    ssize_t i;
    int alloc;

    block = e->blocks;
    pos = 0;
    for ( i = 0; i < (ssize_t)nb; i++ ) {
        alloc = (i >= (ssize_t)e->attr.n_blocks) ? 1 : 0;

        b2 = 0;
        /* Next chain */
        if ( i == ADVFS_INODE_BLOCKPTR - 1 ) {
            if ( alloc ) {
                b2 = _alloc_block(advfs);
                if ( 0 == b2 ) {
                    return -1;
                }
                block[ADVFS_INODE_BLOCKPTR - 1] = b2;
            } else {
                b2 = block[ADVFS_INODE_BLOCKPTR - 1];
            }
            block = _get_block(advfs, b2);
            pos = 0;
        } else if ( pos == (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            if ( alloc ) {
                b2 = _alloc_block(advfs);
                if ( 0 == b2 ) {
                    return -1;
                }
                block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1] = b2;
            } else {
                b2 = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            }
            block = _get_block(advfs, b2);
            pos = 0;
        }

        if ( alloc ) {
            /* Allocate */
            b1 = _alloc_block(advfs);
            if ( 0 == b1 ) {
                if ( 0 != b2 ) {
                    _free_block(advfs, b2);
                }
                return -1;
            }
            block[pos] = b1;
            e->attr.n_blocks++;
        }
        pos++;
    }

    return 0;
}

/*
 * Shrink the block
 */
static int
_shrink_block(advfs_t *advfs, advfs_inode_t *e, uint64_t nb)
{
    uint64_t fb;
    uint64_t b;
    uint64_t pos;
    uint64_t *block;
    ssize_t i;
    int free;

    block = e->blocks;
    pos = 0;
    fb = 0;
    for ( i = 0; i < (ssize_t)nb; i++ ) {
        free = (i >= (ssize_t)nb) ? 1 : 0;

        /* Next chain */
        if ( i == ADVFS_INODE_BLOCKPTR - 1 ) {
            if ( 0 != fb ) {
                _free_block(advfs, fb);
            }
            b = block[ADVFS_INODE_BLOCKPTR - 1];
            block = _get_block(advfs, b);
            pos = 0;
            if ( free ) {
                fb = b;
            }
        } else if ( pos == (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            if ( 0 != fb ) {
                _free_block(advfs, fb);
            }
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            block = _get_block(advfs, b);
            pos = 0;
            if ( free ) {
                fb = b;
            }
        }

        if ( free ) {
            _free_block(advfs, block[pos]);
        }
        pos++;
    }
    if ( 0 != fb ) {
        _free_block(advfs, fb);
    }

    e->attr.n_blocks = nb;

    return 0;
}

/*
 * Resize
 */
static int
_resize_block(advfs_t *advfs, advfs_inode_t *e, uint64_t nb)
{
    if ( nb < e->attr.n_blocks ) {
        /* Shrink the file size */
        return _shrink_block(advfs, e, nb);
    } else if ( nb > e->attr.n_blocks ) {
        /* Increase the file size */
        return _increase_block(advfs, e, nb);
    } else {
        return 0;
    }
}

/*
 * Get the inode number from the directory
 */
static uint64_t
_get_inode_in_dir(advfs_t *advfs, advfs_inode_t *inode, uint64_t nr)
{
    uint64_t b;
    uint64_t idx;
    uint64_t *block;
    uint64_t bidx;

    /* Get the block index for the specified index nr */
    bidx = nr / (ADVFS_BLOCK_SIZE / sizeof(uint64_t));
    idx = nr % (ADVFS_BLOCK_SIZE / sizeof(uint64_t));
    if ( bidx < ADVFS_INODE_BLOCKPTR - 1 ) {
        /* The block number is included in the inode structure */
        b = inode->blocks[bidx];
    } else {
        /* Resolve from the chain */
        b = inode->blocks[ADVFS_INODE_BLOCKPTR - 1];
        block = _get_block(advfs, b);
        bidx -= ADVFS_INODE_BLOCKPTR - 1;
        while ( bidx >= (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Get the next chain */
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            block = _get_block(advfs, b);
            bidx -= ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1;
        }
        b = block[bidx];
    }
    block = _get_block(advfs, b);

    /* Get the index to the inode number in the block */
    return block[idx];
}

/*
 * Set an entry to the directory
 */
static int
_set_inode_in_dir(advfs_t *advfs, advfs_inode_t *dir, uint64_t inode)
{
    uint64_t nb;
    uint64_t b;
    uint64_t idx;
    uint64_t *block;
    uint64_t bidx;
    int ret;

    if ( dir->attr.type != ADVFS_DIR ) {
        return -1;
    }

    /* Get the block index for the specified index nr */
    bidx = dir->attr.size / (ADVFS_BLOCK_SIZE / sizeof(uint64_t));
    nb = bidx + 1;
    idx = dir->attr.size % (ADVFS_BLOCK_SIZE / sizeof(uint64_t));

    /* Increase the block region */
    ret = _resize_block(advfs, dir, nb);
    if ( 0 != ret ) {
        return -1;
    }

    if ( bidx < ADVFS_INODE_BLOCKPTR - 1 ) {
        /* The block number is included in the inode structure */
        b = dir->blocks[bidx];
    } else {
        /* Resolve from the chain */
        b = dir->blocks[ADVFS_INODE_BLOCKPTR - 1];
        block = _get_block(advfs, b);
        bidx -= ADVFS_INODE_BLOCKPTR - 1;
        while ( bidx >= (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Get the next chain */
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            block = _get_block(advfs, b);
            bidx -= ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1;
        }
        b = block[bidx];
    }
    block = _get_block(advfs, b);
    block[idx] = inode;
    dir->attr.size++;

    return 0;
}

/*
 * Read a block
 */
static int
_read_block(advfs_t *advfs, advfs_inode_t *inode, void *buf, uint64_t pos)
{
    uint64_t b;
    uint64_t *block;

    if ( pos < ADVFS_INODE_BLOCKPTR - 1 ) {
        /* The block number is included in the inode structure */
        b = inode->blocks[pos];
    } else {
        /* Resolve from the chain */
        b = inode->blocks[ADVFS_INODE_BLOCKPTR - 1];
        block = _get_block(advfs, b);
        pos -= ADVFS_INODE_BLOCKPTR - 1;
        while ( pos >= (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Get the next chain */
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            block = _get_block(advfs, b);
            pos -= ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1;
        }
        b = block[pos];
    }
    block = _get_block(advfs, b);
    memcpy(buf, block, ADVFS_BLOCK_SIZE);

    return 0;
}

/*
 * Write a block
 */
static int
_write_block(advfs_t *advfs, advfs_inode_t *inode, void *buf, uint64_t pos)
{
    uint64_t b;
    uint64_t *block;
    unsigned char hash[SHA384_DIGEST_LENGTH];
    advfs_block_mgt_t *mgt;

    /* Calculate the hash value */
    SHA384(buf, ADVFS_BLOCK_SIZE, hash);
    mgt = _get_block_mgt(advfs, pos);
    memcpy(mgt->hash, hash, SHA384_DIGEST_LENGTH);

    if ( pos < ADVFS_INODE_BLOCKPTR - 1 ) {
        /* The block number is included in the inode structure */
        b = inode->blocks[pos];
    } else {
        /* Resolve from the chain */
        b = inode->blocks[ADVFS_INODE_BLOCKPTR - 1];
        block = _get_block(advfs, b);
        pos -= ADVFS_INODE_BLOCKPTR - 1;
        while ( pos >= (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Get the next chain */
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            block = _get_block(advfs, b);
            pos -= ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1;
        }
        b = block[pos];
    }
    block = _get_block(advfs, b);
    memcpy(block, buf, ADVFS_BLOCK_SIZE);

    return 0;
}

/*
 * Find a free inode
 */
static int
_find_free_inode(advfs_t *advfs, uint64_t *nr)
{
    ssize_t i;
    advfs_inode_t *inode;

    for ( i = 0; i < ADVFS_NUM_ENTRIES; i++ ) {
        inode = _get_inode(advfs, i);
        if ( inode->attr.type == ADVFS_UNUSED ) {
            *nr = i;
            return 0;
        }
    }

    return -1;
}

/*
 * Resolve the entry corresponding to the path name
 */
static advfs_inode_t *
_path2inode_rec(advfs_t *advfs, advfs_inode_t *cur, const char *path,
                int create)
{
    int ret;
    advfs_inode_t *e;
    char name[ADVFS_NAME_MAX + 1];
    char *s;
    size_t len;
    ssize_t i;
    uint64_t inode;

    if ( cur->attr.type != ADVFS_DIR ) {
        return NULL;
    }

    /* Remove the head '/'s */
    if ( '/' != *path ) {
        return NULL;
    }
    while ( '/' == *path ) {
        path++;
    }

    /* Get the file/directory entry name */
    s = index(path, '/');
    if ( NULL == s ) {
        len = strlen(path);
    } else {
        len = s - path;
    }
    if ( len > ADVFS_NAME_MAX ) {
        /* Invalid path name */
        return NULL;
    } else if ( len == 0 ) {
        return cur;
    }
    memcpy(name, path, len);
    name[len] = '\0';
    path += len;

    /* Resolve the entry */
    for ( i = 0; i < (ssize_t)cur->attr.size; i++ ) {
        e = _get_inode(advfs, _get_inode_in_dir(advfs, cur, i));
        if ( 0 == strcmp(name, e->name) ) {
            /* Found */
            if ( '\0' == *path ) {
                return e;
            } else if ( e->attr.type == ADVFS_DIR ) {
                return _path2inode_rec(advfs, e, path, create);
            } else {
                /* Invalid file type */
                return NULL;
            }
        }
    }

    /* Not found */
    if ( '\0' == *path && create ) {
        /* Create */
        if ( cur->attr.size >= ADVFS_MAX_CHILDREN ) {
            return NULL;
        }
        /* Search an unused inode */
        ret = _find_free_inode(advfs, &inode);
        if ( 0 != ret ) {
            return NULL;
        }
        ret = _set_inode_in_dir(advfs, cur, inode);
        if ( 0 != ret ) {
            return NULL;
        }
        e = _get_inode(advfs, inode);
        memset(e, 0, sizeof(advfs_inode_t));
        memcpy(e->name, name, len + 1);

        return e;
    }

    return NULL;
}
advfs_inode_t *
advfs_path2inode(advfs_t *advfs, const char *path, int create)
{
    advfs_inode_t *e;

    e = &advfs->superblock->root;
    return _path2inode_rec(advfs, e, path, create);
}

/*
 * Remove an entry
 */
int
_remove_inode_rec(advfs_t *advfs, advfs_inode_t *cur, const char *path)
{
    advfs_inode_t *e;
    advfs_inode_t *e0;
    char name[ADVFS_NAME_MAX + 1];
    char *s;
    size_t len;
    ssize_t i;
    uint64_t nb;
    int ret;

    if ( cur->attr.type != ADVFS_DIR ) {
        return -ENOENT;
    }

    /* Remove the head '/'s */
    if ( '/' != *path ) {
        return -ENOENT;
    }
    while ( '/' == *path ) {
        path++;
    }

    /* Get the file/directory entry name */
    s = index(path, '/');
    if ( NULL == s ) {
        len = strlen(path);
    } else {
        len = s - path;
    }
    if ( len > ADVFS_NAME_MAX ) {
        /* Invalid path name */
        return -ENOENT;
    } else if ( len == 0 ) {
        return -ENOENT;
    }
    memcpy(name, path, len);
    name[len] = '\0';
    path += len;

    /* Resolve the entry */
    for ( i = 0; i < (ssize_t)cur->attr.size; i++ ) {
        e = _get_inode(advfs, _get_inode_in_dir(advfs, cur, i));
        if ( 0 == strcmp(name, e->name) ) {
            /* Found */
            if ( '\0' == *path ) {
                break;
            } else if ( e->attr.type == ADVFS_DIR ) {
                return _remove_inode_rec(advfs, e, path);
            } else {
                /* Invalid file type */
                return -ENOENT;
            }
        }
    }
    if ( i == (ssize_t)cur->attr.size ) {
        return -ENOENT;
    }

    /* Free the entry */
    if ( e->attr.type == ADVFS_DIR && e->attr.size > 0 ) {
        return -ENOTEMPTY;
    }
    e->attr.type = ADVFS_UNUSED;

    /* Shift the child entries */
    cur->attr.size--;
    for ( ; i < (ssize_t)cur->attr.size; i++ ) {
        e0 = _get_inode(advfs, _get_inode_in_dir(advfs, cur, i));
        e = _get_inode(advfs, _get_inode_in_dir(advfs, cur, i + 1));
        memcpy(e0, e, sizeof(advfs_inode_t));
    }

    /* Resize */
    nb = (cur->attr.size + (ADVFS_BLOCK_SIZE / sizeof(uint64_t)) - 1)
        / (ADVFS_BLOCK_SIZE / sizeof(uint64_t));
    ret = _resize_block(advfs, cur, nb);
    if ( 0 != ret ) {
        return -EFAULT;
    }

    return 0;
}
int
advfs_remove_inode(advfs_t *advfs, const char *path)
{
    advfs_inode_t *root;

    root = &advfs->superblock->root;
    return _remove_inode_rec(advfs, root, path);
}


/*
 * getattr
 */
int
advfs_getattr(const char *path, struct stat *stbuf)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;
    int status;

    /* Reset the stat structure */
    memset(stbuf, 0, sizeof(struct stat));

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        /* No entry found */
        return -ENOENT;
    }
    if ( e->attr.type == ADVFS_DIR ) {
        /* Directory */
        stbuf->st_mode = S_IFDIR | e->attr.mode;
        stbuf->st_nlink = 2 + e->attr.size;
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        status = 0;
        stbuf->st_atime = e->attr.atime;
        stbuf->st_mtime = e->attr.mtime;
        stbuf->st_ctime = e->attr.ctime;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
        stbuf->st_birthtime = e->attr.ctime;
#endif
        stbuf->st_rdev = 0;
        stbuf->st_size = e->attr.n_blocks * ADVFS_BLOCK_SIZE;
        stbuf->st_blksize = ADVFS_BLOCK_SIZE;
        stbuf->st_blocks = e->attr.n_blocks;
    } else if ( e->attr.type == ADVFS_REGULAR_FILE ) {
        stbuf->st_mode = S_IFREG | e->attr.mode;
        stbuf->st_nlink = 1;
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        status = 0;
        stbuf->st_atime = e->attr.atime;
        stbuf->st_mtime = e->attr.mtime;
        stbuf->st_ctime = e->attr.ctime;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
        stbuf->st_birthtime = e->attr.ctime;
#endif
        stbuf->st_rdev = 0;
        stbuf->st_size = e->attr.size;
        stbuf->st_blksize = ADVFS_BLOCK_SIZE;
        stbuf->st_blocks = e->attr.n_blocks;
    } else {
        status = -ENOENT;
    }

    return status;
}

/*
 * readdir
 */
int
advfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;
    advfs_inode_t *inodes;
    ssize_t i;
    uint64_t *block;
    uint64_t b;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e || e->attr.type != ADVFS_DIR ) {
        /* No entry found or non-directory entry */
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for ( i = 0; i < (ssize_t)e->attr.size; i++ ) {
        assert( i < 512 );
        b = e->blocks[0];
        block = _get_block(advfs, b);
        inodes = _get_inode(advfs, 0);
        filler(buf, inodes[block[i]].name, NULL, 0);
    }

    return 0;
}

/*
 * statfs
 */
int
advfs_statfs(const char *path, struct statvfs *buf)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_superblock_t *sblk;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;
    sblk = advfs->superblock;

    memset(buf, 0, sizeof(struct statvfs));

    buf->f_bsize = ADVFS_BLOCK_SIZE;
    buf->f_frsize = ADVFS_BLOCK_SIZE;
    buf->f_blocks = advfs->superblock->n_blocks;    /* in f_frsize unit */
    buf->f_bfree = sblk->n_blocks - sblk->n_block_used;
    buf->f_bavail = sblk->n_blocks - sblk->n_block_used;

    buf->f_files = sblk->n_inodes;
    buf->f_ffree = sblk->n_inodes - sblk->n_inode_used;
    buf->f_favail = sblk->n_inodes - sblk->n_inode_used;

    buf->f_fsid = 0;
    buf->f_flag = 0;
    buf->f_namemax = ADVFS_NAME_MAX;

    return 0;
}

/*
 * open
 */
int
advfs_open(const char *path, struct fuse_file_info *fi)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }

    return 0;
}

/*
 * read
 */
int
advfs_read(const char *path, char *buf, size_t size, off_t offset,
           struct fuse_file_info *fi)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;
    int perm;
    uint8_t block[ADVFS_BLOCK_SIZE];
    off_t pos;
    ssize_t remain;
    ssize_t i;
    ssize_t j;
    off_t k;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->attr.type != ADVFS_REGULAR_FILE ) {
        return -EISDIR;
    }

    /* Mode check */
    perm = fi->flags & 3;
    if ( perm != O_RDONLY && perm != O_RDWR ) {
        return -EACCES;
    }

    remain = size;
    k = 0;
    while ( remain > 0 ) {
        pos = offset / ADVFS_BLOCK_SIZE;
        _read_block(advfs, e, block, pos);
        for ( i = (offset % ADVFS_BLOCK_SIZE), j = 0;
              i < ADVFS_BLOCK_SIZE && j < remain; i++, j++, k++ ) {
            buf[k] = block[j];
        }
        offset += j;
        remain -= j;
    }

    return k;
}

/*
 * write
 */
int
advfs_write(const char *path, const char *buf, size_t size, off_t offset,
            struct fuse_file_info *fi)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;
    int perm;
    size_t nsize;
    uint64_t nb;
    int ret;
    ssize_t i;
    ssize_t j;
    ssize_t remain;
    off_t pos;
    uint8_t block[ADVFS_BLOCK_SIZE];

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->attr.type != ADVFS_REGULAR_FILE ) {
        return -EISDIR;
    }

    /* Mode check */
    perm = fi->flags & 3;
    if ( perm != O_WRONLY && perm != O_RDWR ) {
        return -EACCES;
    }
    if ( size <= 0 ) {
        return 0;
    }

    /* Resize the block region */
    nsize = offset + size;
    nb = (nsize + ADVFS_BLOCK_SIZE - 1) / ADVFS_BLOCK_SIZE;
    ret = _resize_block(advfs, e, nb);
    if ( 0 != ret ) {
        return -EFAULT;
    }
    if ( nsize > e->attr.size ) {
        e->attr.size = nsize;
    }

    remain = size;
    while ( remain > 0 ) {
        if ( 0 != (offset % ADVFS_BLOCK_SIZE) ) {
            pos = offset / ADVFS_BLOCK_SIZE;
            _read_block(advfs, e, block, pos);
            for ( i = (offset % ADVFS_BLOCK_SIZE), j = 0;
                  i < ADVFS_BLOCK_SIZE && j < remain; i++, j++ ) {
                block[i] = buf[j];
            }
            _write_block(advfs, e, block, pos);
        }
        offset += j;
        remain -= j;
    }

    return size;
}

/*
 * truncate
 */
int
advfs_truncate(const char *path, off_t size)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;
    uint64_t nb;
    uint8_t block[ADVFS_BLOCK_SIZE];
    int i;
    int ret;
    uint64_t pos;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->attr.type != ADVFS_REGULAR_FILE ) {
        return -EISDIR;
    }

    /* Calculate the number of blocks */
    nb = (size + ADVFS_BLOCK_SIZE - 1) / ADVFS_BLOCK_SIZE;
    ret = _resize_block(advfs, e, nb);
    if ( 0 != ret ) {
        return -EFAULT;
    }

    while ( (off_t)e->attr.size < size ) {
        pos = e->attr.size / ADVFS_BLOCK_SIZE;
        _read_block(advfs, e, block, pos);
        for ( i = e->attr.size % ADVFS_BLOCK_SIZE;
              i < ADVFS_BLOCK_SIZE && (off_t)e->attr.size < size; i++ ) {
            block[i] = 0;
            e->attr.size++;
        }
        _write_block(advfs, e, block, pos);
    }
    e->attr.size = size;

    return 0;
}

/*
 * utimens
 */
int
advfs_utimens(const char *path, const struct timespec tv[2])
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        /* No entry found or non-directory entry */
        return -ENOENT;
    }
    if ( NULL != tv ) {
        e->attr.atime = tv[0].tv_sec;
        e->attr.mtime = tv[1].tv_sec;
    }

    return 0;
}

/*
 * create
 */
int
advfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;
    struct timeval tv;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    gettimeofday(&tv, NULL);

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL != e ) {
        /* Already exists */
        return -EEXIST;
    }

    e = advfs_path2inode(advfs, path, 1);
    if ( NULL == e ) {
        /* No entry found or non-directory entry */
        return -EACCES;
    }
    e->attr.type = ADVFS_REGULAR_FILE;
    e->attr.mode = mode;
    e->attr.atime = tv.tv_sec;
    e->attr.mtime = tv.tv_sec;
    e->attr.ctime = tv.tv_sec;

    return 0;
}

/*
 * mkdir
 */
int
advfs_mkdir(const char *path, mode_t mode)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;
    struct timeval tv;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    gettimeofday(&tv, NULL);

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL != e ) {
        /* Already exists */
        return -EEXIST;
    }

    e = advfs_path2inode(advfs, path, 1);
    if ( NULL == e ) {
        /* No entry found or non-directory entry */
        return -EACCES;
    }
    e->attr.type = ADVFS_DIR;
    e->attr.mode = mode;
    e->attr.atime = tv.tv_sec;
    e->attr.mtime = tv.tv_sec;
    e->attr.ctime = tv.tv_sec;

    return 0;
}

/*
 * rmdir
 */
int
advfs_rmdir(const char *path)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->attr.type != ADVFS_DIR ) {
        return -ENOTDIR;
    }

    return advfs_remove_inode(advfs, path);
}

/*
 * unlink
 */
int
advfs_unlink(const char *path)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_inode_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2inode(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->attr.type != ADVFS_REGULAR_FILE ) {
        return -ENOENT;
    }

    return advfs_remove_inode(advfs, path);
}

static struct fuse_operations advfs_oper = {
    .getattr    = advfs_getattr,
    .readdir    = advfs_readdir,
    .statfs     = advfs_statfs,
    .open       = advfs_open,
    .read       = advfs_read,
    .write      = advfs_write,
    .truncate   = advfs_truncate,
    .create     = advfs_create,
    .mkdir      = advfs_mkdir,
    .rmdir      = advfs_rmdir,
    .utimens    = advfs_utimens,
    .unlink     = advfs_unlink,
};

/*
 * main
 */
int
main(int argc, char *argv[])
{
    advfs_t advfs;
    ssize_t i;
    struct timeval tv;
    void *blkdev;
    advfs_superblock_t *sblk;
    advfs_inode_t *inode;
    advfs_block_mgt_t *mgt;
    void *block;
    int ratio;
    int nblk_inode;
    int nblk_mgt;
    advfs_free_list_t *fl;

    /* Initialize the block device */
    blkdev = malloc(ADVFS_BLOCK_SIZE * ADVFS_BLOCK_NUM);
    if ( NULL == blkdev ) {
        return -1;
    }
    sblk = blkdev;

    /* Ensure that each data structure size must be aligned. */
    assert( (ADVFS_BLOCK_SIZE % sizeof(advfs_inode_t)) == 0 );
    ratio = ADVFS_BLOCK_SIZE / sizeof(advfs_inode_t);
    assert( (ADVFS_INODE_NUM % ratio) == 0 );
    nblk_inode = (ADVFS_INODE_NUM / ratio);

    assert( (ADVFS_BLOCK_SIZE % sizeof(advfs_block_mgt_t)) == 0 );
    ratio = ADVFS_BLOCK_SIZE / sizeof(advfs_block_mgt_t);
    nblk_mgt = (ADVFS_BLOCK_NUM / ratio);

    sblk->ptr_inode = 1;
    sblk->n_inodes = ADVFS_INODE_NUM;
    sblk->n_inode_used = 0;
    sblk->ptr_block_mgt = 1 + nblk_inode;
    sblk->ptr_block = 1 + nblk_inode + nblk_mgt;
    sblk->n_blocks = ADVFS_BLOCK_NUM - (1 + nblk_inode + nblk_mgt);
    sblk->n_block_used = 0;

    /* Initialize all inodes */
    inode = blkdev + ADVFS_BLOCK_SIZE * sblk->ptr_inode;
    for ( i = 0; i < (ssize_t)sblk->n_inodes; i++ ) {
        inode[i].attr.type = ADVFS_UNUSED;
    }

    /* Initialize the block management array */
    mgt = blkdev + ADVFS_BLOCK_SIZE * sblk->ptr_block_mgt;
    for ( i = 0; i < (ssize_t)sblk->n_blocks; i++ ) {
        mgt[i].ref = 0;
    }

    /* Initialize all blocks */
    block = blkdev + ADVFS_BLOCK_SIZE * sblk->ptr_block;
    fl = block;
    for ( i = 0; i < (ssize_t)sblk->n_blocks - 1; i++ ) {
        fl->next = sblk->ptr_block + i + 1;
        block += ADVFS_BLOCK_SIZE;
        fl = block;
    }
    fl->next = 0;
    sblk->freelist = sblk->ptr_block;

    /* Initialize the root inode */
    gettimeofday(&tv, NULL);
    sblk->root.attr.type = ADVFS_DIR;
    sblk->root.attr.mode = S_IFDIR | 0777;
    sblk->root.attr.atime = tv.tv_sec;
    sblk->root.attr.mtime = tv.tv_sec;
    sblk->root.attr.ctime = tv.tv_sec;
    sblk->root.attr.size = 0;
    sblk->root.attr.n_blocks = 0;
    sblk->root.name[0] = '\0';

    advfs.superblock = sblk;

    return fuse_main(argc, argv, &advfs_oper, &advfs);
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
