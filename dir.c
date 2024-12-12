#include <linux/fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include "osfs.h"

/**
 * Function: osfs_lookup
 * Description: Looks up a file within a directory.
 * Inputs:
 *   - dir: The inode of the directory to search in.
 *   - dentry: The dentry representing the file to look up.
 *   - flags: Flags for the lookup operation.
 * Returns:
 *   - A pointer to the dentry if the file is found.
 *   - NULL if the file is not found, allowing the VFS to handle it.
 */
static struct dentry *osfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct osfs_sb_info *sb_info = dir->i_sb->s_fs_info;
    struct osfs_inode *parent_inode = dir->i_private;
    struct osfs_dir_entry *dir_entries;
    int dir_entry_count = 0;
    int i, j;
    struct inode *inode = NULL;

    pr_info("osfs_lookup: Looking up '%.*s' in inode %lu\n",
            (int)dentry->d_name.len, dentry->d_name.name, dir->i_ino);

    // 遍歷每個 extent 來讀取目錄數據
    for (i = 0; i < parent_inode->extent_count; i++) {
        void *extent_start = sb_info->data_blocks + parent_inode->extents[i].start_block * BLOCK_SIZE;
        int extent_size = parent_inode->extents[i].block_count * BLOCK_SIZE;
        int extent_entry_count = extent_size / sizeof(struct osfs_dir_entry);

        dir_entries = (struct osfs_dir_entry *)extent_start;

        // 遍歷這個 extent 中的所有目錄項
        for (j = 0; j < extent_entry_count; j++) {
            if (strlen(dir_entries[j].filename) == dentry->d_name.len &&
                strncmp(dir_entries[j].filename, dentry->d_name.name, dentry->d_name.len) == 0) {
                // 找到文件，獲取 inode
                pr_info("CHECKING %s",dir_entries[j].filename);
                inode = osfs_iget(dir->i_sb, dir_entries[j].inode_no);
                if (IS_ERR(inode)) {
                    pr_err("osfs_lookup: Error getting inode %u\n", dir_entries[j].inode_no);
                    return ERR_CAST(inode);
                }
                return d_splice_alias(inode, dentry);
            }
        }
        dir_entry_count += extent_entry_count;
    }

    // 如果找不到文件，返回 NULL
    return NULL;
}


/**
 * Function: osfs_iterate
 * Description: Iterates over the entries in a directory.
 * Inputs:
 *   - filp: The file pointer representing the directory.
 *   - ctx: The directory context used for iteration.
 * Returns:
 *   - 0 on successful iteration.
 *   - A negative error code on failure.
 */
static int osfs_iterate(struct file *filp, struct dir_context *ctx)
{
    struct inode *inode = file_inode(filp);
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_dir_entry *dir_entries;
    int i, j;
    size_t extent_size, extent_entry_count;

    // Output initial dot entries like '.' and '..'
    if (ctx->pos == 0) {
        if (!dir_emit_dots(filp, ctx))
            return 0;
    }

    // Iterate through each extent to read directory entries
    for (i = 0; i < osfs_inode->extent_count; i++) {
        void *extent_start = sb_info->data_blocks + osfs_inode->extents[i].start_block * BLOCK_SIZE;
        extent_size = osfs_inode->extents[i].block_count * BLOCK_SIZE;
        extent_entry_count = extent_size / sizeof(struct osfs_dir_entry);
        dir_entries = (struct osfs_dir_entry *)extent_start;

        // Traverse the entries within this extent
        for (j = ctx->pos - 2; j < extent_entry_count; j++) {
            struct osfs_dir_entry *entry = &dir_entries[j];
            unsigned int type = DT_UNKNOWN;

            if (entry->inode_no == 0)
                continue;  // Skip unused entries

            // Emit the directory entry
            pr_info("osfs_iterate: Emitting entry '%s' with inode %u\n", entry->filename, entry->inode_no);

            if (!dir_emit(ctx, entry->filename, strlen(entry->filename), entry->inode_no, type)) {
                pr_err("osfs_iterate: dir_emit failed for entry '%s'\n", entry->filename);
                return -EINVAL;
            }

            ctx->pos++;  // Update position after successfully emitting an entry
        }
    }

    return 0;
}






/**
 * Function: osfs_new_inode
 * Description: Creates a new inode within the filesystem.
 * Inputs:
 *   - dir: The inode of the directory where the new inode will be created.
 *   - mode: The mode (permissions and type) for the new inode.
 * Returns:
 *   - A pointer to the newly created inode on success.
 *   - ERR_PTR(-EINVAL) if the file type is not supported.
 *   - ERR_PTR(-ENOSPC) if there are no free inodes or blocks.
 *   - ERR_PTR(-ENOMEM) if memory allocation fails.
 *   - ERR_PTR(-EIO) if an I/O error occurs.
 */
struct inode *osfs_new_inode(const struct inode *dir, umode_t mode)
{
    struct super_block *sb = dir->i_sb;
    struct osfs_sb_info *sb_info = sb->s_fs_info;
    struct inode *inode;
    struct osfs_inode *osfs_inode;
    int ino, ret;
    uint32_t required_blocks = 1;  // 假設初始分配1個塊，根據需要調整

    /* Check if the mode is supported */
    if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
        pr_err("File type not supported (only directory, regular file and symlink supported)\n");
        return ERR_PTR(-EINVAL);
    }

    /* Check if there are free inodes and blocks */
    if (sb_info->nr_free_inodes == 0 || sb_info->nr_free_blocks == 0)
        return ERR_PTR(-ENOSPC);

    /* Allocate a new inode number */
    ino = osfs_get_free_inode(sb_info);
    if (ino < 0 || ino >= sb_info->inode_count)
        return ERR_PTR(-ENOSPC);

    /* Allocate a new VFS inode */
    inode = new_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    /* Initialize inode owner and permissions */
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_blocks = 0;
    simple_inode_init_ts(inode);

    /* Set inode operations based on file type */
    if (S_ISDIR(mode)) {
        inode->i_op = &osfs_dir_inode_operations;
        inode->i_fop = &osfs_dir_operations;
        set_nlink(inode, 2); /* . and .. */
        inode->i_size = 0;
    } else if (S_ISREG(mode)) {
        inode->i_op = &osfs_file_inode_operations;
        inode->i_fop = &osfs_file_operations;
        set_nlink(inode, 1);
        inode->i_size = 0;
    } else if (S_ISLNK(mode)) {
        // inode->i_op = &osfs_symlink_inode_operations;
        set_nlink(inode, 1);
        inode->i_size = 0;
    }

    /* Get osfs_inode */
    osfs_inode = osfs_get_osfs_inode(sb, ino);
    if (!osfs_inode) {
        pr_err("osfs_new_inode: Failed to get osfs_inode for inode %d\n", ino);
        iput(inode);
        return ERR_PTR(-EIO);
    }
    memset(osfs_inode, 0, sizeof(*osfs_inode));

    /* Initialize osfs_inode */
    osfs_inode->i_ino = ino;
    osfs_inode->i_mode = inode->i_mode;
    osfs_inode->i_uid = i_uid_read(inode);
    osfs_inode->i_gid = i_gid_read(inode);
    osfs_inode->i_size = inode->i_size;
    osfs_inode->i_blocks = 0;  // 初始化為 0
    osfs_inode->__i_atime = osfs_inode->__i_mtime = osfs_inode->__i_ctime = current_time(inode);
    inode->i_private = osfs_inode;

    /* Allocate extent */
    ret = osfs_alloc_extent(sb_info, required_blocks, osfs_inode);
    if (ret) {
        pr_err("osfs_new_inode: Failed to allocate extent\n");
        iput(inode);
        return ERR_PTR(ret);
    }

    /* Update superblock information */
    sb_info->nr_free_inodes--;

    /* Mark inode as dirty */
    mark_inode_dirty(inode);

    return inode;
}


static int osfs_add_dir_entry(struct inode *dir, uint32_t inode_no, const char *name, size_t name_len)
{
    struct osfs_inode *parent_inode = dir->i_private;
    struct osfs_sb_info *sb_info = dir->i_sb->s_fs_info;
    struct osfs_dir_entry *dir_entries;
    void *data_block;
    int i, j, dir_entry_count, ret;

    // 確認名字的長度
    if (name_len > MAX_FILENAME_LEN) {
        pr_err("osfs_add_dir_entry: Filename too long\n");
        return -ENAMETOOLONG;
    }

    // 確定目錄的數據塊位置
    for (i = 0; i < parent_inode->extent_count; i++) {
        data_block = sb_info->data_blocks + parent_inode->extents[i].start_block * BLOCK_SIZE;
        dir_entries = (struct osfs_dir_entry *)data_block;
        dir_entry_count = parent_inode->extents[i].block_count * (BLOCK_SIZE / sizeof(struct osfs_dir_entry));

        // 查找空閒目錄項
        for (j = 0; j < dir_entry_count; j++) {
            if (dir_entries[j].inode_no == 0) {
                dir_entries[j].inode_no = inode_no;
                strncpy(dir_entries[j].filename, name, name_len);
                dir_entries[j].filename[name_len] = '\0';
                pr_info("osfs_add_dir_entry: Added entry '%s' with inode %u\n", name, inode_no);
                return 0;
            }
        }
    }

    // 如果沒有找到空閒目錄項，分配新的 extent
    pr_info("osfs_add_dir_entry: No free entry found, allocating new extent\n");
    ret = osfs_alloc_extent(sb_info, 1, parent_inode);  // 假設需要分配 1 個塊
    if (ret) {
        pr_err("osfs_add_dir_entry: Failed to allocate new extent\n");
        return ret;
    }

    // 遞歸調用以添加目錄項
    return osfs_add_dir_entry(dir, inode_no, name, name_len);
}




/**
 * Function: osfs_create
 * Description: Creates a new file within a directory.
 * Inputs:
 *   - idmap: The mount namespace ID map.
 *   - dir: The inode of the parent directory.
 *   - dentry: The dentry representing the new file.
 *   - mode: The mode (permissions and type) for the new file.
 *   - excl: Whether the creation should be exclusive.
 * Returns:
 *   - 0 on successful creation.
 *   - -EEXIST if the file already exists.
 *   - -ENAMETOOLONG if the file name is too long.
 *   - -ENOSPC if the parent directory is full.
 *   - A negative error code from osfs_new_inode on failure.
 */
static int osfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
    struct osfs_inode *parent_inode = dir->i_private;
    struct osfs_inode *osfs_inode;
    struct inode *inode;
    int ret;
    struct osfs_extent extent;

    // Step 1: Validate the file name length
    if (dentry->d_name.len > MAX_FILENAME_LEN) {
        pr_err("osfs_create: File name too long\n");
        return -ENAMETOOLONG;
    }

    // Step 2: Allocate and initialize VFS & osfs inode
    inode = osfs_new_inode(dir, mode);
    if (IS_ERR(inode)) {
        pr_err("osfs_create: Failed to allocate inode\n");
        return PTR_ERR(inode);
    }
    osfs_inode = inode->i_private;
    if (!osfs_inode) {
        pr_err("osfs_create: Failed to get osfs_inode for inode %lu\n", inode->i_ino);
        iput(inode);
        return -EIO;
    }

    // Step 3: Initialize osfs_inode attributes
    osfs_inode->i_size = 0;
    osfs_inode->i_blocks = 0;

    // Step 4: Allocate extent for the new file
    ret = osfs_alloc_extent(dir->i_sb->s_fs_info, 1, osfs_inode);  // Assuming we need only 1 block initially
    if (ret) {
        pr_err("osfs_create: Failed to allocate extent\n");
        iput(inode);
        return ret;
    }

    // Initialize extent information
    osfs_inode->extent_count = 1;

    // Step 5: Parent directory entry update for the new file
    ret = osfs_add_dir_entry(dir, inode->i_ino, dentry->d_name.name, dentry->d_name.len);
    if (ret) {
        pr_err("osfs_create: Failed to add directory entry\n");
        iput(inode);
        return ret;
    }

    // Step 6: Update the parent directory's metadata
    parent_inode->i_size += sizeof(struct osfs_dir_entry);
    mark_inode_dirty(dir);
    parent_inode->__i_mtime = parent_inode->__i_atime = current_time(dir);
    dir->__i_atime = dir->__i_mtime = current_time(dir);

    dir->i_size = parent_inode->i_size;

    // Step 7: Bind the inode to the VFS dentry
    d_instantiate(dentry, inode);

    pr_info("osfs_create: File '%.*s' created with inode %lu\n",
            (int)dentry->d_name.len, dentry->d_name.name, inode->i_ino);

    return 0;
}






const struct inode_operations osfs_dir_inode_operations = {
    .lookup = osfs_lookup,
    .create = osfs_create,
    // Add other operations as needed
};

const struct file_operations osfs_dir_operations = {
    .iterate_shared = osfs_iterate,
    .llseek = generic_file_llseek,
    // Add other operations as needed
};
