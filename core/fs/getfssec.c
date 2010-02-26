/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2010 Intel Corporation; author: H. Peter Anvin
 *
 *   Permission is hereby granted, free of charge, to any person
 *   obtaining a copy of this software and associated documentation
 *   files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use,
 *   copy, modify, merge, publish, distribute, sublicense, and/or
 *   sell copies of the Software, and to permit persons to whom
 *   the Software is furnished to do so, subject to the following
 *   conditions:
 *
 *   The above copyright notice and this permission notice shall
 *   be included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *   OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 *   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *   OTHER DEALINGS IN THE SOFTWARE.
 *
 * ----------------------------------------------------------------------- */

/*
 * getfssec.c
 *
 * Generic getfssec implementation for disk-based filesystems, which
 * support the populate_next_extent method.
 *
 * The expected semantics of populate_next_extent are as follows:
 *
 * inode->next_extent.lstart will contain the initial sector number to
 * be mapped.  The routine is expected to populate inode->next_extent.pstart
 * and inode->next_extent.len.  If inode->prev_extent.pstart != EXTENT_VOID
 * then the routine is allowed to assume inode->prev_extent contains valid
 * data.
 *
 * If the filesystem can map the entire file as a single extent
 * (e.g. iso9660), then the filesystem can simply insert the extent
 * information into inode->prev_extent at searchdir/iget time, and leave
 * populate_next_extent as NULL.
 *
 * Note: the filesystem driver is not required to do extent coalescing,
 * if that is difficult to do; this routine will perform extent lookahead
 * and coalescing.
 */

#include <minmax.h>
#include "fs.h"

static inline sector_t next_psector(sector_t psector, uint32_t skip)
{
    if (EXTENT_SPECIAL(psector))
	return psector;
    else
	return psector + skip;
}

static inline sector_t next_pstart(const struct extent *e)
{
    return next_psector(e->pstart, e->len);
}

uint32_t generic_getfssec(struct file *file, char *buf,
			  int sectors, bool *have_more)
{
    struct inode *inode = file->inode;
    struct fs_info *fs = file->fs;
    struct disk *disk = fs->fs_dev->disk;
    uint32_t bytes_read = 0;
    uint32_t bytes_left = inode->size - file->offset;
    uint32_t sectors_left =
	(bytes_left + SECTOR_SIZE(fs) - 1) >> SECTOR_SHIFT(fs);
    uint32_t lsector;

    if (sectors > sectors_left)
	sectors = sectors_left;

    lsector = file->offset >> SECTOR_SHIFT(fs);

    if (lsector < inode->this_extent.lstart ||
	lsector >= inode->this_extent.lstart + inode->this_extent.len) {
	/* inode->this_extent unusable, maybe prev_extent is... */
	inode->this_extent = inode->prev_extent;
    }

    if (lsector < inode->this_extent.lstart ||
	lsector >= inode->this_extent.lstart + inode->this_extent.len) {
	/* Still nothing useful... */
	inode->this_extent.lstart = lsector;
	inode->this_extent.len    = 0;
    } else {
	/* We have some usable information */
	uint32_t delta = lsector - inode->this_extent.lstart;
	inode->this_extent.lstart = lsector;
	inode->this_extent.len -= delta;
	inode->this_extent.pstart
	    = next_psector(inode->this_extent.pstart, delta);
    }

    while (sectors) {
	uint32_t chunk;
	size_t len;

	if (!inode->this_extent.len && inode->next_extent.len &&
	    inode->this_extent.lstart == inode->next_extent.lstart)
	    inode->this_extent = inode->next_extent;

	while (sectors > inode->this_extent.len) {
	    /* Whatever we had before... */
	    inode->prev_extent = inode->next_extent;

	    /* Dummy information to make failure returns easier */
	    inode->next_extent.len = 0;

	    fs->fs_ops->populate_next_extent(inode);

	    if (inode->next_extent.len &&
		inode->next_extent.pstart == next_pstart(&inode->this_extent)) {
		/* Coalesce extents and loop */
		inode->this_extent.len += inode->next_extent.len;
	    } else {
		/* Discontiguous extents */
		break;
	    }
	}

	chunk = min(sectors, inode->this_extent.len);
	len = chunk << SECTOR_SHIFT(fs);

	if (inode->this_extent.pstart == EXTENT_ZERO) {
	    memset(buf, 0, len);
	} else {
	    disk->rdwr_sectors(disk, buf, inode->this_extent.pstart, chunk, 0);
	    inode->this_extent.pstart += chunk;
	}

	buf += len;
	sectors -= chunk;
	bytes_read += len;
	inode->this_extent.lstart += chunk;
	inode->this_extent.len -= chunk;
    }

    bytes_read = min(bytes_read, bytes_left);
    file->offset += bytes_read;

    if (have_more)
	*have_more = bytes_read < bytes_left;

    return bytes_read;
}