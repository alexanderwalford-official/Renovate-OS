/*
    aer_parser.c
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

    The grammar, one rule per function below ( | = or, [ ] = optional, { } = any number of times):

        program     = block
        block       = { statement NEWLINE }
        statement   = "let" NAME "=" expression
                    | "func" NAME "(" [ NAME { "," NAME } ] ")" block "end"
                    | "if" expression "then" block { "elif" expression "then" block } [ "else" block ] "end"
                    | "while" expression "do" block "end"
                    | "for" NAME "=" expression "to" expression [ "step" expression ] "do" block "end"
                    | "for" NAME "in" expression "do" block "end"
                    | "return" [ expression ] | "break" | "continue"
                    | expression [ "=" expression ]          (assignment to a name or list item)
        expression  = conjunction { "or" conjunction }
        conjunction = negation { "and" negation }
        negation    = "not" negation | comparison
        comparison  = sum { ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) sum }
        sum         = product { ( "+" | "-" ) product }
        product     = unary { ( "*" | "/" | "%" ) unary }
        unary       = "-" unary | postfix
        postfix     = primary { "(" [ arguments ] ")" | "[" expression "]" }
        primary     = NUMBER | STRING | "true" | "false" | "nil" | NAME
                    | "(" expression ")" | "[" [ expression { "," expression } ] "]"

    For example  let y = a + b * 2  becomes

        N_LET y
         `-- b: N_BINARY +
                 |-- a: N_NAME a
                 `-- b: N_BINARY *
                         |-- a: N_NAME b
                         `-- b: N_NUMBER 2

    The tree is made once, before the program starts, and the interpreter walks it as it runs.
*/

#include "aer_internal.h"
#include "../lib/aer_string.h"

#define MAX_NESTING 64 // deepest expression / block nesting, keeps the kernel stack safe

// where the parser has got to, and what it is inside of (so misplaced break / return are caught
// here, before the program runs)
typedef struct {
    token_t* tokens;
    int pos;
    int loops;     // how many loops we are inside, for break / continue
    int functions; // 1 inside a function body, for return
    int nesting;
} parser_t;

static parser_t P;

// the token helpers: peek looks at the next token, next takes it, check asks what type it is,
// accept takes it only if it is that type, expect takes it or reports what was missing.
// Every token list ends in T_EOF, which next() never moves past, so reading can't run off the end

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
        aer_error(peek()->line, "expected %s", what);
    }
    return next();
}

static void skip_newlines (void) {
    while (accept(T_NEWLINE)) {
    }
}

// a blank node, with the line it came from for error messages
static aer_node_t* new_node (node_type_t type, int line) {
    aer_node_t* n = aer_alloc(sizeof(aer_node_t));
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->line = line;
    return n;
}

// every recursive rule calls enter() first and leave() at the end. Each level of nesting is a
// few C function calls deep, and the kernel stack is shared by everything, so something like
// ((((((((... 1000 deep is refused instead of overflowing the stack
static void enter (void) {
    if (++P.nesting > MAX_NESTING) {
        aer_error(peek()->line, "code is nested too deeply");
    }
}

static void leave (void) {
    P.nesting--;
}

// a growing array of nodes, for blocks, lists and arguments
typedef struct {
    aer_node_t** items;
    int count;
    int capacity;
} node_list_t;

static void push_node (node_list_t* list, aer_node_t* node) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = aer_realloc(list->items, (size_t)list->capacity * sizeof(aer_node_t*));
    }
    list->items[list->count++] = node;
}

// expressions

static aer_node_t* expression (void);

// the smallest pieces: a literal, a name, a (bracketed expression) or a [list]
static aer_node_t* primary (void) {
    token_t* t = next();
    aer_node_t* n;
    switch (t->type) {
        case T_NUMBER:
            n = new_node(N_NUMBER, t->line);
            n->number = t->number;
            return n;
        case T_STRING:
            // the string value is made now, once, and shared by every run of this line
            n = new_node(N_STRING, t->line);
            n->value = aer_string(t->text, t->length);
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
            aer_error(t->line, "expected a value at the end of the line");
        default:
            aer_error(t->line, "expected a value");
    }
}

// calls and indexing, which can be chained: grid[y][x], make_list()[0], f(1)(2)
static aer_node_t* postfix (void) {
    aer_node_t* n = primary();
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
            if (args.count > AER_MAX_ARGS) aer_error(line, "too many arguments (at most %d)", AER_MAX_ARGS);
            aer_node_t* call = new_node(N_CALL, line);
            call->a = n;
            call->items = args.items;
            call->count = args.count;
            n = call;
        } else if (accept(T_LBRACKET)) {
            aer_node_t* index = new_node(N_INDEX, line);
            index->a = n;
            index->b = expression();
            expect(T_RBRACKET, "']'");
            n = index;
        } else {
            return n;
        }
    }
}

// a minus in front (-x, --x), it binds tighter than * so -a * b is (-a) * b
static aer_node_t* unary (void) {
    if (check(T_MINUS)) {
        int line = next()->line;
        enter();
        aer_node_t* n = new_node(N_UNARY, line);
        n->op = T_MINUS;
        n->a = unary();
        leave();
        return n;
    }
    return postfix();
}

// one level of left associative binary operators: operand (op operand)*
// "left associative" means a - b - c is (a - b) - c: each new operator takes the tree so far as
// its left side. operand is the rule for the next tighter level, so precedence falls out of
// which rule calls which (product, sum and comparison below are each one line)
static aer_node_t* binary (aer_node_t* (*operand)(void), const token_type_t* ops, int op_count) {
    aer_node_t* left = operand();
    for (;;) {
        token_type_t type = peek()->type;
        bool match = false;
        for (int i = 0; i < op_count; i++) {
            if (ops[i] == type) match = true;
        }
        if (!match) return left;
        int line = next()->line;
        aer_node_t* n = new_node(N_BINARY, line);
        n->op = type;
        n->a = left;
        n->b = operand();
        left = n;
    }
}

static aer_node_t* product (void) {
    static const token_type_t ops[] = { T_STAR, T_SLASH, T_PERCENT };
    return binary(unary, ops, 3);
}

static aer_node_t* sum (void) {
    static const token_type_t ops[] = { T_PLUS, T_MINUS };
    return binary(product, ops, 2);
}

static aer_node_t* comparison (void) {
    static const token_type_t ops[] = { T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE };
    return binary(sum, ops, 6);
}

static aer_node_t* negation (void) {
    if (check(T_NOT)) {
        int line = next()->line;
        enter();
        aer_node_t* n = new_node(N_UNARY, line);
        n->op = T_NOT;
        n->a = negation();
        leave();
        return n;
    }
    return comparison();
}

// "and" / "or" get their own node types, not N_BINARY, because they don't always evaluate their
// right side (false and x never looks at x)
static aer_node_t* conjunction (void) {
    aer_node_t* left = negation();
    while (check(T_AND)) {
        aer_node_t* n = new_node(N_AND, next()->line);
        n->a = left;
        n->b = negation();
        left = n;
    }
    return left;
}

static aer_node_t* expression (void) {
    enter();
    aer_node_t* left = conjunction();
    while (check(T_OR)) {
        aer_node_t* n = new_node(N_OR, next()->line);
        n->a = left;
        n->b = conjunction();
        left = n;
    }
    leave();
    return left;
}

// statements

static aer_node_t* statement (void);

// statements until one of the words that close a block ("end", "else", "elif") or the end of the file
static aer_node_t* block (void) {
    aer_node_t* n = new_node(N_BLOCK, peek()->line);
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

// a statement must be followed by a line break, or by a word that ends the block it is in
static void end_of_statement (void) {
    if (accept(T_NEWLINE)) return;
    if (check(T_END) || check(T_ELSE) || check(T_ELIF) || check(T_EOF)) return;
    aer_error(peek()->line, "expected the end of the line");
}

// "if" / "elif": the condition, "then", the block, and whatever follows
// an elif chain becomes nested N_IFs, each one's else (c) being the next:
//     if a then X elif b then Y else Z end   ->   N_IF(a, X, N_IF(b, Y, Z))
static aer_node_t* if_rest (int line) {
    aer_node_t* n = new_node(N_IF, line);
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

// "do" block "end", counting how deep in loops we are so break / continue can be checked
static aer_node_t* loop_body (void) {
    expect(T_DO, "'do'");
    P.loops++;
    aer_node_t* body = block();
    P.loops--;
    expect(T_END, "'end' to close the loop");
    return body;
}

static aer_node_t* statement (void) {
    token_t* t = peek();
    int line = t->line;
    aer_node_t* n;

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
            // functions are global, so defining one inside an if or a loop would be misleading
            if (P.functions > 0 || P.loops > 0 || P.nesting > 1) {
                aer_error(line, "functions can only be made at the top level");
            }
            n = new_node(N_FUNC, line);
            n->name = expect(T_NAME, "the function's name")->text;
            expect(T_LPAREN, "'(' after the function's name");
            const char* params[AER_MAX_ARGS];
            if (!check(T_RPAREN)) {
                do {
                    if (n->count == AER_MAX_ARGS) aer_error(line, "too many parameters (at most %d)", AER_MAX_ARGS);
                    params[n->count++] = expect(T_NAME, "a parameter name")->text;
                } while (accept(T_COMMA));
            }
            expect(T_RPAREN, "')' after the parameters");
            n->params = aer_alloc((size_t)(n->count ? n->count : 1) * sizeof(char*));
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
            if (P.functions == 0) aer_error(line, "return can only be used inside a function");
            n = new_node(N_RETURN, line);
            if (!check(T_NEWLINE) && !check(T_END) && !check(T_ELSE) && !check(T_ELIF) && !check(T_EOF)) {
                n->a = expression();
            }
            break;

        case T_BREAK:
        case T_CONTINUE:
            next();
            if (P.loops == 0) aer_error(line, "%s can only be used inside a loop", t->type == T_BREAK ? "break" : "continue");
            n = new_node(t->type == T_BREAK ? N_BREAK : N_CONTINUE, line);
            break;

        // anything else is an expression (usually a call like print(x)), or an assignment if an
        // "=" follows. The left side is parsed as an ordinary expression first and then checked,
        // which is how list[i] = x works without a special rule
        default: {
            aer_node_t* e = expression();
            if (accept(T_ASSIGN)) {
                if (e->type != N_NAME && e->type != N_INDEX) {
                    aer_error(line, "can only assign to a variable or a list item");
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

// tokens -> the syntax tree of the whole program (an N_BLOCK of its statements)
aer_node_t* aer_parse (token_t* tokens, int count) {
    (void)count;
    memset(&P, 0, sizeof(P));
    P.tokens = tokens;
    aer_node_t* program = block();
    // block() stops at end / else / elif, at the top level that means one too many of them
    if (!check(T_EOF)) {
        token_t* t = peek();
        aer_error(t->line, "'%s' without a matching start", t->type == T_END ? "end" : t->type == T_ELSE ? "else" : "elif");
    }
    return program;
}
