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
#include <assert.h>

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

    if ( 0 == parent ) {
        return 0;
    }

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
 * Resolve the block number from the position
 */
static uint64_t *
_resolve_block(advfs_t *advfs, advfs_inode_t *inode, uint64_t pos)
{
    uint64_t *b;
    uint64_t *block;

    if ( pos < ADVFS_INODE_BLOCKPTR - 1 ) {
        /* The block number is included in the inode structure */
        b = &inode->blocks[pos];
    } else {
        /* Resolve from the chain */
        b = &inode->blocks[ADVFS_INODE_BLOCKPTR - 1];
        block = _get_block(advfs, *b);
        pos -= ADVFS_INODE_BLOCKPTR - 1;
        while ( pos >= (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Get the next chain */
            b = &block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            block = _get_block(advfs, *b);
            pos -= ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1;
        }
        b = &block[pos];
    }

    return b;
}

/*
 * Read the superblock
 */
int
advfs_read_superblock(advfs_t *advfs, advfs_superblock_t *sb)
{
    memcpy(sb, advfs->superblock, sizeof(advfs_superblock_t));

    return 0;
}

/*
 * Write back the superblock
 */
int
advfs_write_superblock(advfs_t *advfs, advfs_superblock_t *sb)
{
    memcpy(advfs->superblock, sb, sizeof(advfs_superblock_t));

    return 0;
}

/*
 * Read a raw block
 */
int
advfs_read_raw_block(advfs_t *advfs, void *buf, uint64_t pos)
{
    uint64_t *block;

    block = _get_block(advfs, pos);
    memcpy(buf, block, ADVFS_BLOCK_SIZE);

    return 0;
}

/*
 * Write a raw block
 */
int
advfs_write_raw_block(advfs_t *advfs, void *buf, uint64_t pos)
{
    uint64_t *block;

    block = _get_block(advfs, pos);
    memcpy(block, buf, ADVFS_BLOCK_SIZE);

    return 0;
}

/*
 * Read a block
 */
int
advfs_read_block(advfs_t *advfs, advfs_inode_t *inode, void *buf, uint64_t pos)
{
    uint64_t *b;
    uint64_t *block;

    b = _resolve_block(advfs, inode, pos);
    if ( 0 == *b ) {
        memset(buf, 0, ADVFS_BLOCK_SIZE);
    } else {
        block = _get_block(advfs, *b);
        memcpy(buf, block, ADVFS_BLOCK_SIZE);
    }

    return 0;
}

/*
 * Write a block
 */
int
advfs_write_block(advfs_t *advfs, advfs_inode_t *inode, void *buf,
                  uint64_t pos)
{
    uint64_t b;
    uint64_t *cur;
    uint64_t *block;
    unsigned char hash[SHA384_DIGEST_LENGTH];
    advfs_block_mgt_t *mgt;

    /* Find the corresponding block */
    cur = _resolve_block(advfs, inode, pos);

    /* Calculate the hash value */
    SHA384(buf, ADVFS_BLOCK_SIZE, hash);

    /* Check the duplication */
    b = _block_search(advfs, hash);
    if ( 0 != b ) {
        /* Found */
        if ( *cur != b ) {
            /* Contents changed */
            if ( 0 != *cur ) {
                /* Release the old block */
                mgt = _get_block_mgt(advfs, *cur);
                mgt->ref--;
            }
            /* Update the new reference */
            mgt = _get_block_mgt(advfs, b);
            mgt->ref++;
        }
        *cur = b;

        return 0;
    } else {
        /* Not found */
        if ( 0 == *cur ) {
            /* Allocate a new block */
            b = advfs_alloc_block(advfs);
            if ( 0 == b ){
                return -1;
            }
            *cur = b;
            mgt = _get_block_mgt(advfs, *cur);
            mgt->ref = 1;
        } else {
            mgt = _get_block_mgt(advfs, *cur);
            assert(mgt->ref >= 1);
            if ( mgt->ref > 1 ) {
                /* Decrement the reference */
                mgt->ref--;

                /* Copy */
                b = advfs_alloc_block(advfs);
                if ( 0 == b ){
                    return -1;
                }
                *cur = b;
                mgt = _get_block_mgt(advfs, *cur);
                mgt->ref = 1;
            } else {
                /* Update the block of *cur */
            }
        }

        block = _get_block(advfs, *cur);
        memcpy(block, buf, ADVFS_BLOCK_SIZE);

        /* Add the block */
        _block_add(advfs, *cur);

        return 0;
    }
}

/*
 * Allocate a new block
 */
uint64_t
advfs_alloc_block(advfs_t *advfs)
{
    uint64_t b;
    advfs_free_list_t *fl;
    advfs_superblock_t sb;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the superblock */
    advfs_read_superblock(advfs, &sb);

    /* Read the first entry of the freelist */
    b = sb.freelist;
    if ( 0 == b ) {
        /* No entry remaining */
        return 0;
    }

    /* Read from the free block */
    advfs_read_raw_block(advfs, buf, b);
    fl = (advfs_free_list_t *)buf;

    /* Update the superblock */
    sb.freelist = fl->next;
    sb.n_block_used++;

    /* Write back the super block */
    advfs_write_superblock(advfs, &sb);

    return b;
}

/*
 * Release a block
 */
void
advfs_free_block(advfs_t *advfs, uint64_t b)
{
    advfs_free_list_t *fl;
    advfs_superblock_t sb;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the superblock */
    advfs_read_superblock(advfs, &sb);

    fl = (advfs_free_list_t *)buf;
    fl->next = sb.freelist;
    advfs_write_raw_block(advfs, buf, b);

    /* Update the superblock */
    sb.freelist = b;
    sb.n_block_used--;

    /* Write back the superblock */
    advfs_write_superblock(advfs, &sb);
}

/*
 * Read inode
 */
int
advfs_read_inode(advfs_t *advfs, advfs_inode_t *inode, uint64_t nr)
{
    void *ptr;
    uint64_t b;
    uint64_t off;
    advfs_superblock_t sb;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the superblock */
    advfs_read_superblock(advfs, &sb);

    /* Resolve the position */
    b = sb.ptr_inode + (sizeof(advfs_inode_t) * nr) / ADVFS_BLOCK_SIZE;
    off = (sizeof(advfs_inode_t) * nr) % ADVFS_BLOCK_SIZE;

    /* Assert the size to prevent buffer overflow */
    assert( off + sizeof(advfs_inode_t) <= ADVFS_BLOCK_SIZE );

    /* Read the block */
    advfs_read_raw_block(advfs, buf, b);
    ptr = buf + off;
    memcpy(inode, ptr, sizeof(advfs_inode_t));

    return 0;
}

/*
 * Write inode
 */
int
advfs_write_inode(advfs_t *advfs, advfs_inode_t *inode, uint64_t nr)
{
    void *ptr;
    uint64_t b;
    uint64_t off;
    advfs_superblock_t sb;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the superblock */
    advfs_read_superblock(advfs, &sb);

    /* Resolve the position */
    b = sb.ptr_inode + (sizeof(advfs_inode_t) * nr) / ADVFS_BLOCK_SIZE;
    off = (sizeof(advfs_inode_t) * nr) % ADVFS_BLOCK_SIZE;

    /* Assert the size to prevent buffer overflow */
    assert( off + sizeof(advfs_inode_t) <= ADVFS_BLOCK_SIZE );

    /* Read the block */
    advfs_read_raw_block(advfs, buf, b);
    ptr = buf + off;
    memcpy(ptr, inode, sizeof(advfs_inode_t));

    /* Write back */
    advfs_write_raw_block(advfs, buf, b);

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
