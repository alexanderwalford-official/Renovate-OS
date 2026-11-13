/*
    bi_files.c
    AerLang's built-in functions for files: reading and writing them (rdf, rdb, wrf, apf), seeing
    whether one is there (isf), deleting one (delf), listing a folder (la), the current folder
    (cwd), and encrypted files (enc, dec, isenc, rdenc, see sys/fcrypt.h).

    Every path a program gives goes through file_arg() or allowed_path() (bi_args.c) first, which
    ask the person at the keyboard if the program needs permission to use it (sys/perm.h). A
    refusal makes the function fail the same way as a file that isn't there.

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_stdio.h"
#include "../lib/crypto.h"
#include "../sys/f_sys.h"
#include "../sys/fcrypt.h"

// rdf(path): a file's contents as a string, nil if there is no such file
value_t bi_read_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "rdf", PERM_READ, dir, name)) {
        return aer_nil();
    }
    char* text = fs_read_f(dir, name);
    if (text == 0) {
        return aer_nil();
    }
    // the file system hands back kernel memory (malloc), so copy it into an AerLang string and
    // free the original straight away rather than leaving it to the end of the program
    value_t s = aer_string(text, (uint32_t)slen(text));
    free(text);
    return s;
}

// rdb(path): a file's contents as bytes (for pictures and sounds), nil if there is no such file
value_t bi_read_bytes (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    (void)argc;
    if (!file_arg(argv, 0, line, "rdb", PERM_READ, dir, name)) {
        return aer_nil();
    }
    char* data = fs_read_bytes(dir, name, &size);
    if (data == 0) {
        return aer_nil();
    }
    // copied for the same reason as in rdf(). By size, not up to a '\0', as binary data can hold zeros
    value_t b = aer_bytes(size);
    memcpy(b.as.obj->data.bytes, data, size);
    free(data);
    return b;
}

// wrf() and apf(): write argv[1] to the file named by argv[0], replacing what is there or adding
// to the end (append). The file is made if it isn't there. fn is the AerLang name, for errors
static value_t save_file (value_t* argv, int line, const char* fn, bool append) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!file_arg(argv, 0, line, fn, PERM_CHANGE, dir, name)) {
        return aer_bool(false);
    }
    // any value can be saved, a number or a list is written the way print() would show it.
    // This makes a new string we own, so it is released below
    value_t text = aer_to_string(argv[1]);
    int result;
    // writing needs a file that is already there, so a new one is created instead
    if (fs_file_exists(dir, name)) {
        result = fs_write_f(dir, name, text.as.obj->data.chars, append);
    } else {
        result = fs_create_f(dir, name, text.as.obj->data.chars);
    }
    aer_release(text);
    // the file system's errors are all negative (FS_ERR_...)
    return aer_bool(result >= 0);
}

// wrf(path, text): replace a file's contents with text, making the file if it isn't there. True if it worked
value_t bi_write_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "wrf", false);
}

// apf(path, text): add text to the end of a file, making the file if it isn't there. True if it worked
value_t bi_append_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "apf", true);
}

// ---- part of a file at a time (for files changed a piece at a time, like a database's pages) ----

// rdat(path, offset, count): count bytes of a file from byte `offset` on, as bytes (fewer at the
// end of the file, none past it). nil if the file isn't there
value_t bi_read_at (int argc, value_t* argv, int line) {
    (void)argc;
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!file_arg(argv, 0, line, "rdat", PERM_READ, dir, name)) {
        return aer_nil();
    }
    int32_t offset = int_arg(argv, 1, line, "rdat");
    int32_t count = int_arg(argv, 2, line, "rdat");
    if (offset < 0 || count < 0) {
        aer_error(line, "rdat() needs an offset and a count of 0 or more");
    }
    // fs_open wants the path in one piece
    char path[FS_PATH_MAX + FS_NAME_MAX];
    sfmt(path, sizeof(path), "%s%s", dir, name);
    fs_file_t file;
    if (!fs_open(path, &file)) {
        return aer_nil();
    }
    // no more than is there: the file's end limits the count (so a huge count can't use up memory)
    uint32_t left = (uint32_t)offset < file.size ? file.size - (uint32_t)offset : 0;
    uint32_t wanted = (uint32_t)count < left ? (uint32_t)count : left;
    value_t b = aer_bytes(wanted);
    int32_t got = wanted > 0 ? fs_read_at(&file, (uint32_t)offset, b.as.obj->data.bytes, wanted) : 0;
    if (got < 0) {
        aer_release(b);
        return aer_nil(); // a disk error
    }
    b.as.obj->length = (uint32_t)got;
    return b;
}

// wrat(path, offset, data): write data (bytes, or a string) into a file at byte `offset`, in
// place: the rest of the file stays as it is. Past the end makes the file longer (a gap is
// zeros). Makes the file if it isn't there. True if it worked. Keeps no old version (see fs_write_at)
value_t bi_write_at (int argc, value_t* argv, int line) {
    (void)argc;
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!file_arg(argv, 0, line, "wrat", PERM_CHANGE, dir, name)) {
        return aer_bool(false);
    }
    int32_t offset = int_arg(argv, 1, line, "wrat");
    if (offset < 0) {
        aer_error(line, "wrat() needs an offset of 0 or more");
    }
    // bytes are written as they are; anything else the way print() would show it (a new
    // string we own, released below)
    value_t text = argv[2].type == V_BYTES ? argv[2] : aer_to_string(argv[2]);
    const void* data = argv[2].type == V_BYTES ? (const void*)text.as.obj->data.bytes
                                               : (const void*)text.as.obj->data.chars;
    uint32_t length = text.as.obj->length;

    int result = FS_OK;
    if (!fs_file_exists(dir, name)) {
        result = fs_create_bytes(dir, name, "", 0);
    }
    if (result == FS_OK) {
        result = fs_write_at(dir, name, (uint32_t)offset, data, length);
    }
    if (argv[2].type != V_BYTES) {
        aer_release(text);
    }
    return aer_bool(result == FS_OK);
}

// trunc(path, size): make a file size bytes long, cutting off its end or adding zeros. True if it worked
value_t bi_truncate (int argc, value_t* argv, int line) {
    (void)argc;
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!file_arg(argv, 0, line, "trunc", PERM_CHANGE, dir, name)) {
        return aer_bool(false);
    }
    int32_t size = int_arg(argv, 1, line, "trunc");
    if (size < 0) {
        aer_error(line, "trunc() needs a size of 0 or more");
    }
    return aer_bool(fs_truncate(dir, name, (uint32_t)size) == FS_OK);
}

// isf(path): is there a file there?
value_t bi_file_exists (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "isf", PERM_READ, dir, name)) {
        return aer_bool(false);
    }
    return aer_bool(fs_file_exists(dir, name));
}

// delf(path): delete a file. True if it worked
value_t bi_delete_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "delf", PERM_CHANGE, dir, name)) {
        return aer_bool(false);
    }
    return aer_bool(fs_delete_f(dir, name) == FS_OK);
}

// add one directory entry to the list, directories get a '/' on the end
// fs_list calls this once for every entry, ctx is the list that la() made
static void list_one (const char* name, bool is_dir, uint32_t size, void* ctx) {
    (void)size;
    // + 2: room for the '/' and the '\0' after the longest name
    char entry[FS_NAME_MAX + 2];
    scpy(entry, name, sizeof(entry));
    if (is_dir) {
        scat(entry, "/", sizeof(entry));
    }
    aer_list_push((aer_object_t*)ctx, aer_string(entry, (uint32_t)slen(entry)));
}

// cwd(): the current folder, e.g. "/bin/"
value_t bi_cwd (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    // cd is the terminal's current folder (the one CD changes)
    return aer_string(cd, (uint32_t)slen(cd));
}

// la(folder): the names in a folder (the current one without folder) as a list, folders ending in "/". nil if the folder isn't there
value_t bi_list_files (int argc, value_t* argv, int line) {
    // "." is the current folder, the same as typing LA with nothing after it
    const char* typed = argc > 0 ? str_arg(argv, 0, line, "la")->data.chars : ".";
    char dir[FS_PATH_MAX];
    // turn what was typed into a full folder path, then check the program may look in it
    // (an empty name asks about the folder itself rather than a file in it)
    if (!fs_resolve_dir(typed, dir, sizeof(dir)) || !perm_allows(dir, "", PERM_READ)) {
        return aer_nil();
    }
    value_t list = aer_list(8);
    if (fs_list(dir, list_one, list.as.obj) < 0) {
        // the folder couldn't be listed after all: let go of the half made list and say nil
        aer_release(list);
        return aer_nil();
    }
    return list;
}

// encrypted files (see sys/fcrypt.h)

// enc(path, password): lock a file with a password (see sys/fcrypt.h). True if it worked
value_t bi_encrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "enc", PERM_CHANGE);
    const char* password = str_arg(argv, 1, line, "enc")->data.chars;
    // a refused path (NULL) stops the && before anything is encrypted
    return aer_bool(path != NULL && fcrypt_encrypt_file(path, password) == FCRYPT_OK);
}

// dec(path, password): turn an encrypted file back into a normal one. True if it worked
value_t bi_decrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "dec", PERM_CHANGE);
    const char* password = str_arg(argv, 1, line, "dec")->data.chars;
    return aer_bool(path != NULL && fcrypt_decrypt_file(path, password) == FCRYPT_OK);
}

// isenc(path): is the file encrypted?
value_t bi_is_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "isenc", PERM_READ);
    return aer_bool(path != NULL && fcrypt_file_is_encrypted(path));
}

// rdenc(path, password): the contents of an encrypted file as a string, without changing the file
// (nil if it can't be read, or the password is wrong)
value_t bi_read_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "rdenc", PERM_READ);
    const char* password = str_arg(argv, 1, line, "rdenc")->data.chars;
    uint8_t* contents;
    uint32_t length;
    if (path == NULL || fcrypt_read_file(path, password, &contents, &length) != FCRYPT_OK) {
        return aer_nil();
    }
    value_t s = aer_string((const char*)contents, length);
    // blank the decrypted copy before freeing it, so the secret text isn't left behind in free memory
    wipe(contents, length);
    free(contents);
    return s;
}
