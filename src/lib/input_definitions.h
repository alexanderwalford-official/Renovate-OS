#ifndef INPUT_DEFINITIONS_H
#define INPUT_DEFINITIONS_H

#define __need_size_t
#include <primary_definitions.h>
#include <f_sys.h>

#define __need___va_list

#define __SLBF  0x0001          // ln buff
#define __SNBF  0x0002          // non ln buff
#define __SRD   0x0004          // OK RD
#define __SWR   0x0008          // OK WRT
#define __SRW   0x0010          // OPN RD WRT
#define __SEOF  0x0020          // EFO FND
#define __SERR  0x0040          // FND ERR
#define __SMBF  0x0080          // BUFF FROM MEM ALLOC
#define __SAPP  0x0100          // fdopen()ed in append mode - so must  write to end */
#define __SSTR  0x0200          // this is an sprintf/snprintf string
#define __SOPT  0x0400          // do fseek() optimisation */
#define __SNPT  0x0800          // do not do fseek() optimisation
#define __SOFF  0x1000          // set iff _offset is in fact correct
#define __SMOD  0x2000          // true => fgetline modified _p text
#if defined(__CYGWIN__) || defined(__CYGWIN__)
#define __SCLE        0x4000          /* convert line endings CR/LF <-> NL */
#endif

// ANSI C
#define _IOFBF  0               // setvbuf set fully buffered
#define _IOLBF  1               // setvbuf set line buffered 
#define _IONBF  2               // setvbuf set unbuffered

#ifndef NULL
#define NULL    0
#endif

#define BUFSIZ  1024
#define EOF     (-1)

#define FOPEN_MAX       20      
 
#define L_tmpnam        1024    /* XXX must be == PATH_MAX */
#ifndef __STRICT_ANSI__
#define P_tmpdir        "/tmp"
#endif
 
#ifndef SEEK_SET
#define SEEK_SET        0       /* set file offset to offset */
#endif
#ifndef SEEK_CUR
#define SEEK_CUR        1       /* set file offset to current plus offset */
#endif
#ifndef SEEK_END
#define SEEK_END        2       /* set file offset to EOF plus offset */
#endif
 
#define TMP_MAX         26

#define ioin   (_impure_ptr->_ioin)
#define ioout  (_impure_ptr->_ioout)
#define ioerr  (_impure_ptr->_ioerr)

#define _ioin_r(x)     ((x)->_ioin)
#define _ioout_r(x)    ((x)->_ioout)
#define _ioerr_r(x)    ((x)->_ioerr)

#ifdef __GNUC__
#define __VALIST __gnuc_va_list
#else
#define __VALIST char*
#endif
 
#ifndef _EXFUN
# define _EXFUN(N,P) N P
#endif
 
// IO type definitions
int     _EXFUN(printf, (const char *, ...));
int     _EXFUN(scanf, (const char *, ...));
int     _EXFUN(sscanf, (const char *, const char *, ...));
int     _EXFUN(fgetc, (file *));
char *  _EXFUN(fgets, (char *, int, file *));
int     _EXFUN(fputc, (int, file *));
int     _EXFUN(fputs, (const char *, file *));
int     _EXFUN(getc, (file *));
int     _EXFUN(getchar, (void));
char *  _EXFUN(gets, (char *));
int     _EXFUN(putc, (int, file *));
int     _EXFUN(putchar, (int));
int     _EXFUN(puts, (const char *));
int     _EXFUN(ungetc, (int, file *));

#endif /* INPUT_DEFINITIONS_H */