/*
    aer_builtins.c
    The functions every AerLang program can call: printing and input, strings, lists, byte
    buffers, files, encrypted files, sound, graphics and a few system services.

    Each one gets its arguments as an array of values, which it must not release (the caller
    still owns them), and hands back a new value that the caller will own. A wrong argument is
    reported with aer_error(), which never returns, so the code after a check can trust it.

    Adding a builtin:
      1. write  static value_t bi_<name> (int argc, value_t* argv, int line)  in the right section,
         using int_arg / str_arg / list_arg / file_arg to check and unpack the arguments
      2. add a line to aer_builtins[] at the bottom: { "name", bi_<name>, fewest args, most args }
         (the interpreter checks the argument count before calling it)
      3. add it to docs/AERLANG.md
*/

#include "aer_internal.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_stdio.h"
#include "../lib/aer_ctype.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../drivers/audio/tone.h"
#include "../sys/sys_clck.h"
#include "../sys/f_sys.h"
#include "../sys/fcrypt.h"
#include "../sys/sound.h"
#include "../sys/mixer.h"
#include "../sys/video.h"
#include "../sys/sysinfo.h"
#include "../sys/ui.h"
#include "../sys/p_man.h"
#include "../sys/terminal/t_int.h"
#include "../sys/perm.h"
#include "../lib/crypto.h"

// argument checks

static int32_t int_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_INT) {
        aer_error(line, "%s() needs a number for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.i;
}

static aer_object_t* str_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_STR) {
        aer_error(line, "%s() needs a string for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

static aer_object_t* list_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_LIST) {
        aer_error(line, "%s() needs a list for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

// Split a file's path, given to a function as argument i, into its folder and name, and check
// the program may read (PERM_READ) or change (PERM_CHANGE) it: if it needs to, AerOS asks the
// person at the keyboard first (sys/perm.h). False if the path is no good or the answer is no,
// and then the function fails the same way as for a file that isn't there (nil, or false)
static bool file_arg (value_t* argv, int i, int line, const char* fn, perm_kind_t kind, char* dir, char* name) {
    aer_object_t* path = str_arg(argv, i, line, fn);
    return fs_split_path(path->data.chars, dir, FS_PATH_MAX, name, FS_NAME_MAX)
           && perm_allows(dir, name, kind);
}

// the same for a function that wants the path itself: the path, or NULL if it is no good or
// the program isn't allowed to use it
static const char* allowed_path (value_t* argv, int i, int line, const char* fn, perm_kind_t kind) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    const char* path = str_arg(argv, i, line, fn)->data.chars;
    bool allowed = fs_split_path(path, dir, sizeof(dir), name, sizeof(name)) && perm_allows(dir, name, kind);
    return allowed ? path : NULL;
}

// console

static void print_values (int argc, value_t* argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            console_putchar_raw(' ');
        }
        aer_print_value(argv[i]);
    }
}

// print(a, b, ...): show the values with a space between each, then start a new line
static value_t bi_print (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    console_putchar('\n');
    return aer_nil();
}

// write(a, b, ...): like print(), but stays on the same line
static value_t bi_write (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    console_update_cursor();
    return aer_nil();
}

// input(prompt): show the prompt (if there is one), wait for a line to be typed, and give it back
static value_t bi_input (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) {
        aer_print_value(argv[0]);
    }
    (void)line;
    getln(buffer, sizeof(buffer));
    return aer_string(buffer, (uint32_t)slen(buffer));
}

// like input(), but shows * for each character (for passwords)
static value_t bi_input_secret (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) {
        aer_print_value(argv[0]);
    }
    (void)line;
    GetSecret(buffer, sizeof(buffer));
    value_t s = aer_string(buffer, (uint32_t)slen(buffer));
    wipe(buffer, sizeof(buffer));
    return s;
}

// cls(): clear the screen
static value_t bi_cls (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    console_clear();
    return aer_nil();
}

// color(fg, bg): the colours text is written in from now on (0 to 15, BLACK to WHITE). bg stays as it is if left out
static value_t bi_color (int argc, value_t* argv, int line) {
    int32_t fg = int_arg(argv, 0, line, "color");
    int32_t bg = argc > 1 ? int_arg(argv, 1, line, "color") : (int32_t)console_bg;
    colour((enum color)(fg & 15), (enum color)(bg & 15));
    return aer_nil();
}

// at(x, y): move the cursor, so the next thing written starts at column x, row y
static value_t bi_at (int argc, value_t* argv, int line) {
    (void)argc;
    console_set_cursor((uint8_t)int_arg(argv, 0, line, "at"), (uint8_t)int_arg(argv, 1, line, "at"));
    return aer_nil();
}

// key(): wait for a key and give back its number: a character's code, or KEY_UP, KEY_ENTER...
static value_t bi_key (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_int(getk());
}

// haskey(): is there a key waiting that key() hasn't taken yet? (never waits)
static value_t bi_haskey (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_bool(kb_haschar());
}

// strings and conversions

// len(x): how many characters are in a string, items in a list, or bytes in bytes
static value_t bi_len (int argc, value_t* argv, int line) {
    (void)argc;
    if (argv[0].type != V_STR && argv[0].type != V_LIST && argv[0].type != V_BYTES) {
        aer_error(line, "len() needs a string, list or bytes, not a %s", aer_type_name(argv[0]));
    }
    return aer_int((int32_t)argv[0].as.obj->length);
}

// str(x): any value as a string, the way print() would show it
static value_t bi_str (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    return aer_to_string(argv[0]);
}

// int(x): a string (or true / false) as a number: "42" is 42, "0x1F" is 31. nil if it isn't a number
static value_t bi_int (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    switch (argv[0].type) {
        case V_INT: return argv[0];
        case V_BOOL: return aer_int(argv[0].as.i);
        case V_STR: {
            char* end;
            const char* text = argv[0].as.obj->data.chars;
            long value = strtol(text, &end, 0);
            while (isspace((unsigned char)*end)) {
                end++;
            }
            if (end == text || *end != '\0') {
                return aer_nil(); // not a number
            }
            return aer_int((int32_t)value);
        }
        default: return aer_nil();
    }
}

// chr(n): the character with that code, chr(65) is "A"
static value_t bi_chr (int argc, value_t* argv, int line) {
    (void)argc;
    char c = (char)int_arg(argv, 0, line, "chr");
    return aer_string(&c, 1);
}

// ord(s): the code of a string's first character, ord("A") is 65
static value_t bi_ord (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* s = str_arg(argv, 0, line, "ord");
    if (s->length == 0) {
        aer_error(line, "ord() needs a string with at least one character");
    }
    return aer_int((uint8_t)s->data.chars[0]);
}

// sub(s, start, count): part of a string, count characters from start (0 is the first). Without count, to the end
static value_t bi_sub (int argc, value_t* argv, int line) {
    aer_object_t* s = str_arg(argv, 0, line, "sub");
    int32_t start = int_arg(argv, 1, line, "sub");
    int32_t count = argc > 2 ? int_arg(argv, 2, line, "sub") : (int32_t)s->length;
    if (start < 0) {
        start = 0;
    }
    if ((uint32_t)start > s->length) {
        start = (int32_t)s->length;
    }
    if (count < 0) {
        count = 0;
    }
    if ((uint32_t)count > s->length - (uint32_t)start) {
        count = (int32_t)(s->length - (uint32_t)start);
    }
    return aer_string(s->data.chars + start, (uint32_t)count);
}

// find(s, part): where part first appears in s (0 is the start), or -1 if it doesn't
static value_t bi_find (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* s = str_arg(argv, 0, line, "find");
    aer_object_t* needle = str_arg(argv, 1, line, "find");
    char* found = strstr(s->data.chars, needle->data.chars);
    return aer_int(found ? (int32_t)(found - s->data.chars) : -1);
}

static value_t change_case (value_t* argv, int line, const char* fn, bool upper) {
    aer_object_t* s = str_arg(argv, 0, line, fn);
    value_t result = aer_string(s->data.chars, s->length);
    for (uint32_t i = 0; i < s->length; i++) {
        char* c = &result.as.obj->data.chars[i];
        *c = (char)(upper ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
    }
    return result;
}

// upper(s): a copy of s in capitals
static value_t bi_upper (int argc, value_t* argv, int line) {
    (void)argc;
    return change_case(argv, line, "upper", true);
}

// lower(s): a copy of s in small letters
static value_t bi_lower (int argc, value_t* argv, int line) {
    (void)argc;
    return change_case(argv, line, "lower", false);
}

// split("a,b,c", ",") is ["a", "b", "c"], without a separator it splits on spaces
static value_t bi_split (int argc, value_t* argv, int line) {
    aer_object_t* s = str_arg(argv, 0, line, "split");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "split")->data.chars : 0;
    size_t sep_length = sep ? slen(sep) : 0;
    if (sep && sep_length == 0) {
        aer_error(line, "split() needs a separator that isn't empty");
    }

    value_t list = aer_list(4);
    const char* p = s->data.chars;
    const char* end = p + s->length;
    if (!sep) {
        while (p < end) {
            while (p < end && *p == ' ') {
                p++;
            }
            const char* start = p;
            while (p < end && *p != ' ') {
                p++;
            }
            if (p > start) {
                aer_list_push(list.as.obj, aer_string(start, (uint32_t)(p - start)));
            }
        }
        return list;
    }
    for (;;) {
        const char* hit = strstr(p, sep);
        if (hit == 0) {
            aer_list_push(list.as.obj, aer_string(p, (uint32_t)(end - p)));
            return list;
        }
        aer_list_push(list.as.obj, aer_string(p, (uint32_t)(hit - p)));
        p = hit + sep_length;
    }
}

// join(list, sep): the items as one string, with sep between them (nothing between them without sep)
static value_t bi_join (int argc, value_t* argv, int line) {
    aer_object_t* list = list_arg(argv, 0, line, "join");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "join")->data.chars : "";
    uint32_t sep_length = (uint32_t)slen(sep);

    // turn every item into a string first, so the total length is known
    value_t* pieces = aer_alloc((list->length ? list->length : 1) * sizeof(value_t));
    uint32_t total = 0;
    for (uint32_t i = 0; i < list->length; i++) {
        pieces[i] = aer_to_string(list->data.items[i]);
        total += pieces[i].as.obj->length + (i > 0 ? sep_length : 0);
    }
    value_t result = aer_string("", 0);
    result.as.obj->data.chars = aer_realloc(result.as.obj->data.chars, total + 1);
    char* out = result.as.obj->data.chars;
    for (uint32_t i = 0; i < list->length; i++) {
        if (i > 0) {
            memcpy(out, sep, sep_length);
            out += sep_length;
        }
        memcpy(out, pieces[i].as.obj->data.chars, pieces[i].as.obj->length);
        out += pieces[i].as.obj->length;
        aer_release(pieces[i]);
    }
    *out = '\0';
    result.as.obj->length = total;
    aer_free(pieces);
    return result;
}

// lists and bytes

// list(n, fill): a new list of n items, each one fill (nil without it)
static value_t bi_list (int argc, value_t* argv, int line) {
    int32_t count = int_arg(argv, 0, line, "list");
    if (count < 0 || count > 1000000) {
        aer_error(line, "list() size must be 0 to 1000000");
    }
    value_t fill = argc > 1 ? argv[1] : aer_nil();
    value_t list = aer_list((uint32_t)count);
    for (int32_t i = 0; i < count; i++) {
        aer_retain(fill);
        aer_list_push(list.as.obj, fill);
    }
    return list;
}

// push(list, x): add x to the end of a list
static value_t bi_push (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "push");
    aer_retain(argv[1]);
    aer_list_push(list, argv[1]);
    return aer_nil();
}

// pop(list): take the last item off a list and give it back (nil if the list is empty)
static value_t bi_pop (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "pop");
    if (list->length == 0) {
        return aer_nil();
    }
    return list->data.items[--list->length]; // the list's reference passes to the caller
}

// insert(list, at, x): put x into a list at position at, moving the items after it along one
static value_t bi_insert (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "insert");
    int32_t at = int_arg(argv, 1, line, "insert");
    if (at < 0 || (uint32_t)at > list->length) {
        aer_error(line, "insert() position %d is out of range", at);
    }
    aer_retain(argv[2]);
    aer_list_push(list, aer_nil()); // make room
    memmove(&list->data.items[at + 1], &list->data.items[at], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->data.items[at] = argv[2];
    return aer_nil();
}

// remove(list, at): take the item at position at out of a list, and give it back
static value_t bi_remove (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "remove");
    int32_t at = int_arg(argv, 1, line, "remove");
    if (at < 0 || (uint32_t)at >= list->length) {
        aer_error(line, "remove() position %d is out of range", at);
    }
    value_t item = list->data.items[at];
    memmove(&list->data.items[at], &list->data.items[at + 1], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->length--;
    return item;
}

// bytes(n): n bytes, all 0, for binary data such as pictures and sounds
static value_t bi_bytes (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t length = int_arg(argv, 0, line, "bytes");
    if (length < 0 || length > 64 * 1024 * 1024) {
        aer_error(line, "bytes() size must be 0 to 64MB");
    }
    return aer_bytes((uint32_t)length);
}

// files

// rdf(path): a file's contents as a string, nil if there is no such file
static value_t bi_read_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "rdf", PERM_READ, dir, name)) {
        return aer_nil();
    }
    char* text = fs_read_f(dir, name);
    if (text == 0) {
        return aer_nil();
    }
    value_t s = aer_string(text, (uint32_t)slen(text));
    free(text);
    return s;
}

// rdb(path): a file's contents as bytes (for pictures and sounds), nil if there is no such file
static value_t bi_read_bytes (int argc, value_t* argv, int line) {
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
    value_t b = aer_bytes(size);
    memcpy(b.as.obj->data.bytes, data, size);
    free(data);
    return b;
}

static value_t save_file (value_t* argv, int line, const char* fn, bool append) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!file_arg(argv, 0, line, fn, PERM_CHANGE, dir, name)) {
        return aer_bool(false);
    }
    value_t text = aer_to_string(argv[1]);
    int result;
    if (fs_file_exists(dir, name)) {
        result = fs_write_f(dir, name, text.as.obj->data.chars, append);
    } else {
        result = fs_create_f(dir, name, text.as.obj->data.chars);
    }
    aer_release(text);
    return aer_bool(result >= 0);
}

// wrf(path, text): replace a file's contents with text, making the file if it isn't there. True if it worked
static value_t bi_write_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "wrf", false);
}

// apf(path, text): add text to the end of a file, making the file if it isn't there. True if it worked
static value_t bi_append_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "apf", true);
}

// isf(path): is there a file there?
static value_t bi_file_exists (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "isf", PERM_READ, dir, name)) {
        return aer_bool(false);
    }
    return aer_bool(fs_file_exists(dir, name));
}

// delf(path): delete a file. True if it worked
static value_t bi_delete_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "delf", PERM_CHANGE, dir, name)) {
        return aer_bool(false);
    }
    return aer_bool(fs_delete_f(dir, name) == FS_OK);
}

// add one directory entry to the list, directories get a '/' on the end
static void list_one (const char* name, bool is_dir, uint32_t size, void* ctx) {
    (void)size;
    char entry[FS_NAME_MAX + 2];
    scpy(entry, name, sizeof(entry));
    if (is_dir) {
        scat(entry, "/", sizeof(entry));
    }
    aer_list_push((aer_object_t*)ctx, aer_string(entry, (uint32_t)slen(entry)));
}

// the current directory, e.g. "/bin/"
static value_t bi_cwd (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_string(cd, (uint32_t)slen(cd));
}

// la(folder): the names in a folder (the current one without folder) as a list, folders ending in "/". nil if the folder isn't there
static value_t bi_list_files (int argc, value_t* argv, int line) {
    const char* typed = argc > 0 ? str_arg(argv, 0, line, "la")->data.chars : ".";
    char dir[FS_PATH_MAX];
    if (!fs_resolve_dir(typed, dir, sizeof(dir)) || !perm_allows(dir, "", PERM_READ)) {
        return aer_nil();
    }
    value_t list = aer_list(8);
    if (fs_list(dir, list_one, list.as.obj) < 0) {
        aer_release(list);
        return aer_nil();
    }
    return list;
}

// encrypted files (see sys/fcrypt.h)

// enc(path, password): lock a file with a password (see sys/fcrypt.h). True if it worked
static value_t bi_encrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "enc", PERM_CHANGE);
    const char* password = str_arg(argv, 1, line, "enc")->data.chars;
    return aer_bool(path != NULL && fcrypt_encrypt_file(path, password) == FCRYPT_OK);
}

// dec(path, password): turn an encrypted file back into a normal one. True if it worked
static value_t bi_decrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "dec", PERM_CHANGE);
    const char* password = str_arg(argv, 1, line, "dec")->data.chars;
    return aer_bool(path != NULL && fcrypt_decrypt_file(path, password) == FCRYPT_OK);
}

// isenc(path): is the file encrypted?
static value_t bi_is_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "isenc", PERM_READ);
    return aer_bool(path != NULL && fcrypt_file_is_encrypted(path));
}

// the contents of an encrypted file as a string, without changing the file (nil if it can't be read)
static value_t bi_read_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "rdenc", PERM_READ);
    const char* password = str_arg(argv, 1, line, "rdenc")->data.chars;
    uint8_t* contents;
    uint32_t length;
    if (path == NULL || fcrypt_read_file(path, password, &contents, &length) != FCRYPT_OK) {
        return aer_nil();
    }
    value_t s = aer_string((const char*)contents, length);
    wipe(contents, length);
    free(contents);
    return s;
}

// system

// sleep(ms): wait that many milliseconds (Ctrl+C still stops the program)
static value_t bi_sleep (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t ms = int_arg(argv, 0, line, "sleep");
    // sleep in small steps so ctrl + C still gets noticed
    while (ms > 0 && !kb_take_break()) {
        sleep_ms(ms > 20 ? 20 : (uint32_t)ms);
        ms -= 20;
    }
    if (ms > 0) {
        aer_stop(line);
    }
    return aer_nil();
}

// play() stops on Ctrl+C, and then the program stops too (like sleep)
static bool play_interrupted;

static bool play_should_stop (void) {
    if (kb_take_break()) {
        play_interrupted = true;
    }
    return play_interrupted;
}

// play a WAV or MP3 file and wait for it to finish, false if it couldn't be played
static value_t bi_play (int argc, value_t* argv, int line) {
    (void)argc;
    play_interrupted = false;
    const char* path = allowed_path(argv, 0, line, "play", PERM_READ);
    if (path == NULL) {
        return aer_bool(false);
    }
    int result = sound_play_file(path, play_should_stop);
    if (play_interrupted) {
        aer_stop(line);
    }
    return aer_bool(result == SOUND_OK);
}

// animation (the same drawing the OS uses, see sys/ui.h)

// a screen position from two arguments, checked so nothing is drawn off the screen
static void cell_args (value_t* argv, int line, const char* fn, uint8_t* x, uint8_t* y) {
    int32_t cx = int_arg(argv, 0, line, fn);
    int32_t cy = int_arg(argv, 1, line, fn);
    if (cx < 0 || cx >= COLS || cy < 0 || cy >= ROWS) {
        aer_error(line, "%s() position %d, %d is off the screen (it is %d x %d)", fn, cx, cy, COLS, ROWS);
    }
    *x = (uint8_t)cx;
    *y = (uint8_t)cy;
}

// big(x, y, text, colour, background): text in big letters, like the splash screen's
// "aerOS", with its top left corner at column x, row y. Each letter is 8 columns wide and 7 rows
// tall. The colour is WHITE and the background BLACK if they are left out.
// Returns how many columns wide it is, so it can be centred
static value_t bi_big_text (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "big", &x, &y);
    aer_object_t* text = str_arg(argv, 2, line, "big");

    enum color colour = WHITE;
    if (argc > 3) {
        colour = (enum color)(int_arg(argv, 3, line, "big") & 15); // one of the 16 colours
    }
    enum color background = BLACK;
    if (argc > 4) {
        background = (enum color)(int_arg(argv, 4, line, "big") & 15);
    }
    return aer_int(big_text(x, y, colour, background, text->data.chars));
}

// progress(x, y, width, done, total, colour): [#####.....]  50%
static value_t bi_progress (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "progress", &x, &y);
    int32_t width = int_arg(argv, 2, line, "progress");
    int32_t done = int_arg(argv, 3, line, "progress");
    int32_t total = int_arg(argv, 4, line, "progress");
    int32_t colour = argc > 5 ? int_arg(argv, 5, line, "progress") : LIGHT_GREEN;
    if (width < 1 || width > COLS) {
        aer_error(line, "progress() width must be 1 to %d", COLS);
    }
    // a negative amount counts as none
    if (done < 0) {
        done = 0;
    }
    if (total < 0) {
        total = 0;
    }
    enum color bar_colour = (enum color)(colour & 15); // one of the 16 text colours
    ui_bar(x, y, (uint8_t)width, (uint32_t)done, (uint32_t)total, bar_colour);
    return aer_nil();
}

// spinner(x, y, style, colour): the frame for this moment, returns how many cells it took
static value_t bi_spinner (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "spinner", &x, &y);
    int32_t style = argc > 2 ? int_arg(argv, 2, line, "spinner") : 0;
    int32_t colour = argc > 3 ? int_arg(argv, 3, line, "spinner") : YELLOW;
    if (style < 0 || style >= UI_SPINNER_STYLES) {
        aer_error(line, "spinner() style must be 0 to %d", UI_SPINNER_STYLES - 1);
    }
    return aer_int(ui_spinner(x, y, style, (enum color)(colour & 15)));
}

// when the last frame was due: frame() counts the time to the next one from here
static uint32_t last_frame_ms = 0;

// frame(fps): wait until it's time for the next frame, so a loop runs at a steady speed
// whatever it draws (sleep would add the drawing time on top). Ctrl+C stops the program
static value_t bi_frame (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t fps = int_arg(argv, 0, line, "frame");
    if (fps < 1 || fps > 1000) {
        aer_error(line, "frame() needs 1 to 1000 frames a second");
    }
    uint32_t period = 1000u / (uint32_t)fps;
    // a long gap (the first frame, or after something slow) starts the timing again
    if (clkuptms() - last_frame_ms > period * 4) {
        last_frame_ms = clkuptms();
    }
    while (clkuptms() - last_frame_ms < period) {
        if (kb_take_break()) {
            aer_stop(line);
        }
        sleep_ms(1);
    }
    last_frame_ms += period;
    return aer_nil();
}

// the most cmd() keeps of what a command prints (the rest is dropped)
#define CMD_OUTPUT_MAX (16 * 1024)

// cmd(line): run a terminal command as if it had been typed at the prompt, and give back what it
// printed as a list of lines, instead of showing it: cmd("la"), len(cmd("mem")).
// Only at the prompt, not in a program: programs have to ask before they touch your files,
// and a terminal command wouldn't. The command can't use the keyboard while it runs, so a
// question it asks gets no answer (DEL's "Delete it? (y/n)" is a no)
static value_t bi_cmd (int argc, value_t* argv, int line) {
    (void)argc;
    if (!aer->shell) {
        aer_error(line, "cmd() only works at the prompt, not in a program");
    }
    if (console_capturing()) {
        aer_error(line, "cmd() can't be used inside another cmd()");
    }
    const char* command = str_arg(argv, 0, line, "cmd")->data.chars;
    char* output = aer_alloc(CMD_OUTPUT_MAX);
    cli_capture(command, output, CMD_OUTPUT_MAX);

    // one string per line ('\r' and the '\n' left off), with no empty line after the last one
    value_t lines = aer_list(8);
    const char* p = output;
    while (*p != '\0') {
        size_t length = strcspn(p, "\n");
        size_t text_length = length > 0 && p[length - 1] == '\r' ? length - 1 : length;
        aer_list_push(lines.as.obj, aer_string(p, (uint32_t)text_length));
        p += length;
        if (*p == '\n') {
            p++;
        }
    }
    aer_free(output);
    return lines;
}

// queue("hello.aer a b"): put a program (and its arguments) at the end of the queue the GO
// command runs, or "s:text" to print a line between programs. Returns how many are now queued,
// or -1 if the process stack is full. The line isn't checked until it runs
static value_t bi_queue (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* code = str_arg(argv, 0, line, "queue");
    if (code->length == 0) {
        aer_error(line, "queue() needs a program to queue");
    }
    return aer_int(p_queue_add(code->data.chars));
}

// add one row of system information to the list as [group, name, value]
static void info_row (const char* group, const char* name, const char* value, void* ctx) {
    value_t row = aer_list(3);
    aer_list_push(row.as.obj, aer_string(group, (uint32_t)slen(group)));
    aer_list_push(row.as.obj, aer_string(name, (uint32_t)slen(name)));
    aer_list_push(row.as.obj, aer_string(value, (uint32_t)slen(value)));
    aer_list_push((aer_object_t*)ctx, row);
}

// everything AerOS knows about the computer, as a list of [group, name, value] (see sys/sysinfo.h)
static value_t bi_system_info (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    value_t rows = aer_list(64);
    sysinfo_collect(info_row, rows.as.obj);
    return rows;
}

// time(): how many milliseconds AerOS has been running, for timing things
static value_t bi_time (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_int((int32_t)clkuptms());
}

// random(n): a random whole number from 0 to n - 1
static value_t bi_random (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t range = int_arg(argv, 0, line, "random");
    if (range <= 0) {
        aer_error(line, "random() needs a number above 0");
    }
    return aer_int(rand() % range);
}

// beep(hz): a beep on the PC speaker, at hz if given (the standard beep without it)
static value_t bi_beep (int argc, value_t* argv, int line) {
    beep(argc > 0 ? int_arg(argv, 0, line, "beep") : 0);
    return aer_nil();
}

// graphics

static void need_graphics (int line, const char* fn) {
    if (!aer->graphics) {
        aer_error(line, "%s() needs graphics, call gopen() first", fn);
    }
}

// gopen(): take the screen over for graphics. False if there is only the text screen
static value_t bi_gfx_open (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (!aer->graphics) {
        aer->graphics = gfx_begin();
    }
    return aer_bool(aer->graphics);
}

// gclose(): give the screen back to the terminal (a program that ends does this by itself)
static value_t bi_gfx_close (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
    return aer_nil();
}

// gw(): the width of the graphics screen in pixels
static value_t bi_gfx_width (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gw");
    return aer_int((int32_t)gfx_width());
}

// gh(): the height of the graphics screen in pixels
static value_t bi_gfx_height (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gh");
    return aer_int((int32_t)gfx_height());
}

// rgb(r, g, b): a colour for the graphics functions, mixed from red, green and blue (each 0 to 255)
static value_t bi_rgb (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t r = int_arg(argv, 0, line, "rgb") & 0xFF;
    int32_t g = int_arg(argv, 1, line, "rgb") & 0xFF;
    int32_t b = int_arg(argv, 2, line, "rgb") & 0xFF;
    return aer_int((r << 16) | (g << 8) | b);
}

// gclear(colour): fill the whole screen with a colour (black without one)
static value_t bi_gfx_clear (int argc, value_t* argv, int line) {
    need_graphics(line, "gclear");
    gfx_clear(argc > 0 ? (uint32_t)int_arg(argv, 0, line, "gclear") : 0);
    return aer_nil();
}

// pixel(x, y, colour): colour one pixel
static value_t bi_pixel (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "pixel");
    gfx_pixel(int_arg(argv, 0, line, "pixel"), int_arg(argv, 1, line, "pixel"), (uint32_t)int_arg(argv, 2, line, "pixel"));
    return aer_nil();
}

// rect(x, y, w, h, colour): a filled rectangle, w by h pixels, with its top left corner at x, y
static value_t bi_rect (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "rect");
    gfx_rect(int_arg(argv, 0, line, "rect"), int_arg(argv, 1, line, "rect"), int_arg(argv, 2, line, "rect"),
             int_arg(argv, 3, line, "rect"), (uint32_t)int_arg(argv, 4, line, "rect"));
    return aer_nil();
}

// line(x1, y1, x2, y2, colour): a straight line from one point to the other
static value_t bi_line (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "line");
    gfx_line(int_arg(argv, 0, line, "line"), int_arg(argv, 1, line, "line"), int_arg(argv, 2, line, "line"),
             int_arg(argv, 3, line, "line"), (uint32_t)int_arg(argv, 4, line, "line"));
    return aer_nil();
}

// text(x, y, s, colour, scale): write s at x, y, every letter scale times its normal size (1 without scale)
static value_t bi_text (int argc, value_t* argv, int line) {
    need_graphics(line, "text");
    value_t s = aer_to_string(argv[2]);
    int scale = argc > 4 ? int_arg(argv, 4, line, "text") : 1;
    gfx_text(int_arg(argv, 0, line, "text"), int_arg(argv, 1, line, "text"), s.as.obj->data.chars,
             (uint32_t)int_arg(argv, 3, line, "text"), scale);
    aer_release(s);
    return aer_nil();
}

static aer_object_t* bytes_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_BYTES) {
        aer_error(line, "%s() needs bytes (from rdb) for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

// image(data, x, y, w, h): draw a BMP picture (bytes from rdb) at x, y, shrunk to fit w x h if given. False if it isn't one
static value_t bi_image (int argc, value_t* argv, int line) {
    need_graphics(line, "image");
    aer_object_t* data = bytes_arg(argv, 0, line, "image");
    int max_w = argc > 3 ? int_arg(argv, 3, line, "image") : 0;
    int max_h = argc > 4 ? int_arg(argv, 4, line, "image") : 0;
    return aer_bool(gfx_image(data->data.bytes, data->length, int_arg(argv, 1, line, "image"),
                              int_arg(argv, 2, line, "image"), max_w, max_h));
}

// imgsz(data): a BMP picture's size as [width, height], nil if it isn't one
static value_t bi_image_size (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* data = bytes_arg(argv, 0, line, "imgsz");
    int w, h;
    if (!gfx_image_size(data->data.bytes, data->length, &w, &h)) {
        return aer_nil();
    }
    value_t size = aer_list(2);
    aer_list_push(size.as.obj, aer_int(w));
    aer_list_push(size.as.obj, aer_int(h));
    return size;
}

// video(path): play an .mpg video full screen with its sound, until it ends or Esc is pressed
// (Space pauses, Left / Right skip 5 seconds). Gives back true if it played. A video takes the
// whole screen and hands it back to the terminal at the end, so a program drawing graphics calls
// gopen() again afterwards
static value_t bi_video (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = allowed_path(argv, 0, line, "video", PERM_READ);
    if (path == NULL) {
        return aer_bool(false);
    }
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
    return aer_bool(video_play_file(path) == VIDEO_OK);
}

// ---- for games: smooth drawing, held keys, and sounds that mix --------------------------------

// gbuf(on): true draws everything into a back buffer, out of sight, until gshow() puts the
// finished frame on the screen (no flicker); false draws straight onto the screen again.
// Returns false if there isn't enough memory for the back buffer
static value_t bi_gbuf (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "gbuf");
    return aer_bool(gfx_buffered(aer_truthy(argv[0])));
}

// gshow(): put the frame drawn in the back buffer on the screen
static value_t bi_gshow (int argc, value_t* argv, int line) {
    (void)argc;
    (void)argv;
    need_graphics(line, "gshow");
    gfx_show();
    return aer_nil();
}

// keydown(key): is this key held down right now? key is a character ("a", " ", or ord("a"))
// or one of the KEY_ constants (KEY_LEFT, KEY_SHIFT...). Unlike key(), it doesn't wait, and
// several keys can be down at once
static value_t bi_keydown (int argc, value_t* argv, int line) {
    (void)argc;
    int key;
    if (argv[0].type == V_STR) {
        if (argv[0].as.obj->length == 0) {
            aer_error(line, "keydown() needs a key, not an empty string");
        }
        key = (uint8_t)argv[0].as.obj->data.chars[0];
    } else {
        key = int_arg(argv, 0, line, "keydown");
    }
    return aer_bool(kb_is_down(key));
}

// sload(path): load a WAV or MP3 file, ready for splay(). Gives back the sound (as bytes), or
// nil if it can't be loaded
static value_t bi_sload (int argc, value_t* argv, int line) {
    (void)argc;
    int16_t* frames;
    uint32_t count;
    const char* path = allowed_path(argv, 0, line, "sload", PERM_READ);
    if (path == NULL || sound_load(path, &frames, &count) != SOUND_OK) {
        return aer_nil();
    }
    uint32_t size = count * 4; // stereo, 2 bytes a sample
    value_t sound = aer_bytes(size);
    memcpy(sound.as.obj->data.bytes, frames, size);
    free(frames);
    return sound;
}

// splay(sound, loop, volume): start playing a sound from sload(), and carry on straight away.
// loop (false if left out) plays it round and round; volume runs from 0 to 100 (100 if left
// out). Gives back a number for sstop() and isply(), or -1 if no sound could be started (no
// sound card, or 8 sounds already playing)
static value_t bi_splay (int argc, value_t* argv, int line) {
    aer_object_t* sound = bytes_arg(argv, 0, line, "splay");
    bool loop = argc > 1 && aer_truthy(argv[1]);
    int32_t percent = 100;
    if (argc > 2) {
        percent = int_arg(argv, 2, line, "splay");
    }
    int volume = (int)(percent * MIXER_FULL_VOLUME / 100);

    // The mixer reads the sound's bytes while it plays, so they must not be freed meanwhile,
    // even if the program lets go of the sound. Holding an extra reference keeps them; they
    // are freed with everything else when the program ends
    aer_retain(argv[0]);
    int voice = mixer_play((const int16_t*)sound->data.bytes, sound->length / 4, loop, volume);
    return aer_int(voice);
}

// sstop(voice): stop a sound splay() started. sstop() on its own stops them all
static value_t bi_sstop (int argc, value_t* argv, int line) {
    if (argc == 0) {
        mixer_stop_all();
        return aer_nil();
    }
    mixer_stop(int_arg(argv, 0, line, "sstop"));
    return aer_nil();
}

// isply(voice): is that sound still playing?
static value_t bi_isply (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_bool(mixer_playing(int_arg(argv, 0, line, "isply")));
}

// exit(): end the program here, quietly
static value_t bi_exit (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    // leaving by the error route, but quietly: there is nothing wrong
    (void)line;
    longjmp(aer->error, 2);
}

// One row of the table: the name programs call it by, the C function that does it, and how
// many arguments it takes (fewest, most). #fn turns the function into its name as a string,
// "bi_print", so WHY and SRC can find the C code behind an AerLang function
#define BUILTIN(name, fn, min_args, max_args) { name, fn, min_args, max_args, #fn }

const aer_builtin_t aer_builtins[] = {
    BUILTIN("print", bi_print, 0, AER_MAX_ARGS),
    BUILTIN("write", bi_write, 0, AER_MAX_ARGS),
    BUILTIN("input", bi_input, 0, 1),
    BUILTIN("inpw", bi_input_secret, 0, 1),
    BUILTIN("cls", bi_cls, 0, 0),
    BUILTIN("color", bi_color, 1, 2),
    BUILTIN("at", bi_at, 2, 2),
    BUILTIN("key", bi_key, 0, 0),
    BUILTIN("haskey", bi_haskey, 0, 0),
    BUILTIN("len", bi_len, 1, 1),
    BUILTIN("str", bi_str, 1, 1),
    BUILTIN("int", bi_int, 1, 1),
    BUILTIN("chr", bi_chr, 1, 1),
    BUILTIN("ord", bi_ord, 1, 1),
    BUILTIN("sub", bi_sub, 2, 3),
    BUILTIN("find", bi_find, 2, 2),
    BUILTIN("upper", bi_upper, 1, 1),
    BUILTIN("lower", bi_lower, 1, 1),
    BUILTIN("split", bi_split, 1, 2),
    BUILTIN("join", bi_join, 1, 2),
    BUILTIN("list", bi_list, 1, 2),
    BUILTIN("push", bi_push, 2, 2),
    BUILTIN("pop", bi_pop, 1, 1),
    BUILTIN("insert", bi_insert, 3, 3),
    BUILTIN("remove", bi_remove, 2, 2),
    BUILTIN("bytes", bi_bytes, 1, 1),
    BUILTIN("rdf", bi_read_file, 1, 1),
    BUILTIN("rdb", bi_read_bytes, 1, 1),
    BUILTIN("wrf", bi_write_file, 2, 2),
    BUILTIN("apf", bi_append_file, 2, 2),
    BUILTIN("isf", bi_file_exists, 1, 1),
    BUILTIN("delf", bi_delete_file, 1, 1),
    BUILTIN("enc", bi_encrypt_file, 2, 2),
    BUILTIN("dec", bi_decrypt_file, 2, 2),
    BUILTIN("isenc", bi_is_encrypted, 1, 1),
    BUILTIN("rdenc", bi_read_encrypted, 2, 2),
    BUILTIN("la", bi_list_files, 0, 1),
    BUILTIN("cwd", bi_cwd, 0, 0),
    BUILTIN("sleep", bi_sleep, 1, 1),
    BUILTIN("time", bi_time, 0, 0),
    BUILTIN("sysinfo", bi_system_info, 0, 0),
    BUILTIN("big", bi_big_text, 3, 5),
    BUILTIN("progress", bi_progress, 5, 6),
    BUILTIN("spinner", bi_spinner, 2, 4),
    BUILTIN("frame", bi_frame, 1, 1),
    BUILTIN("queue", bi_queue, 1, 1),
    BUILTIN("cmd", bi_cmd, 1, 1),
    BUILTIN("random", bi_random, 1, 1),
    BUILTIN("beep", bi_beep, 0, 1),
    BUILTIN("play", bi_play, 1, 1),
    BUILTIN("exit", bi_exit, 0, 0),
    BUILTIN("gopen", bi_gfx_open, 0, 0),
    BUILTIN("gclose", bi_gfx_close, 0, 0),
    BUILTIN("gw", bi_gfx_width, 0, 0),
    BUILTIN("gh", bi_gfx_height, 0, 0),
    BUILTIN("gclear", bi_gfx_clear, 0, 1),
    BUILTIN("rgb", bi_rgb, 3, 3),
    BUILTIN("pixel", bi_pixel, 3, 3),
    BUILTIN("rect", bi_rect, 5, 5),
    BUILTIN("line", bi_line, 5, 5),
    BUILTIN("text", bi_text, 4, 5),
    BUILTIN("image", bi_image, 3, 5),
    BUILTIN("imgsz", bi_image_size, 1, 1),
    BUILTIN("gbuf", bi_gbuf, 1, 1),
    BUILTIN("gshow", bi_gshow, 0, 0),
    BUILTIN("keydown", bi_keydown, 1, 1),
    BUILTIN("sload", bi_sload, 1, 1),
    BUILTIN("splay", bi_splay, 1, 3),
    BUILTIN("sstop", bi_sstop, 0, 1),
    BUILTIN("isply", bi_isply, 1, 1),
    BUILTIN("video", bi_video, 1, 1),
};

const int aer_builtin_count = sizeof(aer_builtins) / sizeof(aer_builtins[0]);

const char* aer_builtin_function (const char* name) {
    for (int i = 0; i < aer_builtin_count; i++) {
        if (strcasecmp(aer_builtins[i].name, name) == 0) {
            return aer_builtins[i].function;
        }
    }
    return NULL;
}

void aer_define_constants (void) {
    static const struct { const char* name; int32_t value; } constants[] = {
        { "KEY_UP", KEY_UP }, { "KEY_DOWN", KEY_DOWN }, { "KEY_LEFT", KEY_LEFT },
        { "KEY_RIGHT", KEY_RIGHT }, { "KEY_HOME", KEY_HOME }, { "KEY_END", KEY_END },
        { "KEY_DELETE", KEY_DELETE }, { "KEY_ESCAPE", KEY_ESCAPE }, { "KEY_ENTER", '\n' },
        { "KEY_BACKSPACE", '\b' }, { "KEY_TAB", '\t' }, { "KEY_PGUP", KEY_PGUP }, { "KEY_PGDN", KEY_PGDN },
        { "KEY_SHIFT", KEY_SHIFT }, { "KEY_CTRL", KEY_CTRL }, { "KEY_ALT", KEY_ALT }, { "KEY_SPACE", ' ' },
        { "BLACK", BLACK }, { "DARK_BLUE", DARK_BLUE }, { "GREEN", GREEN }, { "CYAN", CYAN },
        { "RED", RED }, { "PURPLE", PURPLE }, { "ORANGE", ORANGE }, { "BROWN", BROWN }, { "BRIGHT", BRIGHT },
        { "DARK_GRAY", DARK_GRAY }, { "LIGHT_BLUE", LIGHT_BLUE }, { "LIGHT_GREEN", LIGHT_GREEN },
        { "LIGHT_CYAN", LIGHT_CYAN }, { "LIGHT_RED", LIGHT_RED }, { "LIGHT_PURPLE", LIGHT_PURPLE },
        { "YELLOW", YELLOW }, { "WHITE", WHITE },
    };
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++) {
        aer_define_global(constants[i].name, aer_int(constants[i].value));
    }
    srand(clkuptms()); // a different random() sequence every run
}

// code that switched to graphics (gopen) and didn't switch back gives the screen back
void aer_give_screen_back (void) {
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
}

// a program that ends (or crashes) in graphics mode gets the terminal back automatically
void aer_builtins_finish (void) {
    mixer_stop_all(); // sounds from splay() stop with the program (their memory is about to go)
    aer_give_screen_back();
}
