/*
    versions.h - files remember their past

    Every time a file in a home folder (/home/<name>/...) is changed or deleted, AerOS first
    keeps a copy of it as it was. The last VERSIONS_KEPT copies of each file are kept, and any
    of them can be read or brought back:

        vers notes.txt      list them, newest first. @1 is the file as it was before its
                            last change, @2 before the one before that...
        rd notes.txt@2      read one. Everything that reads files understands name@N: RD, the
                            reader (READ notes.txt@2), AerLang's rdf("notes.txt@2")
        undo notes.txt      bring back @1 (UNDO notes.txt 3 brings back @3). What it replaces
                            is kept as a version first, so an undo can be undone
        undo notes.txt      after DEL notes.txt, too: the file comes back

    WHERE THEY ARE KEPT

    In the home folder's own versions folder, /home/<name>/versions/, so they belong to the
    same person as the file (and the three wrong passwords wipe takes them along with
    everything else). On a FAT disk a name has to fit 8.3, so a version is named after a number
    worked out from the file's path, and the slot (0 to 9) it is kept in:

        /home/alex/notes.txt  ->  /home/alex/versions/3F2A91C0.V4

    Each version starts with a line saying what it is, then the file's bytes as they were:

        #AERV 17 10-11-26--14:30:05 /home/alex/notes.txt
        ...the old contents...

    17 is how many versions of the file were kept before this one, which puts them in order. The
    ten slots are used in turn: the 11th version goes in the place of the oldest.

    WHAT ISN'T KEPT

    - files outside home folders (the system's own files, /bin/, /docs/...)
    - files bigger than VERSIONS_LARGEST: copying a video every time would fill the disk
    - more than one version of a file every VERSIONS_GAP_MS: a program writing a log line by
      line keeps the file as it was before it started, not a thousand copies
    - the files of a folder deleted in one go (DEL on a folder): only DEL on a file keeps one
    - encrypted files: encrypting a file deletes its versions (zeroed first), as they are the
      very copies of it the encryption is there to protect
*/
#ifndef VERSIONS_H
#define VERSIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VERSIONS_KEPT     10                // versions kept of each file
#define VERSIONS_LARGEST  (1024 * 1024)     // bigger files aren't versioned (1MB)
#define VERSIONS_GAP_MS   10000             // at most one version of a file every 10 seconds
#define VERSIONS_FOLDER   "versions/"       // inside each home folder

// one version of a file
typedef struct {
    int number;          // 1 is the newest (name@1), 2 the one before...
    uint32_t size;       // the file's size when it was kept
    char saved[20];      // when it was kept, "DD-MM-YY--HH:MM:SS"
} version_t;

// Keep a copy of a file as it is now, before it is changed or deleted. f_sys.c calls this from
// every function that changes or deletes a file, so nothing else needs to. It does nothing for a
// file that isn't versioned (see above), or that doesn't exist yet
void versions_keep (const char* folder, const char* name);

// delete every version of a file, overwriting each with zeros first (for encryption)
void versions_forget (const char* folder, const char* name);

// stop keeping (and forgetting) versions for a while, and start again (fs_empty_dir deletes
// whole folders without them). These can nest: it starts again after as many resumes as pauses
void versions_pause (void);
void versions_resume (void);

// the versions of a file, newest first, written into list (room for VERSIONS_KEPT).
// Returns how many there are (0 if none)
int versions_list (const char* folder, const char* name, version_t* list);

// version `number` of a file (1 is the newest) as it was: its bytes, with a '\0' after them so
// a text file can be used as a string, and *size its length. NULL if there is no such version.
// free() the result
char* versions_read (const char* folder, const char* name, int number, uint32_t* size);

// make version `number` the file's contents again, or make the file again if it was deleted.
// What it replaces is kept as a version first. Returns FS_OK or an FS_ERR_ code
// (FS_ERR_NOT_FOUND if there is no such version)
int versions_restore (const char* folder, const char* name, int number);

// "notes.txt@2" -> "notes.txt" and 2. False if the name doesn't end in @ and a number
bool versions_split_name (const char* name, char* base, size_t size, int* number);

// call fn for every file in a home folder that has versions: its path, and how many
// (for VERS on its own). Returns how many files there were
typedef void (*versions_file_fn)(const char* path, int count, void* ctx);
int versions_each_file (const char* home, versions_file_fn fn, void* ctx);

// is a file in this folder versioned? (it is inside a home folder, and not in its versions
// folder). home_out (may be NULL) gets the home folder, "/home/alex/"
bool versions_apply (const char* folder, char* home_out, size_t size);

#endif /* VERSIONS_H */
