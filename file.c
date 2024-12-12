#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

/**
 * Function: osfs_read
 * Description: Reads data from a file.
 * Inputs:
 *   - filp: The file pointer representing the file to read from.
 *   - buf: The user-space buffer to copy the data into.
 *   - len: The number of bytes to read.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes read on success.
 *   - 0 if the end of the file is reached.
 *   - -EFAULT if copying data to user space fails.
 */
static ssize_t osfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_read = 0;
    size_t to_read;
    int i;

    if (osfs_inode->extent_count == 0 || *ppos >= osfs_inode->i_size) {
        return 0;
    }

    if (*ppos + len > osfs_inode->i_size) {
        len = osfs_inode->i_size - *ppos;
    }

    for (i = 0; i < osfs_inode->extent_count && len > 0; i++) {
        struct osfs_extent *extent = &osfs_inode->extents[i];
        uint32_t start_block = extent->start_block * BLOCK_SIZE;
        uint32_t end_block = start_block + (extent->block_count * BLOCK_SIZE);

        if (*ppos >= start_block && *ppos < end_block) {
            to_read = min(len, (size_t)(end_block - *ppos));
            data_block = sb_info->data_blocks + (*ppos);

            if (copy_to_user(buf + bytes_read, data_block, to_read)) {
                return -EFAULT;
            }

            *ppos += to_read;
            bytes_read += to_read;
            len -= to_read;
        }
    }

    return bytes_read;
}



/**
 * Function: osfs_write
 * Description: Writes data to a file.
 * Inputs:
 *   - filp: The file pointer representing the file to write to.
 *   - buf: The user-space buffer containing the data to write.
 *   - len: The number of bytes to write.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes written on success.
 *   - -EFAULT if copying data from user space fails.
 *   - Adjusted length if the write exceeds the block size.
 */
static ssize_t osfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_written = 0;
    size_t to_write;
    int i, ret;

    // 檢查是否分配了任何 extents，如果沒有，分配一個
    if (osfs_inode->extent_count == 0) {
        ret = osfs_alloc_extent(sb_info, 1, osfs_inode);
        if (ret) {
            pr_err("osfs_write: Failed to allocate extent\n");
            return ret;
        }
        osfs_inode->extent_count = 1;
    }

    // 循環寫入數據
    while (len > 0) {
        for (i = 0; i < osfs_inode->extent_count && len > 0; i++) {
            struct osfs_extent *extent = &osfs_inode->extents[i];
            uint32_t start_block = extent->start_block * BLOCK_SIZE;
            uint32_t end_block = start_block + (extent->block_count * BLOCK_SIZE);

            if (*ppos >= start_block && *ppos < end_block) {
                to_write = min(len, (size_t)(end_block - *ppos));
                data_block = sb_info->data_blocks + (*ppos - start_block);

                if (copy_from_user(data_block, buf + bytes_written, to_write)) {
                    pr_err("osfs_write: copy_from_user failed\n");
                    return -EFAULT;
                }

                *ppos += to_write;
                bytes_written += to_write;
                len -= to_write;

                // 更新文件大小
                if (*ppos > osfs_inode->i_size) {
                    osfs_inode->i_size = *ppos;
                }
            }
        }

        // 如果還有數據要寫，但當前 extents 已滿，則分配新 extent
        if (len > 0) {
            ret = osfs_alloc_extent(sb_info, 1, osfs_inode);
            if (ret) {
                pr_err("osfs_write: Failed to allocate additional extent\n");
                return ret;
            }
        }
    }

    inode->i_size = osfs_inode->i_size;
    mark_inode_dirty(inode);

    return bytes_written;
}




/**
 * Struct: osfs_file_operations
 * Description: Defines the file operations for regular files in osfs.
 */
const struct file_operations osfs_file_operations = {
    .open = generic_file_open, // Use generic open or implement osfs_open if needed
    .read = osfs_read,
    .write = osfs_write,
    .llseek = default_llseek,
    // Add other operations as needed
};

/**
 * Struct: osfs_file_inode_operations
 * Description: Defines the inode operations for regular files in osfs.
 * Note: Add additional operations such as getattr as needed.
 */
const struct inode_operations osfs_file_inode_operations = {
    // Add inode operations here, e.g., .getattr = osfs_getattr,
};
