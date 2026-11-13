/*
    cmd_files.c - the terminal's commands for files and folders

    Everything you do to a file or a folder at the prompt is here:

        LA, SD               list the current folder, and move to another one
        RD, MKF, WR, MKD     read a file, make a file, add a line to one, make a folder
        DEL                  delete a file, or a folder and everything in it (it asks first)
        VERS, UNDO           a file's old versions, and bringing one back (sys/versions.h)
        ENC, DEC             lock a file with a password, and unlock it again (sys/fcrypt.h)
        EDT                  open a file in the text editor (editor.c)

    The commands check what was typed and say what happened. The work itself is done by the
    file system (sys/f_sys.c), which keeps the files on the disk or, without one, in memory.
    Each command is a row in the table in t_int.c, and commands.h says how the files of the
    terminal fit together.
*/

#include "commands.h"
#include <stdint.h>
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../lib/crypto.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../f_sys.h"
#include "../fcrypt.h"
#include "../ui.h"
#include "../explain.h"
#include "../source.h"
#include "../versions.h"
#include "../accounts/auth.h"
#include "editor.h"


// =================================================================================================
//  Small helpers only these commands use
// =================================================================================================

// print some text, adding a newline at the end unless it already ends with one, so the
// prompt after it always starts on a fresh line
static void print_text_block (const char* text, size_t length) {
    prtf("%s", text);
    bool ends_with_newline = length > 0 && text[length - 1] == '\n';
    if (!ends_with_newline) {
        prtf("\n");
    }
}

// ask "(y/n)" and wait for one key. Prints the answer so it stays on the screen.
// Only y or Y means yes: any other key is a no, the safe answer for "delete everything?"
static bool ask_yes_no (void) {
    int key = getk();
    bool yes = key == 'y' || key == 'Y';
    prtf("%c\n", yes ? 'y' : 'n');
    return yes;
}


// =================================================================================================
//  Encrypted files (how the encryption works: sys/fcrypt.h)
// =================================================================================================

// commands that would break an encrypted file (editing it, adding text to it) call this first.
// Returns true, after saying why, if the file is encrypted
static bool refuse_if_encrypted (const char* path) {
    if (!fcrypt_file_is_encrypted(path)) {
        return false;
    }
    explain_say(PROBLEM_ENCRYPTED, path, "%s is encrypted, %s it first.", path, command_name(cli_decrypt));
    return true;
}

// RD on an encrypted file: ask for the password and show what is inside, leaving the file
// encrypted on the disk
static void read_encrypted_file (const char* path) {
    char password[CLI_PASSWORD_MAX];
    ask_secret("Password: ", password, sizeof(password));

    uint8_t* contents;
    uint32_t length;
    ui_busy_begin("Checking the password"); // turning a password into a key takes a moment
    int result = fcrypt_read_file(path, password, &contents, &length);
    ui_busy_end();
    wipe(password, sizeof(password)); // never leave a password lying around in memory

    if (result != FCRYPT_OK) {
        explain_say(explain_fcrypt_problem(result), path,
                    "Can't read %s: %s", path, fcrypt_strerror(result));
        return;
    }
    print_text_block((char*)contents, length);

    // `contents` is the decrypted text, so it is wiped too: free() only hands the memory back,
    // it doesn't clear it, and the next thing to use that memory could find the secret there
    wipe(contents, length);
    free(contents);
}

// ENC <FILE>: lock a file with a password, asked for twice to catch typing mistakes
void cli_encrypt (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage(NULL);
        return;
    }

    // check the file is there before asking for a password, so nobody types one for nothing
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    bool found = fs_split_path(arguments, folder, sizeof(folder), name, sizeof(name))
                 && fs_file_exists(folder, name);
    if (!found) {
        explain_say(PROBLEM_NOT_FOUND, arguments, "File not found: %s", arguments);
        return;
    }
    // locking it a second time would mean two passwords to get it back, so that is refused
    if (fcrypt_file_is_encrypted(arguments)) {
        prtf("%s is already encrypted.\n", arguments);
        return;
    }

    // the password twice: a typing mistake here would lock the file with a password nobody knows
    char password[CLI_PASSWORD_MAX];
    char again[CLI_PASSWORD_MAX];
    ask_secret("Password: ", password, sizeof(password));
    ask_secret("Again:    ", again, sizeof(again));

    if (scmp(password, again) != 0) {
        prtf("The passwords don't match, nothing was changed.\n");
    } else {
        ui_busy_begin("Encrypting");
        int result = fcrypt_encrypt_file(arguments, password);
        ui_busy_end();

        if (result == FCRYPT_OK) {
            prtf("Encrypted %s. Don't lose the password, there is no way back without it.\n", arguments);
        } else {
            explain_say(explain_fcrypt_problem(result), arguments,
                        "Couldn't encrypt %s: %s", arguments, fcrypt_strerror(result));
        }
    }

    // whatever happened, clear both copies of the password out of memory
    wipe(password, sizeof(password));
    wipe(again, sizeof(again));
}

// DEC <FILE>: turn an encrypted file back into a normal one. It asks for the password once:
// a wrong one changes nothing, the file just stays encrypted
void cli_decrypt (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage(NULL);
        return;
    }
    if (!fcrypt_file_is_encrypted(arguments)) {
        prtf("%s isn't an encrypted file.\n", arguments);
        return;
    }

    char password[CLI_PASSWORD_MAX];
    ask_secret("Password: ", password, sizeof(password));

    ui_busy_begin("Decrypting");
    int result = fcrypt_decrypt_file(arguments, password);
    ui_busy_end();
    wipe(password, sizeof(password));

    if (result == FCRYPT_OK) {
        prtf("Decrypted %s.\n", arguments);
    } else {
        explain_say(explain_fcrypt_problem(result), arguments,
                    "Couldn't decrypt %s: %s", arguments, fcrypt_strerror(result));
    }
}


// =================================================================================================
//  Files and folders
// =================================================================================================

// LA: list the files and folders in the current folder
void cli_list (char* arguments) {
    (void)arguments; // LA takes no arguments, anything typed after it is ignored
    fs_print();
}

// SD <DIR>: set (change) the current folder. The path can be absolute ("/sys") or relative
// ("cache", "..", "."), fs_set_cd() works that out
void cli_set_directory (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. /sys, .. or /");
        return;
    }
    if (fs_set_cd(arguments) != 0) {
        explain_say(PROBLEM_NOT_FOUND, arguments, "Directory not found: %s", arguments);
    }
}

// RD <FILE>: print what is in a file (asking for the password first if it is encrypted)
void cli_read_file (char* arguments) {
    // fs_split_path turns "notes.txt" into the current folder + "notes.txt", and
    // "/sys/conf.aer" into "/sys/" + "conf.aer". It fails for an empty or invalid name
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(arguments, folder, sizeof(folder), name, sizeof(name))) {
        show_usage(NULL);
        return;
    }

    // an encrypted file read as it is would just be scrambled bytes, so it gets its password
    if (fcrypt_file_is_encrypted(arguments)) {
        read_encrypted_file(arguments);
        return;
    }

    // fs_read_f gives back its own copy of the contents, which has to be freed afterwards
    char* contents = fs_read_f(folder, name);
    if (contents == NULL) {
        explain_say(PROBLEM_NOT_FOUND, arguments, "File not found: %s", arguments);
        return;
    }
    print_text_block(contents, slen(contents));
    free(contents);
}

// MKF <FILE> [TEXT]: make a new file, optionally with some text already in it
void cli_make_file (char* arguments) {
    char* text;
    char* file = split_first_word(arguments, &text);

    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        show_usage(NULL);
        return;
    }

    // fails if the file is already there, the folder isn't, or the disk is full
    int result = fs_create_f(folder, name, text);
    if (result != FS_OK) {
        explain_say_fs(result, file, "Could not create %s", file);
    }
}

// WR <FILE> <TEXT>: add a line of text to the end of a file that already exists
void cli_write_file (char* arguments) {
    char* text;
    char* file = split_first_word(arguments, &text);

    // unlike MKF, both the file and the text are needed
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (text[0] == '\0' || !fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        show_usage(NULL);
        return;
    }
    if (refuse_if_encrypted(file)) {
        return; // added text would break the file's tag, and it couldn't be decrypted any more
    }

    // read the file first: to check it is there, and to see whether it is empty
    char* existing = fs_read_f(folder, name);
    if (existing == NULL) {
        explain_say(PROBLEM_NOT_FOUND, file, "File not found: %s", file);
        return;
    }
    bool file_is_empty = existing[0] == '\0';
    free(existing);

    // the text goes on a new line, "\n<text>". An empty file gets just "<text>", so a file
    // never starts with a blank line
    size_t line_size = slen(text) + 2; // room for the '\n' and the '\0'
    char* line = malloc(line_size);
    if (line == NULL) {
        explain_say(PROBLEM_NO_MEMORY, NULL, "Out of memory.");
        return;
    }
    if (file_is_empty) {
        scpy(line, text, line_size);
    } else {
        scpy(line, "\n", line_size);
        scat(line, text, line_size);
    }

    bool append = true; // add to the end rather than replace what is there
    int result = fs_write_f(folder, name, line, append);
    free(line);

    if (result != FS_OK) {
        explain_say_fs(result, file, "Could not write %s", file);
    }
}

// MKD <DIR>: make a new folder
void cli_make_directory (char* arguments) {
    // fs_resolve_dir turns e.g. "cache" (typed in /sys/) into the full path "/sys/cache/".
    // "/" is turned away: it always exists, and has no name of its own
    char path[FS_PATH_MAX];
    bool valid = arguments[0] != '\0'
                 && fs_resolve_dir(arguments, path, sizeof(path))
                 && !seq(path, "/");
    if (!valid) {
        show_usage(NULL);
        return;
    }

    // the new folder's name is the last part of the path:
    //   path "/sys/cache/"  ->  parent "/sys/"  ->  what is left, "cache/"  ->  name "cache"
    char parent[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    fs_parent_path(path, parent, sizeof(parent));
    scpy(name, path + slen(parent), sizeof(name));
    name[slen(name) - 1] = '\0'; // drop the '/' on the end

    int result = fs_create_d(path, name);
    if (result != FS_OK) {
        explain_say_fs(result, path, "Could not create %s", path);
    }
}

// ---- DEL ----------------------------------------------------------------------------------------

// what DEL keeps while it empties a folder: its progress bar, and how many files have gone
typedef struct {
    ui_progress_t bar;
    uint32_t files_done;
} delete_progress_t;

// fs_empty_dir() calls this after each file it deletes, to move DEL's progress bar along.
// `context` is whatever DEL handed to fs_empty_dir: here, a delete_progress_t
static void delete_progress_step (const char* name, void* context) {
    delete_progress_t* progress = context;
    progress->files_done++;
    ui_progress_update(&progress->bar, progress->files_done, name);
}

// fs_list() calls a function for every entry it finds. Here only the number of entries it
// returns is wanted, so this one does nothing
static void ignore_entry (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)name;
    (void)is_dir;
    (void)size;
    (void)context;
}

// DEL on a file: a spinner turns while its clusters are freed (a big file has thousands),
// then "Deleted notes.txt." takes its place
static void delete_file (const char* typed, const char* folder, const char* name) {
    char label[FS_NAME_MAX + 16];
    sfmt(label, sizeof(label), "Deleting %s", name);

    ui_busy_begin(label);
    int result = fs_delete_f(folder, name);
    ui_busy_end();

    if (result == FS_OK) {
        prtf("Deleted %s.\n", name);
    } else {
        explain_say_fs(result, typed, "Can't delete %s", typed);
    }
}

// DEL on a folder: an empty one just goes. One with things in it is emptied first, after
// asking, with a progress bar
static void delete_folder (const char* path) {
    // the same rules fs_delete_d() has, but checked BEFORE anything is emptied:
    // never the top folder, and never the folder you are in (or one it is inside)
    if (seq(path, "/")) {
        prtf("The top folder, /, can't be deleted.\n");
        return;
    }
    // /src/ holds AerOS's own source code, built in rather than kept on the disk, so it can
    // only be read
    if (source_in(path)) {
        explain_say_fs(FS_ERR_READ_ONLY, path, "Can't delete %s", path);
        return;
    }
    // both paths end in '/', so if the current folder starts with the whole of `path` it is
    // that folder or somewhere inside it ("/home/" and "/home/alex/" both start "/home/")
    bool standing_in_it = strncmp(cd, path, slen(path)) == 0;
    if (standing_in_it) {
        explain_say(PROBLEM_IN_USE, path, "Can't delete %s: you are in it (%s somewhere else first).", path,
             command_name(cli_set_directory));
        return;
    }

    // look inside: how many files in all (for the question and the progress bar), and whether
    // there is anything at all (an empty sub folder is something, but not a file)
    ui_busy_begin("Looking inside the folder");
    int file_count = fs_count_files(path);
    int entry_count = fs_list(path, ignore_entry, NULL);
    ui_busy_end();

    // a negative count means the folder couldn't be read: say 0 rather than print "-1 file(s)"
    if (file_count < 0) {
        file_count = 0;
    }

    // something inside: ask first, then empty it with a progress bar
    if (entry_count > 0) {
        prtf("%s has %d file(s) in it. Delete it and everything in it? (y/n) ", path, file_count);
        if (!ask_yes_no()) {
            prtf("Nothing was deleted.\n");
            return;
        }

        // a bar out of 0 makes no sense (only empty sub folders inside), so it is out of 1
        delete_progress_t progress = { { 0, 0, 0 }, 0 };
        uint32_t bar_total = file_count > 0 ? (uint32_t)file_count : 1;
        bool zero_files_first = false; // a normal delete, not the secure wipe
        ui_progress_begin(&progress.bar, bar_total);
        int deleted = fs_empty_dir(path, zero_files_first, delete_progress_step, &progress);
        ui_progress_end(&progress.bar);

        // fs_empty_dir gives back how many files it deleted, or a negative number on a problem
        if (deleted < 0) {
            deleted = 0;
        }
        prtf("Deleted %d file(s).\n", deleted);
    }

    // the folder is empty now, so it can go
    int result = fs_delete_d(path);
    if (result == FS_OK) {
        prtf("Deleted %s.\n", path);
    } else {
        explain_say_fs(result, path, "Can't delete %s", path);
    }
}

// DEL <NAME>: delete a file, or a folder and everything in it.
// A name could be either, so it is tried as a file first, then as a folder
void cli_delete (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage(NULL);
        return;
    }

    // a file? (if a file and a folder share a name, the file goes first)
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    bool is_file = fs_split_path(arguments, folder, sizeof(folder), name, sizeof(name))
                   && fs_file_exists(folder, name);
    if (is_file) {
        delete_file(arguments, folder, name);
        return;
    }

    // a folder? fs_resolve_dir gives its full path, ending in '/'
    char path[FS_PATH_MAX];
    bool is_folder = fs_resolve_dir(arguments, path, sizeof(path)) && fs_dir_exists(path);
    if (is_folder) {
        delete_folder(path);
        return;
    }

    explain_say(PROBLEM_NOT_FOUND, arguments, "Not found: %s", arguments);
}

// ---- VERS and UNDO: a file's past (how versions are kept: sys/versions.h) -----------------------

// versions_each_file() calls this for every file that has versions
static void print_versioned_file (const char* path, int count, void* context) {
    (void)context;
    prtf("  %-48s %2d version%s\n", path, count, count == 1 ? "" : "s");
}

// "10-11-26--14:30:05" (how versions.c keeps the time) -> "10-11-26 14:30:05"
static void readable_time (const char* saved, char* out, size_t size) {
    scpy(out, saved, size);
    char* dashes = strstr(out, "--");
    if (dashes != NULL) {
        // the first '-' becomes the space, and everything after the second one moves back a
        // place over it (the +1 brings the '\0' along). memmove, not memcpy: the two overlap
        dashes[0] = ' ';
        memmove(dashes + 1, dashes + 2, slen(dashes + 2) + 1);
    }
}

// VERS [FILE]: a file's old versions, newest first. On its own: every file in your home folder
// that has any
void cli_versions (char* arguments) {
    // no file given: every file in your home folder that has old versions, one line each
    if (arguments[0] == '\0') {
        char home[FS_PATH_MAX];
        if (!auth_home(auth_user, home, sizeof(home))) {
            return;
        }
        int files = versions_each_file(home, print_versioned_file, NULL);
        if (files == 0) {
            prtf("No file in %s has an old version yet.\n", home);
        } else {
            prtf("%s <FILE> lists a file's versions.\n", command_name(cli_versions));
        }
        return;
    }

    // one file: fill `list` with its versions, newest (number 1) first
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(arguments, folder, sizeof(folder), name, sizeof(name))) {
        show_usage(NULL);
        return;
    }
    version_t list[VERSIONS_KEPT];
    int count = versions_list(folder, name, list);
    if (count == 0) {
        explain_say(PROBLEM_NO_VERSIONS, arguments, "%s has no old versions.", arguments);
        return;
    }

    // a line for each, as "notes.txt@2" so it can be typed straight after RD
    prtf("Versions of %s%s, newest first:\n", folder, name);
    for (int i = 0; i < count; i++) {
        char when[20];
        readable_time(list[i].saved, when, sizeof(when));
        prtf("  %s@%-3d  %s  %8u bytes\n", name, list[i].number, when, list[i].size);
    }
    // the versions outlive the file itself, so a deleted file can still be listed and brought back
    if (!fs_file_exists(folder, name)) {
        prtf("%s was deleted: %s %s brings it back.\n", name, command_name(cli_undo), arguments);
    }
    prtf("%s %s@1 shows one, %s %s 2 brings @2 back.\n",
         command_name(cli_read_file), arguments, command_name(cli_undo), arguments);
}

// UNDO <FILE> [N]: bring back version N of a file (1, the newest, without N). What it replaces
// is kept as a version first, so another UNDO puts it back
void cli_undo (char* arguments) {
    // "notes.txt 2" -> the file and the version number, 1 (the newest) when there isn't one.
    // Something that isn't a number gives 0, which the check below turns away
    char* number_text;
    char* file = split_first_word(arguments, &number_text);
    int number = number_text[0] != '\0' ? atoi(number_text) : 1;

    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (number < 1 || !fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        show_usage("e.g. UNDO notes.txt, or UNDO notes.txt 2 for the one before");
        return;
    }

    // when the version was kept, to say so afterwards
    version_t list[VERSIONS_KEPT];
    int count = versions_list(folder, name, list);
    if (number > count) {
        if (count == 0) {
            explain_say(PROBLEM_NO_VERSIONS, file, "%s has no old versions.", file);
        } else {
            explain_say(PROBLEM_NO_VERSIONS, file, "%s has %d version(s), not %d (%s %s lists them).",
                        file, count, number, command_name(cli_versions), file);
        }
        return;
    }
    // both looked at now, before versions_restore changes them. Version 1 is list[0]
    bool was_there = fs_file_exists(folder, name);
    char when[20];
    readable_time(list[number - 1].saved, when, sizeof(when));

    int result = versions_restore(folder, name, number);
    if (result != FS_OK) {
        explain_say_fs(result, file, "Couldn't bring back %s", file);
        return;
    }
    prtf("%s is back as it was at %s.\n", file, when);
    // a file that had been deleted replaced nothing, so there is nothing to mention
    if (was_there) {
        prtf("(What it replaced is %s@1 now, %s %s puts that back.)\n", file, command_name(cli_undo), file);
    }
}


// =================================================================================================
//  Editing a file
// =================================================================================================

// EDT <FILE>: open a file in the text editor (sys/terminal/editor.c)
void cli_edit (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. hello.aer");
        return;
    }
    if (refuse_if_encrypted(arguments)) {
        return;
    }
    editor_open(arguments);
}
