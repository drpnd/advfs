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
#include <stdlib.h>
#include <string.h>

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
 * Search
 */
static uint64_t
_block_search_rec(advfs_t *advfs, uint64_t parent, const unsigned char *hash)
{
    int ret;
    advfs_block_mgt_t *mgt;

    mgt = _get_block_mgt(advfs, parent);

    /* Compare the hash value */
    ret = memcmp(mgt->hash, hash, sizeof(mgt->hash));
    if ( 0 == ret ) {
        /* Found */
        return parent;
    } else if ( ret < 0 ) {
        /* Search right */
        return _block_search_rec(advfs, mgt->right, hash);
    } else {
        /* Search left */
        return _block_search_rec(advfs, mgt->left, hash);
    }
}
static uint64_t
_block_search(advfs_t *advfs, const unsigned char *hash)
{
    uint64_t root;

    root = advfs->superblock->block_mgt_root;

    return _block_search_rec(advfs, root, hash);
}

/*
 * Add
 */
static int
_block_add_rec(advfs_t *advfs, uint64_t *parent, uint64_t b)
{
    advfs_block_mgt_t *mgt;
    advfs_block_mgt_t *tmp;
    int ret;

    if ( *parent == 0 ) {
        *parent = b;
        return 0;
    }

    mgt = _get_block_mgt(advfs, *parent);
    tmp = _get_block_mgt(advfs, b);

    /* Compare the hash value */
    ret = memcmp(mgt->hash, tmp->hash, sizeof(mgt->hash));
    if ( 0 == ret ) {
        /* Hash value conflict */
        return -1;
    } else if ( ret < 0 ) {
        /* Search right */
        return _block_add_rec(advfs, &mgt->right, b);
    } else {
        /* Search left */
        return _block_add_rec(advfs, &mgt->left, b);
    }
}
static int
_block_add(advfs_t *advfs, uint64_t b)
{
    return _block_add_rec(advfs, &advfs->superblock->block_mgt_root, b);
}

/*
 * Delete
 */
static uint64_t
_block_remove_max(advfs_t *advfs, uint64_t *parent)
{
    advfs_block_mgt_t *mgt;
    uint64_t maxc;

    mgt = _get_block_mgt(advfs, *parent);
    while ( 0 != mgt->right ) {
        parent = &mgt->right;
        mgt = _get_block_mgt(advfs, *parent);
    }

    maxc = *parent;
    if ( 0 != mgt->left ) {
        *parent = mgt->left;
    }

    return maxc;
}
static int
_block_delete_rec(advfs_t *advfs, uint64_t *parent, uint64_t b)
{
    advfs_block_mgt_t *mgt;
    advfs_block_mgt_t *tmp;
    uint64_t maxc;
    int ret;

    if ( *parent == 0 ) {
        /* Not found */
        return -1;
    }

    if ( *parent == b ) {
        /* Found, then pull one of the children of the  */
        mgt = _get_block_mgt(advfs, b);
        if ( 0 != mgt->left && 0 != mgt->right ) {
            /* Both children */
            maxc = _block_remove_max(advfs, &mgt->left);
            *parent = maxc;
            tmp = _get_block_mgt(advfs, maxc);
            tmp->left = mgt->left;
            tmp->right = mgt->right;
        } else if ( 0 != mgt->left ) {
            /* Only left child */
            *parent = mgt->left;
        } else if ( 0 != mgt->left ) {
            *parent = mgt->right;
        } else {
            /* No children */
            *parent = 0;
        }

        return 0;
    } else {
        mgt = _get_block_mgt(advfs, *parent);
        tmp = _get_block_mgt(advfs, b);
        ret = memcmp(mgt->hash, tmp->hash, sizeof(mgt->hash));
        if ( ret < 0 ) {
            /* Right */
            return _block_delete_rec(advfs, &mgt->right, b);
        } else if ( ret > 0 ) {
            /* Left */
            return _block_delete_rec(advfs, &mgt->left, b);
        } else {
            /* Found the hash but not the same block number */
            return -1;
        }
    }
}
static int
_block_delete(advfs_t *advfs, uint64_t b)
{
    return _block_delete_rec(advfs, &advfs->superblock->block_mgt_root, b);
}

/*
 * Read a block
 */
int
advfs_read_block(advfs_t *advfs, advfs_inode_t *inode, void *buf, uint64_t pos)
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
int
advfs_write_block(advfs_t *advfs, advfs_inode_t *inode, void *buf, uint64_t pos)
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
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
