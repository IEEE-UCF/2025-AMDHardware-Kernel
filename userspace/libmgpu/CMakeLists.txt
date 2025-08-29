cmake_minimum_required(VERSION 3.10)
project(libmgpu C)
add_library(mgpu STATIC mgpu_device.c mgpu_bo.c mgpu_queue.c mgpu_shader.c mgpu_debug.c)
target_include_directories(mgpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
