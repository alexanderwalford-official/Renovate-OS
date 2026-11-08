/*
    ren_interp.c
    Runs a RenLang program by walking its syntax tree.

    eval() works out the value of an expression and exec() carries out a statement, each a switch
    over the node types that calls itself for the node's children.

    Reference counting. Strings, lists and bytes are objects shared by reference, with a count of
    how many places hold them. The rules every function here follows:
      - a value_t that eval() returns is "owned": the caller must either ren_release() it or hand
        it to something that keeps it (a variable, a list item)
      - storing a value somewhere (scope_set, ren_list_push) takes over the caller's reference
      - reading a value out of a variable or list without taking it away needs ren_retain()
    When a count reaches 0 the object is freed at once (and a list releases its items).

    Scopes. There are only two places a name can live: the program's globals, and the locals of
    the function that is running (no nesting and no closures, a function sees its own variables
    and the globals). Builtins like print are ordinary globals.

    Control flow. exec() returns what should happen next: EXEC_NEXT (carry on), or EXEC_BREAK /
    EXEC_CONTINUE / EXEC_RETURN, which every block passes straight back up until a loop or a
    function call deals with it. So "break" is just a return value, no jumps needed.

    Errors. ren_error() longjmps back to the setjmp in run_program(), straight out of however
    deep the program was. Everything it allocated is in the ren_alloc list and freed in one go
    by session_end(), so nothing leaks however the program ends.
*/

#include "ren_internal.h"
#include "renlang.h"
#include <stdarg.h>
#include "../lib/ren_string.h"
#include "../lib/ren_stdlib.h"
#include "../lib/ren_stdio.h"
#include "../lib/primary_definitions.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../sys/f_sys.h"

#define STACK_RESERVE (32 * 1024) // stop a runaway program before it uses the last 32KB of stack

extern char stack[]; // the bottom of the kernel stack (entry.asm)

ren_state_t* ren = 0;

// ---- memory -----------------------------------------------------------------------------------
//
// Every allocation has a small header in front of it linking it into one list:
//
//     blocks -> [header|data] <-> [header|data] <-> [header|data] -> NULL
//                       ^ the pointer ren_alloc hands out
//
// so ren_free_all() can give everything back when the program ends, even after an error left
// things half built. ren_free() unlinks one block early (used by reference counting).

typedef struct ren_block {
    struct ren_block* prev;
    struct ren_block* next;
    size_t size;
} ren_block_t;

static ren_block_t* blocks = 0; // every allocation of the running program

// out of memory is a RenLang error like any other, so callers never have to check for NULL
void* ren_alloc (size_t size) {
    ren_block_t* b = malloc(sizeof(ren_block_t) + size);
    if (b == 0) {
        ren_error(0, "out of memory");
    }
    b->prev = 0;
    b->next = blocks;
    b->size = size;
    if (blocks) blocks->prev = b;
    blocks = b;
    return b + 1;
}

void ren_free (void* ptr) {
    if (ptr == 0) return;
    ren_block_t* b = (ren_block_t*)ptr - 1;
    if (b->prev) b->prev->next = b->next;
    else blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    free(b);
}

// a new block and a copy, rather than the heap's realloc, so the block list stays right
void* ren_realloc (void* ptr, size_t size) {
    void* bigger = ren_alloc(size);
    if (ptr) {
        size_t old = ((ren_block_t*)ptr - 1)->size;
        memcpy(bigger, ptr, old < size ? old : size);
        ren_free(ptr);
    }
    return bigger;
}

void ren_free_all (void) {
    while (blocks) {
        ren_block_t* next = blocks->next;
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

const char* ren_intern (const char* text, size_t length) {
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
    name_entry_t* e = ren_alloc(sizeof(name_entry_t) + length + 1);
    memcpy(e->text, text, length);
    e->text[length] = '\0';
    e->next = *bucket;
    *bucket = e;
    return e->text;
}

// ---- errors -----------------------------------------------------------------------------------

// print "Error in <file> on line <n>: <message>" in red, then jump out of the program
void ren_error (int line, const char* format, ...) {
    va_list ap;
    enum color old_fg = console_fg;
    console_set_color(LIGHT_RED, console_bg);
    if (line > 0) {
        printf("Error in %s on line %d: ", ren->name, line);
    } else {
        printf("Error in %s: ", ren->name);
    }
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
    printf("\n");
    console_set_color(old_fg, console_bg);
    longjmp(ren->error, 1);
}

// called for every node evaluated: stops deep recursion before it overflows the kernel stack
// (which everything shares), and notices Ctrl+C so a stuck loop can always be stopped
static void check_stack (int line) {
    char here;
    if ((uintptr_t)&here < (uintptr_t)stack + STACK_RESERVE) {
        ren_error(line, "too much recursion");
    }
    if (kb_take_break()) {
        ren_error(line, "stopped (Ctrl+C)");
    }
}

// ---- values -----------------------------------------------------------------------------------

// a new object with one reference (the caller's)
static ren_object_t* new_object (value_type_t type) {
    ren_object_t* o = ren_alloc(sizeof(ren_object_t));
    memset(o, 0, sizeof(*o));
    o->refs = 1;
    o->type = type;
    return o;
}

static value_t object_value (ren_object_t* o) {
    value_t v;
    v.type = o->type;
    v.as.obj = o;
    return v;
}

value_t ren_string (const char* chars, uint32_t length) {
    ren_object_t* o = new_object(V_STR);
    o->data.chars = ren_alloc(length + 1);
    memcpy(o->data.chars, chars, length);
    o->data.chars[length] = '\0';
    o->length = length;
    return object_value(o);
}

value_t ren_list (uint32_t capacity) {
    ren_object_t* o = new_object(V_LIST);
    o->capacity = capacity ? capacity : 4;
    o->data.items = ren_alloc(o->capacity * sizeof(value_t));
    return object_value(o);
}

value_t ren_bytes (uint32_t length) {
    ren_object_t* o = new_object(V_BYTES);
    o->data.bytes = ren_alloc(length ? length : 1);
    memset(o->data.bytes, 0, length);
    o->length = length;
    return object_value(o);
}

// add to the end of a list, doubling its array when full (so pushing n items costs about n copies)
void ren_list_push (ren_object_t* list, value_t item) {
    if (list->length == list->capacity) {
        list->capacity *= 2;
        list->data.items = ren_realloc(list->data.items, list->capacity * sizeof(value_t));
    }
    list->data.items[list->length++] = item;
}

static bool is_object (value_t v) {
    return v.type == V_STR || v.type == V_LIST || v.type == V_BYTES;
}

void ren_retain (value_t v) {
    if (is_object(v)) v.as.obj->refs++;
}

void ren_release (value_t v) {
    if (!is_object(v) || --v.as.obj->refs > 0) return;
    ren_object_t* o = v.as.obj;
    if (o->type == V_LIST) {
        for (uint32_t i = 0; i < o->length; i++) {
            ren_release(o->data.items[i]);
        }
    }
    ren_free(o->data.chars); // the same pointer whichever kind it is
    ren_free(o);
}

// what counts as "true" in if / while / and / or: everything except nil, false and 0
bool ren_truthy (value_t v) {
    switch (v.type) {
        case V_NIL: return false;
        case V_BOOL:
        case V_INT: return v.as.i != 0;
        default: return true;
    }
}

// == compares strings by their characters, but lists and bytes by identity (the same object)
bool ren_equal (value_t a, value_t b) {
    if (a.type != b.type) return false;
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

const char* ren_type_name (value_t v) {
    static const char* const type_names[] = { "nil", "bool", "number", "string", "list", "bytes", "function", "function" };
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
        t->chars = ren_realloc(t->chars, t->capacity);
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
        case V_BOOL: text_add(t, v.as.i ? "true" : "false", v.as.i ? 4 : 5); break;
        case V_INT:
            itoa(v.as.i, number, 10);
            text_add(t, number, (uint32_t)strlen(number));
            break;
        case V_STR:
            if (depth > 0) text_add(t, "\"", 1); // strings inside a list are shown in quotes
            text_add(t, v.as.obj->data.chars, v.as.obj->length);
            if (depth > 0) text_add(t, "\"", 1);
            break;
        case V_LIST:
            if (depth > 4) { text_add(t, "[...]", 5); break; } // also stops a list that contains itself
            text_add(t, "[", 1);
            for (uint32_t i = 0; i < v.as.obj->length; i++) {
                if (i > 0) text_add(t, ", ", 2);
                text_value(t, v.as.obj->data.items[i], depth + 1);
            }
            text_add(t, "]", 1);
            break;
        case V_BYTES: {
            char label[32];
            snprintf(label, sizeof(label), "<%u bytes>", v.as.obj->length);
            text_add(t, label, (uint32_t)strlen(label));
            break;
        }
        case V_FUNC:
        case V_NATIVE:
            text_add(t, "<function>", 10);
            break;
    }
}

value_t ren_to_string (value_t v) {
    if (v.type == V_STR) {
        ren_retain(v);
        return v;
    }
    text_t t = { 0, 0, 0 };
    text_value(&t, v, 0);
    value_t s = ren_string(t.chars ? t.chars : "", t.length);
    ren_free(t.chars);
    return s;
}

// a string is written character by character (it may contain '\0' or % signs), anything else
// is turned into text first
void ren_print_value (value_t v) {
    if (v.type == V_STR) {
        for (uint32_t i = 0; i < v.as.obj->length; i++) {
            console_putchar_raw(v.as.obj->data.chars[i]);
        }
        console_update_cursor();
        return;
    }
    value_t s = ren_to_string(v);
    printf("%s", s.as.obj->data.chars);
    ren_release(s);
}

// ---- variables --------------------------------------------------------------------------------
// A scope is a plain array searched from the start. Programs have few variables and names are
// compared by pointer, so this is faster in practice than anything cleverer

static ren_var_t* scope_find (ren_scope_t* scope, const char* name) {
    for (int i = 0; i < scope->count; i++) {
        if (scope->vars[i].name == name) return &scope->vars[i];
    }
    return 0;
}

// create (or replace) a variable, taking over the reference to value
static void scope_set (ren_scope_t* scope, const char* name, value_t value) {
    ren_var_t* var = scope_find(scope, name);
    if (var) {
        ren_release(var->value);
        var->value = value;
        return;
    }
    if (scope->count == scope->capacity) {
        scope->capacity = scope->capacity ? scope->capacity * 2 : 8;
        scope->vars = ren_realloc(scope->vars, (size_t)scope->capacity * sizeof(ren_var_t));
    }
    scope->vars[scope->count].name = name;
    scope->vars[scope->count].value = value;
    scope->count++;
}

static void scope_free (ren_scope_t* scope) {
    for (int i = 0; i < scope->count; i++) {
        ren_release(scope->vars[i].value);
    }
    ren_free(scope->vars);
}

void ren_define_global (const char* name, value_t value) {
    scope_set(&ren->globals, ren_intern(name, strlen(name)), value);
}

// the function's own variables first, then the globals
static ren_var_t* find_variable (const char* name) {
    ren_var_t* var = ren->locals ? scope_find(ren->locals, name) : 0;
    return var ? var : scope_find(&ren->globals, name);
}

// a new variable in the current scope (the function's, or the program's at the top level)
static void declare (const char* name, value_t value) {
    scope_set(ren->locals ? ren->locals : &ren->globals, name, value);
}

// the value of a variable, retained because the caller gets its own reference
static value_t lookup (const char* name, int line) {
    ren_var_t* var = find_variable(name);
    if (var == 0) {
        ren_error(line, "unknown name '%s'", name);
    }
    ren_retain(var->value);
    return var->value;
}

// ---- expressions ------------------------------------------------------------------------------

static value_t eval (ren_node_t* n);

static int32_t need_int (value_t v, int line, const char* what) {
    if (v.type != V_INT) ren_error(line, "%s must be a number, not a %s", what, ren_type_name(v));
    return v.as.i;
}

// target[index]: a list item, a one character string, or a byte as a number
static value_t index_get (value_t target, value_t index, int line) {
    if (!is_object(target)) ren_error(line, "can't index a %s", ren_type_name(target));
    int32_t i = need_int(index, line, "an index");
    if (i < 0 || (uint32_t)i >= target.as.obj->length) {
        ren_error(line, "index %d is out of range (length %u)", i, target.as.obj->length);
    }
    switch (target.type) {
        case V_LIST: {
            value_t item = target.as.obj->data.items[i];
            ren_retain(item);
            return item;
        }
        case V_STR: return ren_string(&target.as.obj->data.chars[i], 1);
        default: return ren_int(target.as.obj->data.bytes[i]);
    }
}

// + on two lists makes a new list, + with a string joins the text of both sides ("n = " + 5)
static value_t concat (value_t a, value_t b) {
    if (a.type == V_LIST && b.type == V_LIST) {
        value_t list = ren_list(a.as.obj->length + b.as.obj->length);
        for (int side = 0; side < 2; side++) {
            ren_object_t* from = side ? b.as.obj : a.as.obj;
            for (uint32_t i = 0; i < from->length; i++) {
                ren_retain(from->data.items[i]);
                ren_list_push(list.as.obj, from->data.items[i]);
            }
        }
        return list;
    }
    text_t t = { 0, 0, 0 };
    text_value(&t, a, 0);
    text_value(&t, b, 0);
    value_t s = ren_string(t.chars ? t.chars : "", t.length);
    ren_free(t.chars);
    return s;
}

// every binary operator (except and / or): joining, equality, string comparison, then numbers
static value_t arithmetic (token_type_t op, value_t a, value_t b, int line) {
    if (op == T_PLUS && (a.type == V_STR || b.type == V_STR || (a.type == V_LIST && b.type == V_LIST))) {
        return concat(a, b);
    }
    if (op == T_EQ) return ren_bool(ren_equal(a, b));
    if (op == T_NE) return ren_bool(!ren_equal(a, b));

    if (a.type == V_STR && b.type == V_STR) {
        int c = strcmp(a.as.obj->data.chars, b.as.obj->data.chars);
        switch (op) {
            case T_LT: return ren_bool(c < 0);
            case T_LE: return ren_bool(c <= 0);
            case T_GT: return ren_bool(c > 0);
            case T_GE: return ren_bool(c >= 0);
            default: break;
        }
    }
    if (a.type != V_INT || b.type != V_INT) {
        ren_error(line, "can't use that operator on a %s and a %s", ren_type_name(a), ren_type_name(b));
    }

    // numbers wrap around at 32 bits instead of overflowing
    uint32_t x = (uint32_t)a.as.i;
    uint32_t y = (uint32_t)b.as.i;
    switch (op) {
        case T_PLUS: return ren_int((int32_t)(x + y));
        case T_MINUS: return ren_int((int32_t)(x - y));
        case T_STAR: return ren_int((int32_t)(x * y));
        case T_SLASH:
        case T_PERCENT:
            if (b.as.i == 0) ren_error(line, "division by zero");
            if (b.as.i == -1) return ren_int(op == T_SLASH ? (int32_t)(0u - x) : 0); // INT_MIN / -1 would trap
            return ren_int(op == T_SLASH ? a.as.i / b.as.i : a.as.i % b.as.i);
        case T_LT: return ren_bool(a.as.i < b.as.i);
        case T_LE: return ren_bool(a.as.i <= b.as.i);
        case T_GT: return ren_bool(a.as.i > b.as.i);
        case T_GE: return ren_bool(a.as.i >= b.as.i);
        default: ren_error(line, "unknown operator");
    }
}

// what exec() tells its caller to do next (see the top of the file)
typedef enum { EXEC_NEXT, EXEC_BREAK, EXEC_CONTINUE, EXEC_RETURN } exec_t;

static exec_t exec_block (ren_node_t* block);

// call a builtin (checking its argument count) or a RenLang function: the arguments become the
// function's local variables, its body runs with those as the local scope, and a "return"
// leaves its value in ren->return_value for us to pick up
static value_t call (value_t callee, int argc, value_t* argv, int line) {
    if (callee.type == V_NATIVE) {
        const ren_builtin_t* b = &ren_builtins[callee.as.i];
        if (argc < b->min_args || argc > b->max_args) {
            if (b->min_args == b->max_args) {
                ren_error(line, "%s() takes %d argument%s", b->name, b->min_args, b->min_args == 1 ? "" : "s");
            }
            ren_error(line, "%s() takes %d to %d arguments", b->name, b->min_args, b->max_args);
        }
        return b->fn(argc, argv, line);
    }
    if (callee.type != V_FUNC) {
        ren_error(line, "can't call a %s", ren_type_name(callee));
    }

    ren_node_t* f = callee.as.func;
    if (argc != f->count) {
        ren_error(line, "%s() takes %d argument%s", f->name, f->count, f->count == 1 ? "" : "s");
    }
    ren_scope_t scope = { 0, 0, 0 };
    for (int i = 0; i < argc; i++) {
        ren_retain(argv[i]);
        scope_set(&scope, f->params[i], argv[i]);
    }
    ren_scope_t* caller = ren->locals;
    ren->locals = &scope;
    exec_t result = exec_block(f->b);
    ren->locals = caller;
    scope_free(&scope);

    value_t value = ren_nil();
    if (result == EXEC_RETURN) {
        value = ren->return_value;
        ren->return_value = ren_nil();
    }
    return value;
}

// the value of an expression (owned by the caller, see the top of the file)
static value_t eval (ren_node_t* n) {
    check_stack(n->line);
    switch (n->type) {
        case N_NUMBER: return ren_int(n->number);
        case N_STRING: ren_retain(n->value); return n->value;
        case N_NIL: return ren_nil();
        case N_TRUE: return ren_bool(true);
        case N_FALSE: return ren_bool(false);
        case N_NAME: return lookup(n->name, n->line);

        case N_LIST: {
            value_t list = ren_list((uint32_t)n->count);
            for (int i = 0; i < n->count; i++) {
                ren_list_push(list.as.obj, eval(n->items[i]));
            }
            return list;
        }

        case N_INDEX: {
            value_t target = eval(n->a);
            value_t index = eval(n->b);
            value_t item = index_get(target, index, n->line);
            ren_release(target);
            ren_release(index);
            return item;
        }

        case N_CALL: {
            value_t callee = eval(n->a);
            value_t argv[REN_MAX_ARGS];
            for (int i = 0; i < n->count; i++) {
                argv[i] = eval(n->items[i]);
            }
            value_t result = call(callee, n->count, argv, n->line);
            for (int i = 0; i < n->count; i++) {
                ren_release(argv[i]);
            }
            ren_release(callee);
            return result;
        }

        case N_UNARY: {
            value_t v = eval(n->a);
            if (n->op == T_NOT) {
                bool truth = ren_truthy(v);
                ren_release(v);
                return ren_bool(!truth);
            }
            return ren_int((int32_t)(0u - (uint32_t)need_int(v, n->line, "the value after -")));
        }

        // "and" / "or" give back one of their operands, so "x or 5" is x unless x is nil / false / 0
        case N_AND:
        case N_OR: {
            value_t left = eval(n->a);
            if (ren_truthy(left) == (n->type == N_OR)) return left;
            ren_release(left);
            return eval(n->b);
        }

        case N_BINARY: {
            value_t a = eval(n->a);
            value_t b = eval(n->b);
            value_t result = arithmetic(n->op, a, b, n->line);
            ren_release(a);
            ren_release(b);
            return result;
        }

        default:
            ren_error(n->line, "not an expression");
    }
}

// ---- statements -------------------------------------------------------------------------------

// target = value, where target is a variable (which must already exist, made with let) or a
// list item / byte (list[i] = x). Takes over the reference to value
static void assign (ren_node_t* target, value_t value) {
    if (target->type == N_NAME) {
        ren_var_t* var = find_variable(target->name);
        if (var == 0) {
            ren_release(value);
            ren_error(target->line, "unknown variable '%s' (make it with let)", target->name);
        }
        ren_release(var->value);
        var->value = value;
        return;
    }
    // a list item or a byte
    value_t container = eval(target->a);
    value_t index = eval(target->b);
    int32_t i = need_int(index, target->line, "an index");
    if (container.type != V_LIST && container.type != V_BYTES) {
        ren_error(target->line, "can't change items of a %s", ren_type_name(container));
    }
    if (i < 0 || (uint32_t)i >= container.as.obj->length) {
        ren_error(target->line, "index %d is out of range (length %u)", i, container.as.obj->length);
    }
    if (container.type == V_LIST) {
        ren_release(container.as.obj->data.items[i]);
        container.as.obj->data.items[i] = value;
    } else {
        container.as.obj->data.bytes[i] = (uint8_t)need_int(value, target->line, "a byte");
    }
    ren_release(container);
}

static exec_t exec (ren_node_t* n);

// run statements in order until one of them breaks, continues or returns
static exec_t exec_block (ren_node_t* block) {
    for (int i = 0; i < block->count; i++) {
        exec_t result = exec(block->items[i]);
        if (result != EXEC_NEXT) return result;
    }
    return EXEC_NEXT;
}

// a loop variable: reuse it if it already exists, otherwise make it in the current scope
static void set_loop_variable (const char* name, value_t value) {
    ren_var_t* var = find_variable(name);
    if (var) {
        ren_release(var->value);
        var->value = value;
    } else {
        declare(name, value);
    }
}

// carry out one statement
static exec_t exec (ren_node_t* n) {
    check_stack(n->line);
    switch (n->type) {
        case N_LET:
            declare(n->name, eval(n->b));
            return EXEC_NEXT;

        case N_ASSIGN:
            assign(n->a, eval(n->b));
            return EXEC_NEXT;

        case N_EXPR: {
            value_t v = eval(n->a);
            if (ren->repl && ren->locals == 0 && v.type != V_NIL) {
                // at the prompt, show what an expression worked out to
                ren_print_value(v);
                printf("\n");
            }
            ren_release(v);
            return EXEC_NEXT;
        }

        case N_IF: {
            value_t condition = eval(n->a);
            bool truth = ren_truthy(condition);
            ren_release(condition);
            if (truth) return exec_block(n->b);
            if (n->c) return n->c->type == N_IF ? exec(n->c) : exec_block(n->c);
            return EXEC_NEXT;
        }

        case N_WHILE:
            for (;;) {
                value_t condition = eval(n->a);
                bool truth = ren_truthy(condition);
                ren_release(condition);
                if (!truth) break;
                exec_t result = exec_block(n->b);
                if (result == EXEC_BREAK) break;
                if (result == EXEC_RETURN) return result;
            }
            return EXEC_NEXT;

        case N_FOR: {
            value_t from = eval(n->a);
            value_t to = eval(n->c);
            value_t step = n->d ? eval(n->d) : ren_int(1);
            // 64 bit, so "for i = 0 to 2147483647" can't overflow and loop forever
            int64_t i = need_int(from, n->line, "the start of a for loop");
            int64_t last = need_int(to, n->line, "the end of a for loop");
            int64_t by = need_int(step, n->line, "the step of a for loop");
            if (by == 0) ren_error(n->line, "a for loop can't step by 0");
            for (; by > 0 ? i <= last : i >= last; i += by) {
                set_loop_variable(n->name, ren_int((int32_t)i));
                exec_t result = exec_block(n->b);
                if (result == EXEC_BREAK) break;
                if (result == EXEC_RETURN) return result;
            }
            return EXEC_NEXT;
        }

        case N_FOR_IN: {
            value_t collection = eval(n->a);
            if (!is_object(collection)) {
                ren_error(n->line, "can't loop over a %s", ren_type_name(collection));
            }
            exec_t result = EXEC_NEXT;
            // the length is checked every time round, so the body may add to or remove from a list
            for (uint32_t i = 0; i < collection.as.obj->length; i++) {
                set_loop_variable(n->name, index_get(collection, ren_int((int32_t)i), n->line));
                result = exec_block(n->b);
                if (result == EXEC_BREAK || result == EXEC_RETURN) break;
            }
            ren_release(collection);
            return result == EXEC_RETURN ? result : EXEC_NEXT;
        }

        case N_FUNC:
            return EXEC_NEXT; // already defined before the program started

        case N_RETURN:
            ren->return_value = n->a ? eval(n->a) : ren_nil();
            return EXEC_RETURN;

        case N_BREAK: return EXEC_BREAK;
        case N_CONTINUE: return EXEC_CONTINUE;

        default:
            ren_error(n->line, "not a statement");
    }
}

// functions can be called before the line that defines them, so define them all up front
static void define_functions (ren_node_t* program) {
    for (int i = 0; i < program->count; i++) {
        ren_node_t* n = program->items[i];
        if (n->type == N_FUNC) {
            value_t f;
            f.type = V_FUNC;
            f.as.func = n;
            scope_set(&ren->globals, n->name, f);
        }
    }
}

// ---- running programs -------------------------------------------------------------------------

static ren_state_t state;

// a clean slate for a program: no variables, no allocations, then the builtins, the constants
// and args (the words typed after the program's name)
static void session_begin (const char* name, const char* args) {
    memset(&state, 0, sizeof(state));
    memset(names, 0, sizeof(names));
    blocks = 0;
    ren = &state;
    ren->name = name;
    kb_take_break(); // forget a Ctrl+C pressed before the program started

    // the builtin functions are ordinary global variables, so a program could even replace one
    for (int i = 0; i < ren_builtin_count; i++) {
        value_t f;
        f.type = V_NATIVE;
        f.as.i = i;
        ren_define_global(ren_builtins[i].name, f);
    }
    ren_define_constants();

    // the rest of the command line, as a list of words
    value_t list = ren_list(4);
    if (args) {
        const char* p = args;
        while (*p) {
            while (*p == ' ') p++;
            const char* start = p;
            while (*p && *p != ' ') p++;
            if (p > start) ren_list_push(list.as.obj, ren_string(start, (uint32_t)(p - start)));
        }
    }
    ren_define_global("args", list);
}

// leave graphics mode if the program used it, and free everything it allocated
static void session_end (void) {
    ren_builtins_finish();
    ren_free_all();
    ren = 0;
}

// lex, parse and run source. Kept out of run_program (noinline) so none of its local variables
// share a stack frame with the setjmp, where a longjmp could leave them with stale values
__attribute__((noinline)) static void parse_and_run (const char* source) {
    int count;
    token_t* tokens = ren_lex(source, &count);
    ren_node_t* program = ren_parse(tokens, count);
    define_functions(program);
    exec_block(program);
}

// run source in the current session. setjmp returns 0 the first time, then 1 if ren_error()
// jumps back here, or 2 if the program called exit()
static int run_program (const char* source) {
    int jumped = setjmp(ren->error);
    if (jumped == 0) {
        parse_and_run(source);
    }
    return jumped;
}

int ren_run_source (const char* source, const char* name, const char* args) {
    session_begin(name, args);
    int jumped = run_program(source);
    session_end();
    return jumped == 1 ? -1 : 0;
}

// look for a program: as given, then with ".ren" added, in the current directory and then /bin/
static char* find_program (const char* path, char* name_out, size_t name_size) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    const char* folders[2] = { 0, "/bin/" };
    for (int with_extension = 0; with_extension < 2; with_extension++) {
        char full[FS_PATH_MAX];
        strlcpy(full, path, sizeof(full));
        if (with_extension) strlcat(full, ".ren", sizeof(full));
        if (!fs_split_path(full, dir, sizeof(dir), name, sizeof(name))) continue;
        for (int f = 0; f < 2; f++) {
            if (f == 1 && strchr(path, '/')) break; // an explicit path isn't searched for in /bin/
            const char* folder = folders[f] ? folders[f] : dir;
            char* source = fs_read_f(folder, name);
            if (source) {
                strlcpy(name_out, name, name_size);
                return source;
            }
        }
    }
    return 0;
}

int ren_run_file (const char* path, const char* args) {
    char name[FS_NAME_MAX];
    char* source = find_program(path, name, sizeof(name));
    if (source == 0) {
        printf("Program not found: %s\n", path);
        return -1;
    }
    int result = ren_run_source(source, name, args);
    free(source);
    return result;
}

// the "ren" prompt: one session for the whole time, so variables and functions typed on one
// line are still there on the next
void ren_repl (void) {
    char line[256];
    printf("RenLang - type code to run it, \"exit\" to leave.\n");
    session_begin("prompt", 0);
    ren->repl = true;
    for (;;) {
        // if the last line ended without a newline (write() does that), start a fresh one
        uint8_t x, y;
        console_get_cursor(&x, &y);
        if (x != 0) printf("\n");
        console_set_color(LIGHT_CYAN, console_bg);
        printf("ren> ");
        console_set_color(BRIGHT, console_bg);
        GetString(line, sizeof(line));
        if (strcmp(StrTrim(line), "exit") == 0) break;
        run_program(line);
        ren->locals = 0; // in case an error happened inside a function
    }
    session_end();
}
