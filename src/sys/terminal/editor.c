/*
    editor.c - the AerOS text editor (EDT <file>)

    WHAT IS ON THE SCREEN

        row 0    | /bin/hello.aer *                              Line 3, Col 7 |  the title bar
        rows 1   | let name = input("Name? ")                                  |
        to 23    | print("Hello " + name)                                      |  the text
                 | ~                                                           |  (~ marks rows
                 | ~                                                           |  past the end)
        row 24   | ^S Save   ^R Run   ^Q Quit                                  |  the help bar

    The '*' after the name means there are unsaved changes. The help bar also shows messages,
    like "Saved 120 bytes", until the next key is pressed.

    HOW THE FILE IS KEPT

    The file is held as an array of lines. Each line is its own text buffer, without the '\n'
    at its end. An empty file is one empty line, so there is always at least one line.

    THE CURSOR AND SCROLLING

    The cursor is a place in the FILE (a line and a column), not on the screen. Before drawing,
    draw() moves the view (first_line, first_column) just enough to keep the cursor in sight,
    so long files scroll up and down and long lines scroll sideways.

    Every key press redraws the whole text area. That sounds slow, but the screen driver only
    repaints the character cells that actually changed, so it costs very little.

    AerLang files (.aer) get their keywords, strings, numbers and comments coloured in.
*/

#include "editor.h"
#include <stdint.h>
#include <stdbool.h>
#include "../../lib/primary_definitions.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../../lang/aerlang.h"
#include "../f_sys.h"

#define TITLE_ROW  0          // the title bar
#define TEXT_TOP   1          // the first screen row of text
#define TEXT_ROWS  (ROWS - 2) // how many rows of text fit between the two bars
#define HELP_ROW   (ROWS - 1) // the help bar
#define TAB_WIDTH  4          // Tab adds spaces up to the next multiple of this

// The code a key sends when Ctrl is held: Ctrl+A is 1, Ctrl+B is 2 ... Ctrl+Q is 17.
// So CTRL('S') is the number the keyboard sends for Ctrl+S
#define CTRL(letter) ((letter) - 'A' + 1)

// one line of the file. `text` always ends with a '\0', which `length` doesn't count.
// `capacity` is how many bytes the buffer has room for (it grows as the line does)
typedef struct {
    char* text;
    int length;
    int capacity;
} line_t;

// the file being edited
static line_t* lines;
static int line_count;
static int lines_capacity;      // how many lines the `lines` array has room for

// the cursor: a line and a column in the file, both counted from 0
static int cursor_line;
static int cursor_column;

// the view: the first line and column shown on the screen
static int first_line;
static int first_column;

static bool modified;           // changed since it was last saved?
static bool is_aerlang;         // an .aer file, to be coloured in
static char folder[FS_PATH_MAX];
static char file_name[FS_NAME_MAX];
static char message[COLS + 1];  // shown in the help bar until the next key ("" for none)


// =================================================================================================
//  Lines
// =================================================================================================

// make sure a line has room for `length` characters plus the '\0'. The buffer doubles in size
// each time it runs out, so typing a long line doesn't mean growing it on every key
static void make_room (line_t* line, int length) {
    int needed = length + 1;
    if (needed <= line->capacity) {
        return;
    }
    // a line that has never had a buffer starts at 16 bytes, as doubling 0 would stay 0
    int capacity = line->capacity;
    if (capacity == 0) {
        capacity = 16;
    }
    while (capacity < needed) {
        capacity *= 2;
    }
    // realloc keeps what the line already holds (and with NULL, as for a new line, it just
    // allocates)
    line->text = realloc(line->text, (size_t)capacity);
    line->capacity = capacity;
}

// add a new line at position `at` (the lines from `at` onwards move down one), holding the
// first `length` characters of `text`
static void insert_line (int at, const char* text, int length) {
    // the array of lines is full: double it
    if (line_count == lines_capacity) {
        if (lines_capacity == 0) {
            lines_capacity = 64;
        } else {
            lines_capacity *= 2;
        }
        lines = realloc(lines, (size_t)lines_capacity * sizeof(line_t));
    }

    // move the lines from `at` to the end down one place, to make a gap at `at`
    int lines_after = line_count - at;
    memmove(&lines[at + 1], &lines[at], (size_t)lines_after * sizeof(line_t));
    line_count++;

    // the gap still holds a copy of the line that moved down (its pointer too), so it is
    // cleared first, or make_room() would grow that other line's buffer
    line_t* line = &lines[at];
    memset(line, 0, sizeof(*line)); // a brand new, empty buffer
    make_room(line, length);
    memcpy(line->text, text, (size_t)length);
    line->length = length;
    line->text[length] = '\0';
}

// remove line `at` (the lines after it move up one)
static void delete_line (int at) {
    // its text buffer first, then close the gap so the lines after it move up
    free(lines[at].text);
    int lines_after = line_count - at - 1;
    memmove(&lines[at], &lines[at + 1], (size_t)lines_after * sizeof(line_t));
    line_count--;
}

// give back all the memory the lines used
static void free_lines (void) {
    for (int i = 0; i < line_count; i++) {
        free(lines[i].text);
    }
    free(lines);
    // back to "no lines", so the next file opened starts the array again from nothing
    lines = NULL;
    line_count = 0;
    lines_capacity = 0;
}


// =================================================================================================
//  Loading and saving
// =================================================================================================

// read the file into lines[], one line at a time. A file that isn't there yet starts as one
// empty line (it is made when first saved)
static void load (void) {
    char* text = fs_read_f(folder, file_name);
    if (text == NULL) {
        insert_line(0, "", 0);
        sfmt(message, sizeof(message), "New file");
        return;
    }

    // cut the text at each '\n'. A file ending in '\n' gives an empty last line, which is
    // right: save() puts the '\n' back between it and the line before
    const char* line_start = text;
    for (;;) {
        const char* newline = strchr(line_start, '\n');
        int length;
        if (newline != NULL) {
            length = (int)(newline - line_start);
        } else {
            length = (int)slen(line_start); // the last line, with no '\n' after it
        }

        // files written on Windows end their lines with "\r\n": leave the '\r' out
        if (length > 0 && line_start[length - 1] == '\r') {
            length--;
        }
        insert_line(line_count, line_start, length);

        if (newline == NULL) {
            break;
        }
        line_start = newline + 1;
    }
    free(text);
}

// join the lines back together with a '\n' between each, and write the file.
// The help bar says how it went
static void save (void) {
    // how big the whole file is: each line plus its '\n'
    int total = 0;
    for (int i = 0; i < line_count; i++) {
        total += lines[i].length + 1;
    }

    // + 1 for the '\0'. total counted a '\n' after the last line too, which isn't written,
    // so there is a byte to spare
    char* text = malloc((size_t)total + 1);
    if (text == NULL) {
        sfmt(message, sizeof(message), "Can't save: out of memory");
        return;
    }

    // a '\n' goes between lines, not after the last one, so loading and saving a file
    // without changing it gives back exactly the same bytes
    int size = 0;
    for (int i = 0; i < line_count; i++) {
        memcpy(text + size, lines[i].text, (size_t)lines[i].length);
        size += lines[i].length;
        bool last_line = i == line_count - 1;
        if (!last_line) {
            text[size] = '\n';
            size++;
        }
    }
    text[size] = '\0';

    // replace the file if it is there (false = don't add to the end), or make it
    int result;
    if (fs_file_exists(folder, file_name)) {
        result = fs_write_f(folder, file_name, text, false);
    } else {
        result = fs_create_f(folder, file_name, text);
    }

    // only a good save clears the '*': run_program() relies on that to spot a failed save
    if (result >= 0) {
        modified = false;
        sfmt(message, sizeof(message), "Saved %d bytes", size);
    } else {
        sfmt(message, sizeof(message), "Can't save: %s", fs_strerror(result));
    }
    free(text);
}


// =================================================================================================
//  Editing (each of these works at the cursor)
// =================================================================================================

// type a character at the cursor. The rest of the line moves one place right to make room
static void insert_character (char c) {
    // room for one more character first, as the move below needs it
    line_t* line = &lines[cursor_line];
    make_room(line, line->length + 1);

    // move everything from the cursor to the end (and the '\0' after it, hence the + 1) along one
    int moving = line->length - cursor_column + 1;
    memmove(line->text + cursor_column + 1, line->text + cursor_column, (size_t)moving);

    line->text[cursor_column] = c;
    line->length++;
    cursor_column++;
    modified = true;
}

// Enter: split the line at the cursor. The new line starts with the same indentation (the
// spaces at the start) as the one it came from, which is what you want when writing code
static void split_line (void) {
    line_t* line = &lines[cursor_line];

    // count the leading spaces, but only up to the cursor: pressing Enter in the middle of the
    // indentation shouldn't give the new line more spaces than were before the cursor
    int indent = 0;
    while (indent < line->length && indent < cursor_column && line->text[indent] == ' ') {
        indent++;
    }

    int rest_length = line->length - cursor_column; // what goes onto the new line
    insert_line(cursor_line + 1, "", 0);
    line = &lines[cursor_line]; // insert_line may have moved the whole array, so look it up again
    line_t* next = &lines[cursor_line + 1];

    // the new line: the indentation, then the rest of the old line
    make_room(next, indent + rest_length);
    memset(next->text, ' ', (size_t)indent);
    memcpy(next->text + indent, line->text + cursor_column, (size_t)rest_length);
    next->length = indent + rest_length;
    next->text[next->length] = '\0';

    // the old line now stops at the cursor
    line->length = cursor_column;
    line->text[cursor_column] = '\0';

    cursor_line++;
    cursor_column = indent;
    modified = true;
}

// join the line below the cursor onto the end of the cursor's line
static void join_next_line (void) {
    bool is_last_line = cursor_line + 1 >= line_count;
    if (is_last_line) {
        return;
    }
    line_t* line = &lines[cursor_line];
    line_t* next = &lines[cursor_line + 1];

    // grow the cursor's line to fit both, then copy the next line onto its end
    make_room(line, line->length + next->length);
    memcpy(line->text + line->length, next->text, (size_t)next->length + 1); // + 1: the '\0' too
    line->length += next->length;

    // the cursor stays where it was, which is now the join between the two
    delete_line(cursor_line + 1);
    modified = true;
}

// Backspace: delete the character before the cursor. At the start of a line, join this line
// onto the end of the one above instead
static void backspace (void) {
    if (cursor_column > 0) {
        line_t* line = &lines[cursor_line];
        // move everything from the cursor to the end (with the '\0') back one place
        int moving = line->length - cursor_column + 1;
        memmove(line->text + cursor_column - 1, line->text + cursor_column, (size_t)moving);
        line->length--;
        cursor_column--;
        modified = true;
    } else if (cursor_line > 0) {
        // step to the end of the line above, then pull this line up onto it, which leaves
        // the cursor at the join. (At the very start of the file there is nothing to do)
        cursor_line--;
        cursor_column = lines[cursor_line].length;
        join_next_line();
    }
}

// Delete: delete the character under the cursor. At the end of a line, pull the next line up
// onto this one instead
static void delete_forward (void) {
    line_t* line = &lines[cursor_line];
    if (cursor_column < line->length) {
        int moving = line->length - cursor_column; // the characters after it, and the '\0'
        memmove(line->text + cursor_column, line->text + cursor_column + 1, (size_t)moving);
        line->length--;
        modified = true;
    } else {
        join_next_line();
    }
}

// Tab: add spaces up to the next tab stop (always at least one)
static void insert_tab (void) {
    // do ... while, so a Tab at a tab stop still moves on to the next one
    do {
        insert_character(' ');
    } while (cursor_column % TAB_WIDTH != 0);
}


// =================================================================================================
//  Colouring AerLang code
// =================================================================================================

// is this word one of AerLang's keywords? (The same words as the table in lang/aer_lexer.c:
// if a keyword is added there, add it here too)
static bool is_keyword (const char* word, int length) {
    static const char* const keywords[] = {
        "let", "func", "return", "if", "then", "elif", "else", "end", "while", "do", "for", "to",
        "step", "in", "break", "continue", "and", "or", "not", "true", "false", "nil"
    };
    int keyword_count = (int)(sizeof(keywords) / sizeof(keywords[0]));

    // word isn't '\0' ended (it points into a line), so the lengths are compared first, then
    // strncmp checks just that many characters: "do" mustn't match the start of "done"
    for (int i = 0; i < keyword_count; i++) {
        bool same_length = (int)slen(keywords[i]) == length;
        if (same_length && strncmp(keywords[i], word, (size_t)length) == 0) {
            return true;
        }
    }
    return false;
}

// work out the colour of every character of a line, into colours[]:
//     comments dark grey, "strings" green, numbers yellow, keywords cyan, the rest light grey.
// This looks at each line on its own, so it is not a full AerLang lexer, but it is plenty for
// colouring. Files that aren't AerLang stay plain light grey
static void colour_line (const line_t* line, enum color* colours) {
    // everything starts light grey, and the loop below only colours over the parts it knows
    for (int i = 0; i < line->length; i++) {
        colours[i] = BRIGHT;
    }
    if (!is_aerlang) {
        return;
    }

    // each time round, the character at i decides what kind of piece starts here, and the
    // branch moves i past the end of that piece. start remembers where it began
    const char* text = line->text;
    int i = 0;
    while (i < line->length) {
        char c = text[i];
        int start = i;

        if (c == '#') {
            // a comment runs to the end of the line
            while (i < line->length) {
                colours[i] = DARK_GRAY;
                i++;
            }
        } else if (c == '"') {
            // a string runs to the next '"', skipping over escaped characters like \"
            i++;
            while (i < line->length && text[i] != '"') {
                if (text[i] == '\\') {
                    i++; // the character after a backslash can't end the string
                }
                i++;
            }
            if (i < line->length) {
                i++; // the closing '"'
            }
            // a backslash as the line's last character can push i one past the end, hence the
            // extra k < line->length. A string with no closing '"' is green to the end of the line
            for (int k = start; k < i && k < line->length; k++) {
                colours[k] = LIGHT_GREEN;
            }
        } else if (isdigit((unsigned char)c)) {
            // a number (letters too, so 0xFF colours as one)
            while (i < line->length && isalnum((unsigned char)text[i])) {
                colours[i] = YELLOW;
                i++;
            }
        } else if (isalpha((unsigned char)c) || c == '_') {
            // a word: only keywords are coloured. Reading the whole word first means a name like
            // "endless" isn't half coloured as the keyword "end"
            while (i < line->length && (isalnum((unsigned char)text[i]) || text[i] == '_')) {
                i++;
            }
            if (is_keyword(text + start, i - start)) {
                for (int k = start; k < i; k++) {
                    colours[k] = LIGHT_CYAN;
                }
            }
        } else {
            i++; // anything else (spaces, brackets, + - * /) stays light grey
        }
    }
}


// =================================================================================================
//  Drawing
// =================================================================================================

// fill a whole screen row with spaces in the given colours
static void blank_row (uint8_t row, enum color text_colour, enum color background) {
    for (int x = 0; x < COLS; x++) {
        putc((uint8_t)x, row, text_colour, background, ' ');
    }
}

// move the view, if needed, so the cursor is on the screen
static void scroll_to_cursor (void) {
    if (cursor_line < first_line) {
        first_line = cursor_line;                    // above the screen: scroll up to it
    }
    if (cursor_line >= first_line + TEXT_ROWS) {
        first_line = cursor_line - TEXT_ROWS + 1;    // below it: scroll down
    }
    if (cursor_column < first_column) {
        first_column = cursor_column;                // left of it: scroll left
    }
    if (cursor_column >= first_column + COLS) {
        first_column = cursor_column - COLS + 1;     // right of it: scroll right
    }
}

// the title bar: the file's name (with * if it has unsaved changes) and where the cursor is
static void draw_title_bar (void) {
    char title[COLS + 1];
    sfmt(title, sizeof(title), " %s%s%s", folder, file_name, modified ? " *" : "");

    // + 1, as people count lines and columns from 1, while the cursor counts from 0
    char position[24];
    sfmt(position, sizeof(position), "Line %d, Col %d ", cursor_line + 1, cursor_column + 1);

    // black on light grey: the bar is drawn the other way round from the text, so it stands out
    blank_row(TITLE_ROW, BLACK, BRIGHT);
    puts(0, TITLE_ROW, BLACK, BRIGHT, title);
    puts((uint8_t)(COLS - slen(position)), TITLE_ROW, BLACK, BRIGHT, position); // right aligned
}

// the text: one screen row for each line in view
static void draw_text (void) {
    // the colour of each character of one line. Lines longer than this just aren't coloured
    // (it lives on the stack, so it has a fixed size rather than growing with the line)
    enum color colours[COLS + 512];
    int most_colours = (int)(sizeof(colours) / sizeof(colours[0]));

    for (int row = 0; row < TEXT_ROWS; row++) {
        int line_number = first_line + row;
        uint8_t y = (uint8_t)(TEXT_TOP + row);

        // past the end of the file: a '~' in the first column, like vi
        if (line_number >= line_count) {
            blank_row(y, DARK_GRAY, BLACK);
            putc(0, y, DARK_GRAY, BLACK, '~');
            continue;
        }

        const line_t* line = &lines[line_number];
        bool coloured = line->length <= most_colours;
        if (coloured) {
            colour_line(line, colours);
        }

        // every cell of the row is drawn, with spaces after the line's end, so nothing is left
        // over from what was there before. column is the place in the line, x on the screen
        for (int x = 0; x < COLS; x++) {
            int column = first_column + x;
            char c = ' ';
            enum color colour = BRIGHT;
            if (column < line->length) {
                c = line->text[column];
                if (coloured) {
                    colour = colours[column];
                }
            }
            putc((uint8_t)x, y, colour, BLACK, c);
        }
    }
}

// the help bar: the keys, with a message on the left when there is one
static void draw_help_bar (void) {
    // ^R only means something for an AerLang file, so it is only shown for one
    const char* keys;
    if (is_aerlang) {
        keys = " ^S Save   ^R Run   ^Q Quit";
    } else {
        keys = " ^S Save   ^Q Quit";
    }

    blank_row(HELP_ROW, BLACK, BRIGHT);
    if (message[0] == '\0') {
        puts(0, HELP_ROW, BLACK, BRIGHT, keys);
    } else {
        puts(0, HELP_ROW, BLACK, BRIGHT, message);
        puts((uint8_t)(COLS - slen(keys) - 1), HELP_ROW, BLACK, BRIGHT, keys); // keys on the right
    }
}

// redraw everything, then put the blinking cursor where the text cursor is
static void draw (void) {
    // the view first, as everything else is drawn from it
    scroll_to_cursor();
    draw_title_bar();
    draw_text();
    draw_help_bar();

    // the file position minus the view's corner gives the screen position. scroll_to_cursor()
    // made sure this lands inside the text area
    colour(BRIGHT, BLACK);
    uint8_t screen_x = (uint8_t)(cursor_column - first_column);
    uint8_t screen_y = (uint8_t)(TEXT_TOP + cursor_line - first_line);
    console_set_cursor(screen_x, screen_y);
}


// =================================================================================================
//  Running the program, and the main loop
// =================================================================================================

// Ctrl+R: save, run the program on a clean screen, then wait for a key to come back
static void run_program (void) {
    save();
    if (modified) {
        return; // the save failed (the help bar says why), so don't run an old copy
    }

    // run the file just saved on the disk, with no arguments
    char path[FS_PATH_MAX + FS_NAME_MAX];
    sfmt(path, sizeof(path), "%s%s", folder, file_name);

    colour(BRIGHT, BLACK);
    console_clear();
    aer_run_file(path, "");

    // wait, so the program's output can be read before the editor covers it up

    colour(DARK_GRAY, BLACK);
    prtf("\n-- press any key to go back to the editor --");
    getk();

    vga_invalidate(); // the program may have drawn graphics: redraw every cell next time
    clear(BLACK);
    message[0] = '\0';
}

// the arrow keys, Home / End and Page Up / Down: move the cursor
static void move_cursor (int key) {
    // the line the cursor was on before moving (used for Right and End)
    const line_t* line = &lines[cursor_line];

    switch (key) {
        case KEY_LEFT:
            if (cursor_column > 0) {
                cursor_column--;
            } else if (cursor_line > 0) {
                // at the start of a line: to the end of the line above
                cursor_line--;
                cursor_column = lines[cursor_line].length;
            }
            break;

        case KEY_RIGHT:
            if (cursor_column < line->length) {
                cursor_column++;
            } else if (cursor_line + 1 < line_count) {
                // at the end of a line: to the start of the line below
                cursor_line++;
                cursor_column = 0;
            }
            break;

        case KEY_UP:
            if (cursor_line > 0) {
                cursor_line--;
            }
            break;

        case KEY_DOWN:
            if (cursor_line + 1 < line_count) {
                cursor_line++;
            }
            break;

        case KEY_PGUP: // a screenful up, stopping at the first line
            cursor_line -= TEXT_ROWS;
            if (cursor_line < 0) {
                cursor_line = 0;
            }
            break;

        case KEY_PGDN: // a screenful down, stopping at the last line
            cursor_line += TEXT_ROWS;
            if (cursor_line > line_count - 1) {
                cursor_line = line_count - 1;
            }
            break;

        case KEY_HOME:
            cursor_column = 0;
            break;

        case KEY_END:
            cursor_column = line->length;
            break;
    }

    // moving up or down onto a shorter line: the cursor can't be past its end
    if (cursor_column > lines[cursor_line].length) {
        cursor_column = lines[cursor_line].length;
    }
}

// one key press that changes the text (or moves the cursor)
static void handle_key (int key) {
    switch (key) {
        case CTRL('S'):  save();               break;
        case '\n':       split_line();         break;
        case '\b':       backspace();          break;
        case KEY_DELETE: delete_forward();     break;
        case '\t':       insert_tab();         break;

        case CTRL('R'):
            if (is_aerlang) {
                run_program();
            }
            break;

        case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
        case KEY_PGUP: case KEY_PGDN: case KEY_HOME: case KEY_END:
            move_cursor(key);
            break;

        // any other key types itself, if it is a printable character. Other Ctrl keys and the
        // KEY_ special values (F1 and so on) are ignored
        default:
            if (isprint(key)) {
                insert_character((char)key);
            }
            break;
    }
}

// open the editor on a file, and stay in it until Ctrl+Q
void editor_open (const char* path) {
    // the file needn't exist yet (load() starts a new one), but its folder must, or it could
    // never be saved
    bool path_ok = fs_split_path(path, folder, sizeof(folder), file_name, sizeof(file_name))
                   && fs_dir_exists(folder);
    if (!path_ok) {
        prtf("Can't edit %s\n", path);
        return;
    }

    // start fresh: these are all static, so they still hold whatever the last file left
    is_aerlang = aer_is_program_name(file_name, slen(file_name));
    cursor_line = 0;
    cursor_column = 0;
    first_line = 0;
    first_column = 0;
    modified = false;
    message[0] = '\0';
    load();

    clear(BLACK); // take over the whole screen (the terminal redraws itself afterwards)

    // Ctrl+Q with unsaved changes only warns the first time, and quits if pressed again
    // straight away
    bool warned_about_quitting = false;

    for (;;) {
        draw();
        int key = getk();
        if (key == 0) {
            break; // no keyboard
        }
        message[0] = '\0'; // a message only lasts until the next key

        if (key == CTRL('Q')) {
            if (modified && !warned_about_quitting) {
                sfmt(message, sizeof(message), "Unsaved changes! Ctrl+Q again to quit without saving");
                warned_about_quitting = true;
                continue;
            }
            break;
        }

        handle_key(key);
        warned_about_quitting = false; // any other key means the next Ctrl+Q warns again
    }
    // quitting doesn't save: anything unsaved was warned about above
    free_lines();
}
