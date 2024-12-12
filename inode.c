#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

/**
 * Function: osfs_get_osfs_inode
 * Description: Retrieves the osfs_inode structure for a given inode number.
 * Inputs:
 *   - sb: The superblock of the filesystem.
 *   - ino: The inode number to retrieve.
 * Returns:
 *   - A pointer to the osfs_inode structure if successful.
 *   - NULL if the inode number is invalid or out of bounds.
 */
struct osfs_inode *osfs_get_osfs_inode(struct super_block *sb, uint32_t ino)
{
    struct osfs_sb_info *sb_info = sb->s_fs_info;

    if (ino == 0 || ino >= sb_info->inode_count) // File system inode count upper bound
        return NULL;
    return &((struct osfs_inode *)(sb_info->inode_table))[ino];
}

/**
 * Function: osfs_get_free_inode
 * Description: Allocates a free inode number from the inode bitmap.
 * Inputs:
 *   - sb_info: The superblock information of the filesystem.
 * Returns:
 *   - The allocated inode number on success.
 *   - -ENOSPC if no free inode is available.
 */
int osfs_get_free_inode(struct osfs_sb_info *sb_info)
{
    uint32_t ino;

    for (ino = 1; ino < sb_info->inode_count; ino++) {
        if (!test_bit(ino, sb_info->inode_bitmap)) {
            set_bit(ino, sb_info->inode_bitmap);
            sb_info->nr_free_inodes--;
            return ino;
        }
    }
    pr_err("osfs_get_free_inode: No free inode available\n");
    return -ENOSPC;
}

/**
 * Function: osfs_iget
 * Description: Creates or retrieves a VFS inode from a given inode number.
 * Inputs:
 *   - sb: The superblock of the filesystem.
 *   - ino: The inode number to load.
 * Returns:
 *   - A pointer to the VFS inode on success.
 *   - ERR_PTR(-EFAULT) if the osfs_inode cannot be retrieved.
 *   - ERR_PTR(-ENOMEM) if memory allocation for the inode fails.
 */
struct inode *osfs_iget(struct super_block *sb, unsigned long ino)
{
    struct osfs_inode *osfs_inode;
    struct inode *inode;

    osfs_inode = osfs_get_osfs_inode(sb, ino);
    if (!osfs_inode)
        return ERR_PTR(-EFAULT);

    inode = new_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_mode = osfs_inode->i_mode;
    i_uid_write(inode, osfs_inode->i_uid);
    i_gid_write(inode, osfs_inode->i_gid);
    inode->__i_atime = osfs_inode->__i_atime;
    inode->__i_mtime = osfs_inode->__i_mtime;
    inode->__i_ctime = osfs_inode->__i_ctime;
    inode->i_size = osfs_inode->i_size;
    inode->i_blocks = osfs_inode->i_blocks;
    inode->i_private = osfs_inode;

    if (S_ISDIR(inode->i_mode)) {
        inode->i_op = &osfs_dir_inode_operations;
        inode->i_fop = &osfs_dir_operations;
    } else if (S_ISREG(inode->i_mode)) {
        inode->i_op = &osfs_file_inode_operations;
        inode->i_fop = &osfs_file_operations;
    }

    // Insert the inode into the inode hash
    insert_inode_hash(inode);

    return inode;
}

/**
 * Function: osfs_alloc_data_block
 * Description: Allocates a free data block from the block bitmap.
 * Inputs:
 *   - sb_info: The superblock information of the filesystem.
 *   - block_no: Pointer to store the allocated block number.
 * Returns:
 *   - 0 on successful allocation.
 *   - -ENOSPC if no free data block is available.
 */
int osfs_alloc_data_block(struct osfs_sb_info *sb_info, uint32_t *block_no)
{
    uint32_t i;

    for (i = 0; i < sb_info->block_count; i++) {
        if (!test_bit(i, sb_info->block_bitmap)) {
            set_bit(i, sb_info->block_bitmap);
            sb_info->nr_free_blocks--;
            *block_no = i;
            return 0;
        }
    }
    pr_err("osfs_alloc_data_block: No free data block available\n");
    return -ENOSPC;
}

int osfs_alloc_extent(struct osfs_sb_info *sb_info, uint32_t required_blocks, struct osfs_inode *inode) {
    uint32_t start_block = 0;
    uint32_t consecutive_free_blocks = 0;
    int extent_index;

    // 遍歷所有資料塊
    for (uint32_t i = 0; i < sb_info->block_count; i++) {
        if (!test_bit(i, sb_info->block_bitmap)) {
            // 如果是第一個連續空閒塊，記錄起始位置
            if (consecutive_free_blocks == 0) {
                start_block = i;
            }
            consecutive_free_blocks++;

            // 找到足夠的連續塊
            if (consecutive_free_blocks == required_blocks) {
                // 查找空閒的 extent 項
                for (extent_index = 0; extent_index < 4; extent_index++) {
                    if (inode->extents[extent_index].block_count == 0) {
                        inode->extents[extent_index].start_block = start_block;
                        inode->extents[extent_index].block_count = required_blocks;
                        inode->extent_count++;
                        break;
                    }
                }

                if (extent_index == 4) {
                    pr_err("osfs_alloc_extent: No free extents available\n");
                    return -ENOSPC;
                }

                // 標記這些塊為已使用
                for (uint32_t j = start_block; j < start_block + required_blocks; j++) {
                    set_bit(j, sb_info->block_bitmap);
                }

                // 更新剩餘可用塊數
                sb_info->nr_free_blocks -= required_blocks;

                return 0;
            }
        } else {
            // 遇到已使用的塊，重置連續計數
            consecutive_free_blocks = 0;
        }
    }

    // 找不到足夠的連續塊
    pr_err("osfs_alloc_extent: No contiguous block range available\n");
    return -ENOSPC;
}



void osfs_free_data_block(struct osfs_sb_info *sb_info, uint32_t block_no)
{
    clear_bit(block_no, sb_info->block_bitmap);
    sb_info->nr_free_blocks++;
}