#pragma once
#include <stdint.h>
typedef struct mgpu_device mgpu_device_t;
mgpu_device_t* mgpu_open(const char* path);
void mgpu_close(mgpu_device_t* d);
