#ifndef F_SYS_H
#define F_SYS_H

#include "stdint.h"
#include "../drivers/video/IO.h"
#include "../lib/primary_definitions.h"

typedef struct file {
    int f_id; // ID
    int f_size; // size
    char* f_path; // path / folder
    char* f_name; // name
    char* f_datetime; // date & time created
    char* f_cont; // contents as string
};

typedef struct directory {
    int d_id;
    char* d_path;
    char* d_name;
};

// dynamic vars
int f_c = 0; // file count
int d_c = 0; // directory count
struct file files[];
struct directory directories[3];
char* cd = "/"; // current directory

// fs init
int fs_init (char* arg) {
    puts(0, 0, BLACK, BRIGHT, "< INIT TMP FS .. >");
    (arg == "n") ? fs_init_base() : (arg == "p") ? fs_print() : ret();
    return 0;
}

// init all base os files
void fs_init_base () {
    puts(0, 1, BLACK, BRIGHT, "< MK TMP REQ FS .. >");
    // .. 
    fs_create_d("", "/");
    fs_create_d("accounts", "/acc/");
    fs_create_d("bin", "/bin/");
    fs_create_d("system", "/sys/");
    fs_create_d("system cache", "/sys/cache/");
    fs_create_d("system temp", "/sys/tmp/");
    fs_create_f("/", "conf.ren", "# here you will find all major system config options");

    puts(0, 1, BLACK, BRIGHT, "< TMP REQ FS DONE .. >");
}

// create a file
void fs_create_f (char* f_path, char* f_name, char* f_cont) {
    f_c = f_c + 1;
    struct file f;
    f.f_id = f_c;
    f.f_datetime = "DD-MM-YY--00:00:00";
    f.f_name = f_name;
    f.f_path = f_path;
    f.f_cont = f_cont;
    f.f_size = sizeof(f_cont);
    struct file files = f;
}

// create a directory
void fs_create_d (char* d_path, char* d_name) {
    d_c = d_c + 1;
    struct directory d;
    d.d_id = d_c;
    d.d_path = d_path;
    d.d_name = d_name;
    struct directory directories = d;
}

void fs_print () {
    // print all files & subdirectories in cd
    clear(BRIGHT);
    puts(0, 0, BLACK, BRIGHT, cd); // print current dir
    for (int i = 0; i < sizeof(directories); ++i) {
        (contains(directories[i].d_path, cd, 0)) ? puts(0, 1, BLACK, BRIGHT, directories[i].d_path) : ret(); // found a directory with cd in path
    }
}

#endif /* f_sys_h */