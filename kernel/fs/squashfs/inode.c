/*
 * Squashfs - a compressed read only filesystem for Linux
 *
 * Copyright (c) 2002, 2003, 2004, 2005, 2006, 2007, 2008
 * Phillip Lougher <phillip@lougher.demon.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * inode.c
 */

/*
 * This file implements code to create and read inodes from disk.
 *
 * Inodes in Squashfs are identified by a 48-bit inode which encodes the
 * location of the compressed metadata block containing the inode, and the byte
 * offset into that block where the inode is placed (<block, offset>).
 *
 * To maximise compression there are different inodes for each file type
 * (regular file, directory, device, etc.), the inode contents and length
 * varying with the type.
 *
 * To further maximise compression, two types of regular file inode and
 * directory inode are defined: inodes optimised for frequently occurring
 * regular files and directories, and extended types where extra
 * information has to be stored.
 */

#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/zlib.h>
#include <linux/squashfs_fs.h>
#include <linux/squashfs_fs_sb.h>
#include <linux/squashfs_fs_i.h>

#include "squashfs.h"

/*
 * Initialise VFS inode with the base inode information common to all
 * Squashfs inode types.  Inodeb contains the unswapped base inode
 * off disk.
 */
static int squashfs_new_inode(struct super_block *sb, struct inode *inode,
				struct squashfs_base_inode *sqsh_ino)
{
	int err;

	err = squashfs_get_id(sb, le16_to_cpu(sqsh_ino->uid), &inode->i_uid);
	if (err)
		goto out;

	err = squashfs_get_id(sb, le16_to_cpu(sqsh_ino->guid), &inode->i_gid);
	if (err)
		goto out;

	inode->i_ino = le32_to_cpu(sqsh_ino->inode_number);
	inode->i_mtime.tv_sec = le32_to_cpu(sqsh_ino->mtime);
	inode->i_atime.tv_sec = inode->i_mtime.tv_sec;
	inode->i_ctime.tv_sec = inode->i_mtime.tv_sec;
	inode->i_mode = le16_to_cpu(sqsh_ino->mode);
	inode->i_size = 0;

out:
	return err;
}


struct inode *squashfs_iget(struct super_block *sb, long long ino,
				unsigned int ino_number)
{
	struct inode *inode = iget_locked(sb, ino_number);
	int err;

	TRACE("Entered squashfs_iget\n");

	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	err = squashfs_read_inode(inode, ino);
	if (err) {
		iget_failed(inode);
		return ERR_PTR(err);
	}

	unlock_new_inode(inode);
	return inode;
}


/*
 * Initialise VFS inode by reading inode from inode table (compressed
 * metadata).  The format and amount of data read depends on type.
 */
int squashfs_read_inode(struct inode *inode, long long ino)
{
	struct super_block *sb = inode->i_sb;
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	long long block = SQUASHFS_INODE_BLK(ino) + msblk->inode_table_start;
	unsigned int offset = SQUASHFS_INODE_OFFSET(ino);
	int err, type;
	union squashfs_inode squashfs_ino;
	struct squashfs_base_inode *sqsh_ino = &squashfs_ino.base;

	TRACE("Entered squashfs_read_inode\n");

	/*
	 * Read inode base common to all inode types.
	 */
	err = squashfs_read_metadata(sb, sqsh_ino, &block,
						&offset, sizeof(*sqsh_ino));
	if (err < 0)
		goto failed_read;

	err = squashfs_new_inode(sb, inode, sqsh_ino);
	if (err)
		goto failed_read;

	block = SQUASHFS_INODE_BLK(ino) + msblk->inode_table_start;
	offset = SQUASHFS_INODE_OFFSET(ino);

	type = le16_to_cpu(sqsh_ino->inode_type);
	switch (type) {
	case SQUASHFS_FILE_TYPE: {
		unsigned int frag_offset, frag_size, frag;
		long long frag_blk;
		struct squashfs_reg_inode *sqsh_ino = &squashfs_ino.reg;

		err = squashfs_read_metadata(sb, sqsh_ino, &block, &offset,
							sizeof(*sqsh_ino));
		if (err < 0)
			goto failed_read;

		frag = le32_to_cpu(sqsh_ino->fragment);
		if (frag != SQUASHFS_INVALID_FRAG) {
			frag_offset = le32_to_cpu(sqsh_ino->offset);
			frag_size = get_fragment_location(sb, frag, &frag_blk);
			if (frag_size < 0) {
				err = frag_size;
				goto failed_read;
			}
		} else {
			frag_blk = SQUASHFS_INVALID_BLK;
			frag_size = 0;
			frag_offset = 0;
		}

		inode->i_nlink = 1;
		inode->i_size = le32_to_cpu(sqsh_ino->file_size);
		inode->i_fop = &generic_ro_fops;
		inode->i_mode |= S_IFREG;
		inode->i_blocks = ((inode->i_size - 1) >> 9) + 1;
		SQUASHFS_I(inode)->fragment_block = frag_blk;
		SQUASHFS_I(inode)->fragment_size = frag_size;
		SQUASHFS_I(inode)->fragment_offset = frag_offset;
		SQUASHFS_I(inode)->start = le32_to_cpu(sqsh_ino->start_block);
		SQUASHFS_I(inode)->block_list_start = block;
		SQUASHFS_I(inode)->offset = offset;
		inode->i_data.a_ops = &squashfs_aops;

		TRACE("File inode %x:%x, start_block %llx, block_list_start "
			"%llx, offset %x\n", SQUASHFS_INODE_BLK(ino),
			offset, SQUASHFS_I(inode)->start, block, offset);
		break;
	}
	case SQUASHFS_LREG_TYPE: {
		unsigned int frag_offset, frag_size, frag;
		long long frag_blk;
		struct squashfs_lreg_inode *sqsh_ino = &squashfs_ino.lreg;

		err = squashfs_read_metadata(sb, sqsh_ino, &block, &offset,
							sizeof(*sqsh_ino));
		if (err < 0)
			goto failed_read;

		frag = le32_to_cpu(sqsh_ino->fragment);
		if (frag != SQUASHFS_INVALID_FRAG) {
			frag_offset = le32_to_cpu(sqsh_ino->offset);
			frag_size = get_fragment_location(sb, frag, &frag_blk);
			if (frag_size < 0) {
				err = frag_size;
				goto failed_read;
			}
		} else {
			frag_blk = SQUASHFS_INVALID_BLK;
			frag_size = 0;
			frag_offset = 0;
		}

		inode->i_nlink = le32_to_cpu(sqsh_ino->nlink);
		inode->i_size = le64_to_cpu(sqsh_ino->file_size);
		inode->i_fop = &generic_ro_fops;
		inode->i_mode |= S_IFREG;
		inode->i_blocks = ((inode->i_size -
				le64_to_cpu(sqsh_ino->sparse) - 1) >> 9) + 1;

		SQUASHFS_I(inode)->fragment_block = frag_blk;
		SQUASHFS_I(inode)->fragment_size = frag_size;
		SQUASHFS_I(inode)->fragment_offset = frag_offset;
		SQUASHFS_I(inode)->start = le64_to_cpu(sqsh_ino->start_block);
		SQUASHFS_I(inode)->block_list_start = block;
		SQUASHFS_I(inode)->offset = offset;
		inode->i_data.a_ops = &squashfs_aops;

		TRACE("File inode %x:%x, start_block %llx, block_list_start "
			"%llx, offset %x\n", SQUASHFS_INODE_BLK(ino),
			offset, SQUASHFS_I(inode)->start, block, offset);
		break;
	}
	case SQUASHFS_DIR_TYPE: {
		struct squashfs_dir_inode *sqsh_ino = &squashfs_ino.dir;

		err = squashfs_read_metadata(sb, sqsh_ino, &block, &offset,
				sizeof(*sqsh_ino));
		if (err < 0)
			goto failed_read;

		inode->i_nlink = le32_to_cpu(sqsh_ino->nlink);
		inode->i_size = le16_to_cpu(sqsh_ino->file_size);
		inode->i_op = &squashfs_dir_inode_ops;
		inode->i_fop = &squashfs_dir_ops;
		inode->i_mode |= S_IFDIR;
		SQUASHFS_I(inode)->start = le32_to_cpu(sqsh_ino->start_block);
		SQUASHFS_I(inode)->offset = le16_to_cpu(sqsh_ino->offset);
		SQUASHFS_I(inode)->dir_idx_cnt = 0;
		SQUASHFS_I(inode)->parent = le32_to_cpu(sqsh_ino->parent_inode);

		TRACE("Directory inode %x:%x, start_block %llx, offset %x\n",
				SQUASHFS_INODE_BLK(inode), offset,
				SQUASHFS_I(i)->start,
				le16_to_cpu(sqsh_ino->offset));
		break;
	}
	case SQUASHFS_LDIR_TYPE: {
		struct squashfs_ldir_inode *sqsh_ino = &squashfs_ino.ldir;

		err = squashfs_read_metadata(sb, sqsh_ino, &block, &offset,
				sizeof(*sqsh_ino));
		if (err < 0)
			goto failed_read;

		inode->i_nlink = le32_to_cpu(sqsh_ino->nlink);
		inode->i_size = le32_to_cpu(sqsh_ino->file_size);
		inode->i_op = &squashfs_dir_inode_ops;
		inode->i_fop = &squashfs_dir_ops;
		inode->i_mode |= S_IFDIR;
		SQUASHFS_I(inode)->start = le32_to_cpu(sqsh_ino->start_block);
		SQUASHFS_I(inode)->offset = le16_to_cpu(sqsh_ino->offset);
		SQUASHFS_I(inode)->dir_idx_start = block;
		SQUASHFS_I(inode)->dir_idx_offset = offset;
		SQUASHFS_I(inode)->dir_idx_cnt = le16_to_cpu(sqsh_ino->i_count);
		SQUASHFS_I(inode)->parent = le32_to_cpu(sqsh_ino->parent_inode);

		TRACE("Long directory inode %x:%x, start_block %llx, offset "
				"%x\n", SQUASHFS_INODE_BLK(ino), offset,
				SQUASHFS_I(inode)->start,
				le16_to_cpu(sqsh_ino->offset));
		break;
	}
	case SQUASHFS_SYMLINK_TYPE: {
		struct squashfs_symlink_inode *sqsh_ino = &squashfs_ino.symlink;

		err = squashfs_read_metadata(sb, sqsh_ino, &block, &offset,
				sizeof(*sqsh_ino));
		if (err < 0)
			goto failed_read;

		inode->i_nlink = le32_to_cpu(sqsh_ino->nlink);
		inode->i_size = le32_to_cpu(sqsh_ino->symlink_size);
		inode->i_op = &page_symlink_inode_operations;
		inode->i_data.a_ops = &squashfs_symlink_aops;
		inode->i_mode |= S_IFLNK;
		SQUASHFS_I(inode)->start = block;
		SQUASHFS_I(inode)->offset = offset;

		TRACE("Symbolic link inode %x:%x, start_block %llx, offset "
				"%x\n", SQUASHFS_INODE_BLK(ino), offset,
				block, offset);
		break;
	}
	case SQUASHFS_BLKDEV_TYPE:
	case SQUASHFS_CHRDEV_TYPE: {
		struct squashfs_dev_inode *sqsh_ino = &squashfs_ino.dev;
		unsigned int rdev;

		err = squashfs_read_metadata(sb, sqsh_ino, &block, &offset,
				sizeof(*sqsh_ino));
		if (err < 0)
			goto failed_read;

		inode->i_nlink = le32_to_cpu(sqsh_ino->nlink);
		inode->i_mode |= (type == SQUASHFS_CHRDEV_TYPE)
						? S_IFCHR : S_IFBLK;
		rdev = le32_to_cpu(sqsh_ino->rdev);
		init_special_inode(inode, le16_to_cpu(inode->i_mode),
					new_decode_dev(rdev));

		TRACE("Device inode %x:%x, rdev %x\n",
				SQUASHFS_INODE_BLK(ino), offset, rdev);
		break;
	}
	case SQUASHFS_FIFO_TYPE:
	case SQUASHFS_SOCKET_TYPE: {
		struct squashfs_ipc_inode *sqsh_ino = &squashfs_ino.ipc;

		err = squashfs_read_metadata(sb, sqsh_ino, &block, &offset,
				sizeof(*sqsh_ino));
		if (err < 0)
			goto failed_read;

		inode->i_nlink = le32_to_cpu(sqsh_ino->nlink);
		inode->i_mode |= (type == SQUASHFS_FIFO_TYPE)
						? S_IFIFO : S_IFSOCK;
		init_special_inode(inode, le16_to_cpu(inode->i_mode), 0);
		break;
	}
	default:
		ERROR("Unknown inode type %d in squashfs_iget!\n", type);
		err = -EINVAL;
		goto failed_read1;
	}

	return 0;

failed_read:
	ERROR("Unable to read inode 0x%llx\n", ino);

failed_read1:
	return err;
}
