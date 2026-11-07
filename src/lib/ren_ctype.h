/*
    ren_ctype.h
    Character classification (freestanding replacement for <ctype.h>).
*/
#ifndef REN_CTYPE_H
#define REN_CTYPE_H

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c)  { return c >= 32 && c < 127; }
int isgraph(int c)  { return c > 32 && c < 127; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }

#endif /* REN_CTYPE_H */
