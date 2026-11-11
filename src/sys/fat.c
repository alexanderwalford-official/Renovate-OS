/*
    fat.c
    FAT16 / FAT32 file system driver, see fat.h.

    fat.h has the big picture (the four areas of a FAT volume), this is the detail you need to
    follow the code below.

    The boot sector and the BPB
    ---------------------------
    The first sector of the volume. Everything is little endian and many fields sit at odd
    offsets, which is why fat_mount() reads them byte by byte with fat_u16() / fat_u32().

        offset size  field                         offset size  field (FAT32 only)
        0      3     jump instruction (EB / E9)    36     4     sectors per FAT (32 bit)
        3      8     OEM name, e.g. "MSDOS5.0"     40     2     flags: bit 7 = mirroring off,
        11     2     bytes per sector (512)                     bits 0-3 = the active FAT
        13     1     sectors per cluster           44     4     first cluster of the root dir
        14     2     reserved sectors              48     2     FSInfo sector number
        16     1     number of FATs (usually 2)    50     2     backup boot sector
        17     2     root dir entries (FAT16)      71     11    volume label
        19     2     total sectors (16 bit, or 0)  82     8     "FAT32   "
        21     1     media descriptor
        22     2     sectors per FAT (FAT16, or 0)        (FAT16 only)
        28     4     hidden sectors                43     11    volume label
        32     4     total sectors (32 bit)        54     8     "FAT16   "
        510    2     0x55 0xAA boot signature

    From those we work out where everything starts (all in sectors, from the volume's start):
        first FAT      = reserved
        FAT16 root dir = reserved + FATs * sectors per FAT
        data area      = that + root dir sectors (root entries * 32 bytes, rounded up to a sector)
        cluster N      = data area + (N - 2) * sectors per cluster

    FAT12, FAT16 or FAT32?
    ----------------------
    Microsoft's rule is based only on the number of data clusters, never on the "FAT16" text:
    fewer than 4085 is FAT12, fewer than 65525 is FAT16, anything more is FAT32. FAT12 packs
    entries into 1.5 bytes (floppy disks), we don't support it.

    FAT entries
    -----------
    One per cluster, entries 0 and 1 are reserved (that's why clusters start at 2).
        FAT16 (16 bit)          FAT32 (32 bit, only the low 28 bits count)
        0x0000                  0x00000000              free
        0x0002 - 0xFFEF         0x00000002 - 0x0FFFFFEF the next cluster of the chain
        0xFFF7                  0x0FFFFFF7              bad cluster, never use it
        0xFFF8 - 0xFFFF         0x0FFFFFF8 - 0x0FFFFFFF end of chain (last cluster of a file)
    On FAT32 the top 4 bits are reserved: we mask them off when reading and keep them as they
    were when writing. Anything that isn't a valid cluster number (end of chain, bad cluster, a
    read error) stops a chain walk, because fat_valid_cluster() says no.

    Directory entries (32 bytes)
    ----------------------------
        0   name, 8 characters + 3 for the extension, upper case, padded with spaces, no dot
            ("NOTES   TXT"). name[0] is special: 0x00 = end of directory, 0xE5 = deleted,
            0x05 = the name really starts with 0xE5
        11  attributes: 0x01 read only, 0x02 hidden, 0x04 system, 0x08 volume label,
            0x10 directory, 0x20 archive (changed since the last backup). 0x0F = long name entry
        12  Windows NT case flags (0x08 = name is lower case, 0x10 = extension is lower case)
        13  creation time, 10 millisecond units (0 - 199)
        14  creation time      16  creation date      18  last access date
        20  high 16 bits of the first cluster (FAT32 only, 0 on FAT16)
        22  modification time  24  modification date
        26  low 16 bits of the first cluster
        28  file size in bytes (0 for directories)

    Long file names (VFAT)
    ----------------------
    Windows hides a long name in extra entries (attribute 0x0F, which old systems skip because
    it looks like a volume label) placed just in front of the normal 8.3 entry, last part first:

        [0x43 "ong name.txt"][0x02 "e is a very l"][0x01 "This file nam"][THISFI~1TXT]

    Each part holds 13 UTF-16 characters at byte offsets 1-10, 14-25 and 28-31, the low 5 bits
    of byte 0 are its sequence number (1, 2, 3...) and 0x40 marks the last part. Byte 13 holds a
    checksum of the 8.3 name, so if an old OS renames the 8.3 entry, the stale long name no
    longer matches and is ignored. We can read long names but only ever write 8.3 names.

    Keeping the disk safe
    ---------------------
    - We only mount a volume labelled AEROS (see fat_mount()).
    - Changes are made in an order where a crash halfway can at worst leave "lost clusters"
      (marked used but owned by nobody, chkdsk / fsck tidy those up), never an entry pointing
      at garbage. E.g. new file data is written before its directory entry, and old data is
      only freed after the entry points at the new data.

    https://wiki.osdev.org/FAT
    https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system
*/
#include "fat.h"
#include "sys_clck.h"
#include "../drivers/storage/ata.h"
#include "../lib/aer_ctype.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_stdio.h"
#include "../lib/busy.h"

#define FAT_SECTOR_SIZE        512   // the only sector size we handle (drive_is_fat_boot_sector() checks it)
#define FAT_ENTRIES_PER_SECTOR (FAT_SECTOR_SIZE / 32)  // 16 directory entries of 32 bytes per sector
#define FAT_ROOT               0     // directory "cluster" used for the root directory
                                     // (no real directory starts at cluster 0, and ".." entries use 0 for the root too)
#define FAT_NAME_MAX           255   // longest long file name (in characters)
#define FAT_LFN_PARTS          20    // 20 long name entries of 13 characters hold the longest name
#define FAT_DIR_MAX_SECTORS    4096  // FAT allows at most 65536 entries in a directory
                                     // (65536 entries * 32 bytes / 512 = 4096 sectors)
#define FAT_NO_SECTOR          0xFFFFFFFF  // "nothing cached" for fat_cache_lba

// directory entry attribute bits (byte 11 of an entry)
#define FAT_ATTR_VOLUME_ID 0x08 // not a file: this entry holds the volume label
#define FAT_ATTR_DIRECTORY 0x10 // a sub directory, its clusters hold more directory entries
#define FAT_ATTR_ARCHIVE   0x20 // "changed since the last backup", set whenever we write a file
#define FAT_ATTR_LFN       0x0F // read only + hidden + system + volume id

// byte 12 of an entry, lets a plain 8.3 name like "notes.txt" show in lower case without a long name
#define FAT_CASE_LOWER_BASE 0x08 // Windows NT: name part is lower case
#define FAT_CASE_LOWER_EXT  0x10 // Windows NT: extension is lower case

// first byte of a directory entry
#define FAT_DIR_END     0x00 // this and every later entry are unused
#define FAT_DIR_DELETED 0xE5 // a deleted entry, the slot can be reused (later entries may still be in use)

// a 32 byte directory entry
// (packed so the compiler can't add padding, the layout has to match the disk byte for byte.
// see the table at the top of this file for what the bytes mean)
typedef struct __attribute__((packed)) {
    uint8_t name[11];      // "NOTES   TXT": 8 name + 3 extension characters, space padded, no dot
    uint8_t attr;          // FAT_ATTR_ bits
    uint8_t nt_case;       // FAT_CASE_ flags (officially "reserved for Windows NT")
    uint8_t ctime_tenths;  // extra creation time precision, we leave it at 0
    uint16_t ctime;        // creation time   (hours << 11 | minutes << 5 | seconds / 2)
    uint16_t cdate;        // creation date   ((year - 1980) << 9 | month << 5 | day)
    uint16_t adate;        // last access date (there is no access time)
    uint16_t cluster_high; // first cluster, top 16 bits (FAT32 only)
    uint16_t mtime;        // last modification time
    uint16_t mdate;        // last modification date
    uint16_t cluster_low;  // first cluster, low 16 bits (0 for an empty file)
    uint32_t size;         // file size in bytes, 0 for directories
} fat_dirent_t;

// a sector seen as bytes, FAT16 / FAT32 entries or directory entries
// (a union lets the same 512 bytes be read whichever way suits, without casts. x86 is little
// endian like FAT, so u16[i] / u32[i] are FAT entries exactly as they are stored on the disk)
typedef union {
    uint8_t u8[FAT_SECTOR_SIZE];
    uint16_t u16[FAT_SECTOR_SIZE / 2];
    uint32_t u32[FAT_SECTOR_SIZE / 4];
    fat_dirent_t dirents[FAT_ENTRIES_PER_SECTOR];
} fat_sector_t;

// where we are while stepping through the sectors of a directory
// (a directory is either the fixed FAT16 root, a plain run of sectors, or a cluster chain like
// a file. This hides the difference: fat_dir_start() / fat_dir_next() fill it in)
typedef struct {
    uint32_t cluster; // 0 in the fixed size FAT16 root directory
    uint32_t index;   // sector number inside the directory
    uint32_t lba;     // absolute sector number of that sector on the drive
} fat_dir_pos_t;

// a file or directory found while walking a directory
// (a copy of its entry plus where it lives, so it can be rewritten or deleted later)
typedef struct {
    fat_dirent_t entry;      // a copy of the 32 byte 8.3 entry
    uint32_t lba;            // sector holding the entry
    uint16_t slot;           // its place in that sector
    fat_dir_pos_t lfn_pos;   // where its long name entries start (the entry itself without a long name)
    uint16_t lfn_slot;       // and the slot of the first long name entry in that sector
    char name[FAT_NAME_MAX + 1];  // long name, or the 8.3 name
    char short_name[13];          // 8.3 name, e.g. "NOTES.TXT"
} fat_entry_t;

// the callback fat_walk_dir() calls for every entry, ctx is whatever the caller wants to pass along
// (C's version of a closure: the caller's state travels in a struct behind the void*)
typedef bool (*fat_visit_t)(const fat_entry_t* entry, void* ctx); // return true to stop walking

fat_volume_t fat_vol;
// a one sector cache: walking a cluster chain reads FAT entries that are usually next to each
// other, so keeping the last FAT sector we read saves a disk read for most fat_get() calls.
// fat_set() writes straight through to the disk and updates the cache, so it never goes stale
static fat_sector_t fat_cache; // one cached sector of the (first) FAT
static uint32_t fat_cache_lba = FAT_NO_SECTOR; // which sector fat_cache holds (FAT_NO_SECTOR = none)

// a short English description of a result code, for the shell's error messages
const char* fs_strerror (int error) {
    switch (error) {
        case FS_OK: return "ok";
        case FS_ERR_NOT_FOUND: return "not found";
        case FS_ERR_EXISTS: return "already exists";
        case FS_ERR_BAD_NAME: return "invalid name (on disk use 8.3 names, e.g. notes.txt)";
        case FS_ERR_FULL: return "no space left";
        case FS_ERR_NOT_EMPTY: return "directory is not empty";
        case FS_ERR_IO: return "disk read / write error";
        case FS_ERR_BUSY: return "it is the current directory";
        case FS_ERR_NO_MEMORY: return "out of memory";
        case FS_ERR_READ_ONLY: return "it can be read but not changed";
        default: return "unknown error";
    }
}

// read one 512 byte sector of the mounted drive (lba is absolute), false on a drive error
// (these two keep fat_vol.bus / fat_vol.slave out of every other function)
static bool fat_read_sector (uint32_t lba, void* buffer) {
    return drive_read(fat_vol.bus, fat_vol.slave, lba, 1, buffer);
}

// write one 512 byte sector to the mounted drive (lba is absolute), false on a drive error
static bool fat_write_sector (uint32_t lba, const void* buffer) {
    return drive_write(fat_vol.bus, fat_vol.slave, lba, 1, buffer);
}

// the first sector of a cluster (clusters are numbered from 2, cluster 2 is the start of the data area)
static uint32_t fat_cluster_lba (uint32_t cluster) {
    return fat_vol.data_lba + (cluster - 2) * fat_vol.sectors_per_cluster;
}

// is this a real cluster of the data area? false for 0 (free / empty file), 1, end of chain markers,
// bad clusters and the 0xFFFFFFFF fat_get() returns on read errors, so it doubles as "is there a next cluster?"
static bool fat_valid_cluster (uint32_t cluster) {
    return cluster >= 2 && cluster < fat_vol.cluster_count + 2;
}

// the value that marks the last cluster of a chain
static uint32_t fat_end_of_chain (void) {
    return (fat_vol.type == 32) ? 0x0FFFFFFF : 0xFFFF;
}

// how many FAT entries fit in one sector: 128 on FAT32 (4 bytes each), 256 on FAT16 (2 bytes each)
static uint32_t fat_entries_per_sector (void) {
    return (fat_vol.type == 32) ? FAT_SECTOR_SIZE / 4 : FAT_SECTOR_SIZE / 2;
}

// the FAT entry for a cluster: 0 = free, a cluster number, or end of chain (0xFFFFFFFF on read errors)
// only the first FAT is read, the copies are only there for repairing a damaged disk
static uint32_t fat_get (uint32_t cluster) {
    // which sector of the FAT holds this entry, and which entry inside that sector
    uint32_t lba = fat_vol.fat_lba + cluster / fat_entries_per_sector();
    uint32_t i = cluster % fat_entries_per_sector();

    // load that sector unless it is the one already in the cache
    if (fat_cache_lba != lba) {
        if (!fat_read_sector(lba, &fat_cache)) {
            fat_cache_lba = FAT_NO_SECTOR; // the buffer may be half overwritten now, forget it
            return 0xFFFFFFFF;
        }
        fat_cache_lba = lba;
    }
    // FAT32 entries are 28 bits, the top 4 are reserved and must be ignored
    return (fat_vol.type == 32) ? fat_cache.u32[i] & 0x0FFFFFFF : fat_cache.u16[i];
}

// change the FAT entry for a cluster in every copy of the FAT
// each copy is read, changed and written back one sector at a time (a read-modify-write).
// gotcha: if a write fails halfway, the copies can disagree. Returns false on drive errors
static bool fat_set (uint32_t cluster, uint32_t value) {
    fat_sector_t sector;
    uint32_t i = cluster % fat_entries_per_sector();

    for (uint8_t copy = 0; copy < fat_vol.fat_copies; copy++) {
        // the copies sit right after each other, fat_sectors apart
        uint32_t lba = fat_vol.fat_lba + copy * fat_vol.fat_sectors + cluster / fat_entries_per_sector();
        // the cached sector is already up to date in memory, no need to read it again
        if (lba == fat_cache_lba) {
            sector = fat_cache;
        } else if (!fat_read_sector(lba, &sector)) {
            return false;
        }
        if (fat_vol.type == 32) {
            sector.u32[i] = (sector.u32[i] & 0xF0000000) | (value & 0x0FFFFFFF); // the top 4 bits are reserved
        } else {
            sector.u16[i] = (uint16_t)value;
        }
        if (!fat_write_sector(lba, &sector)) {
            return false;
        }
        // keep the cache in step with the disk (only ever true for the first copy)
        if (lba == fat_cache_lba) {
            fat_cache = sector;
        }
    }
    return true;
}

// FAT32 keeps a "free clusters" count in the FSInfo sector, mark it as unknown once we change the FAT
// why: we don't keep that count up to date as we go. If we left the old number there, Windows or
// Linux would trust it and show the wrong free space. 0xFFFFFFFF tells them to count again.
// it only has to be done once per mount, fsinfo_invalidated remembers that we did
// FSInfo layout: 0 "RRaA" signature, 484 "rrAa" signature, 488 free cluster count,
// 492 next free cluster hint, 508 0xAA550000 signature
static void fat_invalidate_fsinfo (void) {
    fat_sector_t sector;
    if (fat_vol.type != 32 || fat_vol.fsinfo_invalidated || fat_vol.fsinfo_lba == 0) {
        return;
    }
    fat_vol.fsinfo_invalidated = true;
    // only touch it if both signatures are right ("RRaA" and "rrAa" read as little endian numbers)
    if (!fat_read_sector(fat_vol.fsinfo_lba, &sector) ||
        sector.u32[0] != 0x41615252 || sector.u32[484 / 4] != 0x61417272) {
        return;
    }
    memset(sector.u8 + 488, 0xFF, 8); // free count & next free = unknown
    fat_write_sector(fat_vol.fsinfo_lba, &sector);
}

// fill a cluster with zeros
// used for new directory clusters: a zeroed sector is full of FAT_DIR_END entries, i.e. empty
static bool fat_zero_cluster (uint32_t cluster) {
    fat_sector_t sector;
    memset(&sector, 0, sizeof(sector));
    for (uint32_t s = 0; s < fat_vol.sectors_per_cluster; s++) {
        if (!fat_write_sector(fat_cluster_lba(cluster) + s, &sector)) {
            return false;
        }
    }
    return true;
}

// take a free cluster and add it to the end of a chain (prev = 0 starts a new chain), returns 0 if the disk is full
// the search is "next fit": start at the next_free hint and wrap around to cluster 2 at the end,
// so files written one after another get clusters next to each other and we don't rescan the
// full part at the front of the FAT every time. 0 is also returned on drive errors
static uint32_t fat_alloc_cluster (uint32_t prev) {
    fat_invalidate_fsinfo();
    for (uint32_t n = 0; n < fat_vol.cluster_count; n++) {
        // the n-th cluster counting from next_free, wrapping round from the last cluster back to 2
        uint32_t cluster = 2 + (fat_vol.next_free - 2 + n) % fat_vol.cluster_count;
        if (fat_get(cluster) != 0) {
            continue; // in use (or unreadable, which we treat as in use to be safe)
        }
        // mark it as the end of a chain before linking it in, so prev never points at a "free" cluster
        if (!fat_set(cluster, fat_end_of_chain())) {
            return 0;
        }
        if (prev != 0 && !fat_set(prev, cluster)) {
            fat_set(cluster, 0); // linking failed, give the cluster back
            return 0;
        }
        // the next search starts just after this one, or back at cluster 2 if this was the last
        uint32_t last_cluster = fat_vol.cluster_count + 1;
        if (cluster < last_cluster) {
            fat_vol.next_free = cluster + 1;
        } else {
            fat_vol.next_free = 2;
        }
        return cluster;
    }
    return 0;
}

// mark entry `i` of a FAT sector as free (0). On FAT32 the top 4 bits are reserved and are
// kept as they were
static void fat_clear_entry (fat_sector_t* sector, uint32_t i) {
    if (fat_vol.type == 32) {
        sector->u32[i] &= 0xF0000000;
    } else {
        sector->u16[i] = 0;
    }
}

// write one changed sector of the FAT to the disk, in every copy of the FAT.
// The first copy is fat_cache, which has already been changed in memory. Each other copy is
// read from the disk, has the same entries freed (the `freed` list), and is written back, so
// it only changes in those places, just as fat_set() would do it. False on a drive error
static bool fat_write_freed_sector (uint32_t sector_index, const uint16_t* freed, uint32_t freed_count) {
    for (uint8_t copy = 0; copy < fat_vol.fat_copies; copy++) {
        uint32_t lba = fat_vol.fat_lba + copy * fat_vol.fat_sectors + sector_index;

        if (copy == 0) {
            if (!fat_write_sector(lba, &fat_cache)) {
                fat_cache_lba = FAT_NO_SECTOR; // the disk may not match the cache now: forget it
                return false;
            }
            continue;
        }

        fat_sector_t sector;
        if (!fat_read_sector(lba, &sector)) {
            return false;
        }
        for (uint32_t n = 0; n < freed_count; n++) {
            fat_clear_entry(&sector, freed[n]);
        }
        if (!fat_write_sector(lba, &sector)) {
            return false;
        }
    }
    return true;
}

// Free every cluster of a chain. `cluster` is the first one; 0 (an empty file) does nothing.
//
// The simple way, fat_set(cluster, 0) for each cluster, costs a read and a write of a FAT
// sector, in each copy of the FAT, for every single cluster: about 30000 disk transfers for a
// 20MB file. Instead this frees all the clusters of the chain whose entries are in the same
// FAT sector (a file's clusters are usually next to each other, so that is up to 256 of them)
// in memory, then writes that sector once.
static void fat_free_chain (uint32_t cluster) {
    if (!fat_valid_cluster(cluster)) {
        return;
    }
    fat_invalidate_fsinfo();
    uint32_t per_sector = fat_entries_per_sector();
    const uint32_t most_freed = FAT_SECTOR_SIZE / 2; // the most entries one sector can hold

    // A damaged disk can have a chain that loops back on itself. No real chain is longer than
    // the disk has clusters, so stop after that many steps whatever happens
    uint32_t steps = 0;

    while (fat_valid_cluster(cluster) && steps < fat_vol.cluster_count) {
        uint32_t sector_index = cluster / per_sector; // which sector of the FAT this entry is in
        uint16_t freed[FAT_SECTOR_SIZE / 2];          // the entries freed in that sector
        uint32_t freed_count = 0;

        // fat_get loads the sector into fat_cache (or fails on a drive error)
        if (fat_get(cluster) == 0xFFFFFFFF) {
            return;
        }

        // free entries for as long as the chain stays in this sector
        for (;;) {
            bool still_in_sector = fat_valid_cluster(cluster) && cluster / per_sector == sector_index;
            bool room_left = freed_count < most_freed && steps < fat_vol.cluster_count;
            if (!still_in_sector || !room_left) {
                break;
            }
            uint32_t entry = cluster % per_sector;
            uint32_t next = fat_get(cluster); // read where the chain goes before clearing the link
            fat_clear_entry(&fat_cache, entry);
            freed[freed_count] = (uint16_t)entry;
            freed_count++;
            cluster = next;
            steps++;
        }

        if (!fat_write_freed_sector(sector_index, freed, freed_count)) {
            return;
        }
        busy(); // a big file spans many FAT sectors: let a spinner turn (DEL)
    }
}

// count the free clusters by checking every FAT entry
// thanks to the cache this is one sector read per 128 (FAT32) or 256 (FAT16) clusters
uint32_t fat_free_clusters (void) {
    uint32_t free_count = 0;
    // the data clusters are numbered 2 to cluster_count + 1 (entries 0 and 1 are reserved)
    for (uint32_t c = 2; c < fat_vol.cluster_count + 2; c++) {
        if (fat_get(c) == 0) {
            free_count++;
        }
    }
    return free_count;
}

// the checksum of an 11 byte 8.3 name that every long name entry belonging to it carries
// (rotate the sum right by one bit, then add the next character, for all 11 characters)
static uint8_t fat_lfn_checksum (const uint8_t* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

// "NOTES   TXT" -> "NOTES.TXT" (or "notes.txt" when the Windows lower case flags are set)
// raw = the 11 name bytes of an entry, out needs room for 13 characters ("12345678.123" + '\0')
// apply_case = false gives the plain upper case name, which is what fat_find() compares against
static void fat_short_to_name (const uint8_t* raw, uint8_t nt_case, char* out, bool apply_case) {
    bool lower_name = apply_case && (nt_case & FAT_CASE_LOWER_BASE) != 0;
    bool lower_extension = apply_case && (nt_case & FAT_CASE_LOWER_EXT) != 0;
    int length = 0;

    // the name part: up to 8 characters, stopping at the space padding
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)raw[i];
        if (i == 0 && raw[i] == 0x05) {
            c = (char)0xE5; // 0x05 in the first byte stands in for a real 0xE5 (0xE5 means "deleted")
        }
        if (lower_name) {
            c = (char)tolower((unsigned char)c);
        }
        out[length++] = c;
    }

    // the extension, bytes 8 to 10. The dot only goes in when there is one
    if (raw[8] != ' ') {
        out[length++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            char c = (char)raw[i];
            if (lower_extension) {
                c = (char)tolower((unsigned char)c);
            }
            out[length++] = c;
        }
    }
    out[length] = '\0';
}

// may this character be used in an 8.3 name? letters, digits and a handful of symbols
// (the c != '\0' check matters: strchr() treats the string's own '\0' as part of it and would match)
static bool fat_valid_83_char (char c) {
    return isalnum((unsigned char)c) || (c != '\0' && strchr("!#$%&'()-@^_`{}~", c) != NULL);
}

// copy the name or extension part of an 8.3 name in upper case
// an all lower case part is remembered with its Windows NT flag, mixed case is just stored upper case
// (the flags can only say "all lower case", so "Notes" can't be kept as it is and becomes "NOTES")
// src / len = the part to copy, out = where it goes in the 11 byte name, lower_flag = which FAT_CASE_ bit this part uses
// returns false if a character isn't allowed in an 8.3 name
static bool fat_copy_83_part (const char* src, size_t len, uint8_t* out, uint8_t lower_flag, uint8_t* nt_case) {
    bool lower = false, upper = false; // did we see any lower / upper case letters?
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (!fat_valid_83_char((char)c)) {
            return false;
        }
        lower = lower || islower(c);
        upper = upper || isupper(c);
        out[i] = (uint8_t)toupper(c);
    }
    if (lower && !upper) {
        *nt_case |= lower_flag;
    }
    return true;
}

// "notes.txt" -> "NOTES   TXT" plus the lower case flags, false if the name does not fit 8.3
// not allowed: an empty name part (".hidden"), more than 8 + 3 characters, a dot with nothing
// after it ("notes.") or more than one dot ("a.b.c"). We don't write long names, so those are refused
static bool fat_make_83 (const char* name, uint8_t* out, uint8_t* nt_case) {
    // split "notes.txt" at the dot into the name ("notes") and the extension ("txt")
    const char* dot = strchr(name, '.');
    const char* extension = "";
    size_t name_length = slen(name);
    size_t extension_length = 0;
    if (dot != NULL) {
        extension = dot + 1;
        name_length = (size_t)(dot - name);
        extension_length = slen(extension);
    }

    bool name_fits = name_length >= 1 && name_length <= 8;
    bool extension_fits = extension_length <= 3;
    bool dot_with_nothing_after = dot != NULL && extension_length == 0;   // "notes."
    bool second_dot = dot != NULL && strchr(extension, '.') != NULL;     // "a.b.c"
    if (!name_fits || !extension_fits || dot_with_nothing_after || second_dot) {
        return false;
    }

    memset(out, ' ', 11); // unused characters are spaces
    *nt_case = 0;

    // the name into bytes 0 to 7, the extension into bytes 8 to 10
    if (!fat_copy_83_part(name, name_length, out, FAT_CASE_LOWER_BASE, nt_case)) {
        return false;
    }
    return fat_copy_83_part(extension, extension_length, out + 8, FAT_CASE_LOWER_EXT, nt_case);
}

// set the modified (and if created, the created) date & time of an entry to now
// FAT packs dates and times into 16 bits each:
//   date: bits 15-9 = years since 1980 (0 - 127), bits 8-5 = month (1 - 12), bits 4-0 = day (1 - 31)
//   time: bits 15-11 = hours, bits 10-5 = minutes, bits 4-0 = seconds / 2 (so only every 2 seconds)
// the last access date is set to today as well
static void fat_stamp (fat_dirent_t* d, bool created) {
    rtc_time_t t;
    rtc_read(&t);
    uint16_t year = (t.year >= 1980) ? (uint16_t)(t.year - 1980) : 0; // FAT can't store years before 1980
    uint16_t date = (uint16_t)((year << 9) | (t.month << 5) | t.day);
    uint16_t time = (uint16_t)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
    d->mdate = date;
    d->mtime = time;
    d->adate = date;
    if (created) {
        d->cdate = date;
        d->ctime = time;
    }
}

// "DD-MM-YY--HH:MM:SS", the same format as the temporary file system
// (unpacks the bit fields fat_stamp() describes, seconds are stored halved so they are doubled again)
static void fat_format_datetime (uint16_t date, uint16_t time, char* out, size_t size) {
    sfmt(out, size, "%02u-%02u-%02u--%02u:%02u:%02u",
             date & 0x1F, (date >> 5) & 0x0F, ((date >> 9) + 1980) % 100,
             time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2);
}

// the first cluster of an entry, put together from its two 16 bit halves
// (on FAT16 the high half is always 0, so this works for both)
static uint32_t fat_entry_cluster (const fat_dirent_t* d) {
    return ((uint32_t)d->cluster_high << 16) | d->cluster_low;
}

// store a first cluster in an entry, split into its two 16 bit halves
static void fat_set_entry_cluster (fat_dirent_t* d, uint32_t cluster) {
    d->cluster_high = (uint16_t)(cluster >> 16);
    d->cluster_low = (uint16_t)(cluster & 0xFFFF);
}

// first sector of a directory, false if the directory is broken
// dir = the directory's first cluster, or FAT_ROOT for the root directory, which is either the
// fixed area after the FATs (FAT16) or an ordinary cluster chain starting at root_cluster (FAT32)
static bool fat_dir_start (uint32_t dir, fat_dir_pos_t* pos) {
    pos->index = 0;
    if (dir == FAT_ROOT && fat_vol.type == 16) {
        pos->cluster = 0; // cluster 0 tells fat_dir_next() this is the fixed root
        pos->lba = fat_vol.root_lba;
        return fat_vol.root_sectors > 0;
    }
    pos->cluster = (dir == FAT_ROOT) ? fat_vol.root_cluster : dir;
    pos->lba = fat_cluster_lba(pos->cluster);
    return fat_valid_cluster(pos->cluster);
}

// step to the next sector of a directory, following its cluster chain, false at its end
static bool fat_dir_next (fat_dir_pos_t* pos) {
    if (++pos->index >= FAT_DIR_MAX_SECTORS) {
        return false; // also stops a looping chain on a damaged disk
    }
    // the FAT16 root is one straight run of root_sectors sectors
    if (pos->cluster == 0) {
        pos->lba++;
        return pos->index < fat_vol.root_sectors;
    }
    // still inside the same cluster: the next sector is right after this one
    if (pos->index % fat_vol.sectors_per_cluster != 0) {
        pos->lba++;
        return true;
    }
    // past the end of the cluster: ask the FAT where the directory carries on
    // (on end of chain pos->cluster is left on the last cluster, fat_free_slot() relies on that)
    uint32_t next = fat_get(pos->cluster);
    if (!fat_valid_cluster(next)) {
        return false;
    }
    pos->cluster = next;
    pos->lba = fat_cluster_lba(next);
    return true;
}

// call visit for every file and sub directory ("." and ".." skipped), returns true if visit stopped early
// this is the heart of the driver: every lookup, listing and "is it empty?" check goes through it.
// it also glues long name entries back together and hands visit the finished name.
// gotcha: a read error also returns false, so to the caller it looks just like "not found"
static bool fat_walk_dir (uint32_t dir, fat_visit_t visit, void* ctx) {
    // where the 13 UTF-16 characters of a long name entry are (byte offsets, 2 bytes each)
    static const uint8_t lfn_char_offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    fat_sector_t sector;
    fat_entry_t e;
    fat_dir_pos_t pos;
    // the long name being collected:
    fat_dir_pos_t lfn_pos = { 0, 0, 0 };     // where its first entry is (for deleting it later)
    uint16_t lfn_slot = 0;
    char lfn[FAT_LFN_PARTS * 13 + 1];        // the characters so far (+1 so it always ends in '\0')
    bool lfn_valid = false;                  // are we in the middle of a good long name?
    int lfn_expect = 0;                      // the sequence number the next part must have
    uint8_t lfn_sum = 0;                     // the checksum every part must share

    for (bool more = fat_dir_start(dir, &pos); more; more = fat_dir_next(&pos)) {
        if (!fat_read_sector(pos.lba, &sector)) {
            return false;
        }
        for (uint16_t slot = 0; slot < FAT_ENTRIES_PER_SECTOR; slot++) {
            const fat_dirent_t* d = &sector.dirents[slot];
            const uint8_t* raw = (const uint8_t*)d; // the same entry as raw bytes, for long name entries

            // no more entries after this one, we are done
            if (d->name[0] == FAT_DIR_END) {
                return false;
            }
            // a deleted entry (could be a deleted long name part too), it breaks any long name in progress
            if (d->name[0] == FAT_DIR_DELETED) {
                lfn_valid = false;
                continue;
            }
            if (d->attr == FAT_ATTR_LFN) {
                int part = raw[0] & 0x1F; // which part this is, 1 is the start of the name
                if (raw[0] & 0x40) {
                    // the last part of a long name is stored first
                    // so this starts a new name: remember its checksum and where it began
                    lfn_valid = (part >= 1 && part <= FAT_LFN_PARTS);
                    lfn_expect = part;
                    lfn_sum = raw[13];
                    lfn_pos = pos;
                    lfn_slot = slot;
                    memset(lfn, 0, sizeof(lfn));
                } else if (!lfn_valid || part != lfn_expect || raw[13] != lfn_sum) {
                    // a part out of order or from another name: the whole long name is unusable
                    lfn_valid = false;
                }
                if (lfn_valid) {
                    // part `part` holds characters (part - 1) * 13 to (part - 1) * 13 + 12 of the name
                    for (int i = 0; i < 13; i++) {
                        const uint8_t* bytes = raw + lfn_char_offsets[i];
                        uint16_t c = (uint16_t)(bytes[0] | (bytes[1] << 8)); // one UTF-16 character

                        // 0x0000 ends the name and 0xFFFF pads out the rest of the part.
                        // AerOS only shows ASCII, so anything else is shown as '?'
                        char shown;
                        if (c == 0xFFFF) {
                            shown = '\0';
                        } else if (c < 128) {
                            shown = (char)c;
                        } else {
                            shown = '?';
                        }
                        lfn[(part - 1) * 13 + i] = shown;
                    }
                    lfn_expect = part - 1; // the parts count down towards 1
                }
                continue;
            }
            // the volume label lives in the root directory as an entry, it is not a file
            if (d->attr & FAT_ATTR_VOLUME_ID) {
                lfn_valid = false;
                continue;
            }

            // a normal 8.3 entry. The long name in front of it only belongs to it if every part
            // arrived (we counted down to 0) and the checksum matches this 8.3 name
            bool has_lfn = lfn_valid && lfn_expect == 0 && lfn_sum == fat_lfn_checksum(d->name);
            lfn_valid = false;
            if (d->name[0] == '.') {
                continue; // "." and ".."
            }

            e.entry = *d;
            e.lba = pos.lba;
            e.slot = slot;
            fat_short_to_name(d->name, d->nt_case, e.short_name, false);
            if (has_lfn) {
                scpy(e.name, lfn, sizeof(e.name));
                e.lfn_pos = lfn_pos;
                e.lfn_slot = lfn_slot;
            } else {
                // no long name: show the 8.3 name (in lower case if Windows says so),
                // and the entry is its own "first long name entry" for fat_mark_deleted()
                fat_short_to_name(d->name, d->nt_case, e.name, true);
                e.lfn_pos = pos;
                e.lfn_slot = slot;
            }
            if (visit(&e, ctx)) {
                return true;
            }
        }
    }
    return false;
}

// what fat_find() passes to fat_find_visit(): the name to look for and where to copy the match
typedef struct {
    const char* name;
    fat_entry_t* found;
} fat_find_ctx_t;

// stop at the first entry whose long name or 8.3 name matches (ignoring case, like Windows)
static bool fat_find_visit (const fat_entry_t* e, void* ctx) {
    fat_find_ctx_t* find = ctx;
    if (strcasecmp(e->name, find->name) == 0 || strcasecmp(e->short_name, find->name) == 0) {
        *find->found = *e;
        return true;
    }
    return false;
}

// look up a name (long or 8.3, any case) in a directory
// so "My Long Name.txt" can be found by its long name or by its 8.3 alias "MYLONG~1.TXT"
// returns true and fills out if found
static bool fat_find (uint32_t dir, const char* name, fat_entry_t* out) {
    fat_find_ctx_t ctx = { name, out };
    return fat_walk_dir(dir, fat_find_visit, &ctx);
}

// absolute directory path ("/sys/cache/") -> its first cluster
// starts at the root and looks up each part of the path in turn ("sys", then "cache"), each one
// has to be a directory. "/" gives FAT_ROOT. f_sys.c has already removed any "." and ".."
static bool fat_resolve_dir (const char* d_path, uint32_t* dir) {
    char component[FAT_NAME_MAX + 1];
    fat_entry_t e;
    uint32_t current = FAT_ROOT;
    const char* p = d_path;

    if (!fat_vol.mounted) {
        return false;
    }
    while (*p) {
        // cut the next part out of the path, between the slashes
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        size_t len = strcspn(p, "/");
        if (len > FAT_NAME_MAX) {
            return false;
        }
        memcpy(component, p, len);
        component[len] = '\0';
        p += len;

        if (!fat_find(current, component, &e) || !(e.entry.attr & FAT_ATTR_DIRECTORY)) {
            return false;
        }
        current = fat_entry_cluster(&e.entry);
    }
    *dir = current;
    return true;
}

// find a free 32 byte slot in a directory, growing the directory if needed
// a slot is free if it was never used (0x00) or was deleted (0xE5). We only ever write a single
// 8.3 entry (no long name parts), so one free slot is all we need.
// reusing a 0x00 slot is fine: the slot after it is 0x00 too, so it becomes the new end marker
static bool fat_free_slot (uint32_t dir, uint32_t* lba, uint16_t* slot) {
    fat_sector_t sector;
    fat_dir_pos_t pos;

    if (!fat_dir_start(dir, &pos)) {
        return false;
    }
    do {
        if (!fat_read_sector(pos.lba, &sector)) {
            return false;
        }
        for (uint16_t i = 0; i < FAT_ENTRIES_PER_SECTOR; i++) {
            uint8_t first = sector.dirents[i].name[0];
            if (first == FAT_DIR_END || first == FAT_DIR_DELETED) {
                *lba = pos.lba;
                *slot = i;
                return true;
            }
        }
    } while (fat_dir_next(&pos));

    // full: the FAT16 root directory has a fixed size, every other directory can grow by a cluster
    // (pos.index reaching FAT_DIR_MAX_SECTORS means it hit the 65536 entry limit)
    if (pos.cluster == 0 || pos.index >= FAT_DIR_MAX_SECTORS) {
        return false;
    }
    // clear the new cluster before linking it in, so the directory never shows old data
    uint32_t added = fat_alloc_cluster(0);
    if (added == 0) {
        return false;
    }
    // pos.cluster is still the directory's last cluster, point it at the new one
    if (!fat_zero_cluster(added) || !fat_set(pos.cluster, added)) {
        fat_free_chain(added);
        return false;
    }
    // the first slot of the new cluster is free
    *lba = fat_cluster_lba(added);
    *slot = 0;
    return true;
}

// write a directory entry into a slot
// (the disk can only write whole sectors, so read the sector, change the one entry, write it back)
static bool fat_write_entry (uint32_t lba, uint16_t slot, const fat_dirent_t* entry) {
    fat_sector_t sector;
    if (!fat_read_sector(lba, &sector)) {
        return false;
    }
    sector.dirents[slot] = *entry;
    return fat_write_sector(lba, &sector);
}

// mark an entry and the long name entries in front of it as deleted
// every slot from (lfn_pos, lfn_slot) up to and including (lba, slot) gets 0xE5 in its first byte.
// 0xE5 rather than 0x00, because 0x00 would mean "end of directory" and hide every entry after it.
// the data itself is not touched (that is why undelete tools can sometimes get files back)
static bool fat_mark_deleted (const fat_entry_t* e) {
    fat_sector_t sector;
    fat_dir_pos_t pos = e->lfn_pos;
    uint16_t from = e->lfn_slot;

    while (true) {
        // in the entry's own sector we stop at its slot, in earlier sectors we go to the end
        bool last = (pos.lba == e->lba);
        uint16_t to = last ? e->slot : FAT_ENTRIES_PER_SECTOR - 1;
        if (!fat_read_sector(pos.lba, &sector)) {
            return false;
        }
        for (uint16_t i = from; i <= to; i++) {
            sector.dirents[i].name[0] = FAT_DIR_DELETED;
        }
        if (!fat_write_sector(pos.lba, &sector)) {
            return false;
        }
        if (last) {
            return true;
        }
        // a long name can run on into the next sector
        if (!fat_dir_next(&pos)) {
            return false;
        }
        from = 0;
    }
}

// store data in a new chain of clusters, first = 0 for an empty file
// clusters are taken one at a time and linked on as we go, each sector is filled with data and
// the last one is padded with zeros. On any error the clusters taken so far are given back,
// so a failed write never leaks space. Returns FS_OK, FS_ERR_FULL or FS_ERR_IO
static int fat_write_chain (const char* data, uint32_t length, uint32_t* first) {
    fat_sector_t sector;
    uint32_t written = 0; // bytes of data written so far
    uint32_t prev = 0;    // the cluster before this one (0 = this is the first)

    *first = 0;
    while (written < length) {
        uint32_t cluster = fat_alloc_cluster(prev);
        if (cluster == 0) {
            fat_free_chain(*first);
            *first = 0;
            return FS_ERR_FULL;
        }
        if (*first == 0) {
            *first = cluster;
        }
        prev = cluster;
        // fill the cluster one sector at a time (stopping early if the data runs out)
        for (uint32_t s = 0; s < fat_vol.sectors_per_cluster && written < length; s++) {
            uint32_t n = MIN(length - written, (uint32_t)FAT_SECTOR_SIZE);
            memset(&sector, 0, sizeof(sector)); // so a short last sector doesn't carry old bytes
            memcpy(&sector, data + written, n);
            if (!fat_write_sector(fat_cluster_lba(cluster) + s, &sector)) {
                fat_free_chain(*first);
                *first = 0;
                return FS_ERR_IO;
            }
            written += n;
        }
    }
    return FS_OK;
}

// read a file's contents into a new buffer (with a '\0' added), NULL on error
// cluster = the first cluster, size = the size from its directory entry.
// if the chain ends early (a damaged disk) we return what we got, so the string can be shorter than size.
// gotcha: files are treated as text, a file with '\0' bytes in it will look cut short to slen()
static char* fat_read_chain (uint32_t cluster, uint32_t size) {
    fat_sector_t sector;
    char* buffer = malloc(size + 1);
    uint32_t done = 0; // bytes copied into buffer so far

    if (buffer == NULL) {
        return NULL;
    }
    // follow the chain until we have size bytes, the chain ends, or the loop guard trips
    for (uint32_t guard = 0; done < size && fat_valid_cluster(cluster) && guard < fat_vol.cluster_count; guard++) {
        for (uint32_t s = 0; s < fat_vol.sectors_per_cluster && done < size; s++) {
            if (!fat_read_sector(fat_cluster_lba(cluster) + s, &sector)) {
                free(buffer);
                return NULL;
            }
            // a whole sector, or just the part of the last one that belongs to the file
            uint32_t n = MIN(size - done, (uint32_t)FAT_SECTOR_SIZE);
            memcpy(buffer + done, &sector, n);
            done += n;
        }
        cluster = fat_get(cluster);
    }
    buffer[done] = '\0';
    return buffer;
}

// does this directory path exist on the disk?
bool fat_dir_exists (const char* d_path) {
    uint32_t dir;
    return fat_resolve_dir(d_path, &dir);
}

// find a file (not a directory) by directory path and name
static bool fat_find_file (const char* f_path, const char* name, fat_entry_t* e) {
    uint32_t dir;
    if (!fat_resolve_dir(f_path, &dir)) {
        return false; // no such folder
    }
    if (!fat_find(dir, name, e)) {
        return false; // nothing with that name in it
    }
    bool is_folder = (e->entry.attr & FAT_ATTR_DIRECTORY) != 0;
    return !is_folder;
}

// does this file exist? (a directory with that name doesn't count)
bool fat_file_exists (const char* f_path, const char* name) {
    fat_entry_t e;
    return fat_find_file(f_path, name, &e);
}

// the whole file in a new buffer (free() it when done), NULL if it isn't there or can't be read
// size (optional) receives its length, which matters for binary files that contain '\0' bytes
char* fat_read_file (const char* f_path, const char* name, uint32_t* size) {
    fat_entry_t e;
    if (!fat_find_file(f_path, name, &e)) {
        return NULL;
    }
    if (size) {
        *size = e.entry.size;
    }
    return fat_read_chain(fat_entry_cluster(&e.entry), e.entry.size);
}

// ---- reading a file a piece at a time (see fat_file_t in fat.h) -------------------------------

// open a file for reading with fat_read_at(), false if there is no such file (see fat.h)
bool fat_open (const char* f_path, const char* name, fat_file_t* file) {
    fat_entry_t e;
    if (!fat_find_file(f_path, name, &e)) {
        return false;
    }
    // nothing is read yet: just remember where the chain starts and stand on its first cluster
    // (first_cluster is 0 for an empty file, fat_read_at() never gets far enough to use it then)
    file->first_cluster = fat_entry_cluster(&e.entry);
    file->size = e.entry.size;
    file->cluster = file->first_cluster;
    file->cluster_index = 0;
    return true;
}

// move file->cluster to the file's cluster number `index` (0 = the first). Going forward it
// carries on from the cluster read last; going back it has to start again from the first.
// False if the chain ends too soon (a damaged disk)
static bool fat_go_to_cluster (fat_file_t* file, uint32_t index) {
    // a FAT chain only links forwards, so going back means walking again from the start
    if (index < file->cluster_index) {
        file->cluster = file->first_cluster;
        file->cluster_index = 0;
    }
    while (file->cluster_index < index) {
        file->cluster = fat_get(file->cluster);
        if (!fat_valid_cluster(file->cluster)) {
            return false;
        }
        file->cluster_index++;
    }
    return fat_valid_cluster(file->cluster);
}

// read up to length bytes from offset into out, returns how many were read (0 at the end, -1 on error)
// each pass of the loop handles one sector, or a run of whole sectors inside one cluster
int32_t fat_read_at (fat_file_t* file, uint32_t offset, void* out, uint32_t length) {
    // starting at or past the end: nothing to read (this also covers an empty file)
    if (offset >= file->size) {
        return 0;
    }
    if (length > file->size - offset) {
        length = file->size - offset; // stop at the end of the file
    }
    uint32_t cluster_bytes = fat_vol.sectors_per_cluster * FAT_SECTOR_SIZE;
    uint8_t* to = out;
    uint32_t done = 0;

    while (done < length) {
        // the byte of the file we are up to, and the cluster of the chain that holds it
        uint32_t position = offset + done;
        if (!fat_go_to_cluster(file, position / cluster_bytes)) {
            return -1;
        }
        // where `position` is inside its cluster: which sector, and where in that sector
        uint32_t in_cluster = position % cluster_bytes;
        uint32_t sector = in_cluster / FAT_SECTOR_SIZE;
        uint32_t in_sector = in_cluster % FAT_SECTOR_SIZE;
        uint32_t lba = fat_cluster_lba(file->cluster) + sector;
        uint32_t left = length - done;

        if (in_sector == 0 && left >= FAT_SECTOR_SIZE) {
            // whole sectors: read as many as possible from this cluster straight into `out`
            // (a cluster's sectors are next to each other on the disk, so one multi sector read
            // does it, but the next cluster can be anywhere, so we stop at the cluster's end)
            uint32_t sectors = left / FAT_SECTOR_SIZE;
            uint32_t sectors_left_in_cluster = fat_vol.sectors_per_cluster - sector;
            if (sectors > sectors_left_in_cluster) {
                sectors = sectors_left_in_cluster;
            }
            if (!drive_read(fat_vol.bus, fat_vol.slave, lba, sectors, to + done)) {
                return -1;
            }
            done += sectors * FAT_SECTOR_SIZE;
        } else {
            // part of a sector (the start or the end of what was asked for): read the whole
            // sector into a spare buffer and copy the part that's wanted
            fat_sector_t whole;
            if (!fat_read_sector(lba, &whole)) {
                return -1;
            }
            uint32_t take = FAT_SECTOR_SIZE - in_sector;
            if (take > left) {
                take = left;
            }
            memcpy(to + done, whole.u8 + in_sector, take);
            done += take;
        }
    }
    return (int32_t)done;
}

// what fat_list_dir() passes to fat_each_visit(): the caller's callback and its own ctx,
// plus a count of the entries seen
typedef struct {
    fs_list_fn fn;
    void* ctx;
    int count;
} fat_each_ctx_t;

// hand one entry on to the caller's fs_list_fn (name, is it a directory, size) and count it
// (always returns false, so the walk carries on through the whole directory)
static bool fat_each_visit (const fat_entry_t* e, void* ctx) {
    fat_each_ctx_t* each = ctx;
    each->fn(e->name, (e->entry.attr & FAT_ATTR_DIRECTORY) != 0, e->entry.size, each->ctx);
    each->count++;
    return false;
}

// call fn for every file and directory in d_path, returns how many there were (-1 if it doesn't exist)
int fat_list_dir (const char* d_path, fs_list_fn fn, void* ctx) {
    uint32_t dir;
    if (!fat_resolve_dir(d_path, &dir)) {
        return -1;
    }
    fat_each_ctx_t each = { fn, ctx, 0 };
    fat_walk_dir(dir, fat_each_visit, &each);
    return each.count;
}

// create a new file, the name has to fit 8.3 (we can read long names but not write them)
// the order matters for crash safety: write the data, find a slot, then write the entry.
// if anything fails the data clusters are freed again, so nothing is left half made
int fat_create_file (const char* f_path, const char* name, const char* contents, uint32_t length) {
    fat_entry_t existing;
    fat_dirent_t d;
    uint32_t dir, first, lba;
    uint16_t slot;

    if (!fat_resolve_dir(f_path, &dir)) {
        return FS_ERR_NOT_FOUND;
    }
    // anything with this name already there (a file or a directory) blocks it
    if (fat_find(dir, name, &existing)) {
        return FS_ERR_EXISTS;
    }
    // turn the name into its 11 byte on-disk form now, before any space is taken
    // (the rest of the entry starts as zeros: no times yet, cluster 0, not hidden or read only)
    memset(&d, 0, sizeof(d));
    if (!fat_make_83(name, d.name, &d.nt_case)) {
        return FS_ERR_BAD_NAME;
    }

    // 1. the data, into a fresh chain nobody points at yet
    int result = fat_write_chain(contents, length, &first);
    if (result != FS_OK) {
        return result;
    }
    // 2. a slot for the entry (this may grow the directory by a cluster)
    if (!fat_free_slot(dir, &lba, &slot)) {
        fat_free_chain(first);
        return FS_ERR_FULL;
    }

    // 3. fill in the entry and write it, which is the moment the file appears
    d.attr = FAT_ATTR_ARCHIVE;
    d.size = length;
    fat_set_entry_cluster(&d, first); // 0 for an empty file, FAT's way of saying "no clusters"
    fat_stamp(&d, true);
    if (!fat_write_entry(lba, slot, &d)) {
        fat_free_chain(first);
        return FS_ERR_IO;
    }
    return FS_OK;
}

// replace (append = false) or add to (append = true) the contents of a file
// we never overwrite the old clusters in place: the new contents go into a fresh chain, then
// the entry is switched over to it in one sector write, and only then is the old chain freed.
// a crash part way leaves either the old file or the new one, never a mix of the two.
// appending is done the simple way: read the old file, join the new text on, write it all out
int fat_write_file (const char* f_path, const char* name, const char* text, bool append) {
    // replacing is exactly fat_replace_file() with the text as the new data
    if (!append) {
        return fat_replace_file(f_path, name, text, (uint32_t)slen(text), false);
    }
    // appending: read the old contents in full first
    fat_entry_t e;
    if (!fat_find_file(f_path, name, &e)) {
        return FS_ERR_NOT_FOUND;
    }
    char* old = fat_read_chain(fat_entry_cluster(&e.entry), e.entry.size);
    if (old == NULL) {
        return FS_ERR_IO;
    }
    // slen rather than entry.size, in case the chain was shorter than the entry claimed
    size_t old_len = slen(old);
    char* joined = realloc(old, old_len + slen(text) + 1);
    if (joined == NULL) {
        free(old); // realloc failed, the old buffer is still ours to free
        return FS_ERR_NO_MEMORY;
    }
    strcpy(joined + old_len, text);
    int result = fat_replace_file(f_path, name, joined, (uint32_t)slen(joined), false);
    free(joined);
    return result;
}

// fill every cluster of a chain with zeros, so what was in it can't be read off the disk later
static void fat_zero_chain (uint32_t cluster) {
    static const uint8_t zeros[512] = { 0 };
    for (uint32_t guard = 0; fat_valid_cluster(cluster) && guard < fat_vol.cluster_count; guard++) {
        uint32_t lba = fat_cluster_lba(cluster);
        for (uint32_t s = 0; s < fat_vol.sectors_per_cluster; s++) {
            fat_write_sector(lba + s, zeros);
        }
        cluster = fat_get(cluster);
        busy();
    }
}

// replace a file's contents with length bytes of data (binary safe), see fat.h
int fat_replace_file (const char* f_path, const char* name, const void* data, uint32_t length, bool wipe_old) {
    fat_entry_t e;
    uint32_t first;
    if (!fat_find_file(f_path, name, &e)) {
        return FS_ERR_NOT_FOUND;
    }
    // remember the old chain now, the entry is about to be changed to point at the new one
    uint32_t old_cluster = fat_entry_cluster(&e.entry);

    // write the new data first, then point the entry at it, then free the old data
    int result = fat_write_chain(data, length, &first);
    if (result != FS_OK) {
        return result;
    }

    // update our copy of the entry (keeping its name, creation date and other attributes),
    // then write it back to the same slot: this one sector write is the switch-over point
    fat_set_entry_cluster(&e.entry, first);
    e.entry.size = length;
    e.entry.attr |= FAT_ATTR_ARCHIVE;
    fat_stamp(&e.entry, false);
    if (!fat_write_entry(e.lba, e.slot, &e.entry)) {
        fat_free_chain(first); // the entry still points at the old data, so drop the new copy
        return FS_ERR_IO;
    }
    // the old data now belongs to nobody. Zero it first if asked to, because freeing a
    // cluster only changes the FAT and leaves the bytes on the disk
    if (wipe_old) {
        fat_zero_chain(old_cluster);
    }
    fat_free_chain(old_cluster);
    return FS_OK;
}

// delete a file: first mark its entries deleted, then free its clusters
// (that way round an entry can never point at clusters that already belong to someone else)
int fat_delete_file (const char* f_path, const char* name) {
    fat_entry_t e;
    if (!fat_find_file(f_path, name, &e)) {
        return FS_ERR_NOT_FOUND;
    }
    if (!fat_mark_deleted(&e)) {
        return FS_ERR_IO;
    }
    fat_free_chain(fat_entry_cluster(&e.entry));
    return FS_OK;
}

// split "/a/b/" into "/a/" and "b" (both buffers FAT_NAME_MAX + 1 long)
// how: copy it, drop the trailing '/' ("/a/b"), the last '/' left separates the parent from the name.
// false for "/" (the root has no parent) or a path too long for the buffers
static bool fat_split_dir (const char* d_path, char* parent, char* name) {
    size_t len = slen(d_path);
    if (len < 2 || len > FAT_NAME_MAX) {
        return false;
    }
    scpy(parent, d_path, FAT_NAME_MAX + 1);
    parent[len - 1] = '\0';
    char* slash = strrchr(parent, '/');
    scpy(name, slash + 1, FAT_NAME_MAX + 1);
    slash[1] = '\0';
    return true;
}

// "." and ".." in a new directory, they take the directory's dates
// every directory except the root starts with these two entries: "." points at the directory
// itself and ".." at its parent. dir = the new directory's own entry (we copy its attributes and
// dates), name = "." or "..", cluster = where it points
static void fat_make_dot (fat_dirent_t* out, const fat_dirent_t* dir, const char* name, uint32_t cluster) {
    *out = *dir;
    out->nt_case = 0; // don't inherit the lower case flags of the directory's name
    memset(out->name, ' ', sizeof(out->name));
    memcpy(out->name, name, slen(name));
    fat_set_entry_cluster(out, cluster);
}

// d_path = the new directory's absolute path, its parent must exist
// steps: build the entry, take a cluster for the directory's contents, write "." and ".." into
// it, then add the entry to the parent. As with files the entry is written last, and the
// cluster is freed again if anything goes wrong
int fat_create_dir (const char* d_path) {
    char parent[FAT_NAME_MAX + 1];
    char name[FAT_NAME_MAX + 1];
    fat_entry_t existing;
    fat_dirent_t d;
    fat_sector_t sector;
    uint32_t parent_dir, lba;
    uint16_t slot;

    if (!fat_split_dir(d_path, parent, name)) {
        return FS_ERR_BAD_NAME;
    }
    if (!fat_resolve_dir(parent, &parent_dir)) {
        return FS_ERR_NOT_FOUND;
    }
    if (fat_find(parent_dir, name, &existing)) {
        return FS_ERR_EXISTS;
    }
    memset(&d, 0, sizeof(d));
    if (!fat_make_83(name, d.name, &d.nt_case)) {
        return FS_ERR_BAD_NAME;
    }
    d.attr = FAT_ATTR_DIRECTORY;
    fat_stamp(&d, true); // size stays 0, directories always have size 0

    // a directory always owns at least one cluster, even when it is empty, to hold "." and ".."
    uint32_t cluster = fat_alloc_cluster(0);
    if (cluster == 0) {
        return FS_ERR_FULL;
    }

    // ".." points at the parent, 0 when the parent is the root
    // (that is the rule even on FAT32 where the root has a real cluster, and it matches FAT_ROOT)
    memset(&sector, 0, sizeof(sector));
    fat_make_dot(&sector.dirents[0], &d, ".", cluster);
    fat_make_dot(&sector.dirents[1], &d, "..", parent_dir);
    // zero the whole cluster (an empty directory), then write the first sector again with the dots in it
    if (!fat_zero_cluster(cluster) || !fat_write_sector(fat_cluster_lba(cluster), &sector)) {
        fat_free_chain(cluster);
        return FS_ERR_IO;
    }

    // last of all, add the new directory's entry to its parent
    if (!fat_free_slot(parent_dir, &lba, &slot)) {
        fat_free_chain(cluster);
        return FS_ERR_FULL;
    }
    fat_set_entry_cluster(&d, cluster);
    if (!fat_write_entry(lba, slot, &d)) {
        fat_free_chain(cluster);
        return FS_ERR_IO;
    }
    return FS_OK;
}

// fat_walk_dir() visitor that stops at the very first entry, so "did the walk stop early?"
// means "the directory has something in it" ("." and ".." are skipped by the walk)
static bool fat_any_visit (const fat_entry_t* e, void* ctx) {
    (void)e;
    (void)ctx;
    return true; // found something, stop
}

// delete a directory, only if it is empty
// like a file: mark the entry deleted first, then free the directory's clusters
int fat_delete_dir (const char* d_path) {
    char parent[FAT_NAME_MAX + 1];
    char name[FAT_NAME_MAX + 1];
    uint32_t parent_dir;
    fat_entry_t e;

    // find the folder's entry in the folder above it
    if (!fat_split_dir(d_path, parent, name)) {
        return FS_ERR_NOT_FOUND;
    }
    if (!fat_resolve_dir(parent, &parent_dir)) {
        return FS_ERR_NOT_FOUND;
    }
    if (!fat_find(parent_dir, name, &e)) {
        return FS_ERR_NOT_FOUND;
    }
    bool is_folder = (e.entry.attr & FAT_ATTR_DIRECTORY) != 0;
    if (!is_folder) {
        return FS_ERR_NOT_FOUND;
    }
    // a broken entry pointing at cluster 0 would otherwise walk the root directory
    // (cluster 0 is FAT_ROOT to fat_walk_dir(), so we only check for contents if the cluster is real)
    uint32_t cluster = fat_entry_cluster(&e.entry);
    if (fat_valid_cluster(cluster) && fat_walk_dir(cluster, fat_any_visit, NULL)) {
        return FS_ERR_NOT_EMPTY;
    }
    if (!fat_mark_deleted(&e)) {
        return FS_ERR_IO;
    }
    fat_free_chain(cluster);
    return FS_OK;
}

// what fat_print_dir() passes to fat_list_visit()
typedef struct {
    bool directories; // list directories (true) or files (false)
    int shown;        // how many lines have been printed
} fat_list_ctx_t;

// print one entry if it is the kind (directory or file) this pass is listing
// (always returns false, so the walk carries on through the whole directory)
static bool fat_list_visit (const fat_entry_t* e, void* ctx) {
    fat_list_ctx_t* list = ctx;
    bool is_dir = (e->entry.attr & FAT_ATTR_DIRECTORY) != 0;
    if (is_dir != list->directories) {
        return false;
    }
    if (is_dir) {
        prtf("  <DIR>   %s/\n", e->name);
    } else {
        char when[20]; // "DD-MM-YY--HH:MM:SS" + '\0'
        fat_format_datetime(e->entry.mdate, e->entry.mtime, when, sizeof(when));
        prtf("  <FILE>  %-22s %6u bytes  %s\n", e->name, e->entry.size, when);
    }
    list->shown++;
    return false;
}

// print the contents of a directory, returns how many entries were shown (-1 if it does not exist)
int fat_print_dir (const char* d_path) {
    uint32_t dir;
    if (!fat_resolve_dir(d_path, &dir)) {
        return -1;
    }
    // directories first, then files
    // (two walks over the directory, it's simpler than sorting and the directories are small)
    fat_list_ctx_t dirs = { true, 0 };
    fat_list_ctx_t files = { false, 0 };
    fat_walk_dir(dir, fat_list_visit, &dirs);
    fat_walk_dir(dir, fat_list_visit, &files);
    return dirs.shown + files.shown;
}

// read a little endian 16 / 32 bit number from any byte address
// (BPB fields sit at odd offsets like 17 and 19, so casting to a uint16_t pointer would be an
// unaligned access, which C doesn't allow even though x86 copes with it)
static uint16_t fat_u16 (const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

// the 32 bit version of fat_u16(): byte 0 is the lowest, byte 3 the highest
static uint32_t fat_u32 (const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// copy an 11 character label, trimming the padding
// (labels are padded with spaces to 11 characters, out needs room for 12)
static void fat_copy_label (const uint8_t* raw, char* out) {
    memcpy(out, raw, 11);
    out[11] = '\0';
    for (int i = 10; i >= 0 && out[i] == ' '; i--) {
        out[i] = '\0';
    }
}

// the volume label stored in the root directory (Windows keeps the real label here)
// it is an entry with the volume ID attribute and the label as its 11 character "name".
// when you rename a drive Windows changes this entry and may leave the boot sector's copy
// alone (often as "NO NAME"), so this is the one to believe
static bool fat_root_label (char* out) {
    fat_sector_t sector;
    fat_dir_pos_t pos;

    // it is one of the first entries, so only the first few sectors are checked
    for (bool more = fat_dir_start(FAT_ROOT, &pos); more && pos.index < 4; more = fat_dir_next(&pos)) {
        if (!fat_read_sector(pos.lba, &sector)) {
            return false;
        }
        for (int i = 0; i < FAT_ENTRIES_PER_SECTOR; i++) {
            const fat_dirent_t* d = &sector.dirents[i];
            if (d->name[0] == FAT_DIR_END) {
                return false;
            }
            // long name entries also have the volume ID bit set, so they have to be ruled out
            if (d->name[0] != FAT_DIR_DELETED && d->attr != FAT_ATTR_LFN && (d->attr & FAT_ATTR_VOLUME_ID)) {
                fat_copy_label(d->name, out);
                return true;
            }
        }
    }
    return false;
}

// try to mount the FAT volume on a drive
// returns FS_OK, FS_ERR_NOT_FOUND (no FAT volume), FS_ERR_BAD_NAME (not labelled AEROS) or FS_ERR_IO
// bus / slave = which IDE drive. Reads the boot sector, works out the layout into fat_vol and only
// sets fat_vol.mounted once the label check has passed. See the BPB table at the top of this file
static int fat_mount (int bus, int slave) {
    uint8_t bs[FAT_SECTOR_SIZE]; // the boot sector
    uint32_t volume_lba;

    // start from a clean slate, and forget any FAT sector cached from a different drive
    memset(&fat_vol, 0, sizeof(fat_vol));
    fat_cache_lba = FAT_NO_SECTOR;

    // where is the volume? at sector 0 for a "superfloppy" (formatted without a partition table,
    // like our disk.img), otherwise the first FAT partition in the MBR (ata.c works this out)
    int found = drive_parse(bus, slave, &volume_lba);
    if (found < 0) {
        return FS_ERR_IO;
    }
    if (found == 0) {
        return FS_ERR_NOT_FOUND;
    }
    if (!drive_read(bus, slave, volume_lba, 1, bs)) {
        return FS_ERR_IO;
    }
    if (!drive_is_fat_boot_sector(bs)) {
        return FS_ERR_NOT_FOUND;
    }

    // the BPB fields, offsets in the table at the top of this file
    uint8_t spc = bs[13];                   // sectors per cluster
    uint16_t reserved = fat_u16(bs + 14);   // reserved sectors (boot sector, FSInfo, backups...)
    uint8_t fats = bs[16];                  // number of FAT copies
    uint16_t root_entries = fat_u16(bs + 17); // FAT16 root directory entries (0 on FAT32)

    // The size of the volume and of each FAT each have an old 16 bit field and a newer 32 bit
    // one. The 16 bit field is 0 when the number doesn't fit in it (and always on FAT32), and
    // then the 32 bit field holds it
    uint32_t total = fat_u16(bs + 19);
    if (total == 0) {
        total = fat_u32(bs + 32);
    }
    uint32_t fat_size = fat_u16(bs + 22);
    if (fat_size == 0) {
        fat_size = fat_u32(bs + 36);
    }
    // root directory size in sectors, rounded up ((x + 511) / 512 rounds a division up)
    uint32_t root_sectors = ((uint32_t)root_entries * 32 + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
    // the data area starts after the reserved sectors, the FATs and the FAT16 root directory
    uint32_t data_start = reserved + fats * fat_size + root_sectors;

    // nonsense numbers mean this isn't really a FAT volume
    if (fat_size == 0 || total <= data_start) {
        return FS_ERR_NOT_FOUND;
    }
    // everything after data_start is clusters (a few leftover sectors at the end go unused)
    uint32_t clusters = (total - data_start) / spc;
    if (clusters < 4085) {
        return FS_ERR_NOT_FOUND; // FAT12 (floppy sized) is not supported
    }

    fat_vol.bus = bus;
    fat_vol.slave = slave;
    // the official rule: the cluster count alone decides the FAT type
    fat_vol.type = (clusters < 65525) ? 16 : 32;
    fat_vol.volume_lba = volume_lba;
    fat_vol.total_sectors = total;
    fat_vol.sectors_per_cluster = spc;
    // turn the offsets into absolute sector numbers on the drive
    fat_vol.fat_lba = volume_lba + reserved;
    fat_vol.fat_sectors = fat_size;
    fat_vol.fat_copies = fats;
    fat_vol.root_lba = volume_lba + reserved + fats * fat_size; // only used on FAT16
    fat_vol.root_sectors = root_sectors;
    fat_vol.data_lba = volume_lba + data_start;
    fat_vol.cluster_count = clusters;
    fat_vol.next_free = 2; // start looking for free clusters at the beginning

    if (fat_vol.type == 32) {
        uint16_t flags = fat_u16(bs + 40);
        if (flags & 0x80) {
            // FAT mirroring is off: only the active FAT is used
            // (bits 0-3 say which copy that is, so point fat_lba at it and leave the others alone)
            fat_vol.fat_lba += (flags & 0x0F) * fat_size;
            fat_vol.fat_copies = 1;
        }
        fat_vol.root_cluster = fat_u32(bs + 44);
        // the FSInfo sector number is counted from the start of the volume; 0 or 0xFFFF means
        // there isn't one
        uint16_t fsinfo = fat_u16(bs + 48);
        bool has_fsinfo = fsinfo != 0 && fsinfo != 0xFFFF;
        fat_vol.fsinfo_lba = 0;
        if (has_fsinfo) {
            fat_vol.fsinfo_lba = volume_lba + fsinfo;
        }
        if (!fat_valid_cluster(fat_vol.root_cluster)) {
            return FS_ERR_NOT_FOUND;
        }
        fat_copy_label(bs + 71, fat_vol.label);
    } else {
        fat_copy_label(bs + 43, fat_vol.label);
    }

    // the root directory label wins over the boot sector one
    // (fat_vol has enough filled in by now for the directory functions to work)
    char root_label[12];
    if (fat_root_label(root_label)) {
        scpy(fat_vol.label, root_label, sizeof(fat_vol.label));
    }
    // the safety check: never use a FAT volume that isn't ours
    if (strcasecmp(fat_vol.label, FAT_VOLUME_LABEL) != 0) {
        return FS_ERR_BAD_NAME;
    }

    fat_vol.mounted = true;
    return FS_OK;
}

// try the four IDE drive positions in order (primary master, primary slave, secondary master,
// secondary slave) and keep the first AEROS volume. Only ATA hard disks are tried, CD drives
// (ATAPI, where the boot ISO usually is) are skipped.
// status gets a line for the boot log either way: what was mounted, or why nothing was
bool fat_mount_auto (char* status, size_t size) {
    const char* bus_names[2] = { "primary", "secondary" };
    bool unlabelled = false;           // did we see a FAT volume with the wrong label?
    char unlabelled_where[32] = "";    // where the first one of those was, for the message

    for (int bus = 0; bus < 2; bus++) {
        for (int slave = 0; slave < 2; slave++) {
            if (drive_probe(bus, slave, NULL, 0) != DRIVE_ATA) {
                continue;
            }
            int result = fat_mount(bus, slave);
            if (result == FS_OK) {
                // 2048 sectors of 512 bytes = 1 MB
                sfmt(status, size, "FAT%d volume %s on %s %s (%u MB)", fat_vol.type, fat_vol.label,
                         bus_names[bus], slave ? "slave" : "master", fat_vol.total_sectors / 2048);
                return true;
            }
            // remember the first FAT volume that wasn't labelled AEROS, so we can tell the user
            if (result == FS_ERR_BAD_NAME && !unlabelled) {
                unlabelled = true;
                sfmt(unlabelled_where, sizeof(unlabelled_where), "%s %s", bus_names[bus], slave ? "slave" : "master");
            }
        }
    }
    // nothing mounted: wipe whatever the last attempt left in fat_vol (mounted = false)
    memset(&fat_vol, 0, sizeof(fat_vol));
    if (unlabelled) {
        sfmt(status, size, "%s is not labelled %s", unlabelled_where, FAT_VOLUME_LABEL);
    } else {
        sfmt(status, size, "no %s disk found", FAT_VOLUME_LABEL);
    }
    return false;
}
