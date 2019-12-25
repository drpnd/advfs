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

#include "advfs.h"
#include <fuse.h>
#include <stdlib.h>
#include <sys/time.h>
#include <assert.h>

/*
 * Initialize
 */
int
advfs_init(advfs_t *advfs)
{
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
    sblk->root = 0;
    inode[sblk->root].attr.type = ADVFS_DIR;
    inode[sblk->root].attr.mode = S_IFDIR | 0777;
    inode[sblk->root].attr.atime = tv.tv_sec;
    inode[sblk->root].attr.mtime = tv.tv_sec;
    inode[sblk->root].attr.ctime = tv.tv_sec;
    inode[sblk->root].attr.size = 0;
    inode[sblk->root].attr.n_blocks = 0;
    inode[sblk->root].name[0] = '\0';

    advfs->superblock = sblk;

    return 0;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
