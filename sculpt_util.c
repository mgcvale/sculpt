#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "sculpt.h"

sc_str sc_mk_str(const char *str) {
    sc_str sc_str = {(char *) str, str == NULL ? 0 : strlen(str)};
    return sc_str;
}

sc_str sc_mk_str_n(const char *str, size_t len) {
    sc_str sc_str = {(char *) str, len};
    return sc_str;
}


int sc_strcmp(const sc_str str1, const sc_str str2) {
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

bool sc_strprefix(const sc_str str, const sc_str prefix) {
    if (str.len < prefix.len) {
        return false; // false if str is shorter than prefix
    }

    size_t i;
    for (i = 0; i < prefix.len; i++) {
        if (str.buf[i] != prefix.buf[i]) {
            return false;
        }
    }
    
    return true;
}

struct _endpoint_list *_endpoint_add(struct _endpoint_list *list, const char *endpoint, bool soft, void (*func)(int)) {
    struct _endpoint_list *new = malloc(sizeof(struct _endpoint_list));
    if (new == NULL) {
        return NULL;
    }

    new->soft = soft;
    new->func = func;
    sc_str val = sc_mk_str(endpoint);
    new->next = list;
    return new;
}
