/*
    fat.h
    FAT16 / FAT32 file system driver, so files are saved to a real disk.

    - Only a volume labelled "RENOVATE" is used. That stops the OS writing to someone's
      own FAT drive (USB sticks, recovery partitions) when it boots on a real computer.
    - Reads long file names (written by Windows / Linux), creates 8.3 names (e.g. notes.txt).
    - Disk images can be made and filled from Linux with mtools, see the Makefile.

    Directory paths given to these functions are absolute and end in '/', f_sys.c resolves them.

    https://wiki.osdev.org/FAT
*/
#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// result codes shared by every file system function
#define FS_OK             0
#define FS_ERR_NOT_FOUND -1
#define FS_ERR_EXISTS    -2
#define FS_ERR_BAD_NAME  -3
#define FS_ERR_FULL      -4
#define FS_ERR_NOT_EMPTY -5
#define FS_ERR_IO        -6
#define FS_ERR_BUSY      -7
#define FS_ERR_NO_MEMORY -8

#define FAT_VOLUME_LABEL "RENOVATE"

// the mounted volume
typedef struct {
    bool mounted;
    int bus;
    int slave;
    int type;                    // 16 or 32
    uint32_t volume_lba;         // first sector of the volume on the drive
    uint32_t total_sectors;
    uint8_t sectors_per_cluster;
    uint32_t fat_lba;            // first FAT (absolute sector)
    uint32_t fat_sectors;        // size of one FAT
    uint8_t fat_copies;          // how many FATs to keep up to date
    uint32_t root_lba;           // FAT16: fixed root directory (absolute sector)
    uint32_t root_sectors;       // FAT16: root directory size
    uint32_t root_cluster;       // FAT32: root directory cluster
    uint32_t data_lba;           // cluster 2 (absolute sector)
    uint32_t cluster_count;
    uint32_t fsinfo_lba;         // FAT32: FSInfo sector (0 = none)
    bool fsinfo_invalidated;
    uint32_t next_free;          // where to start looking for a free cluster
    char label[12];
} fat_volume_t;

extern fat_volume_t fat_vol;

const char* fs_strerror (int error);

// look at every IDE drive and mount the first FAT volume labelled RENOVATE
// status receives a line for the boot log
bool fat_mount_auto (char* status, size_t size);
uint32_t fat_free_clusters (void);

bool fat_dir_exists (const char* d_path);
bool fat_file_exists (const char* f_path, const char* name);
char* fat_read_file (const char* f_path, const char* name);
int fat_create_file (const char* f_path, const char* name, const char* contents);
int fat_write_file (const char* f_path, const char* name, const char* text, bool append);
int fat_delete_file (const char* f_path, const char* name);
int fat_create_dir (const char* d_path);
int fat_delete_dir (const char* d_path);
int fat_print_dir (const char* d_path);

#endif /* FAT_H */
