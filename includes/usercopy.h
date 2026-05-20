#pragma once

#include <stddef.h>
#include <stdint.h>

int copy_from_user(void *dst, uint64_t user_src, size_t len);
int copy_to_user(uint64_t user_dst, const void *src, size_t len);
int copy_string_from_user(char *dst, size_t dst_size, uint64_t user_src);
int user_range_readable(uint64_t user_src, size_t len);
