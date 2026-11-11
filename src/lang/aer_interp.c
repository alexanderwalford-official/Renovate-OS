/*
    aer_interp.c
    Runs an AerLang program by walking its syntax tree.

    eval() works out the value of an expression and exec() carries out a statement, each a switch
    over the node types that calls itself for the node's children.

    Reference counting. Strings, lists and bytes are objects shared by reference, with a count of
    how many places hold them. The rules every function here follows:
      - a value_t that eval() returns is "owned": the caller must either aer_release() it or hand
        it to something that keeps it (a variable, a list item)
      - storing a value somewhere (scope_set, aer_list_push) takes over the caller's reference
      - reading a value out of a variable or list without taking it away needs aer_retain()
    When a count reaches 0 the object is freed at once (and a list releases its items).

    Scopes. There are only two places a name can live: the program's globals, and the locals of
    the function that is running (no nesting and no closures, a function sees its own variables
    and the globals). Builtins like print are ordinary globals.

    Control flow. exec() returns what should happen next: EXEC_NEXT (carry on), or EXEC_BREAK /
    EXEC_CONTINUE / EXEC_RETURN, which every block passes straight back up until a loop or a
    function call deals with it. So "break" is just a return value, no jumps needed.

    Errors. aer_error() longjmps back to the setjmp in run_program(), straight out of however
    deep the program was. Everything it allocated is in the aer_alloc list and freed in one go
    by session_end(), so nothing leaks however the program ends. WHY can explain the error
    afterwards (aer_error hands it to sys/explain.c).

    The shell. The terminal's prompt runs AerLang too, in a session of its own that lasts from
    logging in to logging out, and is put aside while a program runs: see "The shell" near the
    end of this file.
*/

#include "aer_internal.h"
#include "aerlang.h"
#include <stdarg.h>
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_stdio.h"
#include "../lib/primary_definitions.h"
#include "../lib/aer_math.h"
#include "../lib/fpu.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../sys/f_sys.h"
#include "../sys/explain.h"
#include "../sys/perm.h"
#include "../sys/appmode.h"

#define STACK_RESERVE (32 * 1024) // stop a runaway program before it uses the last 32KB of stack

extern char stack[]; // the bottom of the kernel stack (entry.asm)

aer_state_t* aer = 0;

// ---- memory -----------------------------------------------------------------------------------
//
// Every allocation has a small header in front of it linking it into one list:
//
//     blocks -> [header|data] <-> [header|data] <-> [header|data] -> NULL
//                       ^ the pointer aer_alloc hands out
//
// so aer_free_all() can give everything back when the program ends, even after an error left
// things half built. aer_free() unlinks one block early (used by reference counting).

typedef struct aer_block {
    struct aer_block* prev;
    struct aer_block* next;
    size_t size;
} aer_block_t;

static aer_block_t* blocks = 0; // every allocation of the running program

// out of memory is an AerLang error like any other, so callers never have to check for NULL
void* aer_alloc (size_t size) {
    aer_block_t* b = malloc(sizeof(aer_block_t) + size);
    if (b == 0) {
        aer_error(0, "out of memory");
    }
    b->prev = 0;
    b->next = blocks;
    b->size = size;
    if (blocks) {
        blocks->prev = b;
    }
    blocks = b;
    return b + 1;
}

void aer_free (void* ptr) {
    if (ptr == 0) {
        return;
    }
    aer_block_t* b = (aer_block_t*)ptr - 1;
    if (b->prev) {
        b->prev->next = b->next;
    }
    else blocks = b->next;
    if (b->next) {
        b->next->prev = b->prev;
    }
    free(b);
}

// a new block and a copy, rather than the heap's realloc, so the block list stays right
void* aer_realloc (void* ptr, size_t size) {
    void* bigger = aer_alloc(size);
    if (ptr) {
        size_t old = ((aer_block_t*)ptr - 1)->size;
        memcpy(bigger, ptr, old < size ? old : size);
        aer_free(ptr);
    }
    return bigger;
}

void aer_free_all (void) {
    while (blocks) {
        aer_block_t* next = blocks->next;
        free(blocks);
        blocks = next;
    }
}

// names are stored once, so comparing two names is just comparing pointers
// a small hash table: hash the name (djb2: hash * 33 + c) to pick one of 128 buckets, each a
// linked list of the names that landed there
#define NAME_BUCKETS 128

typedef struct name_entry {
    struct name_entry* next;
    char text[];
} name_entry_t;

static name_entry_t* names[NAME_BUCKETS];

const char* aer_intern (const char* text, size_t length) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < length; i++) {
        hash = hash * 33 + (uint8_t)text[i];
    }
    name_entry_t** bucket = &names[hash % NAME_BUCKETS];
    for (name_entry_t* e = *bucket; e; e = e->next) {
        if (strncmp(e->text, text, length) == 0 && e->text[length] == '\0') {
            return e->text;
        }
    }
    name_entry_t* e = aer_alloc(sizeof(name_entry_t) + length + 1);
    memcpy(e->text, text, length);
    e->text[length] = '\0';
    e->next = *bucket;
    *bucket = e;
    return e->text;
}

// ---- errors -----------------------------------------------------------------------------------

// print "Error in <file> on line <n>: <message>" in red, then jump out of the program.
// WHY can explain it afterwards: it is handed to explain_program() (sys/explain.c) with the
// program's code, so WHY can show the lines around the error
void aer_error (int line, const char* format, ...) {
    char message[160];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);

    enum color old_fg = console_fg;
    colour(LIGHT_RED, console_bg);
    if (aer->shell) {
        // code typed at the prompt: it is right there on the screen, so no name is needed
        if (line > 1) {
            prtf("Error on line %d: %s\n", line, message);
        } else {
            prtf("Error: %s\n", message);
        }
    } else if (line > 0) {
        prtf("Error in %s on line %d: %s\n", aer->name, line, message);
    } else {
        prtf("Error in %s: %s\n", aer->name, message);
    }
    colour(old_fg, console_bg);

    // WHY shows the code around the error, except for a single line typed at the prompt
    // (it's the line just typed, so there's nothing to add)
    const char* code = aer->source;
    if (aer->shell && code != NULL && strchr(code, '\n') == NULL) {
        code = NULL;
    }
    explain_program(aer->shell ? NULL : aer->name, line, message, code);
    longjmp(aer->error, 1);
}

// called for every node evaluated: stops deep recursion before it overflows the kernel stack
// (which everything shares), and notices Ctrl+C so a stuck loop can always be stopped
static void check_stack (int line) {
    char here;
    if ((uintptr_t)&here < (uintptr_t)stack + STACK_RESERVE) {
        aer_error(line, "too much recursion");
    }
    if (kb_take_break()) {
        aer_stop(line);
    }
}

// set when a program is stopped with Ctrl+C, so whoever ran it (the GO command running the
// queue) can tell a deliberate stop from a program that simply finished or failed
static bool stopped_by_user = false;

void aer_stop (int line) {
    stopped_by_user = true;
    aer_error(line, "stopped (Ctrl+C)");
}

bool aer_take_stopped (void) {
    bool stopped = stopped_by_user;
    stopped_by_user = false;
    return stopped;
}

// ---- values -----------------------------------------------------------------------------------

// a new object with one reference (the caller's)
static aer_object_t* new_object (value_type_t type) {
    aer_object_t* o = aer_alloc(sizeof(aer_object_t));
    memset(o, 0, sizeof(*o));
    o->refs = 1;
    o->type = type;
    return o;
}

static value_t object_value (aer_object_t* o) {
    value_t v;
    v.type = o->type;
    v.as.obj = o;
    return v;
}

value_t aer_string (const char* chars, uint32_t length) {
    aer_object_t* o = new_object(V_STR);
    o->data.chars = aer_alloc(length + 1);
    memcpy(o->data.chars, chars, length);
    o->data.chars[length] = '\0';
    o->length = length;
    return object_value(o);
}

value_t aer_list (uint32_t capacity) {
    aer_object_t* o = new_object(V_LIST);
    o->capacity = capacity ? capacity : 4;
    o->data.items = aer_alloc(o->capacity * sizeof(value_t));
    return object_value(o);
}

value_t aer_bytes (uint32_t length) {
    aer_object_t* o = new_object(V_BYTES);
    o->data.bytes = aer_alloc(length ? length : 1);
    memset(o->data.bytes, 0, length);
    o->length = length;
    return object_value(o);
}

// add to the end of a list, doubling its array when full (so pushing n items costs about n copies)
void aer_list_push (aer_object_t* list, value_t item) {
    if (list->length == list->capacity) {
        list->capacity *= 2;
        list->data.items = aer_realloc(list->data.items, list->capacity * sizeof(value_t));
    }
    list->data.items[list->length++] = item;
}

static bool is_object (value_t v) {
    return v.type == V_STR || v.type == V_LIST || v.type == V_BYTES;
}

void aer_retain (value_t v) {
    if (is_object(v)) {
        v.as.obj->refs++;
    }
}

void aer_release (value_t v) {
    if (!is_object(v) || --v.as.obj->refs > 0) {
        return;
    }
    aer_object_t* o = v.as.obj;
    if (o->type == V_LIST) {
        for (uint32_t i = 0; i < o->length; i++) {
            aer_release(o->data.items[i]);
        }
    }
    aer_free(o->data.chars); // the same pointer whichever kind it is
    aer_free(o);
}

// what counts as "true" in if / while / and / or: everything except nil, false and 0
bool aer_truthy (value_t v) {
    switch (v.type) {
        case V_NIL: return false;
        case V_BOOL:
        case V_INT: return v.as.i != 0;
        case V_FLOAT: return v.as.f != 0;
        default: return true;
    }
}

// == compares strings by their characters, but lists and bytes by identity (the same object).
// Numbers compare by value, whole or decimal: 2 == 2.0
bool aer_equal (value_t a, value_t b) {
    bool decimal = a.type == V_FLOAT || b.type == V_FLOAT;
    if (decimal && aer_is_number(a) && aer_is_number(b)) {
        return aer_real(a) == aer_real(b);
    }
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case V_NIL: return true;
        case V_STR:
            return a.as.obj->length == b.as.obj->length &&
                   memcmp(a.as.obj->data.chars, b.as.obj->data.chars, a.as.obj->length) == 0;
        case V_LIST:
        case V_BYTES: return a.as.obj == b.as.obj;
        case V_FUNC: return a.as.func == b.as.func;
        default: return a.as.i == b.as.i;
    }
}

const char* aer_type_name (value_t v) {
    static const char* const type_names[] = { "nil", "bool", "number", "string", "list", "bytes", "function", "function", "number" };
    return type_names[v.type];
}

// building strings bit by bit (for str(), + and printing lists)
typedef struct {
    char* chars;
    uint32_t length;
    uint32_t capacity;
} text_t;

static void text_add (text_t* t, const char* s, uint32_t length) {
    if (t->length + length + 1 > t->capacity) {
        while (t->length + length + 1 > t->capacity) {
            t->capacity = t->capacity ? t->capacity * 2 : 32;
        }
        t->chars = aer_realloc(t->chars, t->capacity);
    }
    memcpy(t->chars + t->length, s, length);
    t->length += length;
    t->chars[t->length] = '\0';
}

// add v to t the way print() shows it, depth = how many lists deep we are
static void text_value (text_t* t, value_t v, int depth) {
    char number[16];
    switch (v.type) {
        case V_NIL: text_add(t, "nil", 3); break;
        case V_BOOL:
            if (v.as.i) {
                text_add(t, "true", 4);
            } else {
                text_add(t, "false", 5);
            }
            break;
        case V_INT:
            itoa(v.as.i, number, 10);
            text_add(t, number, (uint32_t)slen(number));
            break;
        case V_FLOAT: {
            char real[32];
            text_add(t, real, (uint32_t)format_real(v.as.f, real, sizeof(real)));
            break;
        }
        case V_STR:
            if (depth > 0) {
                text_add(t, "\"", 1); // strings inside a list are shown in quotes
            }
            text_add(t, v.as.obj->data.chars, v.as.obj->length);
            if (depth > 0) {
                text_add(t, "\"", 1);
            }
            break;
        case V_LIST:
            // lists more than 4 deep are cut short. That also stops a list that contains itself
            // from printing forever
            if (depth > 4) {
                text_add(t, "[...]", 5);
                break;
            }
            text_add(t, "[", 1);
            for (uint32_t i = 0; i < v.as.obj->length; i++) {
                if (i > 0) {
                    text_add(t, ", ", 2);
                }
                text_value(t, v.as.obj->data.items[i], depth + 1);
            }
            text_add(t, "]", 1);
            break;
        case V_BYTES: {
            char label[32];
            sfmt(label, sizeof(label), "<%u bytes>", v.as.obj->length);
            text_add(t, label, (uint32_t)slen(label));
            break;
        }
        case V_FUNC:
        case V_NATIVE:
            text_add(t, "<function>", 10);
            break;
    }
}

value_t aer_to_string (value_t v) {
    if (v.type == V_STR) {
        aer_retain(v);
        return v;
    }
    text_t t = { 0, 0, 0 };
    text_value(&t, v, 0);
    value_t s = aer_string(t.chars ? t.chars : "", t.length);
    aer_free(t.chars);
    return s;
}

// a string is written character by character (it may contain '\0' or % signs), anything else
// is turned into text first
void aer_print_value (value_t v) {
    if (v.type == V_STR) {
        for (uint32_t i = 0; i < v.as.obj->length; i++) {
            console_putchar_raw(v.as.obj->data.chars[i]);
        }
        console_update_cursor();
        return;
    }
    value_t s = aer_to_string(v);
    prtf("%s", s.as.obj->data.chars);
    aer_release(s);
}

// ---- variables --------------------------------------------------------------------------------
// A scope is a plain array searched from the start. Programs have few variables and names are
// compared by pointer, so this is faster in practice than anything cleverer

static aer_var_t* scope_find (aer_scope_t* scope, const char* name) {
    for (int i = 0; i < scope->count; i++) {
        if (scope->vars[i].name == name) {
            return &scope->vars[i];
        }
    }
    return 0;
}

// create (or replace) a variable, taking over the reference to value
static void scope_set (aer_scope_t* scope, const char* name, value_t value) {
    aer_var_t* var = scope_find(scope, name);
    if (var) {
        aer_release(var->value);
        var->value = value;
        return;
    }
    if (scope->count == scope->capacity) {
        scope->capacity = scope->capacity ? scope->capacity * 2 : 8;
        scope->vars = aer_realloc(scope->vars, (size_t)scope->capacity * sizeof(aer_var_t));
    }
    scope->vars[scope->count].name = name;
    scope->vars[scope->count].value = value;
    scope->count++;
}

static void scope_free (aer_scope_t* scope) {
    for (int i = 0; i < scope->count; i++) {
        aer_release(scope->vars[i].value);
    }
    aer_free(scope->vars);
}

void aer_define_global (const char* name, value_t value) {
    scope_set(&aer->globals, aer_intern(name, slen(name)), value);
}

// the function's own variables first, then the globals
static aer_var_t* find_variable (const char* name) {
    aer_var_t* var = aer->locals ? scope_find(aer->locals, name) : 0;
    return var ? var : scope_find(&aer->globals, name);
}

// a new variable in the current scope (the function's, or the program's at the top level)
static void declare (const char* name, value_t value) {
    scope_set(aer->locals ? aer->locals : &aer->globals, name, value);
}

// the closest name in a scope to `name`, if it is closer than *best_distance (see closest_name)
static void closest_in (const aer_scope_t* scope, const char* name, const char** best, int* best_distance) {
    for (int i = 0; i < scope->count; i++) {
        int distance = sdist(scope->vars[i].name, name);
        if (distance < *best_distance) {
            *best = scope->vars[i].name;
            *best_distance = distance;
        }
    }
}

// For an "unknown name" error: the name the program does have (a variable, a function or a
// built-in) that is most like the one it used, so the error can ask "did you mean print?".
// Writes " (did you mean print?)" into `out`, or "" if no name is close: one character out for
// a short name (3 letters or fewer), two for a longer one
static void did_you_mean (const char* name, char* out, size_t size) {
    int allowed = slen(name) <= 3 ? 1 : 2;
    int best_distance = allowed + 1;
    const char* best = NULL;
    if (aer->locals) {
        closest_in(aer->locals, name, &best, &best_distance);
    }
    closest_in(&aer->globals, name, &best, &best_distance);
    if (best != NULL) {
        sfmt(out, size, " (did you mean %s?)", best);
    } else {
        out[0] = '\0';
    }
}

// the value of a variable, retained because the caller gets its own reference
static value_t lookup (const char* name, int line) {
    aer_var_t* var = find_variable(name);
    if (var == 0) {
        char hint[48];
        did_you_mean(name, hint, sizeof(hint));
        aer_error(line, "unknown name '%s'%s", name, hint);
    }
    aer_retain(var->value);
    return var->value;
}

// ---- expressions ------------------------------------------------------------------------------

static value_t eval (aer_node_t* n);

// a whole number, for a list index or a for loop. A decimal is fine if it is whole (2.0); one
// that isn't (2.5) is an error rather than quietly rounded
static int32_t need_int (value_t v, int line, const char* what) {
    if (v.type == V_FLOAT && v.as.f == floor(v.as.f) && fabs(v.as.f) < 2147483648.0) {
        return (int32_t)v.as.f;
    }
    if (v.type == V_FLOAT) {
        aer_error(line, "%s must be a whole number, not %s (int() cuts off the decimals)", what,
                  v.as.f != v.as.f ? "nan" : "a decimal");
    }
    if (v.type != V_INT) {
        aer_error(line, "%s must be a number, not a %s", what, aer_type_name(v));
    }
    return v.as.i;
}

// target[index]: a list item, a one character string, or a byte as a number
static value_t index_get (value_t target, value_t index, int line) {
    if (!is_object(target)) {
        aer_error(line, "can't index a %s", aer_type_name(target));
    }
    int32_t i = need_int(index, line, "an index");
    if (i < 0 || (uint32_t)i >= target.as.obj->length) {
        aer_error(line, "index %d is out of range (length %u)", i, target.as.obj->length);
    }
    switch (target.type) {
        case V_LIST: {
            value_t item = target.as.obj->data.items[i];
            aer_retain(item);
            return item;
        }
        case V_STR: return aer_string(&target.as.obj->data.chars[i], 1);
        default: return aer_int(target.as.obj->data.bytes[i]);
    }
}

// + on two lists makes a new list, + with a string joins the text of both sides ("n = " + 5)
static value_t concat (value_t a, value_t b) {
    if (a.type == V_LIST && b.type == V_LIST) {
        value_t list = aer_list(a.as.obj->length + b.as.obj->length);
        for (int side = 0; side < 2; side++) {
            aer_object_t* from = side ? b.as.obj : a.as.obj;
            for (uint32_t i = 0; i < from->length; i++) {
                aer_retain(from->data.items[i]);
                aer_list_push(list.as.obj, from->data.items[i]);
            }
        }
        return list;
    }
    text_t t = { 0, 0, 0 };
    text_value(&t, a, 0);
    text_value(&t, b, 0);
    value_t s = aer_string(t.chars ? t.chars : "", t.length);
    aer_free(t.chars);
    return s;
}

// an operator on two numbers when at least one is a decimal: the answer is a decimal (or, for
// a comparison, true / false)
static value_t decimal_arithmetic (token_type_t op, double x, double y, int line) {
    switch (op) {
        case T_PLUS: return aer_float(x + y);
        case T_MINUS: return aer_float(x - y);
        case T_STAR: return aer_float(x * y);
        case T_SLASH:
        case T_PERCENT:
            if (y == 0) {
                aer_error(line, "division by zero");
            }
            return aer_float(op == T_SLASH ? x / y : fmod(x, y));
        case T_LT: return aer_bool(x < y);
        case T_LE: return aer_bool(x <= y);
        case T_GT: return aer_bool(x > y);
        case T_GE: return aer_bool(x >= y);
        default: aer_error(line, "unknown operator");
    }
}

// every binary operator (except and / or): joining, equality, string comparison, then numbers
static value_t arithmetic (token_type_t op, value_t a, value_t b, int line) {
    if (op == T_PLUS && (a.type == V_STR || b.type == V_STR || (a.type == V_LIST && b.type == V_LIST))) {
        return concat(a, b);
    }
    if (op == T_EQ) {
        return aer_bool(aer_equal(a, b));
    }
    if (op == T_NE) {
        return aer_bool(!aer_equal(a, b));
    }

    if (a.type == V_STR && b.type == V_STR) {
        int c = scmp(a.as.obj->data.chars, b.as.obj->data.chars);
        switch (op) {
            case T_LT: return aer_bool(c < 0);
            case T_LE: return aer_bool(c <= 0);
            case T_GT: return aer_bool(c > 0);
            case T_GE: return aer_bool(c >= 0);
            default: break;
        }
    }
    if (!aer_is_number(a) || !aer_is_number(b)) {
        aer_error(line, "can't use that operator on a %s and a %s", aer_type_name(a), aer_type_name(b));
    }
    if (a.type == V_FLOAT || b.type == V_FLOAT) {
        return decimal_arithmetic(op, aer_real(a), aer_real(b), line);
    }

    // two whole numbers: whole number maths. They wrap around at 32 bits instead of
    // overflowing, and / drops the remainder (7 / 2 is 3; 7 / 2.0 is 3.5)
    uint32_t x = (uint32_t)a.as.i;
    uint32_t y = (uint32_t)b.as.i;
    switch (op) {
        case T_PLUS: return aer_int((int32_t)(x + y));
        case T_MINUS: return aer_int((int32_t)(x - y));
        case T_STAR: return aer_int((int32_t)(x * y));
        case T_SLASH:
        case T_PERCENT:
            if (b.as.i == 0) {
                aer_error(line, "division by zero");
            }
            if (b.as.i == -1) {
                return aer_int(op == T_SLASH ? (int32_t)(0u - x) : 0); // INT_MIN / -1 would trap
            }
            return aer_int(op == T_SLASH ? a.as.i / b.as.i : a.as.i % b.as.i);
        case T_LT: return aer_bool(a.as.i < b.as.i);
        case T_LE: return aer_bool(a.as.i <= b.as.i);
        case T_GT: return aer_bool(a.as.i > b.as.i);
        case T_GE: return aer_bool(a.as.i >= b.as.i);
        default: aer_error(line, "unknown operator");
    }
}

// what exec() tells its caller to do next (see the top of the file)
typedef enum { EXEC_NEXT, EXEC_BREAK, EXEC_CONTINUE, EXEC_RETURN } exec_t;

static exec_t exec_block (aer_node_t* block);

// call a builtin (checking its argument count) or an AerLang function: the arguments become the
// function's local variables, its body runs with those as the local scope, and a "return"
// leaves its value in aer->return_value for us to pick up
static value_t call (value_t callee, int argc, value_t* argv, int line) {
    if (callee.type == V_NATIVE) {
        const aer_builtin_t* b = &aer_builtins[callee.as.i];
        if (argc < b->min_args || argc > b->max_args) {
            if (b->min_args == b->max_args) {
                aer_error(line, "%s() takes %d argument%s", b->name, b->min_args, b->min_args == 1 ? "" : "s");
            }
            aer_error(line, "%s() takes %d to %d arguments", b->name, b->min_args, b->max_args);
        }
        return b->fn(argc, argv, line);
    }
    if (callee.type != V_FUNC) {
        aer_error(line, "can't call a %s", aer_type_name(callee));
    }

    aer_node_t* f = callee.as.func;
    if (argc != f->count) {
        aer_error(line, "%s() takes %d argument%s", f->name, f->count, f->count == 1 ? "" : "s");
    }
    aer_scope_t scope = { 0, 0, 0 };
    for (int i = 0; i < argc; i++) {
        aer_retain(argv[i]);
        scope_set(&scope, f->params[i], argv[i]);
    }
    aer_scope_t* caller = aer->locals;
    aer->locals = &scope;
    exec_t result = exec_block(f->body);
    aer->locals = caller;
    scope_free(&scope);

    value_t value = aer_nil();
    if (result == EXEC_RETURN) {
        value = aer->return_value;
        aer->return_value = aer_nil();
    }
    return value;
}

// the value of an expression (owned by the caller, see the top of the file)
static value_t eval (aer_node_t* n) {
    check_stack(n->line);
    switch (n->type) {
        case N_NUMBER: return aer_int(n->number);
        case N_FLOAT: return aer_float(n->real);
        case N_STRING: aer_retain(n->value); return n->value;
        case N_NIL: return aer_nil();
        case N_TRUE: return aer_bool(true);
        case N_FALSE: return aer_bool(false);
        case N_NAME: return lookup(n->name, n->line);

        case N_LIST: {
            value_t list = aer_list((uint32_t)n->count);
            for (int i = 0; i < n->count; i++) {
                aer_list_push(list.as.obj, eval(n->items[i]));
            }
            return list;
        }

        case N_INDEX: {
            value_t target = eval(n->container);
            value_t index = eval(n->index);
            value_t item = index_get(target, index, n->line);
            aer_release(target);
            aer_release(index);
            return item;
        }

        case N_CALL: {
            value_t callee = eval(n->callee);
            value_t argv[AER_MAX_ARGS];
            for (int i = 0; i < n->count; i++) {
                argv[i] = eval(n->items[i]);
            }
            value_t result = call(callee, n->count, argv, n->line);
            for (int i = 0; i < n->count; i++) {
                aer_release(argv[i]);
            }
            aer_release(callee);
            return result;
        }

        case N_UNARY: {
            value_t v = eval(n->operand);
            if (n->op == T_NOT) {
                bool truth = aer_truthy(v);
                aer_release(v);
                return aer_bool(!truth);
            }
            if (v.type == V_FLOAT) {
                return aer_float(-v.as.f);
            }
            if (v.type != V_INT) {
                aer_error(n->line, "the value after - must be a number, not a %s", aer_type_name(v));
            }
            return aer_int((int32_t)(0u - (uint32_t)v.as.i));
        }

        // "and" / "or" give back one of their operands, so "x or 5" is x unless x is nil / false / 0
        case N_AND:
        case N_OR: {
            value_t left = eval(n->left);
            if (aer_truthy(left) == (n->type == N_OR)) {
                return left;
            }
            aer_release(left);
            return eval(n->right);
        }

        case N_BINARY: {
            value_t a = eval(n->left);
            value_t b = eval(n->right);
            value_t result = arithmetic(n->op, a, b, n->line);
            aer_release(a);
            aer_release(b);
            return result;
        }

        default:
            aer_error(n->line, "not an expression");
    }
}

// ---- statements -------------------------------------------------------------------------------

// target = value, where target is a variable (which must already exist, made with let) or a
// list item / byte (list[i] = x). Takes over the reference to value
static void assign (aer_node_t* target, value_t value) {
    if (target->type == N_NAME) {
        aer_var_t* var = find_variable(target->name);
        if (var == 0) {
            aer_release(value);
            char hint[48];
            did_you_mean(target->name, hint, sizeof(hint));
            if (hint[0] == '\0') {
                scpy(hint, " (make it with let)", sizeof(hint));
            }
            aer_error(target->line, "unknown variable '%s'%s", target->name, hint);
        }
        aer_release(var->value);
        var->value = value;
        return;
    }
    // a list item or a byte
    value_t container = eval(target->container);
    value_t index = eval(target->index);
    int32_t i = need_int(index, target->line, "an index");
    if (container.type != V_LIST && container.type != V_BYTES) {
        aer_error(target->line, "can't change items of a %s", aer_type_name(container));
    }
    if (i < 0 || (uint32_t)i >= container.as.obj->length) {
        aer_error(target->line, "index %d is out of range (length %u)", i, container.as.obj->length);
    }
    if (container.type == V_LIST) {
        aer_release(container.as.obj->data.items[i]);
        container.as.obj->data.items[i] = value;
    } else {
        container.as.obj->data.bytes[i] = (uint8_t)need_int(value, target->line, "a byte");
    }
    aer_release(container);
}

static exec_t exec (aer_node_t* n);

// run statements in order until one of them breaks, continues or returns
static exec_t exec_block (aer_node_t* block) {
    for (int i = 0; i < block->count; i++) {
        exec_t result = exec(block->items[i]);
        if (result != EXEC_NEXT) {
            return result;
        }
    }
    return EXEC_NEXT;
}

// a loop variable: reuse it if it already exists, otherwise make it in the current scope
static void set_loop_variable (const char* name, value_t value) {
    aer_var_t* var = find_variable(name);
    if (var) {
        aer_release(var->value);
        var->value = value;
    } else {
        declare(name, value);
    }
}

// carry out one statement
static exec_t exec (aer_node_t* n) {
    check_stack(n->line);
    switch (n->type) {
        case N_LET:
            declare(n->name, eval(n->new_value));
            return EXEC_NEXT;

        case N_ASSIGN:
            assign(n->target, eval(n->new_value));
            return EXEC_NEXT;

        case N_EXPR: {
            value_t v = eval(n->expression);
            if (aer->repl && aer->locals == 0 && v.type != V_NIL) {
                // at the prompt, show what an expression worked out to
                aer_print_value(v);
                prtf("\n");
            }
            aer_release(v);
            return EXEC_NEXT;
        }

        case N_IF: {
            value_t condition = eval(n->condition);
            bool truth = aer_truthy(condition);
            aer_release(condition);
            if (truth) {
                return exec_block(n->then_block);
            }
            if (n->else_block) {
                return n->else_block->type == N_IF ? exec(n->else_block) : exec_block(n->else_block);
            }
            return EXEC_NEXT;
        }

        case N_WHILE:
            for (;;) {
                value_t condition = eval(n->condition);
                bool truth = aer_truthy(condition);
                aer_release(condition);
                if (!truth) {
                    break;
                }
                exec_t result = exec_block(n->body);
                if (result == EXEC_BREAK) {
                    break;
                }
                if (result == EXEC_RETURN) {
                    return result;
                }
            }
            return EXEC_NEXT;

        case N_FOR: {
            value_t from = eval(n->from);
            value_t to = eval(n->to);
            value_t step = n->step ? eval(n->step) : aer_int(1);
            // 64 bit, so "for i = 0 to 2147483647" can't overflow and loop forever
            int64_t i = need_int(from, n->line, "the start of a for loop");
            int64_t last = need_int(to, n->line, "the end of a for loop");
            int64_t by = need_int(step, n->line, "the step of a for loop");
            if (by == 0) {
                aer_error(n->line, "a for loop can't step by 0");
            }
            // counting up runs while i <= last, counting down (a negative step) while i >= last
            for (;;) {
                bool finished;
                if (by > 0) {
                    finished = i > last;
                } else {
                    finished = i < last;
                }
                if (finished) {
                    break;
                }
                set_loop_variable(n->name, aer_int((int32_t)i));
                exec_t result = exec_block(n->body);
                if (result == EXEC_BREAK) {
                    break;
                }
                if (result == EXEC_RETURN) {
                    return result;
                }
                i += by; // (continue lands here too, and still moves on to the next number)
            }
            return EXEC_NEXT;
        }

        case N_FOR_IN: {
            value_t collection = eval(n->collection);
            if (!is_object(collection)) {
                aer_error(n->line, "can't loop over a %s", aer_type_name(collection));
            }
            exec_t result = EXEC_NEXT;
            // the length is checked every time round, so the body may add to or remove from a list
            for (uint32_t i = 0; i < collection.as.obj->length; i++) {
                set_loop_variable(n->name, index_get(collection, aer_int((int32_t)i), n->line));
                result = exec_block(n->body);
                if (result == EXEC_BREAK || result == EXEC_RETURN) {
                    break;
                }
            }
            aer_release(collection);
            return result == EXEC_RETURN ? result : EXEC_NEXT;
        }

        case N_FUNC:
            return EXEC_NEXT; // already defined before the program started

        case N_RETURN:
            aer->return_value = n->expression ? eval(n->expression) : aer_nil();
            return EXEC_RETURN;

        case N_BREAK: return EXEC_BREAK;
        case N_CONTINUE: return EXEC_CONTINUE;

        default:
            aer_error(n->line, "not a statement");
    }
}

// functions can be called before the line that defines them, so define them all up front
static void define_functions (aer_node_t* program) {
    for (int i = 0; i < program->count; i++) {
        aer_node_t* n = program->items[i];
        if (n->type == N_FUNC) {
            value_t f;
            f.type = V_FUNC;
            f.as.func = n;
            scope_set(&aer->globals, n->name, f);
        }
    }
}

// ---- running programs -------------------------------------------------------------------------

static aer_state_t state;

// ---- the shell's session, put away while a program runs ----------------------------------------
//
// The terminal runs lines that aren't commands as AerLang, all in one session that lasts from
// logging in to logging out (see "The shell" below). But only one session can be the running
// one: `state`, `names` and `blocks` above. So when a program starts, the shell's session is
// copied out of them into shell_copy, and copied back when the program ends. The program gets a
// clean slate and the shell's variables are all still there afterwards.
//
// (A program can't start another program, so one copy is all that's ever needed.)

typedef struct {
    aer_state_t state;
    name_entry_t* names[NAME_BUCKETS];
    aer_block_t* blocks;
} session_copy_t;

static bool shell_started = false;    // someone is logged in, and the shell has a session
static bool shell_put_away = false;   // a program is running: the shell's session is in shell_copy
static session_copy_t shell_copy;

static void put_shell_away (void) {
    shell_copy.state = state;
    memcpy(shell_copy.names, names, sizeof(names));
    shell_copy.blocks = blocks;
    shell_put_away = true;
}

static void bring_shell_back (void) {
    state = shell_copy.state;
    memcpy(names, shell_copy.names, sizeof(names));
    blocks = shell_copy.blocks;
    aer = &state;
    shell_put_away = false;
}

// a clean slate for a program: no variables, no allocations, then the builtins, the constants
// and args (the words typed after the program's name)
static void session_begin (const char* name, const char* args) {
    if (shell_started && !shell_put_away) {
        put_shell_away();
    }
    memset(&state, 0, sizeof(state));
    memset(names, 0, sizeof(names));
    blocks = 0;
    aer = &state;
    aer->name = name;
    kb_take_break(); // forget a Ctrl+C pressed before the program started
    fpu_enable();    // decimal numbers need the FPU (lib/fpu.h): on before any code runs

    // the builtin functions are ordinary global variables, so a program could even replace one
    for (int i = 0; i < aer_builtin_count; i++) {
        value_t f;
        f.type = V_NATIVE;
        f.as.i = i;
        aer_define_global(aer_builtins[i].name, f);
    }
    aer_define_constants();

    // the rest of the command line, as a list of words
    value_t list = aer_list(4);
    if (args) {
        const char* p = args;
        while (*p) {
            while (*p == ' ') {
                p++;
            }
            const char* start = p;
            while (*p && *p != ' ') {
                p++;
            }
            if (p > start) {
                aer_list_push(list.as.obj, aer_string(start, (uint32_t)(p - start)));
            }
        }
    }
    aer_define_global("args", list);
}

// leave graphics mode if the program used it, and free everything it allocated. Then the
// shell's session (if it was put away for the program) is the running one again
static void session_end (void) {
    aer_builtins_finish();
    aer_free_all();
    aer = 0;
    if (shell_put_away) {
        bring_shell_back();
    }
}

// lex, parse and run source. Kept out of run_program (noinline) so none of its local variables
// share a stack frame with the setjmp, where a longjmp could leave them with stale values
__attribute__((noinline)) static void parse_and_run (const char* source) {
    int count;
    token_t* tokens = aer_lex(source, &count);
    aer_node_t* program = aer_parse(tokens, count);

    // App mode: the whole program has been read, so every part of AerOS it can use is named in
    // it. Put the rest away before it starts (sys/appmode.h). Not for code typed at the prompt,
    // which might use anything on its next line
    if (!aer->shell) {
        aer->uses = aer_profile(tokens, count);
        appmode_begin(aer->uses);
    }
    define_functions(program);
    exec_block(program);
}

// run source in the current session. setjmp returns 0 the first time, then 1 if aer_error()
// jumps back here, or 2 if the program called exit()
static int run_program (const char* source) {
    aer->source = source;
    int jumped = setjmp(aer->error);
    if (jumped == 0) {
        parse_and_run(source);
    }
    return jumped;
}

int aer_run_source (const char* source, const char* name, const char* args) {
    session_begin(name, args);
    int jumped = run_program(source);
    session_end();
    return jumped == 1 ? -1 : 0;
}

bool aer_is_program_name (const char* name, size_t length) {
    if (length < 4) {
        return false;
    }
    const char* extension = name + length - 4;
    return strncasecmp(extension, ".aer", 4) == 0 || strncasecmp(extension, ".ren", 4) == 0;
}

const char* const aer_app_folders[] = { "/bin/", "/bin/games/", NULL };

// look for a program: as given, then with ".aer" added, in the current directory and then in
// each of aer_app_folders. Its source code (free() it), or NULL. folder_out and name_out get
// where it was found
static char* find_program (const char* path, char* folder_out, size_t folder_size, char* name_out, size_t name_size) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    for (int with_extension = 0; with_extension < 2; with_extension++) {
        char full[FS_PATH_MAX];
        scpy(full, path, sizeof(full));
        if (with_extension) {
            scat(full, ".aer", sizeof(full));
        }
        if (!fs_split_path(full, dir, sizeof(dir), name, sizeof(name))) {
            continue;
        }
        // the folder it was given in (the current one for a plain name), then the app folders
        // (not for a path like "games/snake": that only means where it says)
        for (int f = -1; f < 0 || aer_app_folders[f] != NULL; f++) {
            if (f >= 0 && strchr(path, '/')) {
                break;
            }
            const char* folder = f < 0 ? dir : aer_app_folders[f];
            char* source = fs_read_f(folder, name);
            if (source) {
                scpy(folder_out, folder, folder_size);
                scpy(name_out, name, name_size);
                return source;
            }
        }
    }
    return 0;
}

int aer_run_file (const char* path, const char* args) {
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    char* source = find_program(path, folder, sizeof(folder), name, sizeof(name));
    if (source == 0) {
        explain_say(PROBLEM_NOT_FOUND, path, "Program not found: %s", path);
        return -1;
    }
    // while it runs, it has to ask before it touches your files (sys/perm.h)
    perm_program_started(folder, name, args);
    int result = aer_run_source(source, name, args);
    perm_program_ended();
    free(source);
    return result;
}


// ---- the shell: AerLang at the terminal's prompt -----------------------------------------------
//
// A line typed at the prompt that isn't a command or an app is AerLang, run in the shell's
// session, which lasts until logging out. So the terminal is a programming language too:
//
//     guest@0//: let notes = 0
//     guest@0//: for f in la() do if find(f, ".txt") >= 0 then notes = notes + 1 end end
//     guest@0//: notes
//     3
//
// The value of an expression is shown (like "notes" above). A line that opens a block (an if,
// for, while or func without its end) carries on over more lines, see aer_open_blocks.
//
// Every line's code is kept until logging out (a function made on one line is used on later
// ones), so a very long session slowly fills memory: logging out and in again empties it.

void aer_shell_start (void) {
    if (shell_started) {
        return;
    }
    session_begin("prompt", 0);
    aer->repl = true;    // show the value of an expression
    aer->shell = true;   // cmd() works (see aer_builtins.c)
    shell_started = true;
}

// how many lines of shell code are running right now (more than one when cmd() runs code), and
// whether logging out asked for the session to end meanwhile
static int shell_lines_running = 0;
static bool shell_stop_wanted = false;

void aer_shell_stop (void) {
    if (!shell_started || shell_put_away) {
        return; // (not while a program is running: its session is the running one)
    }
    if (shell_lines_running > 0) {
        // cmd("out") logs out from inside a line of code: the session can't be freed under the
        // line's feet, so it ends once the line is done (see aer_shell_run)
        shell_stop_wanted = true;
        return;
    }
    shell_started = false;
    shell_stop_wanted = false;
    session_end();
}

// the global variable called text (length characters, not interned), or NULL
static aer_var_t* global_named (const char* text, size_t length) {
    for (int i = 0; i < aer->globals.count; i++) {
        const char* name = aer->globals.vars[i].name;
        if (strncmp(name, text, length) == 0 && name[length] == '\0') {
            return &aer->globals.vars[i];
        }
    }
    return NULL;
}

// can this character be in a word that looks like part of a command ("notes.txt", "/bin")?
static bool command_word_character (char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '/';
}

bool aer_shell_wants (const char* line) {
    if (!shell_started || shell_put_away) {
        return false; // (while a program runs, its session is the running one)
    }
    while (*line == ' ') {
        line++;
    }
    // a line that starts with anything but a letter (a number, a quote, a bracket) is code
    if (!isalpha((unsigned char)*line) && *line != '_') {
        return true;
    }

    // the first word, and what comes after it
    const char* word = line;
    const char* p = line;
    while (command_word_character(*p)) {
        p++;
    }
    size_t length = (size_t)(p - word);
    while (*p == ' ') {
        p++;
    }

    // "x = 1", "f(2)", "list[0]": the word is followed by something only code has
    bool next_could_be_a_command = *p == '\0' || command_word_character(*p) || *p == '"' || *p == '-';
    if (!next_could_be_a_command) {
        return true;
    }
    // It looks like a command: "frob", "dle notes.txt". It is still code if the word is one
    // AerLang knows: a keyword ("let x = 1", "for ...") or a variable made at the prompt ("x").
    // Not a function though: "print hello" is more likely a mistake than code
    if (aer_is_keyword(word, length)) {
        return true;
    }
    aer_var_t* variable = global_named(word, length);
    return variable != NULL && variable->value.type != V_FUNC && variable->value.type != V_NATIVE;
}

void aer_shell_run (const char* code) {
    if (!shell_started || shell_put_away) {
        return;
    }
    // Code at the prompt can run more code at the prompt: cmd("1 + 1") runs a terminal command,
    // which is AerLang. So whatever was running is put back afterwards: where an error jumps to,
    // the code, and the function that was running (if any)
    jmp_buf outer_error;
    memcpy(outer_error, aer->error, sizeof(jmp_buf));
    const char* outer_source = aer->source;
    aer_scope_t* outer_locals = aer->locals;

    aer->locals = 0;     // a line typed at the prompt runs at the top level
    shell_lines_running++;
    run_program(code);   // (exit() at the prompt has nothing to end, so it is the same as an error)
    shell_lines_running--;

    memcpy(aer->error, outer_error, sizeof(jmp_buf));
    aer->source = outer_source;
    aer->locals = outer_locals;
    aer_give_screen_back(); // a line that used graphics gives the screen back when it is done

    if (shell_stop_wanted && shell_lines_running == 0) {
        aer_shell_stop();
    }
}

// AER: a prompt where every line is AerLang, even one that looks like a command. It uses the
// shell's session, so the variables are the same ones the terminal's prompt sees
void aer_repl (void) {
    if (console_capturing()) {
        return; // run by cmd(), where nothing can be typed: it would wait for ever
    }
    char line[256];
    prtf("AerLang - type code to run it, \"exit\" to leave.\n");
    bool own_session = !shell_started; // (only if the terminal hasn't started the shell)
    if (own_session) {
        aer_shell_start();
    }
    for (;;) {
        // if the last line ended without a newline (write() does that), start a fresh one
        uint8_t x, y;
        console_get_cursor(&x, &y);
        if (x != 0) {
            prtf("\n");
        }
        colour(LIGHT_CYAN, console_bg);
        prtf("aer> ");
        colour(BRIGHT, console_bg);
        getln(line, sizeof(line));
        if (scmp(trim(line), "exit") == 0) {
            break;
        }
        aer_shell_run(line);
    }
    if (own_session) {
        aer_shell_stop();
    }
}
