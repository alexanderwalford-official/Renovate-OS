/*
    fat.h
    FAT16 / FAT32 file system driver, so files are saved to a real disk.

    - Only a volume labelled "AEROS" is used. That stops the OS writing to someone's
      own FAT drive (USB sticks, recovery partitions) when it boots on a real computer.
    - Reads long file names (written by Windows / Linux), creates 8.3 names (e.g. notes.txt).
    - Disk images can be made and filled from Linux with mtools, see the Makefile.

    Directory paths given to these functions are absolute and end in '/', f_sys.c resolves them.

    How it fits into the OS
    -----------------------
    Hardly anything else in the OS calls these functions directly (the shell uses fs_strerror(),
    fat_free_clusters() and has a read-only peek at fat_vol for its welcome text and system
    info). Everything else goes through f_sys.c, which
    tidies up the paths the user types and then calls either this driver (DISK mode) or its own
    in-memory tables (VFS mode). Underneath us sits the ATA PIO driver (drivers/storage/ata.c),
    which reads and writes raw 512 byte sectors. So the stack looks like:

        shell (t_int.c)  ->  f_sys.c  ->  fat.c  ->  ata.c  ->  the IDE ports on the hardware

    A quick tour of a FAT volume
    ----------------------------
    A FAT volume is just a run of 512 byte sectors split into four areas, one after the other:

        [ reserved sectors ][ FAT #1 ][ FAT #2 ][ FAT16 root dir ][ data area (clusters) ...... ]
          ^ boot sector                           (FAT32 has none,
            (BPB) + FSInfo                         its root lives in
            on FAT32                               the data area)

    - The boot sector holds the BIOS Parameter Block (BPB): sector size, sectors per cluster,
      how many reserved sectors, how many FATs, how big they are and so on. fat_mount() reads it.
    - The data area is cut into clusters (a few sectors each). File contents and directories
      live in clusters. Clusters are numbered from 2 (0 and 1 are reserved), so cluster 2 is the
      very first cluster of the data area.
    - The FAT (File Allocation Table) is an array with one entry per cluster. Each entry is either
      0 (free), the number of the next cluster of the same file, or "end of chain". A file is a
      linked list of clusters ("cluster chain") threaded through this table. There are usually two
      identical copies of the FAT in case one gets damaged, so we update every copy.
    - Directories are files too, made of 32 byte entries: an 8.3 name, attributes, dates, the
      first cluster and the size. Long names are stored in extra "VFAT" entries in front of it.
    - FAT12, FAT16 and FAT32 differ mostly in the size of a FAT entry (12, 16 or 28 bits) and
      the type is decided by the number of clusters, nothing else. We support FAT16 and FAT32.

    https://wiki.osdev.org/FAT
    https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system
    Microsoft's "FAT: General Overview of On-Disk Format" (fatgen103) is the official spec.
*/
#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// result codes shared by every file system function
// (f_sys.c returns the same codes in VFS mode, so the shell can report errors the same way in both modes)
#define FS_OK             0  // it worked
#define FS_ERR_NOT_FOUND -1  // the file, directory or disk is not there
#define FS_ERR_EXISTS    -2  // something with that name is already there
#define FS_ERR_BAD_NAME  -3  // the name is not allowed (on disk it must fit 8.3), or the volume is not labelled AEROS
#define FS_ERR_FULL      -4  // no free clusters / slots left (or the fixed FAT16 root directory is full)
#define FS_ERR_NOT_EMPTY -5  // tried to delete a directory that still has something in it
#define FS_ERR_IO        -6  // the drive reported a read or write error
#define FS_ERR_BUSY      -7  // tried to delete the current directory (or one of its parents)
#define FS_ERR_NO_MEMORY -8  // malloc / realloc failed

// the volume label a disk must have before we dare to write to it (compared ignoring case)
// format one with "mformat -v AEROS" (the Makefile does this for disk.img)
#define FAT_VOLUME_LABEL "AEROS"

// the mounted volume
// everything we learned from the boot sector, worked out once in fat_mount() so the rest of the
// driver can turn cluster numbers into sector numbers with a bit of simple arithmetic.
// all the "_lba" fields are absolute sector numbers on the drive (they already include volume_lba)
typedef struct {
    bool mounted;                // true once a AEROS volume was found and fully checked
    int bus;                     // IDE bus the drive is on: 0 = primary, 1 = secondary
    int slave;                   // 0 = master drive, 1 = slave drive on that bus
    int type;                    // 16 or 32
    uint32_t volume_lba;         // first sector of the volume on the drive
                                 // (0 for a "superfloppy" without a partition table, otherwise where the MBR partition starts)
    uint32_t total_sectors;      // size of the whole volume in sectors (512 bytes each)
    uint8_t sectors_per_cluster; // cluster size in sectors, always a power of two (1, 2, 4 ... 128)
    uint32_t fat_lba;            // first FAT (absolute sector)
                                 // (on FAT32 with mirroring turned off this is the "active" FAT instead)
    uint32_t fat_sectors;        // size of one FAT
    uint8_t fat_copies;          // how many FATs to keep up to date
    uint32_t root_lba;           // FAT16: fixed root directory (absolute sector)
    uint32_t root_sectors;       // FAT16: root directory size (0 on FAT32)
    uint32_t root_cluster;       // FAT32: root directory cluster (usually 2)
    uint32_t data_lba;           // cluster 2 (absolute sector)
    uint32_t cluster_count;      // how many clusters the data area holds, valid ones are 2 .. cluster_count + 1
    uint32_t fsinfo_lba;         // FAT32: FSInfo sector (0 = none)
    bool fsinfo_invalidated;     // already marked the FSInfo free count as "unknown" since mounting (only done once)
    uint32_t next_free;          // where to start looking for a free cluster
                                 // (a hint so we don't rescan the full start of the FAT every time we allocate)
    char label[12];              // volume label, 11 characters + '\0' with the space padding trimmed
} fat_volume_t;

// the one and only mounted volume (the OS only ever uses one disk at a time)
// mounted is false when we are in VFS mode
extern fat_volume_t fat_vol;

// a short English description of an FS_ / FS_ERR_ result code, for error messages in the shell
const char* fs_strerror (int error);

// look at every IDE drive and mount the first FAT volume labelled AEROS
// status receives a line for the boot log
// returns true if a volume was mounted (fat_vol is then filled in), false if none was found
bool fat_mount_auto (char* status, size_t size);

// count the free clusters by reading the whole FAT (slow on big disks, used by the system info command)
uint32_t fat_free_clusters (void);

// does this absolute directory path ("/sys/cache/") exist on the disk?
bool fat_dir_exists (const char* d_path);

// does a file (not a directory) called name exist in directory f_path? name may be a long name or 8.3, any case
bool fat_file_exists (const char* f_path, const char* name);

// read a whole file into a new '\0' terminated buffer (free() it when done), NULL if missing or on errors
// size (optional, may be NULL) receives the file's length in bytes
char* fat_read_file (const char* f_path, const char* name, uint32_t* size);

// READING A FILE A PIECE AT A TIME, for files too big to read whole (a video). fat_open fills in
// a fat_file_t, then fat_read_at reads any part of it. The file's clusters form a chain, so to
// read from the middle the chain has to be followed from the start; fat_file_t remembers the
// cluster it last read, so reading on from there (the usual case) costs nothing extra
typedef struct {
    uint32_t first_cluster;   // where the file starts
    uint32_t size;            // its length in bytes
    uint32_t cluster;         // the cluster read last...
    uint32_t cluster_index;   // ...and which one of the file's clusters that is (0 = the first)
} fat_file_t;

// find a file and get it ready for fat_read_at, false if there is no such file
bool fat_open (const char* f_path, const char* name, fat_file_t* file);

// read up to `length` bytes from `offset` into out. Returns how many were read: fewer than
// length at the end of the file, 0 past it, and -1 on a disk error
int32_t fat_read_at (fat_file_t* file, uint32_t offset, void* out, uint32_t length);

// create a new file holding contents, name must fit the 8.3 format (FS_ERR_BAD_NAME otherwise)
int fat_create_file (const char* f_path, const char* name, const char* contents, uint32_t length);

// replace (append = false) or add to (append = true) the contents of an existing file
int fat_write_file (const char* f_path, const char* name, const char* text, bool append);

// replace the contents of an existing file with length bytes of data (which may contain '\0')
// wipe_old = true fills the old clusters with zeros before freeing them, so the old contents
// can't be recovered from the disk (used after encrypting a file)
int fat_replace_file (const char* f_path, const char* name, const void* data, uint32_t length, bool wipe_old);

// delete a file and give its clusters back
int fat_delete_file (const char* f_path, const char* name);

// create a directory, d_path is its full path ending in '/' (e.g. "/sys/cache/"), the parent must exist
int fat_create_dir (const char* d_path);

// delete an empty directory (FS_ERR_NOT_EMPTY if anything is still in it)
int fat_delete_dir (const char* d_path);

// print a directory listing (directories first, then files)
// returns how many entries were printed, or -1 if the directory does not exist
int fat_print_dir (const char* d_path);

// called once per entry by fat_list_dir / fs_list: its name, whether it is a directory, its size
typedef void (*fs_list_fn)(const char* name, bool is_dir, uint32_t size, void* ctx);

// call fn for every file and directory in d_path, returns how many there were (-1 if it doesn't exist)
int fat_list_dir (const char* d_path, fs_list_fn fn, void* ctx);

#endif /* FAT_H */
