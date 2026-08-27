#include "sculpt.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

typedef enum {
    TYPE_INT,
    TYPE_BOOL,
    TYPE_STR
} sc_cfg_type;

typedef struct _sc_cfg_ptr {
    sc_cfg_type type;
    void *data;
} sc_cfg_ptr;

struct _config_entry {
    const char *name;
    const sc_cfg_type type;
    int (*config_callback)(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr);
};

int _sc_cfg_ptr_get_int(sc_cfg_ptr container, int *out) {
    assert(out != NULL);
    FPRINTF_RETURN_ERROR_IF(out == NULL, SC_BAD_ARGUMENTS_ERR, "[developer bug] NULL output pointer passed to _sc_cfg_ptr_get_int\n");
    FPRINTF_RETURN_ERROR_IF(container.type != TYPE_INT || !container.data, SC_BAD_ARGUMENTS_ERR, "[developer bug] _sc_cfg_ptr_get_int called with wrong type on container or invalid data\n");

    *out = *(const int*)container.data;
    return SC_OK;
}

int _sc_cfg_ptr_get_bool(sc_cfg_ptr container, bool *out) {
    assert(out != NULL);
    FPRINTF_RETURN_ERROR_IF(out == NULL, SC_BAD_ARGUMENTS_ERR, "[developer bug] NULL output pointer passed to _sc_cfg_ptr_get_bool\n");
    FPRINTF_RETURN_ERROR_IF(container.type != TYPE_BOOL || !container.data, SC_BAD_ARGUMENTS_ERR, "[developer bug] _sc_cfg_ptr_get_bool called with wrong type on container or invalid data\n");

    *out = *(const bool*)container.data;
    return SC_OK;
}

int _sc_cfg_ptr_get_str(sc_cfg_ptr container, const char **out) {
    assert(out != NULL);
    FPRINTF_RETURN_ERROR_IF(out == NULL, SC_BAD_ARGUMENTS_ERR, "[developer bug] NULL output pointer passed to _sc_cfg_ptr_get_str\n");
    FPRINTF_RETURN_ERROR_IF(container.type != TYPE_STR || !container.data, SC_BAD_ARGUMENTS_ERR, "[developer bug] _sc_cfg_ptr_get_str called with wrong type on container or invalid data\n");

    *out = (const char*)container.data;
    return SC_OK;
}

static int apply_generic_int(
        sc_conn_mgr *mgr, 
        sc_cfg_ptr typed_ptr, 
        const char *cfg_name, 
        void (*setter)(sc_conn_mgr *, int)
        ) {
    int value;
    int err = _sc_cfg_ptr_get_int(typed_ptr, &value);
    if (err != SC_OK) return err;
    setter(mgr, value);
    sc_log(mgr, SC_LL_DEBUG, "Set %s to %d via config file\n", cfg_name, value);
    return SC_OK;
}

static int apply_generic_bool(
        sc_conn_mgr *mgr, 
        sc_cfg_ptr typed_ptr, 
        const char *cfg_name, 
        void (*setter)(sc_conn_mgr *, int)
        ) {
    bool value;
    int err = _sc_cfg_ptr_get_bool(typed_ptr, &value);
    if (err != SC_OK) return err;
    setter(mgr, value);
    sc_log(mgr, SC_LL_DEBUG, "Set %s to %s via config file\n", cfg_name, value ? "true" : "false");
    return SC_OK;
}

static int apply_generic_str(
        sc_conn_mgr *mgr, 
        sc_cfg_ptr typed_ptr, 
        const char *cfg_name, 
        void (*setter)(sc_conn_mgr *, const char *)
        ) {
    const char *value; // sc_mgr_set will always copy the string when setting it, so this is safe
    int err = _sc_cfg_ptr_get_str(typed_ptr, &value);
    if (err != SC_OK) return err;
    setter(mgr, value);
    sc_log(mgr, SC_LL_DEBUG, "Set %s to %s via config file\n", cfg_name, value);
    return SC_OK;
}

static int apply_log_level(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_int(mgr, typed_ptr, "log level", sc_mgr_ll_set);
}

static int apply_backlog_size(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_int(mgr, typed_ptr, "backlog size", sc_mgr_backlog_set);
}

static int apply_epoll_maxevents(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_int(mgr, typed_ptr, "epoll max events", sc_mgr_epoll_maxevents_set);
}

static int apply_conn_recycling(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_bool(mgr, typed_ptr, "connection recycling",sc_mgr_conn_recycling_set);
}

static int apply_conn_pool_size(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_int(mgr, typed_ptr, "connection pool size", sc_mgr_conn_pool_size_set);
}

static int apply_addr(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_str(mgr, typed_ptr, "address", sc_mgr_addr_set);
}

static int apply_port(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    int value;
    int err = _sc_cfg_ptr_get_int(typed_ptr, &value);
    if (err != SC_OK) return err;
    if (value < 0 || value > 65535) {
        sc_log(mgr, SC_LL_MINIMAL, "[W] Read an invalid port (%d; either <0 or >65535) in the config file. Ignoring this entry\n", value); 
        return SC_BAD_ARGUMENTS_ERR;
    }
    sc_mgr_port_set(mgr, (unsigned short) value);
    sc_log(mgr, SC_LL_DEBUG, "Set port to %d via config file\n", value);
    return SC_OK;
}

static int apply_conn_timeout(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_int(mgr, typed_ptr, "connection timeout", sc_mgr_conn_timeout_set);    
}

static int apply_conn_max_age(sc_conn_mgr *mgr, sc_cfg_ptr typed_ptr) {
    return apply_generic_int(mgr, typed_ptr, "connection max age", sc_mgr_conn_max_age_set);    
}


const struct _config_entry config_registry[] = {
    { "log_level", TYPE_INT, apply_log_level },
    { "backlog_size", TYPE_INT, apply_backlog_size },
    { "epoll_maxevents", TYPE_INT, apply_epoll_maxevents },
    { "conn_recycling", TYPE_BOOL, apply_conn_recycling },
    { "conn_pool_size", TYPE_INT, apply_conn_pool_size },
    { "address", TYPE_STR, apply_addr },
    { "port", TYPE_INT, apply_port },
    { "conn_timeout", TYPE_INT, apply_conn_timeout },
    { "conn_max_age", TYPE_INT, apply_conn_max_age }
};

static int parse_config_value(sc_cfg_type type, char *val_buf, char *str_result, int* int_result, bool *bool_result, size_t str_result_size, size_t curr_line) {
    // not much else we can do if not a switch with every type
    errno = 0;

    char *endptr;
    switch (type) {
        case TYPE_INT:
            *int_result = strtol(val_buf, &endptr, 10);
            if (errno != 0 || endptr == val_buf) {
                return SC_CFG_INT_PARSE_ERR;
            }
            if (*endptr != '\0') {
                fprintf(stderr, "[W] Found bad integer when parsing value %s at line %zu, proceeding with value %d.\n", val_buf, curr_line, *int_result);

            }
            break;
        case TYPE_BOOL:
            *bool_result = (bool) (strncmp(val_buf, "true", 4) == 0);
            break;
        case TYPE_STR:
            strncpy(str_result, val_buf, str_result_size);
            str_result[str_result_size - 1] = '\0';
            break;
    }

    return SC_OK;
}

static int find_entry(const char *key, const struct _config_entry **found_entry) {
    *found_entry = NULL;
    for (size_t idx = 0; idx < sizeof(config_registry) / sizeof(config_registry[0]); idx++) {
        const struct _config_entry entry = config_registry[idx];
        if (strcmp(entry.name, key) == 0) {
            *found_entry = &config_registry[idx];
            return SC_OK;
        }
    }
    return SC_NOT_FOUND_ERR;
}

static int parse_line(char *buf, size_t buf_size, char *key_buf, size_t key_buf_size, char *value_buf, size_t value_buf_size, size_t line_idx) {
    bool reading_key = true;
    bool is_reading = false;
    size_t last_space = -1;

    key_buf[0] = '\0';
    size_t key_len = 0;
    value_buf[0] = '\0';
    size_t value_len = 0;

    for (
            size_t idx = 0; 
            idx < buf_size && buf[idx] != '\0' && buf[idx] != '#' && buf[idx] != '\n' && buf[idx] != '\r';
            idx++
        ) {
        // we ignore IF: 
        // 1. we encounter a tab
        // 2. we encounter a space AND we are not yet reading the value (leading space)
        // 3. we encounter a space AND we are reading the key (leading or trailing; doesn't matter in key)
        if ((buf[idx] == ' ' && (!is_reading || reading_key)) || buf[idx] == '\t') continue;
        is_reading = true;

        // logic to avoid trailing spaces
        if (!reading_key && buf[idx] == ' ' && last_space == -1) {
            last_space = value_len;
        }
        if (!reading_key && buf[idx] != ' ') {
            last_space = -1;
        }

        if (buf[idx] == '=') {
            reading_key = false;
            is_reading = false;
            continue;
        }

        if (key_len >= key_buf_size - 1 || value_len >= value_buf_size - 1) {
            fprintf(stderr, "Config line %zu too long; either the config key or value exceeded the maximum buffer length of %zu and %zu characters, respectively.\n", line_idx, key_buf_size, value_buf_size);
            return SC_BUFFER_OVERFLOW_ERR;
        }

        if (reading_key) {
            key_buf[key_len++] = buf[idx];
            key_buf[key_len] = '\0';
        } else {
            value_buf[value_len++] = buf[idx];
            value_buf[value_len] = '\0';
        }
    }

    // trim value_buf's trailing spaces
    if (last_space != -1) {
        value_buf[last_space] = '\0';
    }

    return 0;
}

int sc_mgr_config_apply(sc_conn_mgr *mgr, char *filepath) {
    FPRINTF_RETURN_ERROR_IF(!mgr || !mgr->mgr_initialized, SC_BAD_STATE_ERR, "[Sculpt] Detected bad state during config loading (sc_conn_mgr is NULL or unitialized)\n");
    FPRINTF_RETURN_ERROR_IF(filepath == NULL, SC_BAD_ARGUMENTS_ERR, "Config filepath can't be NULL\n");
    if (mgr->epoll_initialized || mgr->pool_initialized || mgr->listening) {
        fprintf(stderr, "[Sculpt] [W] It seems some sculpt components were already started/initialized before you tried to load configs. It is reccomended to load the configs *before* initializing anything, as most configs can't be changed after initialization. Some configs may not be properly applied");
    }
    FILE *fptr;

    fptr = fopen(filepath, "r");
    RETURN_ERROR_IF(!fptr, SC_BAD_ARGUMENTS_ERR, "Config filepath on sc_mgr_apply_config invalid\n");

    char line_buf[SC_CONFIG_LINE_BUF];
    char key_buf[SC_CONFIG_KEY_BUF];
    char value_buf[SC_CONFIG_VALUE_BUF];

    size_t parsed_lines= 0, curr_line = 0;
    for (curr_line = 0; fgets(line_buf, sizeof(line_buf), fptr) != NULL; curr_line++) {
        const char last_char = line_buf[strlen(line_buf) - 1];

        if (last_char != '\n' && !feof(fptr)) {
            // line is longer than buffer
            // throw buffer overflow err
            fprintf(stderr, "Config line %zu length exceeded the limit of %zu chars\n", curr_line, sizeof(line_buf));
            fclose(fptr);
            return SC_BUFFER_OVERFLOW_ERR;
        }
        
        key_buf[0] = '\0';
        value_buf[0] = '\0';
        
        int err = parse_line(line_buf, SC_CONFIG_LINE_BUF, key_buf, sizeof(key_buf), value_buf, sizeof(value_buf), curr_line);
        if (err != SC_OK) {
            fclose(fptr);
            return err;
        }

        if (key_buf[0] == '\0') {
            // blank line OR comment line
            continue;
        }

        if (value_buf[0] =='\0') {
            // empty config
            fprintf(stderr, "[W] Config with key %s (line %zu) seems to have an empty value. Ignoring this entry.\n", key_buf, curr_line);
            continue;
        }

        // now, we try to find a config with this key. if it is found, we parse the value and apply it
        const struct _config_entry *entry;
        err = SC_OK;
        if ((err = find_entry(key_buf, &entry)) == SC_NOT_FOUND_ERR) { 
            fprintf(stderr, "[W] Config with key %s (line %zu) is not known. Ignoring this entry.\n", key_buf, curr_line);
            continue;
        }
        if (err != SC_OK) {
            fprintf(stderr, "[W] Error code %d when parsing config with key %s (line %zu). Ignoring this entry.\n", err, key_buf, curr_line);
            continue;
        }
        if (entry == NULL) {
            fprintf(stderr, "[W] Got a null entry when parsing config with key %s (line %zu). This is likely a developer error, since the error code was SC_OK. Ignoring this entry.\n", key_buf, curr_line);
            continue; 
        }

        // finally, we parse the value and apply using the entry's function
        struct _sc_cfg_ptr typed_config = {0};
        char str_value[SC_CONFIG_VALUE_BUF];
        int int_value;
        bool bool_value;
        err = parse_config_value(entry->type, value_buf, str_value, &int_value, &bool_value, SC_CONFIG_VALUE_BUF, curr_line);
        if (err == SC_CFG_INT_PARSE_ERR) {
            fprintf(stderr, "[W] Error parsing integer `%s` for key %s (line %zu). Ignoring this entry.\n", key_buf, value_buf, curr_line);
            continue;
        }
        if (err != SC_OK) {
            fprintf(stderr, "[W] Error code %d when parsing config `%s` with key %s (line %zu). Ignoring this entry.\n", err, value_buf, key_buf, curr_line);
            continue;
        }

        typed_config.type = entry->type;
        switch (entry->type) {
            case TYPE_INT:
                typed_config.data = &int_value;
                break;
            case TYPE_BOOL:
                typed_config.data = &bool_value;
                break;
            case TYPE_STR:
                typed_config.data = str_value;
                break;
        }

        err = entry->config_callback(mgr, typed_config);
        if (err != SC_OK) {
            fprintf(stderr, "[W] Error code %d when applying config `%s` with key `%s` (line %zu). Make sure your config is correct. Ignoring this entry.\n", err, value_buf, key_buf, curr_line);
            continue;
        }

        parsed_lines++;
    }

    fclose(fptr);
    sc_log(mgr, SC_LL_NORMAL, "Read %zu lines and applied %zu configs from %s\n", curr_line, parsed_lines, filepath);

    return SC_OK;
}

static inline bool file_exists(const char *name) {
    return (access(name, F_OK) == 0);
}

int sc_mgr_config_auto_apply(sc_conn_mgr *mgr) {
    // we will check for: 1. sculpt.conf; 2. sculpt.txt; 3. config.sculpt

    if (file_exists("sculpt.conf")) {
        fprintf(stdout, "[Sculpt] [Config Loader] Auto loading config file 'sculpt.conf'\n");
        return sc_mgr_config_apply(mgr, "sculpt.conf");
    }

    if (file_exists("sculpt.txt")) {
        fprintf(stdout, "[Sculpt] [Config Loader] Auto loading config file 'sculpt.txt'\n");
        return sc_mgr_config_apply(mgr, "sculpt.txt");
    }

    if (file_exists("config.sculpt")) {
        fprintf(stdout, "[Sculpt] [Config Loader] Auto loading config file 'config.sculpt'\n");
        return sc_mgr_config_apply(mgr, "config.sculpt");
    }

    fprintf(stderr, "[Sculpt] [Config Loader] [W] No valid config file found via the automatic loader. Proceeding with the default config.\n");
    
    return SC_NOT_FOUND_ERR;
}
