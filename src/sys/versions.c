/*
    versions.c - files remember their past (see versions.h for what is kept, and where)

    How a version is kept, step by step (keep_now):

        1. work out which versions folder and which ten slot names belong to the file
           (/home/alex/notes.txt -> /home/alex/versions/3F2A91C0.V0 ... .V9)
        2. read the first line of each slot, to see which hold versions of this file, and how
           many there have been (the sequence number)
        3. pick a slot: an empty one while there are fewer than ten, then the oldest one
        4. write "#AERV <sequence> <date> <path>" and the file's bytes into it

    Everything else (listing, reading, restoring) reads the same first lines to find the
    versions of a file and put them in order, newest first.
*/

#include "versions.h"
#include "f_sys.h"
#include "sys_clck.h"
#include "../lib/primary_definitions.h"
#include "explain.h"
#include "trace.h"

#define HOMES "/home/"
#define HEADER_MARK "#AERV "   // every version's first line starts with this
#define HEADER_MAX 192         // and is never longer than this
#define FULL_PATH_MAX (FS_PATH_MAX + FS_NAME_MAX)

static int paused = 0; // see versions_pause

// stop keeping versions until versions_resume() (a count, so pauses can nest)
void versions_pause (void) {
    paused++;
}

// undo one versions_pause(), versions are kept again once every pause has been resumed
void versions_resume (void) {
    // never below 0, so a stray extra resume can't make the next pause do nothing
    if (paused > 0) {
        paused--;
    }
}


// =================================================================================================
//  Where a file's versions are
// =================================================================================================

// is a file in this folder versioned? (inside a home folder, but not in its versions folder)
bool versions_apply (const char* folder, char* home_out, size_t size) {
    // "/home/alex/notes/" is in the home folder "/home/alex/": the part up to the slash after
    // the account's name
    size_t homes_length = slen(HOMES);
    if (strncasecmp(folder, HOMES, homes_length) != 0) {
        return false;
    }
    const char* account = folder + homes_length;
    // the account name runs up to the next '/' (folder paths always end in one)
    const char* slash = strchr(account, '/');
    if (slash == NULL || slash == account) {
        return false; // "/home/" itself
    }
    // copy out "/home/alex/", the '/' after the name included
    char home[FS_PATH_MAX];
    size_t home_length = (size_t)(slash - folder) + 1;
    if (home_length >= sizeof(home)) {
        return false;
    }
    memcpy(home, folder, home_length);
    home[home_length] = '\0';

    // the versions folder's own files don't have versions
    // (otherwise keeping a version would change a file in there, which would keep a version of
    // that, and so on)
    char versions[FS_PATH_MAX];
    sfmt(versions, sizeof(versions), "%s%s", home, VERSIONS_FOLDER);
    if (strncasecmp(folder, versions, slen(versions)) == 0) {
        return false;
    }
    if (home_out != NULL) {
        scpy(home_out, home, size);
    }
    return true;
}

// everything about where one file's versions go
typedef struct {
    char versions[FS_PATH_MAX];    // the versions folder, "/home/alex/versions/"
    char path[FULL_PATH_MAX];      // the file itself, "/home/alex/notes.txt"
    uint32_t hash;                 // the number its versions are named after (path_hash)
} place_t;

// FNV-1a, a quick way to turn a string into a number that is very unlikely to be the same for
// two different strings: start with a fixed number, then for each character mix it in (xor) and
// multiply by a prime. Lower case first, as FAT names don't care about case.
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
static uint32_t path_hash (const char* path) {
    uint32_t hash = 2166136261u;
    for (const char* c = path; *c != '\0'; c++) {
        hash ^= (uint8_t)tolower((unsigned char)*c);
        hash *= 16777619u;
    }
    return hash;
}

// where the versions of folder + name go. False if the file isn't versioned
static bool find_place (const char* folder, const char* name, place_t* place) {
    char home[FS_PATH_MAX];
    if (!versions_apply(folder, home, sizeof(home))) {
        return false;
    }
    sfmt(place->versions, sizeof(place->versions), "%s%s", home, VERSIONS_FOLDER);
    sfmt(place->path, sizeof(place->path), "%s%s", folder, name);
    place->hash = path_hash(place->path);
    return true;
}

// the name of slot `slot` (0 to 9): "3F2A91C0.V4"
// (8 hex digits, a dot and "V" + 1 digit fits the 8.3 format, so it works on a FAT disk too)
static void slot_name (const place_t* place, int slot, char* out, size_t size) {
    sfmt(out, size, "%08X.V%d", place->hash, slot);
}


// =================================================================================================
//  The first line of a version
// =================================================================================================

// a version's first line, taken apart by read_header()
typedef struct {
    uint32_t sequence;             // how many versions of the file were kept before this one
    char saved[20];                // when, "DD-MM-YY--HH:MM:SS"
    char path[FULL_PATH_MAX];      // the file it is a version of
    uint32_t header_length;        // where the old contents start
    uint32_t size;                 // and how long they are
} header_t;

// read and take apart the first line of the version at `version_path`:
//     "#AERV 17 10-11-26--14:30:05 /home/alex/notes.txt\n"
// False if there is no such file, or it doesn't start like that
static bool read_header (const char* version_path, header_t* header) {
    fs_file_t file;
    if (!fs_open(version_path, &file)) {
        return false;
    }
    // only the start of the file is read: the first line is never longer than HEADER_MAX,
    // so there is no need to load the whole version
    char line[HEADER_MAX + 1];
    int32_t got = fs_read_at(&file, 0, line, HEADER_MAX);
    if (got <= 0) {
        return false;
    }
    line[got] = '\0';
    // no newline within HEADER_MAX, or no "#AERV " at the start: not one of our versions
    char* end = strchr(line, '\n');
    size_t mark_length = slen(HEADER_MARK);
    if (end == NULL || strncmp(line, HEADER_MARK, mark_length) != 0) {
        return false;
    }
    *end = '\0';

    // the three fields after the mark: the sequence number, the date (no spaces in it), and
    // the path, which is the whole rest of the line (it may have spaces in it)
    char* p = line + mark_length;
    header->sequence = (uint32_t)strtoul(p, &p, 10);
    while (*p == ' ') {
        p++;
    }
    size_t date_length = strcspn(p, " ");
    if (date_length >= sizeof(header->saved) || p[date_length] != ' ') {
        return false;
    }
    memcpy(header->saved, p, date_length);
    header->saved[date_length] = '\0';
    scpy(header->path, p + date_length + 1, sizeof(header->path));

    // the old contents start just after the '\n', and run to the end of the file
    header->header_length = (uint32_t)(end - line) + 1;
    header->size = file.size - header->header_length;
    return true;
}

// what is in one of a file's ten slots
typedef struct {
    bool used;        // something is kept there
    bool ours;        // ...and it is a version of this file (and not of another file whose
                      // path happens to have the same hash)
    header_t header;
} slot_t;

// look at one of a file's slots: is there a version in it, and is it one of this file's?
static void read_slot (const place_t* place, int slot, slot_t* out) {
    char name[16];
    char path[FULL_PATH_MAX];
    slot_name(place, slot, name, sizeof(name));
    sfmt(path, sizeof(path), "%s%s", place->versions, name);

    out->used = fs_file_exists(place->versions, name);
    // the path in the first line has to match, as two paths can share a hash (and so slot names)
    out->ours = out->used && read_header(path, &out->header) && strcasecmp(out->header.path, place->path) == 0;
}

// The slots holding versions of the file, newest first: slot numbers into `order`, their
// contents into `slots` (all ten, in slot order). Returns how many there are
static int our_versions (const place_t* place, slot_t slots[VERSIONS_KEPT], int order[VERSIONS_KEPT]) {
    int count = 0;
    for (int slot = 0; slot < VERSIONS_KEPT; slot++) {
        read_slot(place, slot, &slots[slot]);
        if (slots[slot].ours) {
            order[count++] = slot;
        }
    }
    // newest (the highest sequence number) first: an insertion sort, there are only ten
    for (int i = 1; i < count; i++) {
        int slot = order[i];
        int j = i;
        while (j > 0 && slots[order[j - 1]].header.sequence < slots[slot].header.sequence) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = slot;
    }
    return count;
}


// =================================================================================================
//  Keeping a version
// =================================================================================================

// the files a version was kept of lately, and when (see VERSIONS_GAP_MS)
#define RECENT_MAX 8

// a small ring: remember_kept() writes over the oldest place once all RECENT_MAX are in use
static struct {
    uint32_t hash;
    uint32_t when;   // clkuptms() when it was kept, 0 for an empty place
} recent[RECENT_MAX];
static int recent_next = 0; // the place remember_kept() fills next

// was a version of this file (by its path's hash) kept less than VERSIONS_GAP_MS ago?
// (clkuptms() - when still works when the millisecond counter wraps round, as it is unsigned)
static bool kept_lately (uint32_t hash) {
    for (int i = 0; i < RECENT_MAX; i++) {
        if (recent[i].when != 0 && recent[i].hash == hash && clkuptms() - recent[i].when < VERSIONS_GAP_MS) {
            return true;
        }
    }
    return false;
}

// note that a version of this file was kept just now, for kept_lately()
static void remember_kept (uint32_t hash) {
    recent[recent_next].hash = hash;
    recent[recent_next].when = clkuptms();
    recent_next = (recent_next + 1) % RECENT_MAX;
}

// let the next change to this file keep a version straight away, however soon it comes
// (when = 0 marks the place empty)
static void forget_kept (uint32_t hash) {
    for (int i = 0; i < RECENT_MAX; i++) {
        if (recent[i].hash == hash) {
            recent[i].when = 0;
        }
    }
}

// keep a version of the file now, whatever the time since the last one
static void keep_now (const place_t* place, const char* folder, const char* name) {
    // the file as it is now (before the change that is about to happen)
    uint32_t size;
    char* contents = fs_read_bytes(folder, name, &size);
    if (contents == NULL) {
        return;
    }

    // which slot: an empty one while there are fewer than ten versions, then the oldest
    slot_t slots[VERSIONS_KEPT];
    int order[VERSIONS_KEPT];
    int count = our_versions(place, slots, order);
    int slot = -1;
    if (count < VERSIONS_KEPT) {
        for (int i = 0; i < VERSIONS_KEPT && slot < 0; i++) {
            if (!slots[i].used) {
                slot = i;
            }
        }
    }
    if (slot < 0 && count > 0) {
        slot = order[count - 1]; // the oldest
    }
    if (slot < 0) {
        free(contents); // every slot holds another file's versions: nowhere to keep this one
        return;
    }

    // the first line, then the contents. The sequence number is one more than the newest
    // version's, so the order survives slots being reused
    char saved[20];
    rtc_datetime(saved, sizeof(saved));
    uint32_t sequence = count > 0 ? slots[order[0]].header.sequence + 1 : 1;
    char header[HEADER_MAX];
    sfmt(header, sizeof(header), HEADER_MARK "%u %s %s\n", sequence, saved, place->path);
    uint32_t header_length = (uint32_t)slen(header);
    char* version = malloc(header_length + size);
    if (version == NULL) {
        free(contents);
        return;
    }
    memcpy(version, header, header_length);
    memcpy(version + header_length, contents, size);
    free(contents);

    // the versions folder is only made the first time something in the home folder changes
    int result = FS_OK;
    if (!fs_dir_exists(place->versions)) {
        result = fs_create_d(place->versions, "old versions of files");
    }
    char file[16];
    slot_name(place, slot, file, sizeof(file));
    // a slot that is already a file gets written over, an empty one is made
    // (neither keeps a version of its own: versions_apply() says no for the versions folder)
    if (result == FS_OK && slots[slot].used) {
        result = fs_write_bytes(place->versions, file, version, header_length + size, false);
    } else if (result == FS_OK) {
        result = fs_create_bytes(place->versions, file, version, header_length + size);
    }
    free(version);

    // The change itself still goes ahead, but say that this version couldn't be kept (a full
    // disk, or a drive error), so nobody counts on UNDO for it. WHY gives the reason
    if (result != FS_OK) {
        explain_say_fs(result, name, "[ ! ] Couldn't keep a version of %s for UNDO", name);
    }
}

// is the file there, and small enough to keep a version of?
static bool keepable (const place_t* place) {
    fs_file_t file;
    return fs_open(place->path, &file) && file.size <= VERSIONS_LARGEST;
}

// keep a copy of a file before it is changed or deleted (see versions.h for when it doesn't)
void versions_keep (const char* folder, const char* name) {
    place_t place;
    // paused, not in a home folder, or a version was kept only moments ago: nothing to do
    if (paused > 0) {
        TRACE("no version of %s: versions are paused while UNDO or ENC writes", name);
        return;
    }
    if (!find_place(folder, name, &place)) {
        TRACE("no version of %s: only files in a home folder keep versions", name);
        return;
    }
    if (kept_lately(place.hash)) {
        TRACE("no new version of %s: one was kept moments ago, and this change joins it", name);
        return;
    }
    if (!keepable(&place)) {
        TRACE("no version of %s: it is new, or too big to keep", name);
        return; // a new file (nothing to keep yet), or too big to keep
    }
    keep_now(&place, folder, name);
    remember_kept(place.hash);
    TRACE("kept %s as it was, as %s@1 (in %s), so UNDO can bring it back", name, name, place.versions);
}

// delete every version of a file, zeroing each one first
void versions_forget (const char* folder, const char* name) {
    place_t place;
    if (paused > 0 || !find_place(folder, name, &place)) {
        return;
    }
    slot_t slots[VERSIONS_KEPT];
    int order[VERSIONS_KEPT];
    int count = our_versions(&place, slots, order);
    for (int i = 0; i < count; i++) {
        char file[16];
        slot_name(&place, order[i], file, sizeof(file));
        // deleting only marks the clusters free, so overwrite the old bytes with zeros first
        bool wipe_old = true;
        fs_write_bytes(place.versions, file, "", 0, wipe_old);
        fs_delete_f(place.versions, file);
    }
    forget_kept(place.hash);
}


// =================================================================================================
//  Reading and bringing back versions
// =================================================================================================

// the versions of a file, newest first, into list. Returns how many
int versions_list (const char* folder, const char* name, version_t* list) {
    place_t place;
    if (!find_place(folder, name, &place)) {
        return 0;
    }
    slot_t slots[VERSIONS_KEPT];
    int order[VERSIONS_KEPT];
    int count = our_versions(&place, slots, order);
    for (int i = 0; i < count; i++) {
        // numbers are by age, newest = 1, not the slot number or the sequence number
        const header_t* header = &slots[order[i]].header;
        list[i].number = i + 1;
        list[i].size = header->size;
        scpy(list[i].saved, header->saved, sizeof(list[i].saved));
    }
    return count;
}

// the contents of version `number` (1 = the newest), without its first line. free() it
char* versions_read (const char* folder, const char* name, int number, uint32_t* size) {
    place_t place;
    if (!find_place(folder, name, &place)) {
        return NULL;
    }
    slot_t slots[VERSIONS_KEPT];
    int order[VERSIONS_KEPT];
    int count = our_versions(&place, slots, order);
    if (number < 1 || number > count) {
        return NULL;
    }

    // the whole version file, then the first line is moved out of the way
    char file[16];
    slot_name(&place, order[number - 1], file, sizeof(file));
    uint32_t total;
    char* bytes = fs_read_bytes(place.versions, file, &total);
    if (bytes == NULL) {
        return NULL;
    }
    uint32_t header_length = slots[order[number - 1]].header.header_length;
    uint32_t length = total > header_length ? total - header_length : 0;
    // memmove rather than memcpy, because the two areas overlap
    memmove(bytes, bytes + header_length, length);
    bytes[length] = '\0'; // (fs_read_bytes leaves room for one after the end)
    *size = length;
    return bytes;
}

// put version `number` back as the file's contents (UNDO), or make the file again if it is gone
int versions_restore (const char* folder, const char* name, int number) {
    place_t place;
    if (!find_place(folder, name, &place)) {
        return FS_ERR_NOT_FOUND;
    }
    uint32_t size;
    char* contents = versions_read(folder, name, number, &size);
    if (contents == NULL) {
        return FS_ERR_NOT_FOUND;
    }

    // keep what is there now, so the undo can be undone (whenever the last version was kept:
    // this one must never be skipped), then put the old contents back
    bool exists = fs_file_exists(folder, name);
    if (exists && keepable(&place)) {
        keep_now(&place, folder, name);
    }
    // paused, so writing the old contents back doesn't keep yet another version
    versions_pause();
    int result;
    if (exists) {
        result = fs_write_bytes(folder, name, contents, size, false);
    } else {
        result = fs_create_bytes(folder, name, contents, size);
    }
    versions_resume();
    free(contents);
    forget_kept(place.hash); // the next change keeps a version, however soon it comes
    return result;
}

// "notes.txt@2" -> "notes.txt" and 2, false if the name doesn't end in '@' and a number
bool versions_split_name (const char* name, char* base, size_t size, int* number) {
    // the last '@', so a name with an '@' of its own still works. It needs a name in front of
    // it and a number after it
    const char* at = strrchr(name, '@');
    if (at == NULL || at == name || at[1] == '\0') {
        return false;
    }
    // only digits after the '@', so "me@home.txt" is just an ordinary name
    for (const char* digit = at + 1; *digit != '\0'; digit++) {
        if (!isdigit((unsigned char)*digit)) {
            return false;
        }
    }
    size_t length = (size_t)(at - name);
    if (length >= size) {
        return false;
    }
    memcpy(base, name, length);
    base[length] = '\0';
    // there is no version 0 ("notes.txt@0" is an ordinary name)
    *number = atoi(at + 1);
    return *number >= 1;
}


// =================================================================================================
//  Every file that has versions (VERS on its own)
// =================================================================================================

#define FILES_LISTED 64   // the most files versions_each_file reports

// what versions_each_file() adds up while listing a versions folder: one path per file,
// with how many versions of it were found
typedef struct {
    const char* versions;  // the versions folder being listed
    char paths[FILES_LISTED][FULL_PATH_MAX];
    int counts[FILES_LISTED];
    int files;
} file_tally_t;

// fs_list() calls this for each version in the folder: count it under its file's path
static void tally_version (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    file_tally_t* tally = context;
    if (is_dir) {
        return;
    }
    char path[FULL_PATH_MAX];
    header_t header;
    sfmt(path, sizeof(path), "%s%s", tally->versions, name);
    if (!read_header(path, &header)) {
        return; // not a version (something else someone put in the folder)
    }
    // the slot names are only hashes, the real path is in the first line.
    // Seen this file before? Add one to its count, otherwise give it a new line
    for (int i = 0; i < tally->files; i++) {
        if (strcasecmp(tally->paths[i], header.path) == 0) {
            tally->counts[i]++;
            return;
        }
    }
    if (tally->files < FILES_LISTED) {
        scpy(tally->paths[tally->files], header.path, FULL_PATH_MAX);
        tally->counts[tally->files] = 1;
        tally->files++;
    }
}

// call fn once for every file in a home folder that has versions, with how many it has
int versions_each_file (const char* home, versions_file_fn fn, void* ctx) {
    char versions[FS_PATH_MAX];
    sfmt(versions, sizeof(versions), "%s%s", home, VERSIONS_FOLDER);
    // 64 paths of 128 bytes is 8KB: too much for the kernel's stack
    file_tally_t* tally = malloc(sizeof(file_tally_t));
    if (tally == NULL) {
        return 0;
    }
    tally->versions = versions;
    tally->files = 0;
    fs_list(versions, tally_version, tally);

    // fn is only called once the listing has finished, so it is free to use the file system
    for (int i = 0; i < tally->files; i++) {
        fn(tally->paths[i], tally->counts[i], ctx);
    }
    int files = tally->files;
    free(tally);
    return files;
}
