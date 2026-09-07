/* LEGOLAND portable build -- MSVC <direct.h> names used by profiles.c and
 * narration2.c. Implemented by the host shim. */
#ifndef LL_HOSTWIN_DIRECT_H
#define LL_HOSTWIN_DIRECT_H

int   _mkdir(const char* path);
int   _chdir(const char* path);
char* _getcwd(char* buf, int size);

#endif
