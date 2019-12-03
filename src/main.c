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

static const char *ramfs_file_path = "/test";
static uint8_t *ramfs_file_buf = NULL;
static size_t ramfs_file_size = 0;
static size_t ramfs_file_max_size = 1 * 1024 * 1024;
static time_t ramfs_file_timestamp = 1575370225;

int
advfs_getattr(const char *path, struct stat *stbuf)
{
    struct fuse_context *ctx;
    int status;

    /* Reset the stat structure */
    memset(stbuf, 0, sizeof(struct stat));

    /* Get the context */
    ctx = fuse_get_context();

    if ( strcmp(path, "/") == 0 ) {
        stbuf->st_mode = S_IFDIR | 0777;
        stbuf->st_nlink = 2;
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        status = 0;
    } else if ( strcmp(path, ramfs_file_path) == 0 ) {
        stbuf->st_mode = S_IFREG | 0666;
        stbuf->st_nlink = 1;
        stbuf->st_size = strlen(ramfs_file_path);
        stbuf->st_atime = (time_t)ramfs_file_timestamp;
        stbuf->st_mtime = (time_t)ramfs_file_timestamp;
        stbuf->st_ctime = (time_t)ramfs_file_timestamp;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
        stbuf->st_birthtime = (time_t)ramfs_file_timestamp;
#endif
        stbuf->st_uid = ctx->uid;
        stbuf->st_gid = ctx->gid;
        stbuf->st_rdev = 0;
        stbuf->st_size = ramfs_file_size;
        stbuf->st_blksize = 4096;
        stbuf->st_blocks = (ramfs_file_size + 4095) / 4096;
        status = 0;
    } else {
        status = -ENOENT;
    }

    return status;
}

int
advfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
    if ( strcmp(path, "/") == 0 ) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);
        filler(buf, ramfs_file_path + 1, NULL, 0);
        return 0;
    }

    return -ENOENT;
}

int
advfs_statfs(const char *path, struct statvfs *buf)
{
    ssize_t used;

    memset(buf, 0, sizeof(struct statvfs));

    used = (ramfs_file_size + 4096 - 1) / 4096;

    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 1024;       /* in f_frsize unit */
    buf->f_bfree = 1024 - used;
    buf->f_bavail = 1024 - used;

    buf->f_files = 1000;
    buf->f_ffree = 100 - 1;
    buf->f_favail = 100 - 1;

    buf->f_fsid = 0;
    buf->f_flag = 0;
    buf->f_namemax = 255;

    return 0;
}

static struct fuse_operations advfs_oper = {
    .getattr    = advfs_getattr,
    .readdir    = advfs_readdir,
    .statfs     = advfs_statfs,
};

/*
 * main
 */
int
main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &advfs_oper, NULL);
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
