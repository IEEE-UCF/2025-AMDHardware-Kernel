#include "mgpu.h"
#include <stdlib.h>
struct mgpu_device { int fd; };
mgpu_device_t* mgpu_open(const char* path){ (void)path; return calloc(1,sizeof(mgpu_device_t)); }
void mgpu_close(mgpu_device_t* d){ free(d); }
