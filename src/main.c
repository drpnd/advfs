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

#define ADVFS_NAME_MAX          255
#define ADVFS_NUM_ENTRIES       100
#define ADVFS_MAX_CHILDREN      128
#define ADVFS_BLOCK_SIZE        4096
#define ADVFS_BLOCK_NUM         10240
#define ADVFS_INODE_NUM         128

/*
 * type
 */
typedef enum {
    ADVFS_UNUSED,
    ADVFS_REGULAR_FILE,
    ADVFS_DIR,
} advfs_entry_type_t;

typedef struct _advfs_entry advfs_entry_t;

/*
 * Regular file
 */
typedef struct {
    uint8_t *buf;
    size_t size;
} advfs_entry_file_t;

/*
 * Directory
 */
typedef struct {
    int nent;
    int children[ADVFS_MAX_CHILDREN];
} advfs_entry_dir_t;

/*
 * entry
 */
struct _advfs_entry {
    char name[ADVFS_NAME_MAX + 1];
    advfs_entry_type_t type;
    int mode;
    time_t atime;
    time_t mtime;
    time_t ctime;
    union {
        advfs_entry_file_t file;
        advfs_entry_dir_t dir;
    } u;
};

/*
 * free list
 */
typedef struct {
    uint64_t next;
} advfs_free_list_t;

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
    uint64_t blocks[16];
} __attribute__ ((packed, aligned(512))) advfs_inode_t;

/*
 * advfs superblock
 */
typedef struct {
    uint64_t ptr_inode;
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
    int root;
    advfs_entry_t *entries;
    advfs_superblock_t *superblock;
} advfs_t;

/* Prototype declarations */
static advfs_entry_t *
_path2ent_rec(advfs_t *, advfs_entry_t *, const char *, int);

/*
 * Resolve the entry corresponding to the path name
 */
static advfs_entry_t *
_path2ent_rec(advfs_t *advfs, advfs_entry_t *cur, const char *path, int create)
{
    advfs_entry_t *e;
    char name[ADVFS_NAME_MAX + 1];
    char *s;
    size_t len;
    int i;

    if ( cur->type != ADVFS_DIR ) {
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
    for ( i = 0; i < cur->u.dir.nent; i++ ) {
        e = &advfs->entries[cur->u.dir.children[i]];
        if ( 0 == strcmp(name, e->name) ) {
            /* Found */
            if ( '\0' == *path ) {
                return e;
            } else if ( e->type == ADVFS_DIR ) {
                return _path2ent_rec(advfs, e, path, create);
            } else {
                /* Invalid file type */
                return NULL;
            }
        }
    }

    /* Not found */
    if ( '\0' == *path && create ) {
        /* Create */
        if ( cur->u.dir.nent >= ADVFS_MAX_CHILDREN ) {
            return NULL;
        }
        /* Search unused inode */
        for ( i = 0; i < ADVFS_NUM_ENTRIES; i++ ) {
            if ( advfs->entries[i].type == ADVFS_UNUSED ) {
                break;
            }
        }
        if ( i >= ADVFS_NUM_ENTRIES ) {
            /* Not found */
            return NULL;
        }
        cur->u.dir.children[cur->u.dir.nent] = i;
        e = &advfs->entries[i];
        memset(e, 0, sizeof(advfs_entry_t));
        memcpy(e->name, name, len + 1);
        cur->u.dir.nent++;
        return e;
    }

    return NULL;
}
advfs_entry_t *
advfs_path2ent(advfs_t *advfs, const char *path, int create)
{
    advfs_entry_t *e;

    e = &advfs->entries[advfs->root];
    return _path2ent_rec(advfs, e, path, create);
}

/*
 * Remove an entry
 */
int
_remove_ent_rec(advfs_t *advfs, advfs_entry_t *cur, const char *path)
{
    advfs_entry_t *e;
    char name[ADVFS_NAME_MAX + 1];
    char *s;
    size_t len;
    int i;

    if ( cur->type != ADVFS_DIR ) {
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
    for ( i = 0; i < cur->u.dir.nent; i++ ) {
        e = &advfs->entries[cur->u.dir.children[i]];
        if ( 0 == strcmp(name, e->name) ) {
            /* Found */
            if ( '\0' == *path ) {
                break;
            } else if ( e->type == ADVFS_DIR ) {
                return _remove_ent_rec(advfs, e, path);
            } else {
                /* Invalid file type */
                return -ENOENT;
            }
        }
    }
    if ( i == cur->u.dir.nent ) {
        return -ENOENT;
    }

    /* Free the entry */
    e = &advfs->entries[cur->u.dir.children[i]];
    if ( e->type == ADVFS_DIR && e->u.dir.nent > 0 ) {
        return -ENOTEMPTY;
    }
    e->type = ADVFS_UNUSED;

    /* Shift the child entries */
    cur->u.dir.nent--;
    for ( ; i < cur->u.dir.nent; i++ ) {
        cur->u.dir.children[i] = cur->u.dir.children[i + 1];
    }

    return 0;
}
int
advfs_remove_ent(advfs_t *advfs, const char *path)
{
    advfs_entry_t *root;

    root = &advfs->entries[advfs->root];
    return _remove_ent_rec(advfs, root, path);
}


/*
 * getattr
 */
int
advfs_getattr(const char *path, struct stat *stbuf)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_entry_t *e;
    int status;

    /* Reset the stat structure */
    memset(stbuf, 0, sizeof(struct stat));

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e ) {
        /* No entry found */
        return -ENOENT;
    }
    if ( e->type == ADVFS_DIR ) {
        /* Directory */
        stbuf->st_mode = S_IFDIR | e->mode;
        stbuf->st_nlink = 2 + e->u.dir.nent;
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        status = 0;
        stbuf->st_atime = e->atime;
        stbuf->st_mtime = e->mtime;
        stbuf->st_ctime = e->ctime;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
        stbuf->st_birthtime = e->ctime;
#endif
    } else if ( e->type == ADVFS_REGULAR_FILE ) {
        stbuf->st_mode = S_IFREG | e->mode;
        stbuf->st_nlink = 1;
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        status = 0;
        stbuf->st_atime = e->atime;
        stbuf->st_mtime = e->mtime;
        stbuf->st_ctime = e->ctime;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
        stbuf->st_birthtime = e->ctime;
#endif
        stbuf->st_rdev = 0;
        stbuf->st_size = e->u.file.size;
        stbuf->st_blksize = ADVFS_BLOCK_SIZE;
        stbuf->st_blocks
            = (e->u.file.size + ADVFS_BLOCK_SIZE - 1) / ADVFS_BLOCK_SIZE;
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
    advfs_entry_t *e;
    advfs_entry_t *e2;
    int i;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e || e->type != ADVFS_DIR ) {
        /* No entry found or non-directory entry */
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for ( i = 0; i < e->u.dir.nent; i++ ) {
        e2 = &advfs->entries[e->u.dir.children[i]];
        filler(buf, e2->name, NULL, 0);
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
    advfs_entry_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
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
    advfs_entry_t *e;
    int perm;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->type != ADVFS_REGULAR_FILE ) {
        return -EISDIR;
    }

    /* Mode check */
    perm = fi->flags & 3;
    if ( perm != O_RDONLY && perm != O_RDWR ) {
        return -EACCES;
    }

    if ( offset < (off_t)e->u.file.size ) {
        if ( offset + size > e->u.file.size ) {
            size = e->u.file.size - offset;
        }
        (void)memcpy(buf, e->u.file.buf + offset, size);
    } else {
        size = 0;
    }

    return size;
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
    advfs_entry_t *e;
    int perm;
    size_t nsize;
    uint8_t *nbuf;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->type != ADVFS_REGULAR_FILE ) {
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

    nsize = offset + size;
    if ( nsize > e->u.file.size ) {
        /* Reallocate */
        nbuf = realloc(e->u.file.buf, nsize);
        if ( NULL == nbuf ) {
            return -EDQUOT;
        }
        e->u.file.buf = nbuf;
        e->u.file.size = nsize;
    }

    (void)memcpy(e->u.file.buf + offset, buf, size);

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
    advfs_entry_t *e;
    uint8_t *nbuf;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->type != ADVFS_REGULAR_FILE ) {
        return -EISDIR;
    }

    if ( (off_t)e->u.file.size != size ) {
        if ( size > 0 ) {
            nbuf = realloc(e->u.file.buf, size);
            if ( NULL == nbuf ) {
                return -EFBIG;
            }
            e->u.file.buf = nbuf;
        } else {
            free(e->u.file.buf);
            e->u.file.buf = NULL;
        }
    }

    while ( (off_t)e->u.file.size < size ) {
        e->u.file.buf[e->u.file.size] = 0;
        e->u.file.size++;
    }
    e->u.file.size = size;

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
    advfs_entry_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e ) {
        /* No entry found or non-directory entry */
        return -ENOENT;
    }
    if ( NULL != tv ) {
        e->atime = tv[0].tv_sec;
        e->mtime = tv[1].tv_sec;
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
    advfs_entry_t *e;
    struct timeval tv;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    gettimeofday(&tv, NULL);

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL != e ) {
        /* Already exists */
        return -EEXIST;
    }

    e = advfs_path2ent(advfs, path, 1);
    if ( NULL == e ) {
        /* No entry found or non-directory entry */
        return -EACCES;
    }
    e->type = ADVFS_REGULAR_FILE;
    e->mode = mode;
    e->atime = tv.tv_sec;
    e->mtime = tv.tv_sec;
    e->ctime = tv.tv_sec;

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
    advfs_entry_t *e;
    struct timeval tv;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    gettimeofday(&tv, NULL);

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL != e ) {
        /* Already exists */
        return -EEXIST;
    }

    e = advfs_path2ent(advfs, path, 1);
    if ( NULL == e ) {
        /* No entry found or non-directory entry */
        return -EACCES;
    }
    e->type = ADVFS_DIR;
    e->mode = mode;
    e->atime = tv.tv_sec;
    e->mtime = tv.tv_sec;
    e->ctime = tv.tv_sec;

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
    advfs_entry_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->type != ADVFS_DIR ) {
        return -ENOTDIR;
    }

    return advfs_remove_ent(advfs, path);
}

/*
 * unlink
 */
int
advfs_unlink(const char *path)
{
    struct fuse_context *ctx;
    advfs_t *advfs;
    advfs_entry_t *e;

    /* Get the context */
    ctx = fuse_get_context();
    advfs = ctx->private_data;

    e = advfs_path2ent(advfs, path, 0);
    if ( NULL == e ) {
        return -ENOENT;
    }
    if ( e->type != ADVFS_REGULAR_FILE ) {
        return -ENOENT;
    }

    return advfs_remove_ent(advfs, path);
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
    void *block;
    int ratio;
    int nblk;
    advfs_free_list_t *fl;

    /* Initialize the block device */
    blkdev = malloc(ADVFS_BLOCK_SIZE * ADVFS_BLOCK_NUM);
    if ( NULL == blkdev ) {
        return -1;
    }
    sblk = blkdev;

    /* Ensure that each data structure size must be aligned. */
    assert( (sizeof(ADVFS_BLOCK_SIZE) % sizeof(advfs_inode_t)) != 0 );
    ratio = sizeof(ADVFS_BLOCK_SIZE) / sizeof(advfs_inode_t);
    assert( (ADVFS_INODE_NUM % ratio) != 0 );
    nblk = (ADVFS_INODE_NUM / ratio);

    sblk->ptr_inode = 1;
    sblk->n_inodes = ADVFS_INODE_NUM;
    sblk->n_inode_used = 0;
    sblk->ptr_block = 1 + nblk;
    sblk->n_blocks = ADVFS_BLOCK_NUM - (1 + nblk);
    sblk->n_block_used = 0;

    /* Initialize all inodes */
    inode = blkdev + ADVFS_BLOCK_SIZE * sblk->ptr_inode;
    for ( i = 0; i < (ssize_t)sblk->n_inodes; i++ ) {
        inode[i].attr.type = ADVFS_UNUSED;
    }

    /* Initialize all blocks */
    block = blkdev + ADVFS_BLOCK_SIZE * sblk->ptr_block;
    fl = block;
    for ( i = 0; i < (ssize_t)sblk->n_inodes - 1; i++ ) {
        fl->next = sblk->ptr_block + i + 1;
        fl = block + ADVFS_BLOCK_SIZE;
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

    /* Allocate entries */
    advfs.entries = malloc(sizeof(advfs_entry_t) * ADVFS_NUM_ENTRIES);
    if ( NULL == advfs.entries ) {
        return -1;
    }
    for ( i = 0; i < ADVFS_NUM_ENTRIES; i++ ) {
        advfs.entries[i].type = ADVFS_UNUSED;
    }

    /* root directory */
    gettimeofday(&tv, NULL);
    advfs.root = 0;
    advfs.entries[advfs.root].type = ADVFS_DIR;
    advfs.entries[advfs.root].name[0] = '\0';
    advfs.entries[advfs.root].mode = 0777;
    advfs.entries[advfs.root].atime = tv.tv_sec;
    advfs.entries[advfs.root].mtime = tv.tv_sec;
    advfs.entries[advfs.root].ctime = tv.tv_sec;
    advfs.entries[advfs.root].u.dir.nent = 0;

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
