/*
    perm.c - programs ask before they touch your files (see perm.h for the rules)

    perm_allows() goes through the rules in order, and stops at the first that decides:

        1. no program running (the terminal, code at the prompt)     allowed
        2. /acc/ (the accounts)                                      refused
        3. reading the system's files, the program's own, or a
           file named after it on the command line                   allowed
        4. answered already while this program runs (Y or N)         that answer
        5. answered "always" before (kept in /acc/perms)             allowed
        6. otherwise                                                 ask

    /acc/perms holds one line for each "always", e.g.

        alex /bin/viewer.aer read /home/alex/
        alex /home/alex/game.aer change /home/alex/

    who answered, the program, what it may do, and where. /acc/ is the one folder no program
    can ever touch, so a program can't give itself permission by writing in there.
*/

#include "perm.h"
#include "f_sys.h"
#include "accounts/auth.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../lib/primary_definitions.h"

#define PERM_FILE_FOLDER "/acc/"
#define PERM_FILE_NAME   "perms"
#define ARGS_MAX 128           // the most of a program's arguments kept for rule 3
#define DECISIONS_MAX 16       // answers remembered while one program runs

// the program that is running (rule 1: none, while running is false)
static struct {
    bool running;
    char folder[FS_PATH_MAX];      // where its file is, "/bin/"
    char name[FS_NAME_MAX];        // and its name, "viewer.aer"
    char args[ARGS_MAX];           // what was typed after it
} program;

// the Y and N answers given while it runs (rule 4)
static struct {
    char folder[FS_PATH_MAX];
    perm_kind_t kind;
    bool allowed;
} decisions[DECISIONS_MAX];
static int decision_count = 0;

void perm_program_started (const char* folder, const char* name, const char* args) {
    program.running = true;
    scpy(program.folder, folder, sizeof(program.folder));
    scpy(program.name, name, sizeof(program.name));
    scpy(program.args, args != NULL ? args : "", sizeof(program.args));
    decision_count = 0;
}

void perm_program_ended (void) {
    program.running = false;
    decision_count = 0;
}

// the words "read" and "change", as they are kept in /acc/perms and shown in questions
static const char* kind_word (perm_kind_t kind) {
    return kind == PERM_CHANGE ? "change" : "read";
}

// does a permission for `given` cover `wanted`? (being allowed to change includes reading)
static bool covers (perm_kind_t given, perm_kind_t wanted) {
    return given == wanted || (given == PERM_CHANGE && wanted == PERM_READ);
}


// =================================================================================================
//  Rule 3: what can be read without asking
// =================================================================================================

static bool system_folder (const char* folder) {
    static const char* const free_to_read[] = { "/bin/", "/docs/", "/src/", "/sys/" };
    for (size_t i = 0; i < sizeof(free_to_read) / sizeof(free_to_read[0]); i++) {
        const char* system = free_to_read[i];
        if (strncasecmp(folder, system, slen(system)) == 0) {
            return true;
        }
    }
    return false;
}

// Was this file (or folder) named on the command line, after the program's name? Each word is
// tried as a file ("notes.txt" -> this folder and that name) and as a folder ("pics" -> every
// file in /home/alex/pics/)
static bool named_by_you (const char* folder, const char* name) {
    char words[ARGS_MAX];
    scpy(words, program.args, sizeof(words));
    char* save = NULL;
    for (char* word = strtok_r(words, " ", &save); word != NULL; word = strtok_r(NULL, " ", &save)) {
        char word_folder[FS_PATH_MAX];
        char word_name[FS_NAME_MAX];
        bool same_file = fs_split_path(word, word_folder, sizeof(word_folder), word_name, sizeof(word_name))
                         && strcasecmp(word_folder, folder) == 0 && strcasecmp(word_name, name) == 0;
        bool same_folder = fs_resolve_dir(word, word_folder, sizeof(word_folder))
                           && strcasecmp(word_folder, folder) == 0;
        if (same_file || same_folder) {
            return true;
        }
    }
    return false;
}


// =================================================================================================
//  Rule 5: the "always" answers, in /acc/perms
// =================================================================================================

// one line of /acc/perms, taken apart (it is changed in place: spaces become '\0's)
typedef struct {
    const char* user;
    const char* program;
    perm_kind_t kind;
    const char* folder;
} entry_t;

// "alex /bin/viewer.aer read /home/alex/" -> its four parts. False if it doesn't have them
static bool split_entry (char* line, entry_t* entry) {
    char* save = NULL;
    entry->user = strtok_r(line, " ", &save);
    entry->program = strtok_r(NULL, " ", &save);
    const char* kind = strtok_r(NULL, " ", &save);
    entry->folder = strtok_r(NULL, " ", &save);
    if (entry->user == NULL || entry->program == NULL || kind == NULL || entry->folder == NULL) {
        return false;
    }
    entry->kind = seq(kind, "change") ? PERM_CHANGE : PERM_READ;
    return true;
}

static void program_path (char* out, size_t size) {
    sfmt(out, size, "%s%s", program.folder, program.name);
}

static bool always_allowed (const char* folder, perm_kind_t kind) {
    char* text = fs_read_f(PERM_FILE_FOLDER, PERM_FILE_NAME);
    if (text == NULL) {
        return false;
    }
    char path[FS_PATH_MAX + FS_NAME_MAX];
    program_path(path, sizeof(path));

    bool allowed = false;
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line != NULL && !allowed; line = strtok_r(NULL, "\n", &save)) {
        entry_t entry;
        allowed = split_entry(line, &entry)
                  && strcasecmp(entry.user, auth_user) == 0
                  && strcasecmp(entry.program, path) == 0
                  && strcasecmp(entry.folder, folder) == 0
                  && covers(entry.kind, kind);
    }
    free(text);
    return allowed;
}

static void remember_always (const char* folder, perm_kind_t kind) {
    char path[FS_PATH_MAX + FS_NAME_MAX];
    program_path(path, sizeof(path));
    char line[FS_PATH_MAX * 2 + FS_NAME_MAX + 48];
    sfmt(line, sizeof(line), "%s %s %s %s\n", auth_user, path, kind_word(kind), folder);

    if (fs_file_exists(PERM_FILE_FOLDER, PERM_FILE_NAME)) {
        bool append = true;
        fs_write_f(PERM_FILE_FOLDER, PERM_FILE_NAME, line, append);
    } else {
        fs_create_f(PERM_FILE_FOLDER, PERM_FILE_NAME, line);
    }
}

int perm_each (perm_each_fn fn, void* ctx) {
    char* text = fs_read_f(PERM_FILE_FOLDER, PERM_FILE_NAME);
    if (text == NULL) {
        return 0;
    }
    int count = 0;
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        entry_t entry;
        if (split_entry(line, &entry) && strcasecmp(entry.user, auth_user) == 0) {
            count++;
            fn(count, entry.program, entry.kind, entry.folder, ctx);
        }
    }
    free(text);
    return count;
}

int perm_forget (int number) {
    char* text = fs_read_f(PERM_FILE_FOLDER, PERM_FILE_NAME);
    if (text == NULL) {
        return 0;
    }
    // write every line back, except the ones being forgotten
    size_t size = slen(text) + 1;
    char* kept = malloc(size);
    if (kept == NULL) {
        free(text);
        return 0;
    }
    kept[0] = '\0';
    int mine = 0;
    int forgotten = 0;
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char copy[FS_PATH_MAX * 2 + FS_NAME_MAX + 48];
        scpy(copy, line, sizeof(copy)); // (split_entry cuts the line up)
        entry_t entry;
        bool is_mine = split_entry(copy, &entry) && strcasecmp(entry.user, auth_user) == 0;
        if (is_mine) {
            mine++;
            if (number == 0 || number == mine) {
                forgotten++;
                continue;
            }
        }
        scat(kept, line, size);
        scat(kept, "\n", size);
    }
    if (forgotten > 0) {
        fs_write_f(PERM_FILE_FOLDER, PERM_FILE_NAME, kept, false);
    }
    free(kept);
    free(text);
    return forgotten;
}


// =================================================================================================
//  Rule 6: asking
// =================================================================================================

typedef enum { ANSWER_NO, ANSWER_YES, ANSWER_ALWAYS } answer_t;

// wait for Y, A or N (Esc and Ctrl+C count as N). With no keyboard (a command run by AerLang's
// cmd(), where nobody can see the question) the answer is no
static answer_t wait_for_answer (void) {
    for (;;) {
        int key = getk();
        if (key == 0 || key == KEY_ESCAPE || kb_take_break()) {
            return ANSWER_NO;
        }
        switch (tolower(key)) {
            case 'y': return ANSWER_YES;
            case 'a': return ANSWER_ALWAYS;
            case 'n': return ANSWER_NO;
        }
    }
}

// on the text screen: two lines, and the answer typed after them
static answer_t ask_in_text (const char* question, const char* choices) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    if (x != 0) {
        prtf("\n");
    }
    enum color old_fg = console_fg;
    colour(YELLOW, console_bg);
    prtf("[ ? ] %s\n      %s  ", question, choices);
    answer_t answer = wait_for_answer();
    colour(WHITE, console_bg);
    prtf("-> %s\n", answer == ANSWER_ALWAYS ? "always" : answer == ANSWER_YES ? "yes" : "no");
    colour(old_fg, console_bg);
    return answer;
}

// over a program's graphics: a box in the middle of the screen, which leaves no trace
static answer_t ask_in_graphics (const char* question, const char* choices) {
    const int margin = 16;        // pixels between the box's edge and the text
    const int line_height = 20;   // the font is 16 pixels tall, plus a gap
    int text_width = 8 * (int)(slen(question) > slen(choices) ? slen(question) : slen(choices));
    int w = text_width + margin * 2;
    int h = line_height * 2 + margin * 2;
    int x = ((int)gfx_width() - w) / 2;
    int y = ((int)gfx_height() - h) / 2;

    gfx_overlay_t* overlay = gfx_overlay_begin(x, y, w, h);
    gfx_rect(x, y, w, h, 0xFF7F00);                       // an orange edge...
    gfx_rect(x + 2, y + 2, w - 4, h - 4, 0x202020);       // ...round a dark grey box
    gfx_text(x + margin, y + margin, question, 0xFFFFFF, 1);
    gfx_text(x + margin, y + margin + line_height, choices, 0xFFD080, 1);
    answer_t answer = wait_for_answer();
    gfx_overlay_end(overlay);
    return answer;
}

static answer_t ask (const char* folder, const char* name, perm_kind_t kind) {
    char question[FS_PATH_MAX + FS_NAME_MAX * 2 + 48];
    if (name[0] != '\0') {
        sfmt(question, sizeof(question), "%s wants to %s files in %s (%s)", program.name, kind_word(kind), folder, name);
    } else {
        sfmt(question, sizeof(question), "%s wants to read the list of files in %s", program.name, folder);
    }
    const char* choices = "Y = yes, this time    A = always    N = no";

    bool graphics = gfx_width() > 0;
    return graphics ? ask_in_graphics(question, choices) : ask_in_text(question, choices);
}


// =================================================================================================
//  The rules
// =================================================================================================

bool perm_allows (const char* folder, const char* name, perm_kind_t kind) {
    // 1. the terminal and the code typed at it are you
    if (!program.running) {
        return true;
    }
    // 2. the accounts: never
    if (strncasecmp(folder, PERM_FILE_FOLDER, slen(PERM_FILE_FOLDER)) == 0) {
        return false;
    }
    // 3. free to read
    if (kind == PERM_READ) {
        bool own_folder = strcasecmp(folder, program.folder) == 0;
        if (system_folder(folder) || own_folder || named_by_you(folder, name)) {
            return true;
        }
    }
    // 4. answered already
    for (int i = 0; i < decision_count; i++) {
        if (strcasecmp(decisions[i].folder, folder) != 0) {
            continue;
        }
        if (decisions[i].allowed && covers(decisions[i].kind, kind)) {
            return true;
        }
        if (!decisions[i].allowed && decisions[i].kind == kind) {
            return false;
        }
    }
    // 5. "always", from before
    if (always_allowed(folder, kind)) {
        return true;
    }

    // 6. ask
    answer_t answer = ask(folder, name, kind);
    if (answer == ANSWER_ALWAYS) {
        remember_always(folder, kind);
    }
    if (decision_count < DECISIONS_MAX) {
        scpy(decisions[decision_count].folder, folder, FS_PATH_MAX);
        decisions[decision_count].kind = kind;
        decisions[decision_count].allowed = answer != ANSWER_NO;
        decision_count++;
    }
    return answer != ANSWER_NO;
}
