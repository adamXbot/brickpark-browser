/* LEGOLAND portable build -- MSVC <io.h> names used by savegame.c and
 * profiles.c. Flag values are MSVC's; the host shim translates them. */
#ifndef LL_HOSTWIN_IO_H
#define LL_HOSTWIN_IO_H

#ifndef _O_RDONLY
#define _O_RDONLY 0x0000
#define _O_WRONLY 0x0001
#define _O_RDWR   0x0002
#define _O_APPEND 0x0008
#define _O_CREAT  0x0100
#define _O_TRUNC  0x0200
#define _O_EXCL   0x0400
#define _O_TEXT   0x4000
#define _O_BINARY 0x8000
#endif
#ifndef _S_IREAD
#define _S_IREAD  0x0100
#define _S_IWRITE 0x0080
#endif

#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH   0x20

struct _finddata_t {
    unsigned int  attrib;
    long          time_create;
    long          time_access;
    long          time_write;
    unsigned long size;
    char          name[260];
};

int  _open(const char* path, int oflag, ...);
int  _close(int fd);
int  _read(int fd, void* buf, unsigned int count);
int  _write(int fd, const void* buf, unsigned int count);
long _lseek(int fd, long offset, int origin);
long _tell(int fd);
long _findfirst(const char* spec, struct _finddata_t* out);
int  _findnext(long handle, struct _finddata_t* out);
int  _findclose(long handle);

#endif
