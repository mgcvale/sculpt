#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "cast.h"

cst_str cst_mk_str(const char *str) {
    cst_str cst_str = {(char *) str, str == NULL ? 0 : strlen(str)};
    return cst_str;
}

cst_str cst_mk_str_n(const char *str, size_t len) {
    cst_str cst_str = {(char *) str, len};
    return cst_str;
}


int cst_strcmp(const cst_str str1, const cst_str str2) {
  size_t i;
  for (i = 0; i < str1.len && i < str2.len; i++) {
    int ch1 = str1.buf[i];
    int ch2 = str2.buf[i];
    if (ch1 < ch2) return -1;
    if (ch1 > ch2) return 1;
  }
  if (i < str1.len) return 1;
  if (i < str2.len) return -1;
  return 0;
}

bool cst_strprefix(const cst_str str, const cst_str prefix) {
    if (str.len < prefix.len) {
        return -1; // false if str is shorter than prefix
    }

    size_t i;
    for (i = 0; i < prefix.len; i++) {
        if (str.buf[i] != prefix.buf[i]) {
            return -1;
        }
    }
    
    return 0;
}

