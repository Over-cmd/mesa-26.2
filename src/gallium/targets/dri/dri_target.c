#include "util/detect_os.h"
 
#include "target-helpers/drm_helper.h"
#include "target-helpers/sw_helper.h"

/* Prototipos */
int sync_wait(int fd, int timeout);
int sync_accumulate(const char *name, int *fd1, int fd2);
void validate_fence_fd(int fd);
/* Cuerpos */
int sync_wait(int fd, int timeout) { return 0; }
int sync_accumulate(const char *name, int *fd1, int fd2) { return 0; }
void validate_fence_fd(int fd) { return; }
