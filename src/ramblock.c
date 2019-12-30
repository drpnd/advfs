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
static uint64_t
_resolve_block_map(advfs_t *advfs, uint64_t inr, uint64_t pos)
{
    uint64_t b;
    uint64_t *block;
    advfs_inode_t inode;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the inode */
    advfs_read_inode(advfs, &inode, inr);

    if ( pos < ADVFS_INODE_BLOCKPTR - 1 ) {
        /* The block number is included in the inode structure */
        b = inode.blocks[pos];
    } else {
        /* Resolve from the chain */
        b = inode.blocks[ADVFS_INODE_BLOCKPTR - 1];
        advfs_read_raw_block(advfs, buf, b);
        block = (uint64_t *)buf;
        pos -= ADVFS_INODE_BLOCKPTR - 1;
        while ( pos >= (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Get the next chain */
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            advfs_read_raw_block(advfs, buf, b);
            block = (uint64_t *)buf;
            pos -= ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1;
        }
        b = block[pos];
    }

    return b;
}

/*
 * Update the mapping of a logical block number to a physical block number
 */
static int
_update_block_map(advfs_t *advfs, uint64_t inr, uint64_t pos, uint64_t pb)
{
    uint64_t b;
    uint64_t *block;
    advfs_inode_t inode;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the inode */
    advfs_read_inode(advfs, &inode, inr);

    if ( pos < ADVFS_INODE_BLOCKPTR - 1 ) {
        /* The block number is included in the inode structure */
        inode.blocks[pos] = pb;

        /* Write back */
        advfs_write_inode(advfs, &inode, inr);
    } else {
        /* Resolve from the chain */
        b = inode.blocks[ADVFS_INODE_BLOCKPTR - 1];
        advfs_read_raw_block(advfs, buf, b);
        block = (uint64_t *)buf;
        pos -= ADVFS_INODE_BLOCKPTR - 1;
        while ( pos >= (ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1) ) {
            /* Get the next chain */
            b = block[ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1];
            advfs_read_raw_block(advfs, buf, b);
            block = (uint64_t *)buf;
            pos -= ADVFS_BLOCK_SIZE / sizeof(uint64_t) - 1;
        }
        block[pos] = pb;

        /* Write back */
        advfs_write_raw_block(advfs, buf, b);
    }

    return 0;
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
    void *block;

    assert( pos > 0 );

    block = (void *)advfs->superblock + ADVFS_BLOCK_SIZE * pos;
    memcpy(buf, block, ADVFS_BLOCK_SIZE);

    return 0;
}

/*
 * Write a raw block
 */
int
advfs_write_raw_block(advfs_t *advfs, void *buf, uint64_t pos)
{
    void *block;

    assert( pos > 0 );

    block = (void *)advfs->superblock + ADVFS_BLOCK_SIZE * pos;
    memcpy(block, buf, ADVFS_BLOCK_SIZE);

    return 0;
}

/*
 * Read a block
 */
int
advfs_read_block(advfs_t *advfs, uint64_t inr, void *buf, uint64_t pos)
{
    uint64_t b;

    b = _resolve_block_map(advfs, inr, pos);
    if ( b == 0 ) {
        memset(buf, 0, ADVFS_BLOCK_SIZE);
    } else {
        advfs_read_raw_block(advfs, buf, b);
    }

    return 0;
}

/*
 * Write a block
 */
int
advfs_write_block(advfs_t *advfs, uint64_t inr, void *buf, uint64_t pos)
{
    uint64_t b;
    uint64_t cur;
    unsigned char hash[SHA384_DIGEST_LENGTH];
    advfs_block_mgt_t mgt;
    advfs_inode_t inode;

    /* Read the inode corresponding to inr */
    advfs_read_inode(advfs, &inode, inr);

    /* Calculate the hash value */
    SHA384(buf, ADVFS_BLOCK_SIZE, hash);

    /* Resolve the physical block corresponding to the logical block */
    cur = _resolve_block_map(advfs, inr, pos);

    /* Check the duplication */
    b = _block_search(advfs, hash);
    if ( b != 0 ) {
        /* Found */
        if ( cur != b ) {
            if ( cur != 0 ) {
                /* Unreference the old block */
                advfs_read_block_mgt(advfs, &mgt, cur);
                mgt.ref--;
                advfs_write_block_mgt(advfs, &mgt, cur);
                if ( mgt.ref == 0 ) {
                    /* Release this block */
                    _block_delete(advfs, cur);
                    advfs_free_block(advfs, cur);
                }
            }
            /* Referencde the new block */
            advfs_read_block_mgt(advfs, &mgt, b);
            mgt.ref++;
            advfs_write_block_mgt(advfs, &mgt, b);
        }
    } else {
        /* Not found, then allocate a new block, then write the content */
        b = advfs_alloc_block(advfs);
        advfs_write_raw_block(advfs, buf, b);
        memcpy(mgt.hash, hash, sizeof(mgt.hash));
        mgt.ref = 1;
        mgt.left = 0;
        mgt.right = 0;
        /* Add to the tree */
        _block_add(advfs, b);

        if ( cur != 0 ) {
            /* Unreference and free if needed */
            advfs_read_block_mgt(advfs, &mgt, cur);
            mgt.ref--;
            advfs_write_block_mgt(advfs, &mgt, cur);
            if ( mgt.ref == 0 ) {
                /* Release this block */
                _block_delete(advfs, cur);
                advfs_free_block(advfs, cur);
            }
        }

        /* Update the block map */
        _update_block_map(advfs, inr, pos, b);
    }

    return 0;
}


/*
 * Unreference the corresponding block
 */
int
advfs_unref_block(advfs_t *advfs, uint64_t inr, uint64_t pos)
{
    uint64_t cur;
    advfs_block_mgt_t mgt;
    advfs_inode_t inode;

    /* Read the inode corresponding to inr */
    advfs_read_inode(advfs, &inode, inr);

    /* Resolve the physical block corresponding to the logical block */
    cur = _resolve_block_map(advfs, inr, pos);

    if ( cur != 0 ) {
        advfs_read_block_mgt(advfs, &mgt, cur);
        mgt.ref--;
        advfs_write_block_mgt(advfs, &mgt, cur);
        if ( mgt.ref == 0 ) {
            /* Release this block */
            _block_delete(advfs, cur);
            advfs_free_block(advfs, cur);
        }
    }

    return 0;
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
 * Read an inode
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
 * Write an inode
 */
int
advfs_write_inode(advfs_t *advfs, const advfs_inode_t *inode, uint64_t nr)
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
 * Read a management block
 */
int
advfs_read_block_mgt(advfs_t *advfs, advfs_block_mgt_t *mgt, uint64_t nr)
{
    void *ptr;
    uint64_t b;
    uint64_t off;
    advfs_superblock_t sb;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the superblock */
    advfs_read_superblock(advfs, &sb);

    /* Resolve the position */
    b = sb.ptr_block_mgt + (sizeof(advfs_block_mgt_t) * nr) / ADVFS_BLOCK_SIZE;
    off = (sizeof(advfs_block_mgt_t) * nr) % ADVFS_BLOCK_SIZE;

    /* Assert the size to prevent buffer overflow */
    assert( off + sizeof(advfs_block_mgt_t) <= ADVFS_BLOCK_SIZE );

    /* Read the block */
    advfs_read_raw_block(advfs, buf, b);
    ptr = buf + off;
    memcpy(mgt, ptr, sizeof(advfs_block_mgt_t));

    return 0;
}

/*
 * Write a management block
 */
int
advfs_write_block_mgt(advfs_t *advfs, const advfs_block_mgt_t *mgt, uint64_t nr)
{
    void *ptr;
    uint64_t b;
    uint64_t off;
    advfs_superblock_t sb;
    uint8_t buf[ADVFS_BLOCK_SIZE];

    /* Read the superblock */
    advfs_read_superblock(advfs, &sb);

    /* Resolve the position */
    b = sb.ptr_block_mgt + (sizeof(advfs_block_mgt_t) * nr) / ADVFS_BLOCK_SIZE;
    off = (sizeof(advfs_block_mgt_t) * nr) % ADVFS_BLOCK_SIZE;

    /* Assert the size to prevent buffer overflow */
    assert( off + sizeof(advfs_block_mgt_t) <= ADVFS_BLOCK_SIZE );

    /* Read the block */
    advfs_read_raw_block(advfs, buf, b);
    ptr = buf + off;
    memcpy(ptr, mgt, sizeof(advfs_block_mgt_t));

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
