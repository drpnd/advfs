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

#include "config.h"
#include "advfs.h"
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

/* Prototype declarations */
static int _path2inode_rec(advfs_t *, uint64_t *, uint64_t, const char *, int);

/*
 * Increase the block
 */
static int
_increase_block(advfs_t *advfs, uint64_t inr, uint64_t nb)
{
    uint64_t b2;
    uint64_t pos;
    uint8_t buf[ADVFS_BLOCK_SIZE];
    uint64_t *block;
    ssize_t i;
    int alloc;
    advfs_inode_t e;

    /* Read the inode */
    advfs_read_inode(advfs, &e, inr);

    block = e.blocks;
    pos = 0;
    b2 = 0;
    for ( i = 0; i < (ssize_t)nb; i++ ) {
        /* Check if allocation needed */
        alloc = (i >= (ssize_t)e.attr.n_blocks) ? 1 : 0;

        /* Next chain */
        if ( i == ADVFS_INODE_BLOCKPTR - 1 ) {
            if ( alloc ) {
                /* Allocate a new block */
                b2 = advfs_alloc_block(advfs);
                if ( 0 == b2 ) {
                    return -1;
                }
                /* Add the block to the chain */
                block[pos] = b2;
            } else {
                /* Get the next chain */
                b2 = block[pos];
            }
            advfs_read_raw_block(advfs, buf, b2);
            block = (uint64_t *)buf;
            pos = 0;
        } else if ( pos == (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Write back */
            advfs_write_raw_block(advfs, buf, b2);

            if ( alloc ) {
                b2 = advfs_alloc_block(advfs);
                if ( 0 == b2 ) {
                    return -1;
                }
                block[pos] = b2;
            } else {
                b2 = block[pos];
            }
            advfs_read_raw_block(advfs, buf, b2);
            block = (uint64_t *)buf;
            pos = 0;
        }

        if ( alloc ) {
            block[pos] = 0;
        }
        pos++;
    }

    /* Write back */
    if ( b2 != 0 ) {
        advfs_write_raw_block(advfs, buf, b2);
    }
    e.attr.n_blocks = nb;
    advfs_write_inode(advfs, &e, inr);

    return 0;
}

/*
 * Shrink the block
 */
static int
_shrink_block(advfs_t *advfs, uint64_t inr, uint64_t nb)
{
    uint64_t fb;
    uint64_t b;
    uint64_t pos;
    uint64_t *block;
    ssize_t i;
    int free;
    advfs_inode_t e;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the inode */
    advfs_read_inode(advfs, &e, inr);

    block = e.blocks;
    pos = 0;
    fb = 0;
    b = 0;
    for ( i = 0; i < (ssize_t)nb; i++ ) {
        free = (i >= (ssize_t)nb) ? 1 : 0;

        /* Next chain */
        if ( i == ADVFS_INODE_BLOCKPTR - 1 ) {
            if ( 0 != fb ) {
                advfs_free_block(advfs, fb);
            }
            b = block[ADVFS_INODE_BLOCKPTR - 1];
            advfs_read_raw_block(advfs, buf, b);
            block = (uint64_t *)buf;
            pos = 0;
            if ( free ) {
                fb = b;
            }
        } else if ( pos == (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Write back */
            if ( 0 != b ) {
                advfs_write_raw_block(advfs, buf, b);
            }
            if ( 0 != fb ) {
                advfs_free_block(advfs, fb);
            }
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            advfs_read_raw_block(advfs, buf, b);
            block = (uint64_t *)buf;
            pos = 0;
            if ( free ) {
                fb = b;
            }
        }

        if ( free ) {
            advfs_unref_block(advfs, inr, pos);
        }
        pos++;
    }

    if ( 0 != fb ) {
        advfs_free_block(advfs, fb);
    } else if ( 0 != b ) {
        advfs_write_raw_block(advfs, buf, b);
    }

    e.attr.n_blocks = nb;
    advfs_write_inode(advfs, &e, inr);

    return 0;
}

/*
 * Resize
 */
static int
_resize_block(advfs_t *advfs, uint64_t inr, uint64_t nb)
{
    advfs_inode_t e;

    advfs_read_inode(advfs, &e, inr);

    if ( nb < e.attr.n_blocks ) {
        /* Shrink the file size */
        return _shrink_block(advfs, inr, nb);
    } else if ( nb > e.attr.n_blocks ) {
        /* Increase the file size */
        return _increase_block(advfs, inr, nb);
    } else {
        return 0;
    }
}

/*
 * Get the inode number from the directory
 */
static uint64_t
_get_inode_in_dir(advfs_t *advfs, uint64_t inr, uint64_t nr)
{
    uint64_t idx;
    uint64_t *block;
    uint64_t bidx;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Get the block index for the specified index nr */
    bidx = nr / (ADVFS_BLOCK_SIZE / sizeof(uint64_t));
    idx = nr % (ADVFS_BLOCK_SIZE / sizeof(uint64_t));

    advfs_read_block(advfs, inr, buf, bidx);
    block = (uint64_t *)buf;

    return block[idx];
}

/*
 * Set an entry to the directory
 */
static int
_set_inode_in_dir(advfs_t *advfs, uint64_t inr, uint64_t inode)
{
    uint64_t nb;
    uint64_t idx;
    uint64_t *block;
    uint64_t bidx;
    int ret;
    uint8_t buf[ADVFS_BLOCK_SIZE];
    advfs_inode_t dir;

    advfs_read_inode(advfs, &dir, inr);

    if ( dir.attr.type != ADVFS_DIR ) {
        return -1;
    }

    /* Get the block index for the specified index nr */
    bidx = dir.attr.size / (ADVFS_BLOCK_SIZE / sizeof(uint64_t));
    nb = bidx + 1;
    idx = dir.attr.size % (ADVFS_BLOCK_SIZE / sizeof(uint64_t));

    /* Increase the block region */
    ret = _resize_block(advfs, inr, nb);
    if ( 0 != ret ) {
        return -1;
    }

    advfs_read_block(advfs, inr, buf, bidx);
    block = (uint64_t *)buf;
    block[idx] = inode;
    advfs_write_block(advfs, inr, buf, bidx);

    advfs_read_inode(advfs, &dir, inr);
    dir.attr.size++;
    advfs_write_inode(advfs, &dir, inr);

    return 0;
}

/*
 * Find a free inode
 */
static int
_find_free_inode(advfs_t *advfs, uint64_t *nr)
{
    ssize_t i;
    advfs_inode_t inode;

    for ( i = 0; i < ADVFS_NUM_ENTRIES; i++ ) {
        advfs_read_inode(advfs, &inode, i);
        if ( inode.attr.type == ADVFS_UNUSED ) {
            *nr = i;
            return 0;
        }
    }

    return -1;
}

/*
 * Resolve the entry corresponding to the path name
 */
static int
_path2inode_rec(advfs_t *advfs, uint64_t *res, uint64_t inr, const char *path,
                int create)
{
    int ret;
    advfs_inode_t e;
    char name[ADVFS_NAME_MAX + 1];
    char *s;
    size_t len;
    ssize_t i;
    uint64_t inode;
    advfs_inode_t cur;
    advfs_superblock_t sb;

    advfs_read_inode(advfs, &cur, inr);

    if ( cur.attr.type != ADVFS_DIR ) {
        return -1;
    }

    /* Remove the head '/'s */
    if ( '/' != *path ) {
        return -1;
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
        return -1;
    } else if ( len == 0 ) {
        *res = inr;
        return 0;
    }
    memcpy(name, path, len);
    name[len] = '\0';
    path += len;

    /* Resolve the entry */
    for ( i = 0; i < (ssize_t)cur.attr.size; i++ ) {
        inode = _get_inode_in_dir(advfs, inr, i);
        advfs_read_inode(advfs, &e, inode);
        if ( 0 == strcmp(name, e.name) ) {
            /* Found */
            if ( '\0' == *path ) {
                *res = inode;
                return 0;
            } else if ( e.attr.type == ADVFS_DIR ) {
                return _path2inode_rec(advfs, res, inode, path, create);
            } else {
                /* Invalid file type */
                return -1;
            }
        }
    }

    /* Not found */
    if ( '\0' == *path && create ) {
        /* Create */
        if ( cur.attr.size >= ADVFS_MAX_CHILDREN ) {
            return -1;
        }
        /* Search an unused inode */
        ret = _find_free_inode(advfs, &inode);
        if ( 0 != ret ) {
            return -1;
        }
        advfs_read_superblock(advfs, &sb);
        sb.n_inode_used++;
        advfs_write_superblock(advfs, &sb);
        ret = _set_inode_in_dir(advfs, inr, inode);
        if ( 0 != ret ) {
            return -1;
        }
        advfs_read_inode(advfs, &e, inode);
        memset(&e, 0, sizeof(advfs_inode_t));
        memcpy(e.name, name, len + 1);
        advfs_write_inode(advfs, &e, inode);
        *res = inode;

        return 0;
    }

    return -1;
}
int
advfs_path2inode(advfs_t *advfs, uint64_t *res, const char *path, int create)
{
    uint64_t root;

    root = advfs->superblock->root;
    return _path2inode_rec(advfs, res, root, path, create);
}

/*
 * Remove an entry
 */
int
_remove_inode_rec(advfs_t *advfs, uint64_t inr, const char *path)
{
    advfs_inode_t cur;
    advfs_inode_t e;
    advfs_inode_t e0;
    char name[ADVFS_NAME_MAX + 1];
    char *s;
    size_t len;
    ssize_t i;
    uint64_t nb;
    uint64_t inr2;
    int ret;

    advfs_read_inode(advfs, &cur, inr);
    if ( cur.attr.type != ADVFS_DIR ) {
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
    for ( i = 0; i < (ssize_t)cur.attr.size; i++ ) {
        inr2 = _get_inode_in_dir(advfs, inr, i);
        advfs_read_inode(advfs, &e, inr2);
        if ( 0 == strcmp(name, e.name) ) {
            /* Found */
            if ( '\0' == *path ) {
                break;
            } else if ( e.attr.type == ADVFS_DIR ) {
                return _remove_inode_rec(advfs, inr2, path);
            } else {
                /* Invalid file type */
                return -ENOENT;
            }
        }
    }
    if ( i == (ssize_t)cur.attr.size ) {
        return -ENOENT;
    }

    /* Free the entry */
    if ( e.attr.type == ADVFS_DIR && e.attr.size > 0 ) {
        return -ENOTEMPTY;
    }
    e.attr.type = ADVFS_UNUSED;

    /* Shift the child entries */
    cur.attr.size--;
    advfs_write_inode(advfs, &cur, inr);

    for ( ; i < (ssize_t)cur.attr.size; i++ ) {
        advfs_read_inode(advfs, &e0, _get_inode_in_dir(advfs, inr, i + 1));
        advfs_write_inode(advfs, &e0, _get_inode_in_dir(advfs, inr, i));
    }

    /* Resize */
    nb = (cur.attr.size + (ADVFS_BLOCK_SIZE / sizeof(uint64_t)) - 1)
        / (ADVFS_BLOCK_SIZE / sizeof(uint64_t));
    ret = _resize_block(advfs, inr, nb);
    if ( 0 != ret ) {
        return -EFAULT;
    }

    return 0;
}
int
advfs_remove_inode(advfs_t *advfs, const char *path)
{
    uint64_t root;

    root = advfs->superblock->root;
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
    uint64_t inr;
    advfs_inode_t e;
    int status;
    int ret;

    /* Reset the stat structure */
    memset(stbuf, 0, sizeof(struct stat));

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        /* No entry found */
        return -ENOENT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( e.attr.type == ADVFS_DIR ) {
        /* Directory */
        stbuf->st_mode = S_IFDIR | e.attr.mode;
        stbuf->st_nlink = 2 + e.attr.size;
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        status = 0;
        stbuf->st_atime = e.attr.atime;
        stbuf->st_mtime = e.attr.mtime;
        stbuf->st_ctime = e.attr.ctime;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
        stbuf->st_birthtime = e.attr.ctime;
#endif
        stbuf->st_rdev = 0;
        stbuf->st_size = e.attr.n_blocks * ADVFS_BLOCK_SIZE;
        stbuf->st_blksize = ADVFS_BLOCK_SIZE;
        stbuf->st_blocks = e.attr.n_blocks;
    } else if ( e.attr.type == ADVFS_REGULAR_FILE ) {
        stbuf->st_mode = S_IFREG | e.attr.mode;
        stbuf->st_nlink = 1;
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        status = 0;
        stbuf->st_atime = e.attr.atime;
        stbuf->st_mtime = e.attr.mtime;
        stbuf->st_ctime = e.attr.ctime;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
        stbuf->st_birthtime = e.attr.ctime;
#endif
        stbuf->st_rdev = 0;
        stbuf->st_size = e.attr.size;
        stbuf->st_blksize = ADVFS_BLOCK_SIZE;
        stbuf->st_blocks = e.attr.n_blocks;
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
    advfs_inode_t e;
    advfs_inode_t e2;
    ssize_t i;
    uint64_t inr;
    uint64_t inr2;
    int ret;

    /* Ignore */
    (void)offset;
    (void)fi;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        return -ENOENT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( e.attr.type != ADVFS_DIR ) {
        /* No entry found or non-directory entry */
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for ( i = 0; i < (ssize_t)e.attr.size; i++ ) {
        inr2 = _get_inode_in_dir(advfs, inr, i);
        advfs_read_inode(advfs, &e2, inr2);
        filler(buf, e2.name, NULL, 0);
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
    uint64_t inr;
    int ret;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
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
    advfs_inode_t e;
    int perm;
    uint8_t block[ADVFS_BLOCK_SIZE];
    off_t pos;
    ssize_t remain;
    ssize_t i;
    ssize_t j;
    off_t k;
    uint64_t inr;
    int ret;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        return -ENOENT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( e.attr.type != ADVFS_REGULAR_FILE ) {
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
        advfs_read_block(advfs, inr, block, pos);
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
    advfs_inode_t e;
    int perm;
    size_t nsize;
    uint64_t nb;
    int ret;
    ssize_t i;
    ssize_t j;
    ssize_t remain;
    off_t pos;
    uint8_t block[ADVFS_BLOCK_SIZE];
    uint64_t inr;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        return -ENOENT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( e.attr.type != ADVFS_REGULAR_FILE ) {
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
    ret = _resize_block(advfs, inr, nb);
    if ( 0 != ret ) {
        return -EFAULT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( nsize > e.attr.size ) {
        e.attr.size = nsize;
    }
    /* Write back */
    advfs_write_inode(advfs, &e, inr);

    remain = size;
    while ( remain > 0 ) {
        pos = offset / ADVFS_BLOCK_SIZE;
        advfs_read_block(advfs, inr, block, pos);
        for ( i = (offset % ADVFS_BLOCK_SIZE), j = 0;
              i < ADVFS_BLOCK_SIZE && j < remain; i++, j++ ) {
            block[i] = buf[j];
        }
        advfs_write_block(advfs, inr, block, pos);

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
    advfs_inode_t e;
    uint64_t nb;
    uint8_t block[ADVFS_BLOCK_SIZE];
    int i;
    int ret;
    uint64_t pos;
    uint64_t inr;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        return -ENOENT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( e.attr.type != ADVFS_REGULAR_FILE ) {
        return -EISDIR;
    }

    /* Calculate the number of blocks */
    nb = (size + ADVFS_BLOCK_SIZE - 1) / ADVFS_BLOCK_SIZE;
    ret = _resize_block(advfs, inr, nb);
    if ( 0 != ret ) {
        return -EFAULT;
    }

    while ( (off_t)e.attr.size < size ) {
        pos = e.attr.size / ADVFS_BLOCK_SIZE;
        advfs_read_block(advfs, inr, block, pos);
        for ( i = e.attr.size % ADVFS_BLOCK_SIZE;
              i < ADVFS_BLOCK_SIZE && (off_t)e.attr.size < size; i++ ) {
            block[i] = 0;
            e.attr.size++;
        }
        advfs_write_block(advfs, inr, block, pos);
    }
    e.attr.size = size;

    /* Write back */
    advfs_write_inode(advfs, &e, inr);

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
    advfs_inode_t e;
    uint64_t inr;
    int ret;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        /* No entry found or non-directory entry */
        return -ENOENT;
    }
    if ( NULL != tv ) {
        e.attr.atime = tv[0].tv_sec;
        e.attr.mtime = tv[1].tv_sec;
    }

    /* Write back */
    advfs_write_inode(advfs, &e, inr);

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
    advfs_inode_t e;
    struct timeval tv;
    uint64_t inr;
    int ret;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    gettimeofday(&tv, NULL);

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret == 0 ) {
        /* Already exists */
        return -EEXIST;
    }

    ret = advfs_path2inode(advfs, &inr, path, 1);
    if ( ret < 0 ) {
        /* No entry found or non-directory entry */
        return -EACCES;
    }
    advfs_read_inode(advfs, &e, inr);
    e.attr.type = ADVFS_REGULAR_FILE;
    e.attr.mode = mode;
    e.attr.atime = tv.tv_sec;
    e.attr.mtime = tv.tv_sec;
    e.attr.ctime = tv.tv_sec;
    advfs_write_inode(advfs, &e, inr);

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
    advfs_inode_t e;
    struct timeval tv;
    uint64_t inr;
    int ret;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    gettimeofday(&tv, NULL);

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret == 0 ) {
        /* Already exists */
        return -EEXIST;
    }

    ret = advfs_path2inode(advfs, &inr, path, 1);
    if ( ret < 0 ) {
        /* No entry found or non-directory entry */
        return -EACCES;
    }
    advfs_read_inode(advfs, &e, inr);
    e.attr.type = ADVFS_DIR;
    e.attr.mode = mode;
    e.attr.atime = tv.tv_sec;
    e.attr.mtime = tv.tv_sec;
    e.attr.ctime = tv.tv_sec;
    advfs_write_inode(advfs, &e, inr);

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
    advfs_inode_t e;
    uint64_t inr;
    int ret;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        return -ENOENT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( e.attr.type != ADVFS_DIR ) {
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
    advfs_inode_t e;
    uint64_t inr;
    int ret;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    ret = advfs_path2inode(advfs, &inr, path, 0);
    if ( ret < 0 ) {
        return -ENOENT;
    }
    advfs_read_inode(advfs, &e, inr);
    if ( e.attr.type != ADVFS_REGULAR_FILE ) {
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
    int ret;

    /* Initialize */
    ret = advfs_init(&advfs);
    if ( 0 != ret ) {
        return EXIT_FAILURE;
    }

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
