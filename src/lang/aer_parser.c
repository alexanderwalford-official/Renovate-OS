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
    int nesting;   // how deep in enter() calls we are (see enter below)
} parser_t;

// only one program is parsed at a time, so the parser's state is one global that aer_parse
// clears at the start, rather than a pointer passed to every function
static parser_t parser;

// the token helpers: peek looks at the next token, next takes it, check asks what type it is,
// accept takes it only if it is that type, expect takes it or reports what was missing.
// Every token list ends in T_EOF, which next() never moves past, so reading can't run off the end

// The next token, without taking it
static token_t* peek (void) {
    return &parser.tokens[parser.pos];
}

// Is the next token of this type? (it isn't taken)
static bool check (token_type_t type) {
    return parser.tokens[parser.pos].type == type;
}

// Take the next token and return it. At T_EOF it stays put, so T_EOF can be taken again and again
static token_t* next (void) {
    token_t* t = &parser.tokens[parser.pos];
    if (t->type != T_EOF) {
        parser.pos++;
    }
    return t;
}

// Take the next token only if it is of this type, and say whether it was: for optional parts
// like the "step" of a for loop
static bool accept (token_type_t type) {
    if (check(type)) {
        next();
        return true;
    }
    return false;
}

// Take the next token, which has to be of this type. If it isn't, the program has a mistake and
// aer_error stops with "expected <what>", so what is written for the person reading it ("'then'
// after the condition")
static token_t* expect (token_type_t type, const char* what) {
    if (!check(type)) {
        aer_error(peek()->line, "expected %s", what);
    }
    return next();
}

// Take any line breaks, for the places blank lines are allowed (between statements)
static void skip_newlines (void) {
    while (accept(T_NEWLINE)) {
    }
}

// a blank node, with the line it came from for error messages
static aer_node_t* new_node (node_type_t type, int line) {
    // cleared, so every child the node doesn't use is NULL (the interpreter relies on that, for
    // example a for loop with no step, or a return with no value)
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
    if (++parser.nesting > MAX_NESTING) {
        aer_error(peek()->line, "code is nested too deeply");
    }
}

// The other half of enter(): one level of nesting less. (After an error there is no leave(),
// but that doesn't matter: aer_parse starts the count again from 0 next time)
static void leave (void) {
    parser.nesting--;
}

// a growing array of nodes, for blocks, lists and arguments
typedef struct {
    aer_node_t** items;
    int count;
    int capacity;
} node_list_t;

// Add a node to the end of the list. The array starts at 8 and doubles whenever it is full; the
// node that is built at the end takes the array over as its items, so it is never freed here
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
        case T_FLOAT:
            n = new_node(N_FLOAT, t->line);
            n->real = t->real;
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
            // brackets only change the shape of the tree, so they need no node of their own:
            // (a + b) * c just puts the + node under the *
            n = expression();
            expect(T_RPAREN, "')'");
            return n;
        case T_LBRACKET: {
            // a list: [], or expressions with commas between them
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
        // anything else can't start a value. aer_error never returns, so there is no need for
        // a return after it
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
    // each ( or [ after a value wraps what we have so far, so grid[y][x] is
    // N_INDEX(N_INDEX(grid, y), x): the leftmost one ends up deepest, and runs first
    for (;;) {
        int line = peek()->line;
        if (accept(T_LPAREN)) {
            // a call: the arguments, with commas between them
            node_list_t args = { 0, 0, 0 };
            if (!check(T_RPAREN)) {
                do {
                    push_node(&args, expression());
                } while (accept(T_COMMA));
            }
            expect(T_RPAREN, "')' after the arguments");
            // the interpreter gathers the arguments into a fixed size array of AER_MAX_ARGS,
            // so more than that is refused here, before the program runs
            if (args.count > AER_MAX_ARGS) {
                aer_error(line, "too many arguments (at most %d)", AER_MAX_ARGS);
            }
            aer_node_t* call = new_node(N_CALL, line);
            call->callee = n;
            call->items = args.items;
            call->count = args.count;
            n = call;
        } else if (accept(T_LBRACKET)) {
            // indexing: one expression in [ ]
            aer_node_t* index = new_node(N_INDEX, line);
            index->container = n;
            index->index = expression();
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
        // unary calls itself for each minus, so ------x needs the nesting check too
        enter();
        aer_node_t* n = new_node(N_UNARY, line);
        n->op = T_MINUS;
        n->operand = unary();
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
        // is the next token one of this level's operators? If not, this level is done and the
        // token is left for a looser level (or the end of the statement) to deal with
        token_type_t type = peek()->type;
        bool match = false;
        for (int i = 0; i < op_count; i++) {
            if (ops[i] == type) {
                match = true;
            }
        }
        if (!match) {
            return left;
        }
        // it is: take the operator and the next operand, and make them a new node with
        // everything so far on its left
        int line = next()->line;
        aer_node_t* n = new_node(N_BINARY, line);
        n->op = type;
        n->left = left;
        n->right = operand();
        left = n;
    }
}

// a * b, a / b, a % b
static aer_node_t* product (void) {
    static const token_type_t ops[] = { T_STAR, T_SLASH, T_PERCENT };
    return binary(unary, ops, 3);
}

// a + b, a - b
static aer_node_t* sum (void) {
    static const token_type_t ops[] = { T_PLUS, T_MINUS };
    return binary(product, ops, 2);
}

// a == b, a != b, a < b, a <= b, a > b, a >= b. These are left associative like the others, so
// 1 < x < 5 is (1 < x) < 5, which compares true or false with 5 and stops with an error when it
// runs, rather than meaning what it would in maths
static aer_node_t* comparison (void) {
    static const token_type_t ops[] = { T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE };
    return binary(sum, ops, 6);
}

// "not" in front: looser than the comparisons, so not a == b is not (a == b)
static aer_node_t* negation (void) {
    if (check(T_NOT)) {
        int line = next()->line;
        enter();
        aer_node_t* n = new_node(N_UNARY, line);
        n->op = T_NOT;
        n->operand = negation();
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
        n->left = left;
        n->right = negation();
        left = n;
    }
    return left;
}

// A whole expression: the loosest level, "or", which works down through all the others. Every
// bracket, list item and argument comes back here, which is why the nesting check is here
static aer_node_t* expression (void) {
    enter();
    aer_node_t* left = conjunction();
    while (check(T_OR)) {
        aer_node_t* n = new_node(N_OR, next()->line);
        n->left = left;
        n->right = conjunction();
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
    // blocks nest inside each other (an if in a loop in a function), so they count towards
    // the nesting limit as well. block() doesn't take the closing word itself: the statement
    // that opened the block does, so it can say what was missing ("'end' to close the loop")
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
    if (accept(T_NEWLINE)) {
        return;
    }
    // the closing word is left for the block to stop at, not taken: that is what lets
    // if x then print("big") end  sit on one line
    if (check(T_END) || check(T_ELSE) || check(T_ELIF) || check(T_EOF)) {
        return;
    }
    aer_error(peek()->line, "expected the end of the line");
}

// "if" / "elif": the condition, "then", the block, and whatever follows
// an elif chain becomes nested N_IFs, each one's else_block being the next:
//     if a then X elif b then Y else Z end   ->   N_IF(a, X, N_IF(b, Y, Z))
// The "if" (or "elif") itself has already been taken; line is where it was
static aer_node_t* if_rest (int line) {
    aer_node_t* n = new_node(N_IF, line);
    n->condition = expression();
    expect(T_THEN, "'then' after the condition");
    n->then_block = block();
    if (check(T_ELIF)) {
        n->else_block = if_rest(next()->line); // the rest of the chain, which shares our "end"
        return n;
    }
    if (accept(T_ELSE)) {
        n->else_block = block();
    }
    expect(T_END, "'end' to close the if");
    return n;
}

// "do" block "end", counting how deep in loops we are so break / continue can be checked
static aer_node_t* loop_body (void) {
    expect(T_DO, "'do'");
    parser.loops++;
    aer_node_t* body = block();
    parser.loops--;
    expect(T_END, "'end' to close the loop");
    return body;
}

// One statement, and the line break (or closing word) after it. The first token decides which
// kind it is: a keyword for let, func, if and the rest, and anything else is an expression
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
            n->new_value = expression();
            break;

        case T_FUNC: {
            next();
            // functions are global, so defining one inside an if or a loop would be misleading.
            // At the top level only the program's own block() has entered, so nesting is
            // exactly 1; inside an if or any other block it is more
            if (parser.functions > 0 || parser.loops > 0 || parser.nesting > 1) {
                aer_error(line, "functions can only be made at the top level");
            }
            n = new_node(N_FUNC, line);
            n->name = expect(T_NAME, "the function's name")->text;
            expect(T_LPAREN, "'(' after the function's name");
            // the parameter names, gathered on the stack first (there can't be more than
            // AER_MAX_ARGS) and then copied into an array of exactly the right size
            const char* params[AER_MAX_ARGS];
            if (!check(T_RPAREN)) {
                do {
                    if (n->count == AER_MAX_ARGS) {
                        aer_error(line, "too many parameters (at most %d)", AER_MAX_ARGS);
                    }
                    params[n->count++] = expect(T_NAME, "a parameter name")->text;
                } while (accept(T_COMMA));
            }
            expect(T_RPAREN, "')' after the parameters");
            // (room for at least one, so a function with no parameters doesn't ask for 0 bytes)
            n->params = aer_alloc((size_t)(n->count ? n->count : 1) * sizeof(char*));
            memcpy(n->params, params, (size_t)n->count * sizeof(char*));
            // inside the body, return is allowed
            parser.functions++;
            n->body = block();
            parser.functions--;
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
            n->condition = expression();
            n->body = loop_body();
            break;

        case T_FOR:
            next();
            // both kinds of for start the same way, so the node starts as N_FOR and is changed
            // to N_FOR_IN if "in" comes after the name
            n = new_node(N_FOR, line);
            n->name = expect(T_NAME, "a variable name after for")->text;
            if (accept(T_IN)) {
                n->type = N_FOR_IN;
                n->collection = expression();
            } else {
                expect(T_ASSIGN, "'=' or 'in' after the loop variable");
                n->from = expression();
                expect(T_TO, "'to'");
                n->to = expression();
                if (accept(T_STEP)) {
                    n->step = expression();
                }
            }
            n->body = loop_body();
            break;

        case T_RETURN:
            next();
            if (parser.functions == 0) {
                aer_error(line, "return can only be used inside a function");
            }
            n = new_node(N_RETURN, line);
            // a value only if something follows on the same line: a bare "return" leaves
            // expression NULL, and the call gives back nil
            if (!check(T_NEWLINE) && !check(T_END) && !check(T_ELSE) && !check(T_ELIF) && !check(T_EOF)) {
                n->expression = expression();
            }
            break;

        case T_BREAK:
        case T_CONTINUE:
            next();
            if (parser.loops == 0) {
                aer_error(line, "%s can only be used inside a loop", t->type == T_BREAK ? "break" : "continue");
            }
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
                n->target = e;
                n->new_value = expression();
            } else {
                n = new_node(N_EXPR, line);
                n->expression = e;
            }
            break;
        }
    }
    end_of_statement();
    return n;
}

// tokens -> the syntax tree of the whole program (an N_BLOCK of its statements)
aer_node_t* aer_parse (token_t* tokens, int count) {
    // count isn't needed: the T_EOF on the end of the tokens marks where they stop
    (void)count;
    memset(&parser, 0, sizeof(parser));
    parser.tokens = tokens;
    aer_node_t* program = block();
    // block() stops at end / else / elif, at the top level that means one too many of them
    if (!check(T_EOF)) {
        token_t* t = peek();
        const char* word = "elif";
        if (t->type == T_END) {
            word = "end";
        } else if (t->type == T_ELSE) {
            word = "else";
        }
        aer_error(t->line, "'%s' without a matching start", word);
    }
    return program;
}
