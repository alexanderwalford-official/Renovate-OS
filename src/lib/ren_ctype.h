/*
    ren_ctype.h
    Character classification (freestanding replacement for <ctype.h>).
*/
#ifndef REN_CTYPE_H
#define REN_CTYPE_H

static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c)  { return isupper(c) || islower(c); }
static inline int isalnum(int c)  { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
static inline int isprint(int c)  { return c >= 32 && c < 127; }
static inline int isgraph(int c)  { return c > 32 && c < 127; }
static inline int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
static inline int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }

#endif /* REN_CTYPE_H */
