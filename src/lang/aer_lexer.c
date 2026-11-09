/*
    aer_lexer.c
    Turns the source text into tokens: numbers, strings, names, keywords and symbols.

    For example the line

        let total = price * 2   # comment

    becomes the tokens  T_LET  T_NAME "total"  T_ASSIGN  T_NAME "price"  T_STAR  T_NUMBER 2  T_NEWLINE

    Lines matter in AerLang (a statement ends at the end of its line), so the lexer emits a
    T_NEWLINE token for each line break. Inside ( ) or [ ] they are skipped, which lets a long
    call or list carry on over several lines.

    The whole program is turned into tokens in one go before parsing starts, so a typo anywhere
    is reported before anything runs. Each token remembers its line number for error messages.
*/

#include "aer_internal.h"
#include "../lib/aer_ctype.h"
#include "../lib/aer_string.h"

// the words that mean something to AerLang, and so can't be used as variable names
static const struct {
    const char* word;
    token_type_t type;
} keywords[] = {
    { "let", T_LET }, { "func", T_FUNC }, { "return", T_RETURN }, { "if", T_IF },
    { "then", T_THEN }, { "elif", T_ELIF }, { "else", T_ELSE }, { "end", T_END },
    { "while", T_WHILE }, { "do", T_DO }, { "for", T_FOR }, { "to", T_TO }, { "step", T_STEP },
    { "in", T_IN }, { "break", T_BREAK }, { "continue", T_CONTINUE }, { "and", T_AND },
    { "or", T_OR }, { "not", T_NOT }, { "true", T_TRUE }, { "false", T_FALSE }, { "nil", T_NIL },
};

// the tokens found so far, in an array that doubles in size whenever it fills up
typedef struct {
    token_t* tokens;
    int count;
    int capacity;
} token_list_t;

static token_t* add_token (token_list_t* list, token_type_t type, int line) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 256;
        list->tokens = aer_realloc(list->tokens, (size_t)list->capacity * sizeof(token_t));
    }
    token_t* t = &list->tokens[list->count++];
    memset(t, 0, sizeof(*t));
    t->type = type;
    t->line = line;
    return t;
}

// the value of a hex digit, c must already be known to be one (0-9, a-f, A-F)
static int hex_value (char c) {
    if (isdigit((unsigned char)c)) {
        return c - '0';
    }
    return tolower((unsigned char)c) - 'a' + 10;
}

// the whole source text -> an array of tokens ending in T_EOF, *count says how many
token_t* aer_lex (const char* src, int* count) {
    token_list_t list = { 0, 0, 0 };
    int line = 1;
    int depth = 0; // how many ( or [ are open
    const char* p = src;

    while (*p) {
        char c = *p;

        // spaces and tabs only separate tokens ('\r' comes from files saved on Windows)
        if (c == ' ' || c == '\t' || c == '\r') {
            p++;
            continue;
        }
        // a comment runs to the end of the line (the '\n' itself is still seen below)
        if (c == '#') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (c == '\n') {
            // one T_NEWLINE however many blank lines there are
            if (depth == 0 && list.count > 0 && list.tokens[list.count - 1].type != T_NEWLINE) {
                add_token(&list, T_NEWLINE, line);
            }
            line++;
            p++;
            continue;
        }

        // numbers: decimal (42) or hex (0xFF), 32 bit signed, so at most 2147483647
        // (a minus sign is a separate T_MINUS token, the parser turns it into a negation)
        if (isdigit((unsigned char)c)) {
            uint32_t value = 0;
            if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                if (!isxdigit((unsigned char)*p)) {
                    aer_error(line, "expected hex digits after 0x");
                }
                while (isxdigit((unsigned char)*p)) {
                    if (value > 0x0FFFFFFF) {
                        aer_error(line, "number is too big");
                    }
                    value = value * 16 + (uint32_t)hex_value(*p++);
                }
            } else {
                while (isdigit((unsigned char)*p)) {
                    uint32_t digit = (uint32_t)(*p++ - '0');
                    if (value > (0x7FFFFFFFu - digit) / 10) {
                        aer_error(line, "number is too big");
                    }
                    value = value * 10 + digit;
                }
            }
            if (isalpha((unsigned char)*p) || *p == '_') {
                aer_error(line, "bad number"); // e.g. 12abc
            }
            add_token(&list, T_NUMBER, line)->number = (int32_t)value;
            continue;
        }

        // strings: "text", with the escapes \n \t \0 \\ and \" (a string can't span lines)
        if (c == '"') {
            // first find the closing quote, skipping escaped characters (so \" doesn't end it)
            const char* start = ++p;
            while (*p && *p != '"' && *p != '\n') {
                if (*p == '\\' && p[1]) {
                    p++;
                }
                p++;
            }
            if (*p != '"') {
                aer_error(line, "string is missing its closing \"");
            }
            // then copy it, decoding the escapes. The text can only get shorter (\n is 2
            // characters in the source and 1 in the string), so the source length is enough room
            char* text = aer_alloc((size_t)(p - start) + 1);
            uint32_t length = 0;
            for (const char* s = start; s < p; s++) {
                if (*s != '\\') {
                    text[length++] = *s;
                    continue;
                }
                s++;
                switch (*s) {
                    case 'n': text[length++] = '\n'; break;
                    case 't': text[length++] = '\t'; break;
                    case '0': text[length++] = '\0'; break;
                    case '\\': text[length++] = '\\'; break;
                    case '"': text[length++] = '"'; break;
                    default: aer_error(line, "unknown escape \\%c in a string", *s);
                }
            }
            text[length] = '\0';
            p++; // the closing quote
            token_t* t = add_token(&list, T_STRING, line);
            t->text = text;
            t->length = length;
            continue;
        }

        // names and keywords: a letter or _, then letters, digits and _
        if (isalpha((unsigned char)c) || c == '_') {
            const char* start = p;
            while (isalnum((unsigned char)*p) || *p == '_') {
                p++;
            }
            size_t length = (size_t)(p - start);
            token_type_t type = T_NAME;
            for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
                if (strlen(keywords[k].word) == length && strncmp(keywords[k].word, start, length) == 0) {
                    type = keywords[k].type;
                    break;
                }
            }
            token_t* t = add_token(&list, type, line);
            if (type == T_NAME) {
                // interned: every use of the same name shares one copy, so the interpreter
                // can compare names by pointer instead of character by character
                t->text = aer_intern(start, length);
            }
            continue;
        }

        // symbols, the two character ones first
        token_type_t type;
        int width = 2;
        if (c == '=' && p[1] == '=') {
            type = T_EQ;
        }
        else if (c == '!' && p[1] == '=') {
            type = T_NE;
        }
        else if (c == '<' && p[1] == '=') {
            type = T_LE;
        }
        else if (c == '>' && p[1] == '=') {
            type = T_GE;
        }
        else {
            width = 1;
            switch (c) {
                case '+': type = T_PLUS; break;
                case '-': type = T_MINUS; break;
                case '*': type = T_STAR; break;
                case '/': type = T_SLASH; break;
                case '%': type = T_PERCENT; break;
                case '<': type = T_LT; break;
                case '>': type = T_GT; break;
                case '=': type = T_ASSIGN; break;
                case ',': type = T_COMMA; break;
                case '(': type = T_LPAREN; depth++; break;
                case ')': type = T_RPAREN; if (depth > 0) depth--; break;
                case '[': type = T_LBRACKET; depth++; break;
                case ']': type = T_RBRACKET; if (depth > 0) depth--; break;
                default:
                    if (isprint((unsigned char)c)) {
                        aer_error(line, "unexpected character '%c'", c);
                    }
                    aer_error(line, "unexpected character (code %d)", (unsigned char)c);
            }
        }
        add_token(&list, type, line);
        p += width;
    }

    // make sure the last statement is ended, even if the file doesn't end with a line break
    if (list.count > 0 && list.tokens[list.count - 1].type != T_NEWLINE) {
        add_token(&list, T_NEWLINE, line);
    }
    add_token(&list, T_EOF, line);
    *count = list.count;
    return list.tokens;
}
