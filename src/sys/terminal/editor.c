/*
    editor.c - the AerOS text editor

    The file is held as an array of lines, each its own growing buffer. The screen shows a title
    bar (row 0), 23 rows of text and a help bar (row 24). Every key press redraws the whole text
    area, which is cheap because the screen driver only repaints cells that actually changed.
    Lines longer than the screen scroll sideways with the cursor.

        row 0    | /bin/hello.ren *                              Line 3, Col 7 |  title bar
        rows 1   | let name = input("Name? ")                                  |
        to 23    | print("Hello " + name)                                      |  the text, from
                 | ~                                                           |  line "top" and
                 | ~                                                           |  column "left"
        row 24   | ^S Save   ^R Run   ^Q Quit                                  |  help / status

    The cursor (cx, cy) is a position in the file, not on the screen: draw() scrolls top and left
    so that it is always visible. .ren files get syntax colouring.
*/

#include "editor.h"
#include <stdint.h>
#include <stdbool.h>
#include "../../lib/primary_definitions.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../../lang/renlang.h"
#include "../f_sys.h"

#define TEXT_TOP   1          // the first screen row of text (row 0 is the title bar)
#define TEXT_ROWS  (ROWS - 2) // rows of text on screen, between the two bars
#define TAB_WIDTH  4          // Tab inserts spaces up to the next multiple of this

// the code a key sends with Ctrl held: Ctrl+A is 1, Ctrl+B is 2 ... Ctrl+Q is 17
#define CTRL(letter) ((letter) - 'A' + 1)

// one line of the file, without its '\n' (text is '\0' terminated, length doesn't count that)

typedef struct {
    char* text;
    int length;
    int capacity;
} line_t;

// the file being edited, always at least one line (an empty file is one empty line)
static line_t* lines;
static int line_count;
static int line_capacity;

static int cx, cy;      // the cursor: column and line in the file
static int top, left;   // the first line and column on screen
static bool modified;
static bool is_renlang; // colour the code in
static char dir[FS_PATH_MAX];
static char name[FS_NAME_MAX];
static char status[COLS + 1]; // a message for the help bar, until the next key

// ---- lines ------------------------------------------------------------------------------------

// make sure line has room for length characters and the '\0', doubling its buffer as needed
static void line_reserve (line_t* line, int length) {
    if (length + 1 <= line->capacity) return;
    int capacity = line->capacity ? line->capacity : 16;
    while (capacity < length + 1) capacity *= 2;
    line->text = realloc(line->text, (size_t)capacity);
    line->capacity = capacity;
}

// add a line holding text[0 .. length) at position at
static void insert_line (int at, const char* text, int length) {
    if (line_count == line_capacity) {
        line_capacity = line_capacity ? line_capacity * 2 : 64;
        lines = realloc(lines, (size_t)line_capacity * sizeof(line_t));
    }
    memmove(&lines[at + 1], &lines[at], (size_t)(line_count - at) * sizeof(line_t));
    line_count++;
    line_t* line = &lines[at];
    memset(line, 0, sizeof(*line));
    line_reserve(line, length);
    memcpy(line->text, text, (size_t)length);
    line->length = length;
    line->text[length] = '\0';
}

static void delete_line (int at) {
    free(lines[at].text);
    memmove(&lines[at], &lines[at + 1], (size_t)(line_count - at - 1) * sizeof(line_t));
    line_count--;
}

static void free_lines (void) {
    for (int i = 0; i < line_count; i++) {
        free(lines[i].text);
    }
    free(lines);
    lines = 0;
    line_count = line_capacity = 0;
}

// read the file into lines[], or start one empty line for a new file
static void load (void) {
    char* text = fs_read_f(dir, name);
    if (text == 0) {
        insert_line(0, "", 0);
        snprintf(status, sizeof(status), "New file");
        return;
    }
    const char* p = text;
    for (;;) {
        const char* end = strchr(p, '\n');
        int length = end ? (int)(end - p) : (int)strlen(p);
        if (length > 0 && p[length - 1] == '\r') length--; // files from Windows
        insert_line(line_count, p, length);
        if (!end) break;
        p = end + 1;
    }
    free(text);
}

// join the lines back up with '\n' between them and write the file (the status bar says how it went)
static void save (void) {
    int total = 0;
    for (int i = 0; i < line_count; i++) {
        total += lines[i].length + 1;
    }
    char* text = malloc((size_t)total + 1);
    if (text == 0) {
        snprintf(status, sizeof(status), "Can't save: out of memory");
        return;
    }
    char* p = text;
    for (int i = 0; i < line_count; i++) {
        memcpy(p, lines[i].text, (size_t)lines[i].length);
        p += lines[i].length;
        if (i < line_count - 1) *p++ = '\n';
    }
    *p = '\0';

    int result = fs_file_exists(dir, name) ? fs_write_f(dir, name, text, false) : fs_create_f(dir, name, text);
    if (result >= 0) {
        modified = false;
        snprintf(status, sizeof(status), "Saved %d bytes", (int)(p - text));
    } else {
        snprintf(status, sizeof(status), "Can't save: %s", fs_strerror(result));
    }
    free(text);
}

// ---- editing ----------------------------------------------------------------------------------
// each of these works at the cursor and marks the file modified

// type a character, the rest of the line moves right (the + 1 in the memmove carries the '\0')
static void insert_char (char c) {
    line_t* line = &lines[cy];
    line_reserve(line, line->length + 1);
    memmove(line->text + cx + 1, line->text + cx, (size_t)(line->length - cx + 1));
    line->text[cx++] = c;
    line->length++;
    modified = true;
}

// split the line at the cursor, the new line starts with the same indentation
static void new_line (void) {
    line_t* line = &lines[cy];
    int indent = 0;
    while (indent < line->length && indent < cx && line->text[indent] == ' ') indent++;

    int rest_length = line->length - cx;
    insert_line(cy + 1, "", 0);
    line = &lines[cy]; // insert_line may have moved the array
    line_t* next = &lines[cy + 1];
    line_reserve(next, indent + rest_length);
    memset(next->text, ' ', (size_t)indent);
    memcpy(next->text + indent, line->text + cx, (size_t)rest_length);
    next->length = indent + rest_length;
    next->text[next->length] = '\0';

    line->length = cx;
    line->text[cx] = '\0';
    cy++;
    cx = indent;
    modified = true;
}

// join line cy + 1 onto the end of line cy
static void join_next (void) {
    if (cy + 1 >= line_count) return;
    line_t* line = &lines[cy];
    line_t* next = &lines[cy + 1];
    line_reserve(line, line->length + next->length);
    memcpy(line->text + line->length, next->text, (size_t)next->length + 1);
    line->length += next->length;
    delete_line(cy + 1);
    modified = true;
}

// delete the character before the cursor, or at the start of a line join it onto the one above
static void backspace (void) {
    if (cx > 0) {
        line_t* line = &lines[cy];
        memmove(line->text + cx - 1, line->text + cx, (size_t)(line->length - cx + 1));
        line->length--;
        cx--;
        modified = true;
    } else if (cy > 0) {
        cy--;
        cx = lines[cy].length;
        join_next();
    }
}

// delete the character under the cursor, or at the end of a line pull the next one up
static void delete_forward (void) {
    line_t* line = &lines[cy];
    if (cx < line->length) {
        memmove(line->text + cx, line->text + cx + 1, (size_t)(line->length - cx));
        line->length--;
        modified = true;
    } else {
        join_next();
    }
}

// ---- drawing ----------------------------------------------------------------------------------

// is this word a RenLang keyword? (the same words as the table in lang/ren_lexer.c, keep them in step)
static bool is_keyword (const char* word, int length) {
    static const char* const keywords[] = {
        "let", "func", "return", "if", "then", "elif", "else", "end", "while", "do", "for", "to",
        "step", "in", "break", "continue", "and", "or", "not", "true", "false", "nil"
    };
    for (unsigned int i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if ((int)strlen(keywords[i]) == length && strncmp(keywords[i], word, (size_t)length) == 0) return true;
    }
    return false;
}

// the colour of every character of a line (RenLang files only, everything else is plain):
// comments grey, strings green, numbers yellow, keywords cyan, everything else light grey.
// A quick scan of the line on its own, not a full lexer, which is plenty for colouring
static void colour_line (const line_t* line, enum color* colours) {
    for (int i = 0; i < line->length; i++) colours[i] = BRIGHT;
    if (!is_renlang) return;

    int i = 0;
    while (i < line->length) {
        char c = line->text[i];
        int start = i;
        if (c == '#') {
            while (i < line->length) colours[i++] = DARK_GRAY;
        } else if (c == '"') {
            i++;
            while (i < line->length && line->text[i] != '"') {
                if (line->text[i] == '\\') i++;
                i++;
            }
            if (i < line->length) i++;
            for (int k = start; k < i && k < line->length; k++) colours[k] = LIGHT_GREEN;
        } else if (isdigit((unsigned char)c)) {
            while (i < line->length && isalnum((unsigned char)line->text[i])) colours[i++] = YELLOW;
        } else if (isalpha((unsigned char)c) || c == '_') {
            while (i < line->length && (isalnum((unsigned char)line->text[i]) || line->text[i] == '_')) i++;
            if (is_keyword(line->text + start, i - start)) {
                for (int k = start; k < i; k++) colours[k] = LIGHT_CYAN;
            }
        } else {
            i++;
        }
    }
}

// redraw the title bar, the visible lines and the help bar, then place the cursor
static void draw (void) {
    // keep the cursor on screen
    if (cy < top) top = cy;
    if (cy >= top + TEXT_ROWS) top = cy - TEXT_ROWS + 1;
    if (cx < left) left = cx;
    if (cx >= left + COLS) left = cx - COLS + 1;

    char bar[COLS + 1];
    snprintf(bar, sizeof(bar), " %s%s%s", dir, name, modified ? " *" : "");
    char position[24];
    snprintf(position, sizeof(position), "Line %d, Col %d ", cy + 1, cx + 1);
    for (int x = 0; x < COLS; x++) putc((uint8_t)x, 0, BLACK, BRIGHT, ' ');
    puts(0, 0, BLACK, BRIGHT, bar);
    puts((uint8_t)(COLS - strlen(position)), 0, BLACK, BRIGHT, position);

    // colours for one line, lines longer than this are just shown uncoloured
    enum color colours[COLS + 512];
    for (int row = 0; row < TEXT_ROWS; row++) {
        int index = top + row;
        uint8_t y = (uint8_t)(TEXT_TOP + row);
        if (index >= line_count) {
            for (int x = 0; x < COLS; x++) putc((uint8_t)x, y, DARK_GRAY, BLACK, x == 0 ? '~' : ' ');
            continue;
        }
        const line_t* line = &lines[index];
        bool coloured = line->length <= (int)(sizeof(colours) / sizeof(colours[0]));
        if (coloured) colour_line(line, colours);
        for (int x = 0; x < COLS; x++) {
            int col = left + x;
            char c = col < line->length ? line->text[col] : ' ';
            putc((uint8_t)x, y, (coloured && col < line->length) ? colours[col] : BRIGHT, BLACK, c);
        }
    }

    const char* help = is_renlang ? " ^S Save   ^R Run   ^Q Quit" : " ^S Save   ^Q Quit";
    for (int x = 0; x < COLS; x++) putc((uint8_t)x, ROWS - 1, BLACK, BRIGHT, ' ');
    puts(0, ROWS - 1, BLACK, BRIGHT, status[0] ? status : help);
    if (status[0]) {
        puts((uint8_t)(COLS - strlen(help) - 1), ROWS - 1, BLACK, BRIGHT, help);
    }

    console_set_color(BRIGHT, BLACK);
    console_set_cursor((uint8_t)(cx - left), (uint8_t)(TEXT_TOP + cy - top));
}

// save, run the program on a clean screen, then wait for a key to come back
static void run_program (void) {
    save();
    if (modified) return; // the save failed, the status bar says why

    char path[FS_PATH_MAX + FS_NAME_MAX];
    snprintf(path, sizeof(path), "%s%s", dir, name);
    console_set_color(BRIGHT, BLACK);
    console_clear();
    ren_run_file(path, "");
    console_set_color(DARK_GRAY, BLACK);
    printf("\n-- press any key to go back to the editor --");
    GetChar();
    vga_invalidate(); // the program may have drawn graphics
    clear(BLACK);
    status[0] = '\0';
}

// the editor's main loop: draw, wait for a key, act on it, until Ctrl+Q
void editor_open (const char* path) {
    if (!fs_split_path(path, dir, sizeof(dir), name, sizeof(name)) || !fs_dir_exists(dir)) {
        printf("Can't edit %s\n", path);
        return;
    }
    size_t length = strlen(name);
    is_renlang = length > 4 && strcasecmp(name + length - 4, ".ren") == 0;
    cx = cy = top = left = 0;
    modified = false;
    status[0] = '\0';
    load();

    clear(BLACK);
    bool quit_warned = false;
    for (;;) {
        draw();
        int key = GetChar();
        if (key == 0) break; // no keyboard
        status[0] = '\0';
        line_t* line = &lines[cy];

        switch (key) {
            case CTRL('Q'): // quit, asking twice if there are unsaved changes
                if (modified && !quit_warned) {
                    snprintf(status, sizeof(status), "Unsaved changes! Ctrl+Q again to quit without saving");
                    quit_warned = true;
                    continue;
                }
                free_lines();
                return;
            case CTRL('S'): save(); break;
            case CTRL('R'): if (is_renlang) run_program(); break; // save and run
            case '\n': new_line(); break;
            case '\b': backspace(); break;
            case KEY_DELETE: delete_forward(); break;
            case '\t':
                do { insert_char(' '); } while (cx % TAB_WIDTH != 0);
                break;
            case KEY_LEFT:
                if (cx > 0) cx--;
                else if (cy > 0) { cy--; cx = lines[cy].length; }
                break;
            case KEY_RIGHT:
                if (cx < line->length) cx++;
                else if (cy + 1 < line_count) { cy++; cx = 0; }
                break;
            case KEY_UP: if (cy > 0) cy--; break;
            case KEY_DOWN: if (cy + 1 < line_count) cy++; break;
            case KEY_PGUP: cy = cy > TEXT_ROWS ? cy - TEXT_ROWS : 0; break;
            case KEY_PGDN: cy = cy + TEXT_ROWS < line_count ? cy + TEXT_ROWS : line_count - 1; break;
            case KEY_HOME: cx = 0; break;
            case KEY_END: cx = line->length; break;
            default:
                if (isprint(key)) insert_char((char)key);
                break;
        }
        if (cx > lines[cy].length) cx = lines[cy].length; // after moving up or down onto a short line
        quit_warned = false;
    }
    free_lines();
}
