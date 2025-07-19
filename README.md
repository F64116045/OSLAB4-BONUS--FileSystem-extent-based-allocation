# 2024 NCKU OSLab4-BONUS

Virtual File System (VFS) using extent-based allocation strategy  
針對 OSFS（模擬檔案系統）進行以下三項功能實作與修改：
### 檔案分配策略修改 — Extent-based Allocation

- 原始設計每個 inode 只有一個區塊指標（`i_block`）。
- 修改為支援最多 4 個 extent。
- 每個檔案的資料配置與擴充透過 `osfs_alloc_extent()` 。
- 讓檔案可跨多個非連續block，避免資料碎片化時無法擴充。

### `file.c` —  osfs_write 

- 實作 `osfs_write()` ，支援從 User Space 寫入資料至檔案。
  1. 先檢查 inode 是否已有分配的 extent，若沒有，則呼叫 `osfs_alloc_extent()` 分配一個。
  2. 進入 while 迴圈處理所有要寫入的資料，每次遍歷 inode 的所有 extent，確認資料是否落在該範圍。
  3. 若落在某個 extent 中，則：
     - 計算可寫入的最大長度 `to_write`（不超過該 extent 的結尾）
     - 計算實體位置，並將 `user space` 中的資料透過 `copy_from_user()` 複製到 kernel 中的 data block
     - 更新檔案位置指標 `ppos`，並記錄成功寫入的總長度
     - 若寫入位置超過目前檔案大小，也會一併更新 `i_size`

- 當所有 extents 已寫滿但還有資料未寫完時，會再次呼叫 `osfs_alloc_extent()` 分配新的 extent，並重複以上步驟，直到資料全部寫入為止。

### `dir.c` —  osfs_create 

- 實作 `osfs_create()`，處理新檔案建立，包括：
  1. 分配與初始化 inode
     - 使用 `osfs_new_inode()` 為新檔案建立 inode，並取得對應的 `osfs_inode`。
     - 初始化 `i_size` 與 `i_blocks` 為 0，表示尚未寫入資料。
  
  2. 分配初始 extent 空間
     - `osfs_alloc_extent()` 分配 1 個data block作為檔案初始儲存空間。。
  
  3. 新增目錄項目
     - 使用 `osfs_add_dir_entry()` 將該檔案登記到 Parent 目錄中。
     - 更新 Parent 目錄的 `i_size` 。
  
  4. 綁定 inode 與 dentry
     - 透過 `d_instantiate()` 將剛建立的 inode 綁定到該檔案的 dentry，完成 VFS 層整合。
