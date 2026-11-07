/*
    fat.h
    FAT16 / FAT32 file system driver, so files are saved to a real disk.

    - Only a volume labelled "RENOVATE" is used. That stops the OS writing to someone's
      own FAT drive (USB sticks, recovery partitions) when it boots on a real computer.
    - Reads long file names (written by Windows / Linux), creates 8.3 names (e.g. notes.txt).
    - Disk images can be made and filled from Linux with mtools, see the Makefile.

    https://wiki.osdev.org/FAT
*/
#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stdbool.h>
#include "../lib/primary_definitions.h"
#include "../drivers/storage/SATA/SATA_scanning.h"
#include "../drivers/storage/SATA/SATA_fs_parse.h"
#include "sys_clck.h"

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
        default: return "unknown error";
    }
}

#define FAT_VOLUME_LABEL  "RENOVATE"
#define FAT_SECTOR_SIZE   512
#define FAT_ROOT          0    // directory "cluster" used for the root directory
#define FAT_NAME_MAX      255

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

#define FAT_CASE_LOWER_BASE 0x08 // Windows NT: name part is lower case
#define FAT_CASE_LOWER_EXT  0x10 // Windows NT: extension is lower case

// a 32 byte directory entry
typedef struct __attribute__((packed)) {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_case;
    uint8_t ctime_tenths;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_high;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_low;
    uint32_t size;
} fat_dirent_t;

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

// a file or directory found while walking a directory
typedef struct {
    fat_dirent_t entry;
    uint32_t dir;          // directory that holds it
    uint32_t index;        // sector of the directory (0 = first) holding the entry
    uint16_t offset;       // byte offset of the entry in that sector
    uint32_t lba;          // absolute sector holding the entry
    uint32_t lfn_index;    // where its long name entries start (same as index / offset without a long name)
    uint16_t lfn_offset;
    char name[FAT_NAME_MAX + 1];  // long name, or the 8.3 name
    char short_name[13];          // 8.3 name, e.g. "NOTES.TXT"
} fat_entry_t;

typedef bool (*fat_visit_t)(fat_entry_t* entry, void* ctx); // return true to stop walking

fat_volume_t fat_vol;
uint8_t fat_cache[FAT_SECTOR_SIZE]; // one cached sector of the FAT
uint32_t fat_cache_lba = 0xFFFFFFFF;

/* ---------------- sectors & clusters ---------------- */

bool fat_read_sector (uint32_t lba, void* buffer) {
    return drive_read(fat_vol.bus, fat_vol.slave, lba, 1, buffer);
}

bool fat_write_sector (uint32_t lba, const void* buffer) {
    return drive_write(fat_vol.bus, fat_vol.slave, lba, 1, buffer);
}

uint32_t fat_cluster_lba (uint32_t cluster) {
    return fat_vol.data_lba + (cluster - 2) * fat_vol.sectors_per_cluster;
}

bool fat_valid_cluster (uint32_t cluster) {
    return cluster >= 2 && cluster < fat_vol.cluster_count + 2;
}

uint32_t fat_end_of_chain (void) {
    return (fat_vol.type == 32) ? 0x0FFFFFFF : 0xFFFF;
}

// the FAT entry for a cluster: 0 = free, a cluster number, or end of chain (0xFFFFFFFF on read errors)
uint32_t fat_get (uint32_t cluster) {
    uint32_t offset = cluster * ((fat_vol.type == 32) ? 4 : 2);
    uint32_t lba = fat_vol.fat_lba + offset / FAT_SECTOR_SIZE;
    uint32_t pos = offset % FAT_SECTOR_SIZE;

    if (fat_cache_lba != lba) {
        if (!fat_read_sector(lba, fat_cache)) {
            fat_cache_lba = 0xFFFFFFFF;
            return 0xFFFFFFFF;
        }
        fat_cache_lba = lba;
    }
    if (fat_vol.type == 32) {
        uint32_t value;
        memcpy(&value, fat_cache + pos, 4);
        return value & 0x0FFFFFFF;
    }
    uint16_t value;
    memcpy(&value, fat_cache + pos, 2);
    return value;
}

// change the FAT entry for a cluster in every copy of the FAT
bool fat_set (uint32_t cluster, uint32_t value) {
    uint8_t sector[FAT_SECTOR_SIZE];
    uint32_t offset = cluster * ((fat_vol.type == 32) ? 4 : 2);
    uint32_t pos = offset % FAT_SECTOR_SIZE;

    for (uint8_t copy = 0; copy < fat_vol.fat_copies; copy++) {
        uint32_t lba = fat_vol.fat_lba + copy * fat_vol.fat_sectors + offset / FAT_SECTOR_SIZE;
        if (!fat_read_sector(lba, sector)) {
            return false;
        }
        if (fat_vol.type == 32) {
            uint32_t old;
            memcpy(&old, sector + pos, 4);
            value = (old & 0xF0000000) | (value & 0x0FFFFFFF); // the top 4 bits are reserved
            memcpy(sector + pos, &value, 4);
        } else {
            uint16_t v16 = (uint16_t)value;
            memcpy(sector + pos, &v16, 2);
        }
        if (!fat_write_sector(lba, sector)) {
            return false;
        }
        if (lba == fat_cache_lba) {
            memcpy(fat_cache, sector, FAT_SECTOR_SIZE);
        }
    }
    return true;
}

// FAT32 keeps a "free clusters" count in the FSInfo sector, mark it as unknown once we change the FAT
void fat_invalidate_fsinfo (void) {
    uint8_t sector[FAT_SECTOR_SIZE];
    if (fat_vol.type != 32 || fat_vol.fsinfo_invalidated || fat_vol.fsinfo_lba == 0) {
        return;
    }
    fat_vol.fsinfo_invalidated = true;
    if (!fat_read_sector(fat_vol.fsinfo_lba, sector)) {
        return;
    }
    uint32_t sig1, sig2;
    memcpy(&sig1, sector, 4);
    memcpy(&sig2, sector + 484, 4);
    if (sig1 != 0x41615252 || sig2 != 0x61417272) {
        return;
    }
    memset(sector + 488, 0xFF, 8); // free count & next free = unknown
    fat_write_sector(fat_vol.fsinfo_lba, sector);
}

// fill a cluster with zeros
bool fat_zero_cluster (uint32_t cluster) {
    uint8_t sector[FAT_SECTOR_SIZE];
    memset(sector, 0, sizeof(sector));
    for (uint32_t s = 0; s < fat_vol.sectors_per_cluster; s++) {
        if (!fat_write_sector(fat_cluster_lba(cluster) + s, sector)) {
            return false;
        }
    }
    return true;
}

// take a free cluster and add it to the end of a chain (prev = 0 starts a new chain), returns 0 if the disk is full
uint32_t fat_alloc_cluster (uint32_t prev) {
    fat_invalidate_fsinfo();
    for (uint32_t n = 0; n < fat_vol.cluster_count; n++) {
        uint32_t cluster = 2 + (fat_vol.next_free - 2 + n) % fat_vol.cluster_count;
        if (fat_get(cluster) != 0) {
            continue;
        }
        if (!fat_set(cluster, fat_end_of_chain())) {
            return 0;
        }
        if (prev != 0 && !fat_set(prev, cluster)) {
            fat_set(cluster, 0);
            return 0;
        }
        fat_vol.next_free = (cluster + 1 < fat_vol.cluster_count + 2) ? cluster + 1 : 2;
        return cluster;
    }
    return 0;
}

// free every cluster in a chain
void fat_free_chain (uint32_t cluster) {
    if (fat_valid_cluster(cluster)) {
        fat_invalidate_fsinfo();
    }
    for (uint32_t guard = 0; fat_valid_cluster(cluster) && guard < fat_vol.cluster_count; guard++) {
        uint32_t next = fat_get(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

uint32_t fat_free_clusters (void) {
    uint32_t free_count = 0;
    for (uint32_t c = 2; c < fat_vol.cluster_count + 2; c++) {
        if (fat_get(c) == 0) {
            free_count++;
        }
    }
    return free_count;
}

/* ---------------- names & dates ---------------- */

uint8_t fat_lfn_checksum (const uint8_t* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

// "NOTES   TXT" -> "NOTES.TXT" (or "notes.txt" when the Windows lower case flags are set)
void fat_short_to_name (const uint8_t* raw, uint8_t nt_case, char* out, bool apply_case) {
    int p = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)((i == 0 && raw[i] == 0x05) ? 0xE5 : raw[i]);
        out[p++] = (apply_case && (nt_case & FAT_CASE_LOWER_BASE)) ? (char)tolower((unsigned char)c) : c;
    }
    if (raw[8] != ' ') {
        out[p++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            char c = (char)raw[i];
            out[p++] = (apply_case && (nt_case & FAT_CASE_LOWER_EXT)) ? (char)tolower((unsigned char)c) : c;
        }
    }
    out[p] = '\0';
}

bool fat_valid_83_char (char c) {
    return isalnum((unsigned char)c) || (c != '\0' && strchr("!#$%&'()-@^_`{}~", c) != NULL);
}

// "notes.txt" -> "NOTES   TXT" plus the lower case flags, false if the name does not fit 8.3
bool fat_make_83 (const char* name, uint8_t* out, uint8_t* nt_case) {
    const char* dot = strchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;
    bool base_lower = false, base_upper = false, ext_lower = false, ext_upper = false;

    if (base_len == 0 || base_len > 8 || ext_len > 3 || (dot && ext_len == 0) || (dot && strchr(dot + 1, '.'))) {
        return false;
    }
    memset(out, ' ', 11);
    for (size_t i = 0; i < base_len; i++) {
        char c = name[i];
        if (!fat_valid_83_char(c)) return false;
        if (islower((unsigned char)c)) base_lower = true;
        if (isupper((unsigned char)c)) base_upper = true;
        out[i] = (uint8_t)toupper((unsigned char)c);
    }
    for (size_t i = 0; i < ext_len; i++) {
        char c = dot[1 + i];
        if (!fat_valid_83_char(c)) return false;
        if (islower((unsigned char)c)) ext_lower = true;
        if (isupper((unsigned char)c)) ext_upper = true;
        out[8 + i] = (uint8_t)toupper((unsigned char)c);
    }
    // all lower case parts are remembered with the Windows NT flags, mixed case is stored upper case
    *nt_case = (uint8_t)(((base_lower && !base_upper) ? FAT_CASE_LOWER_BASE : 0) |
                         ((ext_lower && !ext_upper) ? FAT_CASE_LOWER_EXT : 0));
    return true;
}

// set the modified (and if created, the created) date & time of an entry to now
void fat_stamp (fat_dirent_t* d, bool created) {
    rtc_time_t t;
    rtc_read(&t);
    uint16_t year = (t.year >= 1980) ? (uint16_t)(t.year - 1980) : 0;
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
void fat_format_datetime (uint16_t date, uint16_t time, char* out, size_t size) {
    snprintf(out, size, "%02u-%02u-%02u--%02u:%02u:%02u",
             date & 0x1F, (date >> 5) & 0x0F, ((date >> 9) + 1980) % 100,
             time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2);
}

uint32_t fat_entry_cluster (const fat_dirent_t* d) {
    return ((uint32_t)d->cluster_high << 16) | d->cluster_low;
}

/* ---------------- directories ---------------- */

// the absolute sector of the index-th sector of a directory, false past its end
bool fat_dir_sector (uint32_t dir, uint32_t index, uint32_t* lba) {
    if (dir == FAT_ROOT && fat_vol.type == 16) {
        if (index >= fat_vol.root_sectors) {
            return false;
        }
        *lba = fat_vol.root_lba + index;
        return true;
    }
    uint32_t cluster = (dir == FAT_ROOT) ? fat_vol.root_cluster : dir;
    for (uint32_t skip = index / fat_vol.sectors_per_cluster; skip > 0; skip--) {
        cluster = fat_get(cluster);
        if (!fat_valid_cluster(cluster)) {
            return false;
        }
    }
    if (!fat_valid_cluster(cluster)) {
        return false;
    }
    *lba = fat_cluster_lba(cluster) + index % fat_vol.sectors_per_cluster;
    return true;
}

// call visit for every file and sub directory ("." and ".." skipped), returns true if visit stopped early
bool fat_walk_dir (uint32_t dir, fat_visit_t visit, void* ctx) {
    uint8_t sector[FAT_SECTOR_SIZE];
    uint16_t lfn[20 * 13 + 1];
    fat_entry_t e;
    bool lfn_valid = false;
    int lfn_expect = 0;
    uint8_t lfn_sum = 0;
    uint32_t lfn_index = 0;
    uint16_t lfn_offset = 0;

    for (uint32_t index = 0; ; index++) {
        uint32_t lba;
        if (!fat_dir_sector(dir, index, &lba) || !fat_read_sector(lba, sector)) {
            return false;
        }
        for (uint16_t off = 0; off < FAT_SECTOR_SIZE; off += 32) {
            const uint8_t* raw = sector + off;
            const fat_dirent_t* d = (const fat_dirent_t*)raw;

            if (raw[0] == 0x00) {
                return false; // end of directory
            }
            if (raw[0] == 0xE5) {
                lfn_valid = false; // deleted
                continue;
            }
            if (d->attr == FAT_ATTR_LFN) {
                int seq = raw[0] & 0x1F;
                if (raw[0] & 0x40) {
                    // the last part of a long name is stored first
                    lfn_valid = (seq >= 1 && seq <= 20);
                    lfn_expect = seq;
                    lfn_sum = raw[13];
                    lfn_index = index;
                    lfn_offset = off;
                    memset(lfn, 0, sizeof(lfn));
                } else if (!lfn_valid || seq != lfn_expect || raw[13] != lfn_sum) {
                    lfn_valid = false;
                }
                if (lfn_valid) {
                    static const uint8_t char_offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                    for (int i = 0; i < 13; i++) {
                        lfn[(seq - 1) * 13 + i] = (uint16_t)(raw[char_offsets[i]] | (raw[char_offsets[i] + 1] << 8));
                    }
                    lfn_expect = seq - 1;
                }
                continue;
            }
            if (d->attr & FAT_ATTR_VOLUME_ID) {
                lfn_valid = false;
                continue;
            }

            memset(&e, 0, sizeof(e));
            memcpy(&e.entry, d, sizeof(fat_dirent_t));
            e.dir = dir;
            e.index = index;
            e.offset = off;
            e.lba = lba;
            fat_short_to_name(raw, d->nt_case, e.short_name, false);

            if (lfn_valid && lfn_expect == 0 && lfn_sum == fat_lfn_checksum(raw)) {
                int p = 0;
                for (int i = 0; i < 20 * 13 && lfn[i] != 0x0000 && lfn[i] != 0xFFFF && p < FAT_NAME_MAX; i++) {
                    e.name[p++] = (lfn[i] < 128) ? (char)lfn[i] : '?';
                }
                e.name[p] = '\0';
                e.lfn_index = lfn_index;
                e.lfn_offset = lfn_offset;
            } else {
                fat_short_to_name(raw, d->nt_case, e.name, true);
                e.lfn_index = index;
                e.lfn_offset = off;
            }
            lfn_valid = false;

            if (raw[0] == '.') {
                continue; // "." and ".."
            }
            if (visit(&e, ctx)) {
                return true;
            }
        }
    }
}

typedef struct {
    const char* name;
    fat_entry_t* found;
} fat_find_ctx_t;

bool fat_find_visit (fat_entry_t* e, void* ctx) {
    fat_find_ctx_t* find = (fat_find_ctx_t*)ctx;
    if (strcasecmp(e->name, find->name) == 0 || strcasecmp(e->short_name, find->name) == 0) {
        *find->found = *e;
        return true;
    }
    return false;
}

// look up a name (long or 8.3, any case) in a directory
bool fat_find (uint32_t dir, const char* name, fat_entry_t* out) {
    fat_find_ctx_t ctx = { name, out };
    return fat_walk_dir(dir, fat_find_visit, &ctx);
}

// absolute directory path ("/sys/cache/") -> its first cluster
bool fat_resolve_dir (const char* d_path, uint32_t* dir) {
    char component[FAT_NAME_MAX + 1];
    fat_entry_t e;
    uint32_t current = FAT_ROOT;
    const char* p = d_path;

    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        size_t len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len > FAT_NAME_MAX) return false;
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
bool fat_free_slot (uint32_t dir, uint32_t* lba_out, uint16_t* offset_out) {
    uint8_t sector[FAT_SECTOR_SIZE];
    uint32_t lba;

    for (uint32_t index = 0; fat_dir_sector(dir, index, &lba); index++) {
        if (!fat_read_sector(lba, sector)) {
            return false;
        }
        for (uint16_t off = 0; off < FAT_SECTOR_SIZE; off += 32) {
            if (sector[off] == 0x00 || sector[off] == 0xE5) {
                *lba_out = lba;
                *offset_out = off;
                return true;
            }
        }
    }

    // full: the FAT16 root directory has a fixed size, every other directory can grow by a cluster
    if (dir == FAT_ROOT && fat_vol.type == 16) {
        return false;
    }
    uint32_t last = (dir == FAT_ROOT) ? fat_vol.root_cluster : dir;
    for (uint32_t guard = 0; guard < fat_vol.cluster_count; guard++) {
        uint32_t next = fat_get(last);
        if (!fat_valid_cluster(next)) break;
        last = next;
    }
    uint32_t added = fat_alloc_cluster(last);
    if (added == 0 || !fat_zero_cluster(added)) {
        return false;
    }
    *lba_out = fat_cluster_lba(added);
    *offset_out = 0;
    return true;
}

// write a directory entry into a slot
bool fat_write_entry (uint32_t lba, uint16_t offset, const fat_dirent_t* entry) {
    uint8_t sector[FAT_SECTOR_SIZE];
    if (!fat_read_sector(lba, sector)) {
        return false;
    }
    memcpy(sector + offset, entry, sizeof(fat_dirent_t));
    return fat_write_sector(lba, sector);
}

// mark an entry (and its long name entries) as deleted
bool fat_mark_deleted (const fat_entry_t* e) {
    uint8_t sector[FAT_SECTOR_SIZE];
    for (uint32_t index = e->lfn_index; index <= e->index; index++) {
        uint32_t lba;
        if (!fat_dir_sector(e->dir, index, &lba) || !fat_read_sector(lba, sector)) {
            return false;
        }
        uint16_t from = (index == e->lfn_index) ? e->lfn_offset : 0;
        uint16_t to = (index == e->index) ? e->offset : FAT_SECTOR_SIZE - 32;
        for (uint32_t off = from; off <= to; off += 32) {
            sector[off] = 0xE5;
        }
        if (!fat_write_sector(lba, sector)) {
            return false;
        }
    }
    return true;
}

/* ---------------- file data ---------------- */

// store data in a new chain of clusters, first = 0 for an empty file
int fat_write_chain (const char* data, uint32_t length, uint32_t* first) {
    uint8_t sector[FAT_SECTOR_SIZE];
    uint32_t written = 0;
    uint32_t prev = 0;

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
        for (uint32_t s = 0; s < fat_vol.sectors_per_cluster && written < length; s++) {
            uint32_t n = MIN(length - written, (uint32_t)FAT_SECTOR_SIZE);
            memset(sector, 0, sizeof(sector));
            memcpy(sector, data + written, n);
            if (!fat_write_sector(fat_cluster_lba(cluster) + s, sector)) {
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
char* fat_read_chain (uint32_t cluster, uint32_t size) {
    uint8_t sector[FAT_SECTOR_SIZE];
    char* buffer = (char*)malloc(size + 1);
    uint32_t done = 0;

    if (buffer == NULL) {
        return NULL;
    }
    for (uint32_t guard = 0; done < size && fat_valid_cluster(cluster) && guard < fat_vol.cluster_count; guard++) {
        for (uint32_t s = 0; s < fat_vol.sectors_per_cluster && done < size; s++) {
            if (!fat_read_sector(fat_cluster_lba(cluster) + s, sector)) {
                free(buffer);
                return NULL;
            }
            uint32_t n = MIN(size - done, (uint32_t)FAT_SECTOR_SIZE);
            memcpy(buffer + done, sector, n);
            done += n;
        }
        cluster = fat_get(cluster);
    }
    buffer[done] = '\0';
    return buffer;
}

/* ---------------- public functions (absolute directory paths ending in '/') ---------------- */

bool fat_dir_exists (const char* d_path) {
    uint32_t dir;
    return fat_vol.mounted && fat_resolve_dir(d_path, &dir);
}

// find a file (not a directory) by directory path and name
bool fat_find_file (const char* f_path, const char* name, fat_entry_t* e) {
    uint32_t dir;
    return fat_vol.mounted && fat_resolve_dir(f_path, &dir) && fat_find(dir, name, e) &&
           !(e->entry.attr & FAT_ATTR_DIRECTORY);
}

bool fat_file_exists (const char* f_path, const char* name) {
    fat_entry_t e;
    return fat_find_file(f_path, name, &e);
}

char* fat_read_file (const char* f_path, const char* name) {
    fat_entry_t e;
    if (!fat_find_file(f_path, name, &e)) {
        return NULL;
    }
    return fat_read_chain(fat_entry_cluster(&e.entry), e.entry.size);
}

int fat_create_file (const char* f_path, const char* name, const char* contents) {
    uint32_t dir, first, lba;
    uint16_t offset;
    fat_entry_t existing;
    fat_dirent_t d;
    uint8_t nt_case;

    if (!fat_vol.mounted || !fat_resolve_dir(f_path, &dir)) return FS_ERR_NOT_FOUND;
    if (fat_find(dir, name, &existing)) return FS_ERR_EXISTS;
    memset(&d, 0, sizeof(d));
    if (!fat_make_83(name, d.name, &nt_case)) return FS_ERR_BAD_NAME;

    uint32_t length = (uint32_t)strlen(contents);
    int result = fat_write_chain(contents, length, &first);
    if (result != FS_OK) return result;
    if (!fat_free_slot(dir, &lba, &offset)) {
        fat_free_chain(first);
        return FS_ERR_FULL;
    }

    d.attr = FAT_ATTR_ARCHIVE;
    d.nt_case = nt_case;
    fat_stamp(&d, true);
    d.cluster_high = (uint16_t)(first >> 16);
    d.cluster_low = (uint16_t)(first & 0xFFFF);
    d.size = length;
    if (!fat_write_entry(lba, offset, &d)) {
        fat_free_chain(first);
        return FS_ERR_IO;
    }
    return FS_OK;
}

// replace (append = false) or add to (append = true) the contents of a file
int fat_write_file (const char* f_path, const char* name, const char* text, bool append) {
    fat_entry_t e;
    uint32_t first;

    if (!fat_find_file(f_path, name, &e)) return FS_ERR_NOT_FOUND;
    uint32_t old_cluster = fat_entry_cluster(&e.entry);

    // build the new contents
    char* old = append ? fat_read_chain(old_cluster, e.entry.size) : NULL;
    if (append && old == NULL) return FS_ERR_IO;
    size_t old_len = old ? strlen(old) : 0;
    size_t add_len = strlen(text);
    char* contents = (char*)malloc(old_len + add_len + 1);
    if (contents == NULL) {
        free(old);
        return FS_ERR_NO_MEMORY;
    }
    if (old) memcpy(contents, old, old_len);
    memcpy(contents + old_len, text, add_len + 1);
    free(old);

    // write the new data first, then point the entry at it, then free the old data
    int result = fat_write_chain(contents, (uint32_t)(old_len + add_len), &first);
    free(contents);
    if (result != FS_OK) return result;

    e.entry.cluster_high = (uint16_t)(first >> 16);
    e.entry.cluster_low = (uint16_t)(first & 0xFFFF);
    e.entry.size = (uint32_t)(old_len + add_len);
    e.entry.attr |= FAT_ATTR_ARCHIVE;
    fat_stamp(&e.entry, false);
    if (!fat_write_entry(e.lba, e.offset, &e.entry)) {
        fat_free_chain(first);
        return FS_ERR_IO;
    }
    fat_free_chain(old_cluster);
    return FS_OK;
}

int fat_delete_file (const char* f_path, const char* name) {
    fat_entry_t e;
    if (!fat_find_file(f_path, name, &e)) return FS_ERR_NOT_FOUND;
    if (!fat_mark_deleted(&e)) return FS_ERR_IO;
    fat_free_chain(fat_entry_cluster(&e.entry));
    return FS_OK;
}

// d_path = the new directory's absolute path, its parent must exist
int fat_create_dir (const char* d_path) {
    char parent[FAT_NAME_MAX + 1];
    char name[FAT_NAME_MAX + 1];
    uint32_t parent_dir, lba;
    uint16_t offset;
    fat_entry_t existing;
    fat_dirent_t d;
    uint8_t nt_case;

    // split "/a/b/" into "/a/" and "b"
    size_t len = strlen(d_path);
    if (!fat_vol.mounted || len < 2 || len > FAT_NAME_MAX) return FS_ERR_BAD_NAME;
    strlcpy(parent, d_path, sizeof(parent));
    parent[len - 1] = '\0';
    char* slash = strrchr(parent, '/');
    strlcpy(name, slash + 1, sizeof(name));
    slash[1] = '\0';

    if (!fat_resolve_dir(parent, &parent_dir)) return FS_ERR_NOT_FOUND;
    if (fat_find(parent_dir, name, &existing)) return FS_ERR_EXISTS;
    memset(&d, 0, sizeof(d));
    if (!fat_make_83(name, d.name, &nt_case)) return FS_ERR_BAD_NAME;

    uint32_t cluster = fat_alloc_cluster(0);
    if (cluster == 0) return FS_ERR_FULL;
    if (!fat_zero_cluster(cluster)) {
        fat_free_chain(cluster);
        return FS_ERR_IO;
    }

    d.attr = FAT_ATTR_DIRECTORY;
    d.nt_case = nt_case;
    fat_stamp(&d, true);

    // "." points at the new directory, ".." at its parent (0 when the parent is the root)
    fat_dirent_t dots[2];
    memset(dots, 0, sizeof(dots));
    memset(dots[0].name, ' ', 11);
    memset(dots[1].name, ' ', 11);
    dots[0].name[0] = '.';
    dots[1].name[0] = '.';
    dots[1].name[1] = '.';
    for (int i = 0; i < 2; i++) {
        uint32_t target = (i == 0) ? cluster : parent_dir;
        dots[i].attr = FAT_ATTR_DIRECTORY;
        dots[i].cdate = dots[i].mdate = dots[i].adate = d.cdate;
        dots[i].ctime = dots[i].mtime = d.ctime;
        dots[i].cluster_high = (uint16_t)(target >> 16);
        dots[i].cluster_low = (uint16_t)(target & 0xFFFF);
    }
    if (!fat_write_entry(fat_cluster_lba(cluster), 0, &dots[0]) ||
        !fat_write_entry(fat_cluster_lba(cluster), 32, &dots[1])) {
        fat_free_chain(cluster);
        return FS_ERR_IO;
    }

    if (!fat_free_slot(parent_dir, &lba, &offset)) {
        fat_free_chain(cluster);
        return FS_ERR_FULL;
    }
    d.cluster_high = (uint16_t)(cluster >> 16);
    d.cluster_low = (uint16_t)(cluster & 0xFFFF);
    if (!fat_write_entry(lba, offset, &d)) {
        fat_free_chain(cluster);
        return FS_ERR_IO;
    }
    return FS_OK;
}

bool fat_any_visit (fat_entry_t* e, void* ctx) {
    (void)e;
    (void)ctx;
    return true; // found something, stop
}

int fat_delete_dir (const char* d_path) {
    char parent[FAT_NAME_MAX + 1];
    char name[FAT_NAME_MAX + 1];
    uint32_t parent_dir;
    fat_entry_t e;

    size_t len = strlen(d_path);
    if (!fat_vol.mounted || len < 2 || len > FAT_NAME_MAX) return FS_ERR_NOT_FOUND;
    strlcpy(parent, d_path, sizeof(parent));
    parent[len - 1] = '\0';
    char* slash = strrchr(parent, '/');
    strlcpy(name, slash + 1, sizeof(name));
    slash[1] = '\0';

    if (!fat_resolve_dir(parent, &parent_dir) || !fat_find(parent_dir, name, &e) ||
        !(e.entry.attr & FAT_ATTR_DIRECTORY)) {
        return FS_ERR_NOT_FOUND;
    }
    uint32_t cluster = fat_entry_cluster(&e.entry);
    if (fat_valid_cluster(cluster) && fat_walk_dir(cluster, fat_any_visit, NULL)) {
        return FS_ERR_NOT_EMPTY;
    }
    if (!fat_mark_deleted(&e)) return FS_ERR_IO;
    fat_free_chain(cluster);
    return FS_OK;
}

typedef struct {
    bool directories; // list directories (true) or files (false)
    int shown;
} fat_list_ctx_t;

bool fat_list_visit (fat_entry_t* e, void* ctx) {
    fat_list_ctx_t* list = (fat_list_ctx_t*)ctx;
    bool is_dir = (e->entry.attr & FAT_ATTR_DIRECTORY) != 0;
    if (is_dir != list->directories) {
        return false;
    }
    if (is_dir) {
        printf("  <DIR>   %s/\n", e->name);
    } else {
        char when[FAT_NAME_MAX];
        fat_format_datetime(e->entry.mdate, e->entry.mtime, when, sizeof(when));
        printf("  <FILE>  %-22s %6u bytes  %s\n", e->name, e->entry.size, when);
    }
    list->shown++;
    return false;
}

// print the contents of a directory, returns how many entries were shown (-1 if it does not exist)
int fat_print_dir (const char* d_path) {
    uint32_t dir;
    if (!fat_vol.mounted || !fat_resolve_dir(d_path, &dir)) {
        return -1;
    }
    fat_list_ctx_t dirs = { true, 0 };
    fat_list_ctx_t files = { false, 0 };
    fat_walk_dir(dir, fat_list_visit, &dirs);
    fat_walk_dir(dir, fat_list_visit, &files);
    return dirs.shown + files.shown;
}

/* ---------------- mounting ---------------- */

uint16_t fat_u16 (const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t fat_u32 (const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// copy an 11 character label, trimming the padding
void fat_copy_label (const uint8_t* raw, char* out) {
    memcpy(out, raw, 11);
    out[11] = '\0';
    for (int i = 10; i >= 0 && out[i] == ' '; i--) {
        out[i] = '\0';
    }
}

// the volume label stored in the root directory (Windows keeps the real label here)
bool fat_root_label (char* out) {
    uint8_t sector[FAT_SECTOR_SIZE];
    uint32_t lba;
    for (uint32_t index = 0; index < 4 && fat_dir_sector(FAT_ROOT, index, &lba); index++) {
        if (!fat_read_sector(lba, sector)) {
            return false;
        }
        for (uint16_t off = 0; off < FAT_SECTOR_SIZE; off += 32) {
            const fat_dirent_t* d = (const fat_dirent_t*)(sector + off);
            if (d->name[0] == 0x00) return false;
            if (d->name[0] != 0xE5 && d->attr != FAT_ATTR_LFN && (d->attr & FAT_ATTR_VOLUME_ID)) {
                fat_copy_label(d->name, out);
                return true;
            }
        }
    }
    return false;
}

// try to mount the FAT volume on a drive
// returns FS_OK, FS_ERR_NOT_FOUND (no FAT volume), FS_ERR_BAD_NAME (not labelled RENOVATE) or FS_ERR_IO
int fat_mount (int bus, int slave) {
    uint8_t bs[FAT_SECTOR_SIZE];
    uint32_t volume_lba;

    memset(&fat_vol, 0, sizeof(fat_vol));
    fat_cache_lba = 0xFFFFFFFF;

    int found = drive_parse(bus, slave, &volume_lba);
    if (found < 0) return FS_ERR_IO;
    if (found == 0) return FS_ERR_NOT_FOUND;
    if (!drive_read(bus, slave, volume_lba, 1, bs)) return FS_ERR_IO;
    if (!drive_is_fat_boot_sector(bs)) return FS_ERR_NOT_FOUND;

    uint8_t spc = bs[13];
    uint16_t reserved = fat_u16(bs + 14);
    uint8_t fats = bs[16];
    uint16_t root_entries = fat_u16(bs + 17);
    uint32_t total = fat_u16(bs + 19) ? fat_u16(bs + 19) : fat_u32(bs + 32);
    uint32_t fat_size = fat_u16(bs + 22) ? fat_u16(bs + 22) : fat_u32(bs + 36);
    uint32_t root_sectors = ((uint32_t)root_entries * 32 + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;
    uint32_t data_start = reserved + fats * fat_size + root_sectors;

    if (fat_size == 0 || total <= data_start) return FS_ERR_NOT_FOUND;
    uint32_t clusters = (total - data_start) / spc;
    if (clusters < 4085) return FS_ERR_NOT_FOUND; // FAT12 (floppy sized) is not supported

    fat_vol.bus = bus;
    fat_vol.slave = slave;
    fat_vol.type = (clusters < 65525) ? 16 : 32;
    fat_vol.volume_lba = volume_lba;
    fat_vol.total_sectors = total;
    fat_vol.sectors_per_cluster = spc;
    fat_vol.fat_lba = volume_lba + reserved;
    fat_vol.fat_sectors = fat_size;
    fat_vol.fat_copies = fats;
    fat_vol.root_lba = volume_lba + reserved + fats * fat_size;
    fat_vol.root_sectors = root_sectors;
    fat_vol.data_lba = volume_lba + data_start;
    fat_vol.cluster_count = clusters;
    fat_vol.next_free = 2;

    if (fat_vol.type == 32) {
        uint16_t flags = fat_u16(bs + 40);
        if (flags & 0x80) {
            // FAT mirroring is off: only the active FAT is used
            fat_vol.fat_lba += (flags & 0x0F) * fat_size;
            fat_vol.fat_copies = 1;
        }
        fat_vol.root_cluster = fat_u32(bs + 44);
        uint16_t fsinfo = fat_u16(bs + 48);
        fat_vol.fsinfo_lba = (fsinfo != 0 && fsinfo != 0xFFFF) ? volume_lba + fsinfo : 0;
        if (!fat_valid_cluster(fat_vol.root_cluster)) return FS_ERR_NOT_FOUND;
        fat_copy_label(bs + 71, fat_vol.label);
    } else {
        fat_copy_label(bs + 43, fat_vol.label);
    }

    // the root directory label wins over the boot sector one
    char root_label[12];
    if (fat_root_label(root_label)) {
        strlcpy(fat_vol.label, root_label, sizeof(fat_vol.label));
    }
    if (strcasecmp(fat_vol.label, FAT_VOLUME_LABEL) != 0) {
        return FS_ERR_BAD_NAME;
    }

    fat_vol.mounted = true;
    return FS_OK;
}

// look at every IDE drive and mount the first FAT volume labelled RENOVATE
// status receives a line for the boot log
bool fat_mount_auto (char* status, size_t size) {
    const char* bus_names[2] = { "primary", "secondary" };
    bool unlabelled = false;
    char unlabelled_where[32] = "";

    for (int bus = 0; bus < 2; bus++) {
        for (int slave = 0; slave < 2; slave++) {
            if (drive_probe(bus, slave, NULL, 0) != DRIVE_ATA) {
                continue;
            }
            int result = fat_mount(bus, slave);
            if (result == FS_OK) {
                snprintf(status, size, "FAT%d volume %s on %s %s (%u MB)", fat_vol.type, fat_vol.label,
                         bus_names[bus], slave ? "slave" : "master", fat_vol.total_sectors / 2048);
                return true;
            }
            if (result == FS_ERR_BAD_NAME && !unlabelled) {
                unlabelled = true;
                snprintf(unlabelled_where, sizeof(unlabelled_where), "%s %s", bus_names[bus], slave ? "slave" : "master");
            }
        }
    }
    memset(&fat_vol, 0, sizeof(fat_vol));
    if (unlabelled) {
        snprintf(status, size, "%s is not labelled %s", unlabelled_where, FAT_VOLUME_LABEL);
    } else {
        snprintf(status, size, "no %s disk found", FAT_VOLUME_LABEL);
    }
    return false;
}

#endif /* FAT_H */
