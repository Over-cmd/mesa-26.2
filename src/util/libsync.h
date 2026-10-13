#ifndef MESA_LIBSYNC_H_WRAPPER
#define MESA_LIBSYNC_H_WRAPPER
#include <sync/sync.h>
int sync_accumulate(const char *name, int *fd1, int fd2);
void validate_fence_fd(int fd);
int sync_valid_fd(int fd);
int sync_wait(int fd, int timeout);
int sync_merge(const char *name, int fd1, int fd2);
#endif
