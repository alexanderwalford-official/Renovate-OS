/*
    ren_internal.h
    The pieces of the RenLang interpreter shared between its files:

        source text --ren_lexer.c--> tokens --ren_parser.c--> syntax tree --ren_interp.c--> runs it
                                                                    calls ren_builtins.c for print() etc.

    Memory: everything a program allocates goes through ren_alloc, which keeps a list of it, so
    when the program ends (normally or with an error half way through) ren_free_all gives it all
    back. While it runs, strings, lists and byte buffers are reference counted, so a long running
    program that keeps making new strings doesn't fill up memory.

    Errors: ren_error prints the message and longjmps out to the start of the run, abandoning
    whatever was half done. That is why nothing has to clean up on the way out of an error.
*/
#ifndef REN_INTERNAL_H
#define REN_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../lib/ren_setjmp.h"

#define REN_MAX_ARGS 16 // most arguments a function call can pass

typedef enum {
    T_EOF, T_NEWLINE, T_NUMBER, T_STRING, T_NAME,
    // keywords
    T_LET, T_FUNC, T_RETURN, T_IF, T_THEN, T_ELIF, T_ELSE, T_END, T_WHILE, T_DO, T_FOR, T_TO,
    T_STEP, T_IN, T_BREAK, T_CONTINUE, T_AND, T_OR, T_NOT, T_TRUE, T_FALSE, T_NIL,
    // symbols
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_ASSIGN,
    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_COMMA
} token_type_t;

typedef struct {
    token_type_t type;
    int line;
    int32_t number;     // T_NUMBER
    const char* text;   // T_NAME: the interned name, T_STRING: the characters
    uint32_t length;    // T_STRING: how many characters
} token_t;

typedef enum { V_NIL, V_BOOL, V_INT, V_STR, V_LIST, V_BYTES, V_FUNC, V_NATIVE } value_type_t;

struct ren_object;
struct ren_node;

// a RenLang value. Numbers, booleans and nil live inside it, strings / lists / bytes point to an object
typedef struct {
    value_type_t type;
    union {
        int32_t i;              // V_INT, V_BOOL (0 or 1), V_NATIVE (index into the builtin table)
        struct ren_object* obj; // V_STR, V_LIST, V_BYTES
        struct ren_node* func;  // V_FUNC: the "func" statement that defined it
    } as;
} value_t;

typedef struct ren_object {
    int32_t refs;       // how many places hold this object, it is freed when this reaches 0
    value_type_t type;
    uint32_t length;    // characters / bytes / list items
    uint32_t capacity;  // V_LIST: room for this many items before the array has to grow
    union {
        char* chars;    // V_STR, always '\0' terminated (length doesn't count it)
        uint8_t* bytes; // V_BYTES
        value_t* items; // V_LIST
    } data;
} ren_object_t;

typedef enum {
    N_NUMBER, N_STRING, N_NIL, N_TRUE, N_FALSE, N_NAME, N_LIST, N_INDEX, N_CALL, N_UNARY,
    N_BINARY, N_AND, N_OR,
    N_BLOCK, N_LET, N_ASSIGN, N_EXPR, N_IF, N_WHILE, N_FOR, N_FOR_IN, N_FUNC, N_RETURN,
    N_BREAK, N_CONTINUE
} node_type_t;

// one node of the syntax tree, the meaning of a / b / c / d depends on the type:
//   N_INDEX a[b]   N_CALL a(items)   N_UNARY op a   N_BINARY / N_AND / N_OR  a op b
//   N_LET name = b   N_ASSIGN a = b   N_EXPR a   N_IF if a then b else c (c is an N_IF for elif)
//   N_WHILE while a do b   N_FOR name = a to c step d do b   N_FOR_IN name in a do b
//   N_FUNC name(params) b   N_RETURN a
typedef struct ren_node {
    node_type_t type;
    int line;
    token_type_t op;
    int32_t number;
    const char* name;
    value_t value;            // N_STRING: the string, made once when the program is parsed
    struct ren_node* a;
    struct ren_node* b;
    struct ren_node* c;
    struct ren_node* d;
    struct ren_node** items;  // N_BLOCK statements, N_LIST elements, N_CALL arguments
    const char** params;      // N_FUNC parameter names
    int count;                // how many items / params
} ren_node_t;

typedef struct {
    const char* name; // interned, so names can be compared with ==
    value_t value;
} ren_var_t;

typedef struct {
    ren_var_t* vars;
    int count;
    int capacity;
} ren_scope_t;

// everything about the program that is running
typedef struct {
    jmp_buf error;        // ren_error jumps back here
    const char* name;     // the file name, for error messages
    ren_scope_t globals;
    ren_scope_t* locals;  // the running function's variables, NULL at the top level
    value_t return_value; // set by "return", picked up by the call
    bool repl;            // typed at the prompt: show the value of expressions
    bool graphics;        // the program switched to graphics, switch back when it ends
} ren_state_t;

extern ren_state_t* ren;

// memory (everything is freed together when the program ends)
void* ren_alloc (size_t size);
void* ren_realloc (void* ptr, size_t size);
void ren_free (void* ptr);
void ren_free_all (void);
const char* ren_intern (const char* text, size_t length);

// errors: print "error in <file> on line <n>: ..." and abandon the program
__attribute__((noreturn, format(printf, 2, 3))) void ren_error (int line, const char* format, ...);

// lexer and parser
token_t* ren_lex (const char* source, int* count);
ren_node_t* ren_parse (token_t* tokens, int count);

// values
static inline value_t ren_nil (void) { value_t v; v.type = V_NIL; v.as.i = 0; return v; }
static inline value_t ren_int (int32_t i) { value_t v; v.type = V_INT; v.as.i = i; return v; }
static inline value_t ren_bool (bool b) { value_t v; v.type = V_BOOL; v.as.i = b ? 1 : 0; return v; }
value_t ren_string (const char* chars, uint32_t length);
value_t ren_list (uint32_t capacity);
value_t ren_bytes (uint32_t length);
void ren_list_push (ren_object_t* list, value_t item); // takes over the reference to item
void ren_retain (value_t v);
void ren_release (value_t v);
bool ren_truthy (value_t v);
bool ren_equal (value_t a, value_t b);
const char* ren_type_name (value_t v);
value_t ren_to_string (value_t v); // a new string showing v the way print() does
void ren_print_value (value_t v);

// builtins (ren_builtins.c)
typedef value_t (*ren_native_t)(int argc, value_t* argv, int line);
typedef struct {
    const char* name;
    ren_native_t fn;
    int min_args;
    int max_args;
} ren_builtin_t;

extern const ren_builtin_t ren_builtins[];
extern const int ren_builtin_count;
void ren_define_constants (void);       // KEY_UP, BLACK ... as global variables
void ren_define_global (const char* name, value_t value);
void ren_builtins_finish (void);       // tidy up after a program (e.g. leave graphics mode)

#endif /* REN_INTERNAL_H */
