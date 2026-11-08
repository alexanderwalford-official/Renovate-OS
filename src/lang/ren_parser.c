/*
    ren_parser.c
    Builds the syntax tree from the tokens, by recursive descent: one function per grammar rule,
    each calling the rules below it. Operator precedence, from loosest to tightest:

        or
        and
        not
        ==  !=  <  <=  >  >=
        +  -
        *  /  %
        -x (negative)
        calls f(x) and indexing x[i]

    A statement ends at the end of its line, or straight before "end" / "else" / "elif", so short
    blocks can sit on one line:   if x > 3 then print("big") end
*/

#include "ren_internal.h"
#include "../lib/ren_string.h"

#define MAX_NESTING 64 // deepest expression / block nesting, keeps the kernel stack safe

typedef struct {
    token_t* tokens;
    int pos;
    int loops;     // how many loops we are inside, for break / continue
    int functions; // 1 inside a function body, for return
    int nesting;
} parser_t;

static parser_t P;

static token_t* peek (void) {
    return &P.tokens[P.pos];
}

static bool check (token_type_t type) {
    return P.tokens[P.pos].type == type;
}

static token_t* next (void) {
    token_t* t = &P.tokens[P.pos];
    if (t->type != T_EOF) P.pos++;
    return t;
}

static bool accept (token_type_t type) {
    if (check(type)) {
        next();
        return true;
    }
    return false;
}

static token_t* expect (token_type_t type, const char* what) {
    if (!check(type)) {
        ren_error(peek()->line, "expected %s", what);
    }
    return next();
}

static void skip_newlines (void) {
    while (accept(T_NEWLINE)) {
    }
}

static ren_node_t* new_node (node_type_t type, int line) {
    ren_node_t* n = ren_alloc(sizeof(ren_node_t));
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->line = line;
    return n;
}

static void enter (void) {
    if (++P.nesting > MAX_NESTING) {
        ren_error(peek()->line, "code is nested too deeply");
    }
}

static void leave (void) {
    P.nesting--;
}

// a growing array of nodes, for blocks, lists and arguments
typedef struct {
    ren_node_t** items;
    int count;
    int capacity;
} node_list_t;

static void push_node (node_list_t* list, ren_node_t* node) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = ren_realloc(list->items, (size_t)list->capacity * sizeof(ren_node_t*));
    }
    list->items[list->count++] = node;
}

// expressions

static ren_node_t* expression (void);

static ren_node_t* primary (void) {
    token_t* t = next();
    ren_node_t* n;
    switch (t->type) {
        case T_NUMBER:
            n = new_node(N_NUMBER, t->line);
            n->number = t->number;
            return n;
        case T_STRING:
            n = new_node(N_STRING, t->line);
            n->value = ren_string(t->text, t->length);
            return n;
        case T_TRUE: return new_node(N_TRUE, t->line);
        case T_FALSE: return new_node(N_FALSE, t->line);
        case T_NIL: return new_node(N_NIL, t->line);
        case T_NAME:
            n = new_node(N_NAME, t->line);
            n->name = t->text;
            return n;
        case T_LPAREN:
            n = expression();
            expect(T_RPAREN, "')'");
            return n;
        case T_LBRACKET: {
            node_list_t items = { 0, 0, 0 };
            if (!check(T_RBRACKET)) {
                do {
                    push_node(&items, expression());
                } while (accept(T_COMMA));
            }
            expect(T_RBRACKET, "']' at the end of the list");
            n = new_node(N_LIST, t->line);
            n->items = items.items;
            n->count = items.count;
            return n;
        }
        case T_NEWLINE:
        case T_EOF:
            ren_error(t->line, "expected a value at the end of the line");
        default:
            ren_error(t->line, "expected a value");
    }
}

static ren_node_t* postfix (void) {
    ren_node_t* n = primary();
    for (;;) {
        int line = peek()->line;
        if (accept(T_LPAREN)) {
            node_list_t args = { 0, 0, 0 };
            if (!check(T_RPAREN)) {
                do {
                    push_node(&args, expression());
                } while (accept(T_COMMA));
            }
            expect(T_RPAREN, "')' after the arguments");
            if (args.count > REN_MAX_ARGS) ren_error(line, "too many arguments (at most %d)", REN_MAX_ARGS);
            ren_node_t* call = new_node(N_CALL, line);
            call->a = n;
            call->items = args.items;
            call->count = args.count;
            n = call;
        } else if (accept(T_LBRACKET)) {
            ren_node_t* index = new_node(N_INDEX, line);
            index->a = n;
            index->b = expression();
            expect(T_RBRACKET, "']'");
            n = index;
        } else {
            return n;
        }
    }
}

static ren_node_t* unary (void) {
    if (check(T_MINUS)) {
        int line = next()->line;
        enter();
        ren_node_t* n = new_node(N_UNARY, line);
        n->op = T_MINUS;
        n->a = unary();
        leave();
        return n;
    }
    return postfix();
}

// one level of left associative binary operators: operand (op operand)*
static ren_node_t* binary (ren_node_t* (*operand)(void), const token_type_t* ops, int op_count) {
    ren_node_t* left = operand();
    for (;;) {
        token_type_t type = peek()->type;
        bool match = false;
        for (int i = 0; i < op_count; i++) {
            if (ops[i] == type) match = true;
        }
        if (!match) return left;
        int line = next()->line;
        ren_node_t* n = new_node(N_BINARY, line);
        n->op = type;
        n->a = left;
        n->b = operand();
        left = n;
    }
}

static ren_node_t* product (void) {
    static const token_type_t ops[] = { T_STAR, T_SLASH, T_PERCENT };
    return binary(unary, ops, 3);
}

static ren_node_t* sum (void) {
    static const token_type_t ops[] = { T_PLUS, T_MINUS };
    return binary(product, ops, 2);
}

static ren_node_t* comparison (void) {
    static const token_type_t ops[] = { T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE };
    return binary(sum, ops, 6);
}

static ren_node_t* negation (void) {
    if (check(T_NOT)) {
        int line = next()->line;
        enter();
        ren_node_t* n = new_node(N_UNARY, line);
        n->op = T_NOT;
        n->a = negation();
        leave();
        return n;
    }
    return comparison();
}

static ren_node_t* conjunction (void) {
    ren_node_t* left = negation();
    while (check(T_AND)) {
        ren_node_t* n = new_node(N_AND, next()->line);
        n->a = left;
        n->b = negation();
        left = n;
    }
    return left;
}

static ren_node_t* expression (void) {
    enter();
    ren_node_t* left = conjunction();
    while (check(T_OR)) {
        ren_node_t* n = new_node(N_OR, next()->line);
        n->a = left;
        n->b = conjunction();
        left = n;
    }
    leave();
    return left;
}

// statements

static ren_node_t* statement (void);

// statements until one of the words that close a block ("end", "else", "elif") or the end of the file
static ren_node_t* block (void) {
    ren_node_t* n = new_node(N_BLOCK, peek()->line);
    node_list_t statements = { 0, 0, 0 };
    enter();
    skip_newlines();
    while (!check(T_END) && !check(T_ELSE) && !check(T_ELIF) && !check(T_EOF)) {
        push_node(&statements, statement());
        skip_newlines();
    }
    leave();
    n->items = statements.items;
    n->count = statements.count;
    return n;
}

static void end_of_statement (void) {
    if (accept(T_NEWLINE)) return;
    if (check(T_END) || check(T_ELSE) || check(T_ELIF) || check(T_EOF)) return;
    ren_error(peek()->line, "expected the end of the line");
}

// "if" / "elif": the condition, "then", the block, and whatever follows
static ren_node_t* if_rest (int line) {
    ren_node_t* n = new_node(N_IF, line);
    n->a = expression();
    expect(T_THEN, "'then' after the condition");
    n->b = block();
    if (check(T_ELIF)) {
        n->c = if_rest(next()->line); // the rest of the chain, which shares our "end"
        return n;
    }
    if (accept(T_ELSE)) {
        n->c = block();
    }
    expect(T_END, "'end' to close the if");
    return n;
}

static ren_node_t* loop_body (void) {
    expect(T_DO, "'do'");
    P.loops++;
    ren_node_t* body = block();
    P.loops--;
    expect(T_END, "'end' to close the loop");
    return body;
}

static ren_node_t* statement (void) {
    token_t* t = peek();
    int line = t->line;
    ren_node_t* n;

    switch (t->type) {
        case T_LET:
            next();
            n = new_node(N_LET, line);
            n->name = expect(T_NAME, "a variable name after let")->text;
            expect(T_ASSIGN, "'=' after the variable name");
            n->b = expression();
            break;

        case T_FUNC: {
            next();
            if (P.functions > 0 || P.loops > 0 || P.nesting > 1) {
                ren_error(line, "functions can only be made at the top level");
            }
            n = new_node(N_FUNC, line);
            n->name = expect(T_NAME, "the function's name")->text;
            expect(T_LPAREN, "'(' after the function's name");
            const char* params[REN_MAX_ARGS];
            if (!check(T_RPAREN)) {
                do {
                    if (n->count == REN_MAX_ARGS) ren_error(line, "too many parameters (at most %d)", REN_MAX_ARGS);
                    params[n->count++] = expect(T_NAME, "a parameter name")->text;
                } while (accept(T_COMMA));
            }
            expect(T_RPAREN, "')' after the parameters");
            n->params = ren_alloc((size_t)(n->count ? n->count : 1) * sizeof(char*));
            memcpy(n->params, params, (size_t)n->count * sizeof(char*));
            P.functions++;
            n->b = block();
            P.functions--;
            expect(T_END, "'end' to close the function");
            break;
        }

        case T_IF:
            next();
            n = if_rest(line);
            break;

        case T_WHILE:
            next();
            n = new_node(N_WHILE, line);
            n->a = expression();
            n->b = loop_body();
            break;

        case T_FOR:
            next();
            n = new_node(N_FOR, line);
            n->name = expect(T_NAME, "a variable name after for")->text;
            if (accept(T_IN)) {
                n->type = N_FOR_IN;
                n->a = expression();
            } else {
                expect(T_ASSIGN, "'=' or 'in' after the loop variable");
                n->a = expression();
                expect(T_TO, "'to'");
                n->c = expression();
                if (accept(T_STEP)) n->d = expression();
            }
            n->b = loop_body();
            break;

        case T_RETURN:
            next();
            if (P.functions == 0) ren_error(line, "return can only be used inside a function");
            n = new_node(N_RETURN, line);
            if (!check(T_NEWLINE) && !check(T_END) && !check(T_ELSE) && !check(T_ELIF) && !check(T_EOF)) {
                n->a = expression();
            }
            break;

        case T_BREAK:
        case T_CONTINUE:
            next();
            if (P.loops == 0) ren_error(line, "%s can only be used inside a loop", t->type == T_BREAK ? "break" : "continue");
            n = new_node(t->type == T_BREAK ? N_BREAK : N_CONTINUE, line);
            break;

        default: {
            ren_node_t* e = expression();
            if (accept(T_ASSIGN)) {
                if (e->type != N_NAME && e->type != N_INDEX) {
                    ren_error(line, "can only assign to a variable or a list item");
                }
                n = new_node(N_ASSIGN, line);
                n->a = e;
                n->b = expression();
            } else {
                n = new_node(N_EXPR, line);
                n->a = e;
            }
            break;
        }
    }
    end_of_statement();
    return n;
}

ren_node_t* ren_parse (token_t* tokens, int count) {
    (void)count;
    memset(&P, 0, sizeof(P));
    P.tokens = tokens;
    ren_node_t* program = block();
    if (!check(T_EOF)) {
        token_t* t = peek();
        ren_error(t->line, "'%s' without a matching start", t->type == T_END ? "end" : t->type == T_ELSE ? "else" : "elif");
    }
    return program;
}
