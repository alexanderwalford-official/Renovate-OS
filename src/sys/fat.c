/*
    fat.c
    FAT16 / FAT32 file system driver, see fat.h.
*/
#include "fat.h"
#include "sys_clck.h"
#include "../drivers/storage/ata.h"
#include "../lib/ren_ctype.h"
#include "../lib/ren_string.h"
#include "../lib/ren_stdlib.h"
#include "../lib/ren_stdio.h"

#define FAT_SECTOR_SIZE        512
#define FAT_ENTRIES_PER_SECTOR (FAT_SECTOR_SIZE / 32)
#define FAT_ROOT               0     // directory "cluster" used for the root directory
#define FAT_NAME_MAX           255
#define FAT_LFN_PARTS          20    // 20 long name entries of 13 characters hold the longest name
#define FAT_DIR_MAX_SECTORS    4096  // FAT allows at most 65536 entries in a directory
#define FAT_NO_SECTOR          0xFFFFFFFF

#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F // read only + hidden + system + volume id

#define FAT_CASE_LOWER_BASE 0x08 // Windows NT: name part is lower case
#define FAT_CASE_LOWER_EXT  0x10 // Windows NT: extension is lower case

// first byte of a directory entry
#define FAT_DIR_END     0x00 // this and every later entry are unused
#define FAT_DIR_DELETED 0xE5

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

// a sector seen as bytes, FAT16 / FAT32 entries or directory entries
typedef union {
    uint8_t u8[FAT_SECTOR_SIZE];
    uint16_t u16[FAT_SECTOR_SIZE / 2];
    uint32_t u32[FAT_SECTOR_SIZE / 4];
    fat_dirent_t dirents[FAT_ENTRIES_PER_SECTOR];
} fat_sector_t;

// where we are while stepping through the sectors of a directory
typedef struct {
    uint32_t cluster; // 0 in the fixed size FAT16 root directory
    uint32_t index;   // sector number inside the directory
    uint32_t lba;
} fat_dir_pos_t;

// a file or directory found while walking a directory
typedef struct {
    fat_dirent_t entry;
    uint32_t lba;            // sector holding the entry
    uint16_t slot;           // its place in that sector
    fat_dir_pos_t lfn_pos;   // where its long name entries start (the entry itself without a long name)
    uint16_t lfn_slot;
    char name[FAT_NAME_MAX + 1];  // long name, or the 8.3 name
    char short_name[13];          // 8.3 name, e.g. "NOTES.TXT"
} fat_entry_t;

typedef bool (*fat_visit_t)(const fat_entry_t* entry, void* ctx); // return true to stop walking

fat_volume_t fat_vol;
static fat_sector_t fat_cache; // one cached sector of the (first) FAT
static uint32_t fat_cache_lba = FAT_NO_SECTOR;

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

static bool fat_read_sector (uint32_t lba, void* buffer) {
    return drive_read(fat_vol.bus, fat_vol.slave, lba, 1, buffer);
}

static bool fat_write_sector (uint32_t lba, const void* buffer) {
    return drive_write(fat_vol.bus, fat_vol.slave, lba, 1, buffer);
}

static uint32_t fat_cluster_lba (uint32_t cluster) {
    return fat_vol.data_lba + (cluster - 2) * fat_vol.sectors_per_cluster;
}

static bool fat_valid_cluster (uint32_t cluster) {
    return cluster >= 2 && cluster < fat_vol.cluster_count + 2;
}

static uint32_t fat_end_of_chain (void) {
    return (fat_vol.type == 32) ? 0x0FFFFFFF : 0xFFFF;
}

static uint32_t fat_entries_per_sector (void) {
    return (fat_vol.type == 32) ? FAT_SECTOR_SIZE / 4 : FAT_SECTOR_SIZE / 2;
}

// the FAT entry for a cluster: 0 = free, a cluster number, or end of chain (0xFFFFFFFF on read errors)
static uint32_t fat_get (uint32_t cluster) {
    uint32_t lba = fat_vol.fat_lba + cluster / fat_entries_per_sector();
    uint32_t i = cluster % fat_entries_per_sector();

    if (fat_cache_lba != lba) {
        if (!fat_read_sector(lba, &fat_cache)) {
            fat_cache_lba = FAT_NO_SECTOR;
            return 0xFFFFFFFF;
        }
        fat_cache_lba = lba;
    }
    return (fat_vol.type == 32) ? fat_cache.u32[i] & 0x0FFFFFFF : fat_cache.u16[i];
}

// change the FAT entry for a cluster in every copy of the FAT
static bool fat_set (uint32_t cluster, uint32_t value) {
    fat_sector_t sector;
    uint32_t i = cluster % fat_entries_per_sector();

    for (uint8_t copy = 0; copy < fat_vol.fat_copies; copy++) {
        uint32_t lba = fat_vol.fat_lba + copy * fat_vol.fat_sectors + cluster / fat_entries_per_sector();
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
        if (lba == fat_cache_lba) {
            fat_cache = sector;
        }
    }
    return true;
}

// FAT32 keeps a "free clusters" count in the FSInfo sector, mark it as unknown once we change the FAT
static void fat_invalidate_fsinfo (void) {
    fat_sector_t sector;
    if (fat_vol.type != 32 || fat_vol.fsinfo_invalidated || fat_vol.fsinfo_lba == 0) {
        return;
    }
    fat_vol.fsinfo_invalidated = true;
    if (!fat_read_sector(fat_vol.fsinfo_lba, &sector) ||
        sector.u32[0] != 0x41615252 || sector.u32[484 / 4] != 0x61417272) {
        return;
    }
    memset(sector.u8 + 488, 0xFF, 8); // free count & next free = unknown
    fat_write_sector(fat_vol.fsinfo_lba, &sector);
}

// fill a cluster with zeros
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
static uint32_t fat_alloc_cluster (uint32_t prev) {
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
static void fat_free_chain (uint32_t cluster) {
    if (!fat_valid_cluster(cluster)) {
        return;
    }
    fat_invalidate_fsinfo();
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

static uint8_t fat_lfn_checksum (const uint8_t* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

// "NOTES   TXT" -> "NOTES.TXT" (or "notes.txt" when the Windows lower case flags are set)
static void fat_short_to_name (const uint8_t* raw, uint8_t nt_case, char* out, bool apply_case) {
    int p = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)((i == 0 && raw[i] == 0x05) ? 0xE5 : raw[i]); // 0x05 stands in for a real 0xE5
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

static bool fat_valid_83_char (char c) {
    return isalnum((unsigned char)c) || (c != '\0' && strchr("!#$%&'()-@^_`{}~", c) != NULL);
}

// copy the name or extension part of an 8.3 name in upper case
// an all lower case part is remembered with its Windows NT flag, mixed case is just stored upper case
static bool fat_copy_83_part (const char* src, size_t len, uint8_t* out, uint8_t lower_flag, uint8_t* nt_case) {
    bool lower = false, upper = false;
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
static bool fat_make_83 (const char* name, uint8_t* out, uint8_t* nt_case) {
    const char* dot = strchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;

    if (base_len == 0 || base_len > 8 || ext_len > 3 || (dot && ext_len == 0) || (dot && strchr(dot + 1, '.'))) {
        return false;
    }
    memset(out, ' ', 11);
    *nt_case = 0;
    return fat_copy_83_part(name, base_len, out, FAT_CASE_LOWER_BASE, nt_case) &&
           fat_copy_83_part(dot ? dot + 1 : "", ext_len, out + 8, FAT_CASE_LOWER_EXT, nt_case);
}

// set the modified (and if created, the created) date & time of an entry to now
static void fat_stamp (fat_dirent_t* d, bool created) {
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
static void fat_format_datetime (uint16_t date, uint16_t time, char* out, size_t size) {
    snprintf(out, size, "%02u-%02u-%02u--%02u:%02u:%02u",
             date & 0x1F, (date >> 5) & 0x0F, ((date >> 9) + 1980) % 100,
             time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2);
}

static uint32_t fat_entry_cluster (const fat_dirent_t* d) {
    return ((uint32_t)d->cluster_high << 16) | d->cluster_low;
}

static void fat_set_entry_cluster (fat_dirent_t* d, uint32_t cluster) {
    d->cluster_high = (uint16_t)(cluster >> 16);
    d->cluster_low = (uint16_t)(cluster & 0xFFFF);
}

// first sector of a directory, false if the directory is broken
static bool fat_dir_start (uint32_t dir, fat_dir_pos_t* pos) {
    pos->index = 0;
    if (dir == FAT_ROOT && fat_vol.type == 16) {
        pos->cluster = 0;
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
    if (pos->cluster == 0) {
        pos->lba++;
        return pos->index < fat_vol.root_sectors;
    }
    if (pos->index % fat_vol.sectors_per_cluster != 0) {
        pos->lba++;
        return true;
    }
    uint32_t next = fat_get(pos->cluster);
    if (!fat_valid_cluster(next)) {
        return false;
    }
    pos->cluster = next;
    pos->lba = fat_cluster_lba(next);
    return true;
}

// call visit for every file and sub directory ("." and ".." skipped), returns true if visit stopped early
static bool fat_walk_dir (uint32_t dir, fat_visit_t visit, void* ctx) {
    static const uint8_t lfn_char_offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    fat_sector_t sector;
    fat_entry_t e;
    fat_dir_pos_t pos;
    fat_dir_pos_t lfn_pos = { 0, 0, 0 };
    uint16_t lfn_slot = 0;
    char lfn[FAT_LFN_PARTS * 13 + 1];
    bool lfn_valid = false;
    int lfn_expect = 0;
    uint8_t lfn_sum = 0;

    for (bool more = fat_dir_start(dir, &pos); more; more = fat_dir_next(&pos)) {
        if (!fat_read_sector(pos.lba, &sector)) {
            return false;
        }
        for (uint16_t slot = 0; slot < FAT_ENTRIES_PER_SECTOR; slot++) {
            const fat_dirent_t* d = &sector.dirents[slot];
            const uint8_t* raw = (const uint8_t*)d;

            if (d->name[0] == FAT_DIR_END) {
                return false;
            }
            if (d->name[0] == FAT_DIR_DELETED) {
                lfn_valid = false;
                continue;
            }
            if (d->attr == FAT_ATTR_LFN) {
                int seq = raw[0] & 0x1F;
                if (raw[0] & 0x40) {
                    // the last part of a long name is stored first
                    lfn_valid = (seq >= 1 && seq <= FAT_LFN_PARTS);
                    lfn_expect = seq;
                    lfn_sum = raw[13];
                    lfn_pos = pos;
                    lfn_slot = slot;
                    memset(lfn, 0, sizeof(lfn));
                } else if (!lfn_valid || seq != lfn_expect || raw[13] != lfn_sum) {
                    lfn_valid = false;
                }
                if (lfn_valid) {
                    for (int i = 0; i < 13; i++) {
                        uint16_t c = (uint16_t)(raw[lfn_char_offsets[i]] | (raw[lfn_char_offsets[i] + 1] << 8));
                        // 0x0000 ends the name and 0xFFFF pads it, characters outside ASCII are shown as '?'
                        lfn[(seq - 1) * 13 + i] = (c == 0xFFFF) ? '\0' : (c < 128) ? (char)c : '?';
                    }
                    lfn_expect = seq - 1;
                }
                continue;
            }
            if (d->attr & FAT_ATTR_VOLUME_ID) {
                lfn_valid = false;
                continue;
            }

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
                strlcpy(e.name, lfn, sizeof(e.name));
                e.lfn_pos = lfn_pos;
                e.lfn_slot = lfn_slot;
            } else {
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

typedef struct {
    const char* name;
    fat_entry_t* found;
} fat_find_ctx_t;

static bool fat_find_visit (const fat_entry_t* e, void* ctx) {
    fat_find_ctx_t* find = ctx;
    if (strcasecmp(e->name, find->name) == 0 || strcasecmp(e->short_name, find->name) == 0) {
        *find->found = *e;
        return true;
    }
    return false;
}

// look up a name (long or 8.3, any case) in a directory
static bool fat_find (uint32_t dir, const char* name, fat_entry_t* out) {
    fat_find_ctx_t ctx = { name, out };
    return fat_walk_dir(dir, fat_find_visit, &ctx);
}

// absolute directory path ("/sys/cache/") -> its first cluster
static bool fat_resolve_dir (const char* d_path, uint32_t* dir) {
    char component[FAT_NAME_MAX + 1];
    fat_entry_t e;
    uint32_t current = FAT_ROOT;
    const char* p = d_path;

    if (!fat_vol.mounted) {
        return false;
    }
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        size_t len = strcspn(p, "/");
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
    if (pos.cluster == 0 || pos.index >= FAT_DIR_MAX_SECTORS) {
        return false;
    }
    // clear the new cluster before linking it in, so the directory never shows old data
    uint32_t added = fat_alloc_cluster(0);
    if (added == 0) {
        return false;
    }
    if (!fat_zero_cluster(added) || !fat_set(pos.cluster, added)) {
        fat_free_chain(added);
        return false;
    }
    *lba = fat_cluster_lba(added);
    *slot = 0;
    return true;
}

// write a directory entry into a slot
static bool fat_write_entry (uint32_t lba, uint16_t slot, const fat_dirent_t* entry) {
    fat_sector_t sector;
    if (!fat_read_sector(lba, &sector)) {
        return false;
    }
    sector.dirents[slot] = *entry;
    return fat_write_sector(lba, &sector);
}

// mark an entry and the long name entries in front of it as deleted
static bool fat_mark_deleted (const fat_entry_t* e) {
    fat_sector_t sector;
    fat_dir_pos_t pos = e->lfn_pos;
    uint16_t from = e->lfn_slot;

    while (true) {
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
static int fat_write_chain (const char* data, uint32_t length, uint32_t* first) {
    fat_sector_t sector;
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
            memset(&sector, 0, sizeof(sector));
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
static char* fat_read_chain (uint32_t cluster, uint32_t size) {
    fat_sector_t sector;
    char* buffer = malloc(size + 1);
    uint32_t done = 0;

    if (buffer == NULL) {
        return NULL;
    }
    for (uint32_t guard = 0; done < size && fat_valid_cluster(cluster) && guard < fat_vol.cluster_count; guard++) {
        for (uint32_t s = 0; s < fat_vol.sectors_per_cluster && done < size; s++) {
            if (!fat_read_sector(fat_cluster_lba(cluster) + s, &sector)) {
                free(buffer);
                return NULL;
            }
            uint32_t n = MIN(size - done, (uint32_t)FAT_SECTOR_SIZE);
            memcpy(buffer + done, &sector, n);
            done += n;
        }
        cluster = fat_get(cluster);
    }
    buffer[done] = '\0';
    return buffer;
}

bool fat_dir_exists (const char* d_path) {
    uint32_t dir;
    return fat_resolve_dir(d_path, &dir);
}

// find a file (not a directory) by directory path and name
static bool fat_find_file (const char* f_path, const char* name, fat_entry_t* e) {
    uint32_t dir;
    return fat_resolve_dir(f_path, &dir) && fat_find(dir, name, e) && !(e->entry.attr & FAT_ATTR_DIRECTORY);
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
    fat_entry_t existing;
    fat_dirent_t d;
    uint32_t dir, first, lba;
    uint16_t slot;

    if (!fat_resolve_dir(f_path, &dir)) return FS_ERR_NOT_FOUND;
    if (fat_find(dir, name, &existing)) return FS_ERR_EXISTS;
    memset(&d, 0, sizeof(d));
    if (!fat_make_83(name, d.name, &d.nt_case)) return FS_ERR_BAD_NAME;

    uint32_t length = (uint32_t)strlen(contents);
    int result = fat_write_chain(contents, length, &first);
    if (result != FS_OK) return result;
    if (!fat_free_slot(dir, &lba, &slot)) {
        fat_free_chain(first);
        return FS_ERR_FULL;
    }

    d.attr = FAT_ATTR_ARCHIVE;
    d.size = length;
    fat_set_entry_cluster(&d, first);
    fat_stamp(&d, true);
    if (!fat_write_entry(lba, slot, &d)) {
        fat_free_chain(first);
        return FS_ERR_IO;
    }
    return FS_OK;
}

// replace (append = false) or add to (append = true) the contents of a file
int fat_write_file (const char* f_path, const char* name, const char* text, bool append) {
    fat_entry_t e;
    uint32_t first;
    char* joined = NULL;
    const char* contents = text;

    if (!fat_find_file(f_path, name, &e)) return FS_ERR_NOT_FOUND;
    uint32_t old_cluster = fat_entry_cluster(&e.entry);

    if (append) {
        char* old = fat_read_chain(old_cluster, e.entry.size);
        if (old == NULL) return FS_ERR_IO;
        size_t old_len = strlen(old);
        joined = realloc(old, old_len + strlen(text) + 1);
        if (joined == NULL) {
            free(old);
            return FS_ERR_NO_MEMORY;
        }
        strcpy(joined + old_len, text);
        contents = joined;
    }

    // write the new data first, then point the entry at it, then free the old data
    uint32_t length = (uint32_t)strlen(contents);
    int result = fat_write_chain(contents, length, &first);
    free(joined);
    if (result != FS_OK) return result;

    fat_set_entry_cluster(&e.entry, first);
    e.entry.size = length;
    e.entry.attr |= FAT_ATTR_ARCHIVE;
    fat_stamp(&e.entry, false);
    if (!fat_write_entry(e.lba, e.slot, &e.entry)) {
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

// split "/a/b/" into "/a/" and "b" (both buffers FAT_NAME_MAX + 1 long)
static bool fat_split_dir (const char* d_path, char* parent, char* name) {
    size_t len = strlen(d_path);
    if (len < 2 || len > FAT_NAME_MAX) {
        return false;
    }
    strlcpy(parent, d_path, FAT_NAME_MAX + 1);
    parent[len - 1] = '\0';
    char* slash = strrchr(parent, '/');
    strlcpy(name, slash + 1, FAT_NAME_MAX + 1);
    slash[1] = '\0';
    return true;
}

// "." and ".." in a new directory, they take the directory's dates
static void fat_make_dot (fat_dirent_t* out, const fat_dirent_t* dir, const char* name, uint32_t cluster) {
    *out = *dir;
    out->nt_case = 0;
    memset(out->name, ' ', sizeof(out->name));
    memcpy(out->name, name, strlen(name));
    fat_set_entry_cluster(out, cluster);
}

// d_path = the new directory's absolute path, its parent must exist
int fat_create_dir (const char* d_path) {
    char parent[FAT_NAME_MAX + 1];
    char name[FAT_NAME_MAX + 1];
    fat_entry_t existing;
    fat_dirent_t d;
    fat_sector_t sector;
    uint32_t parent_dir, lba;
    uint16_t slot;

    if (!fat_split_dir(d_path, parent, name)) return FS_ERR_BAD_NAME;
    if (!fat_resolve_dir(parent, &parent_dir)) return FS_ERR_NOT_FOUND;
    if (fat_find(parent_dir, name, &existing)) return FS_ERR_EXISTS;
    memset(&d, 0, sizeof(d));
    if (!fat_make_83(name, d.name, &d.nt_case)) return FS_ERR_BAD_NAME;
    d.attr = FAT_ATTR_DIRECTORY;
    fat_stamp(&d, true);

    uint32_t cluster = fat_alloc_cluster(0);
    if (cluster == 0) return FS_ERR_FULL;

    // ".." points at the parent, 0 when the parent is the root
    memset(&sector, 0, sizeof(sector));
    fat_make_dot(&sector.dirents[0], &d, ".", cluster);
    fat_make_dot(&sector.dirents[1], &d, "..", parent_dir);
    if (!fat_zero_cluster(cluster) || !fat_write_sector(fat_cluster_lba(cluster), &sector)) {
        fat_free_chain(cluster);
        return FS_ERR_IO;
    }

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

static bool fat_any_visit (const fat_entry_t* e, void* ctx) {
    (void)e;
    (void)ctx;
    return true; // found something, stop
}

int fat_delete_dir (const char* d_path) {
    char parent[FAT_NAME_MAX + 1];
    char name[FAT_NAME_MAX + 1];
    uint32_t parent_dir;
    fat_entry_t e;

    if (!fat_split_dir(d_path, parent, name) || !fat_resolve_dir(parent, &parent_dir) ||
        !fat_find(parent_dir, name, &e) || !(e.entry.attr & FAT_ATTR_DIRECTORY)) {
        return FS_ERR_NOT_FOUND;
    }
    // a broken entry pointing at cluster 0 would otherwise walk the root directory
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

static bool fat_list_visit (const fat_entry_t* e, void* ctx) {
    fat_list_ctx_t* list = ctx;
    bool is_dir = (e->entry.attr & FAT_ATTR_DIRECTORY) != 0;
    if (is_dir != list->directories) {
        return false;
    }
    if (is_dir) {
        printf("  <DIR>   %s/\n", e->name);
    } else {
        char when[20];
        fat_format_datetime(e->entry.mdate, e->entry.mtime, when, sizeof(when));
        printf("  <FILE>  %-22s %6u bytes  %s\n", e->name, e->entry.size, when);
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
    fat_list_ctx_t dirs = { true, 0 };
    fat_list_ctx_t files = { false, 0 };
    fat_walk_dir(dir, fat_list_visit, &dirs);
    fat_walk_dir(dir, fat_list_visit, &files);
    return dirs.shown + files.shown;
}

static uint16_t fat_u16 (const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t fat_u32 (const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// copy an 11 character label, trimming the padding
static void fat_copy_label (const uint8_t* raw, char* out) {
    memcpy(out, raw, 11);
    out[11] = '\0';
    for (int i = 10; i >= 0 && out[i] == ' '; i--) {
        out[i] = '\0';
    }
}

// the volume label stored in the root directory (Windows keeps the real label here)
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
            if (d->name[0] != FAT_DIR_DELETED && d->attr != FAT_ATTR_LFN && (d->attr & FAT_ATTR_VOLUME_ID)) {
                fat_copy_label(d->name, out);
                return true;
            }
        }
    }
    return false;
}

// try to mount the FAT volume on a drive
// returns FS_OK, FS_ERR_NOT_FOUND (no FAT volume), FS_ERR_BAD_NAME (not labelled RENOVATE) or FS_ERR_IO
static int fat_mount (int bus, int slave) {
    uint8_t bs[FAT_SECTOR_SIZE];
    uint32_t volume_lba;

    memset(&fat_vol, 0, sizeof(fat_vol));
    fat_cache_lba = FAT_NO_SECTOR;

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
