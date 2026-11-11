/*
    aer_internal.h
    The pieces of the AerLang interpreter shared between its files:

        source text --aer_lexer.c--> tokens --aer_parser.c--> syntax tree --aer_interp.c--> runs it
                                                                    calls aer_builtins.c for print() etc.

    Memory: everything a program allocates goes through aer_alloc, which keeps a list of it, so
    when the program ends (normally or with an error half way through) aer_free_all gives it all
    back. While it runs, strings, lists and byte buffers are reference counted, so a long running
    program that keeps making new strings doesn't fill up memory.

    Errors: aer_error prints the message and longjmps out to the start of the run, abandoning
    whatever was half done. That is why nothing has to clean up on the way out of an error.
*/
#ifndef AER_INTERNAL_H
#define AER_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../lib/aer_setjmp.h"

#define AER_MAX_ARGS 16 // most arguments a function call can pass

typedef enum {
    T_EOF, T_NEWLINE, T_NUMBER, T_STRING, T_NAME,
    // keywords
    T_LET, T_FUNC, T_RETURN, T_IF, T_THEN, T_ELIF, T_ELSE, T_END, T_WHILE, T_DO, T_FOR, T_TO,
    T_STEP, T_IN, T_BREAK, T_CONTINUE, T_AND, T_OR, T_NOT, T_TRUE, T_FALSE, T_NIL,
    // symbols
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_ASSIGN,
    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_COMMA,
    T_FLOAT             // a number with a decimal point (or an exponent): 3.14, 2.5e3
} token_type_t;

typedef struct {
    token_type_t type;
    int line;
    int32_t number;     // T_NUMBER
    double real;        // T_FLOAT
    const char* text;   // T_NAME: the interned name, T_STRING: the characters
    uint32_t length;    // T_STRING: how many characters
} token_t;

// (V_FLOAT is last, so the others keep their numbers: aer_type_name and is_object rely on them)
typedef enum { V_NIL, V_BOOL, V_INT, V_STR, V_LIST, V_BYTES, V_FUNC, V_NATIVE, V_FLOAT } value_type_t;

struct aer_object;
struct aer_node;

// an AerLang value. Numbers, booleans and nil live inside it, strings / lists / bytes point to an object
typedef struct {
    value_type_t type;
    union {
        int32_t i;              // V_INT, V_BOOL (0 or 1), V_NATIVE (index into the builtin table)
        struct aer_object* obj; // V_STR, V_LIST, V_BYTES
        struct aer_node* func;  // V_FUNC: the "func" statement that defined it
        double f;               // V_FLOAT: a decimal number
    } as;
} value_t;

typedef struct aer_object {
    int32_t refs;       // how many places hold this object, it is freed when this reaches 0
    value_type_t type;
    uint32_t length;    // characters / bytes / list items
    uint32_t capacity;  // V_LIST: room for this many items before the array has to grow
    union {
        char* chars;    // V_STR, always '\0' terminated (length doesn't count it)
        uint8_t* bytes; // V_BYTES
        value_t* items; // V_LIST
    } data;
} aer_object_t;

typedef enum {
    N_NUMBER, N_STRING, N_NIL, N_TRUE, N_FALSE, N_NAME, N_LIST, N_INDEX, N_CALL, N_UNARY,
    N_BINARY, N_AND, N_OR,
    N_BLOCK, N_LET, N_ASSIGN, N_EXPR, N_IF, N_WHILE, N_FOR, N_FOR_IN, N_FUNC, N_RETURN,
    N_BREAK, N_CONTINUE,
    N_FLOAT                   // a decimal number, 3.14
} node_type_t;

// One node of the syntax tree: a piece of the program, like "x + 1" or a whole while loop.
//
// A node has up to four child nodes. Which ones it uses, and what they mean, depends on its
// type, so each child slot goes by several names: one for each kind of node that uses it.
// They are "anonymous unions": all the names inside one union are the SAME slot, just called
// something that makes sense where it is used. A while loop reads n->condition and n->body;
// "a + b" reads n->left and n->right. (A node only ever uses one name per slot.)
//
//     N_BINARY, N_AND, N_OR   left op right                 "a + b", "a and b"
//     N_UNARY                 op operand                    "-x", "not done"
//     N_INDEX                 container[index]              "list[2]"
//     N_CALL                  callee(items)                 "print(x)"
//     N_LET                   let name = new_value
//     N_ASSIGN                target = new_value            target is an N_NAME or an N_INDEX
//     N_EXPR                  expression                    a call on its own, like print(x)
//     N_RETURN                return expression             (expression may be NULL)
//     N_IF                    if condition then then_block else else_block
//                             (for "elif", else_block is another N_IF)
//     N_WHILE                 while condition do body
//     N_FOR                   for name = from to to step step do body   (step may be NULL)
//     N_FOR_IN                for name in collection do body
//     N_FUNC                  func name(params) body
typedef struct aer_node {
    node_type_t type;
    int line;
    token_type_t op;          // N_BINARY / N_UNARY: which operator (T_PLUS, T_MINUS...)
    int32_t number;           // N_NUMBER: its value
    double real;              // N_FLOAT: its value
    const char* name;         // N_NAME, N_LET, N_FOR, N_FOR_IN, N_FUNC: the variable or function
    value_t value;            // N_STRING: the string, made once when the program is parsed

    union {                   // the first child
        struct aer_node* left;
        struct aer_node* operand;
        struct aer_node* container;
        struct aer_node* callee;
        struct aer_node* target;
        struct aer_node* expression;
        struct aer_node* condition;
        struct aer_node* from;
        struct aer_node* collection;
    };
    union {                   // the second child
        struct aer_node* right;
        struct aer_node* index;
        struct aer_node* new_value;
        struct aer_node* then_block;
        struct aer_node* body;
    };
    union {                   // the third child
        struct aer_node* else_block;
        struct aer_node* to;
    };
    union {                   // the fourth child
        struct aer_node* step;
    };

    struct aer_node** items;  // N_BLOCK statements, N_LIST elements, N_CALL arguments
    const char** params;      // N_FUNC parameter names
    int count;                // how many items / params
} aer_node_t;

typedef struct {
    const char* name; // interned, so names can be compared with ==
    value_t value;
} aer_var_t;

typedef struct {
    aer_var_t* vars;
    int count;
    int capacity;
} aer_scope_t;

// everything about the program that is running
typedef struct {
    jmp_buf error;        // aer_error jumps back here
    const char* name;     // the file name, for error messages
    const char* source;   // the code being run, so an error can be shown in it (see aer_error)
    aer_scope_t globals;
    aer_scope_t* locals;  // the running function's variables, NULL at the top level
    value_t return_value; // set by "return", picked up by the call
    bool repl;            // typed at the prompt: show the value of expressions
    bool shell;           // the terminal's own session (see "The shell" in aer_interp.c): cmd() works
    uint32_t uses;        // the program's profile: the parts of AerOS it uses (sys/appmode.h)
    bool graphics;        // the program switched to graphics, switch back when it ends
} aer_state_t;

extern aer_state_t* aer;

// memory (everything is freed together when the program ends)
void* aer_alloc (size_t size);
void* aer_realloc (void* ptr, size_t size);
void aer_free (void* ptr);
void aer_free_all (void);
const char* aer_intern (const char* text, size_t length);

// errors: print "error in <file> on line <n>: ..." and abandon the program
__attribute__((noreturn, format(printf, 2, 3))) void aer_error (int line, const char* format, ...);

// Ctrl+C was pressed: say so, remember it (aer_take_stopped) and abandon the program
__attribute__((noreturn)) void aer_stop (int line);

// lexer and parser
token_t* aer_lex (const char* source, int* count);
aer_node_t* aer_parse (token_t* tokens, int count);

// values
static inline value_t aer_nil (void) { value_t v; v.type = V_NIL; v.as.i = 0; return v; }
static inline value_t aer_int (int32_t i) { value_t v; v.type = V_INT; v.as.i = i; return v; }
static inline value_t aer_bool (bool b) { value_t v; v.type = V_BOOL; v.as.i = b ? 1 : 0; return v; }
static inline value_t aer_float (double f) { value_t v; v.type = V_FLOAT; v.as.f = f; return v; }

// Numbers come in two kinds: whole (V_INT, 32 bits) and decimal (V_FLOAT, a double). These let
// code take either: is it a number at all, and what is it as a decimal?
static inline bool aer_is_number (value_t v) { return v.type == V_INT || v.type == V_FLOAT; }
static inline double aer_real (value_t v) { return v.type == V_FLOAT ? v.as.f : (double)v.as.i; }
value_t aer_string (const char* chars, uint32_t length);
value_t aer_list (uint32_t capacity);
value_t aer_bytes (uint32_t length);
void aer_list_push (aer_object_t* list, value_t item); // takes over the reference to item
void aer_retain (value_t v);
void aer_release (value_t v);
bool aer_truthy (value_t v);
bool aer_equal (value_t a, value_t b);
const char* aer_type_name (value_t v);
value_t aer_to_string (value_t v); // a new string showing v the way print() does
void aer_print_value (value_t v);

// builtins (aer_builtins.c)
typedef value_t (*aer_native_t)(int argc, value_t* argv, int line);
typedef struct {
    const char* name;
    aer_native_t fn;
    int min_args;
    int max_args;
    const char* function; // fn's name, "bi_print" (for WHY and SRC, see aer_builtin_function)
} aer_builtin_t;

extern const aer_builtin_t aer_builtins[];
extern const int aer_builtin_count;
void aer_define_constants (void);       // KEY_UP, BLACK ... as global variables
void aer_define_global (const char* name, value_t value);
void aer_builtins_finish (void);       // tidy up after a program (e.g. leave graphics mode)
void aer_give_screen_back (void);      // leave graphics mode, if the code switched to it

// which parts of AerOS (APP_ bits, sys/appmode.h) a program's tokens name (aer_builtins.c)
uint32_t aer_profile (const token_t* tokens, int count);

// is this word (length characters) one of AerLang's keywords? (aer_lexer.c)
bool aer_is_keyword (const char* word, size_t length);

#endif /* AER_INTERNAL_H */
