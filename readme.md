# Sculpt

A Minimal C HTTP server framework providing direct socket access for maximum user control.
A sample application is provided in the app.c file

## Quickstart

To use the sculpt framework in your project, you can statically link it.

### Stable release
Get the latest release from github, download and place the `sculpt.c` and `sculpt.h` files where you find adequate (e.g. ./external/sculpt).
After that, statically link them using a build system, build system generator, or just a plain old compiler.

### Unstable release
If you wan't the latest version, you can either get it from the `stable` branch (more stable) or the `devel` branch, which is more unstable and prone to bugs. Clone the repo, remove the `prod/sculpt.c` and `prod/sculpt.h` files, and run the `/prod/package.sh` script to generate them. You can then follow the same steps in the Stable release section above for linking.

### Documentation
For the full documentation on actually using the framework, check out [documentation.md](documentation.md)


## Running test app

If you wish to run the test app provided by this repo, clone it and run:

```
mkdir build; cd build
cmake ..
make
./testapp
```

## Contributing

There are several ways to contribute to Sculpt. You can create your own fork, help with issues or with new features.
To compile the project when developing it, just follow the same instructions on the Running the test app section above.

### Coding style

Here are the general coding style rules used in this project:

- The identation style used is the `K&R`, even for functions, so ou should put the braces in the same line of the statements.
- Use whitespaces between operators (a + 1 instead of a+1)
- Space the parenthesis in control structures (if, while, for, etc) (while (condition) instead of while(condition)).
For example:
```
void foo() {
  if (bar == fubar) {
    do_thing();
  } else {
    do_other_thing();
  }
}
```

Always use braces with single-lined if, while, for, else, and other statements. The only situation where you may omit the braces is when checking for invalid arguments in functions, or checking for simple conditions that result in an immediate return. For example:
```
void do_thing(char *arg) {
  if (arg == NULL) return;

  // other function stuff
}
```

For the structs used by the user, you can typedef them for better readability. For example:
```
typedef struct {...} sc_conn_mgr;
instead of
struct mg_conn_mgr {...}
```

You can also mark structs that the user should not use with an underscore (_) before its name, and they shouldn't be typedef'd. For example:

```
// this struct is completely abstracted by functions, so the user shouldn't tinker with it.
struct _endpoint_list {
    sc_str val;
    void (*func)(int, sc_http_msg);
    bool soft;
    struct _endpoint_list *next;
};
```

#### Naming conventions

Generally, the name for your functions should follow the following pattern: `sc_{specificator}_{scope}_{subject}_{action}`. Of course, you can omit the fields that you don't need, but the action and sc_ are always necessary.
Examples:
```
char *sc_easy_request_build(); // here, we specify that the function is "easy" (it doesn't require specific configurations, and does most things for you). Its 'subject' is the request, and the "action" is build. From this, we conclude that it is a simple request builder.
void sc_mgr_conn_pool_destroy(); // here, the specificator is none. The "scope" is mgr, the "subject" is conn_pool, and the "action" is destroy. From this, we conclude that it destroys the connection pool of the sc manager.
sc_addr_info sc_addr_create(); // here, the specificator is none, the "scope" and the "subject" is addr, and the "action" is create. From this,  we conclude that the function creates an sc address.
sc_mgr_epoll_init();
sc_mgr_listen();
sc_log();
sc_error_log();
And so on.
```

You may not use these conventions when creating functions that the user shall not use, such as static functions within the source files.

#### Error handling

##### Error logging

Firstly, you should ALWAYS log your errors at least on the debug log level (as explained later). No exceptions here.
For better logging, the sculpt framework uses a logging system, with different levels. Here are them, and a small exlpanation about each:
- SC_LL_NONE: this Log Level (LL) doesn't log anything at all.
- SC_LL_MINIMAL: This LL only logs the most important events with essential info, such as the URI's and methods of incoming requests, and critical errors.
- SC_LL_NORMAL: This LL logs everything of the lower log levels, plus all semi-critical and other important errors. It also logs important events and information, such as if a request is from a new connection or from an already existing one.
- SC_LL_DEBUG: This LL logs absolutely everything, including all errors, warnings and information about the server. This includes the number of connectoins after each epoll poll, the status of new and current connections, keep-alive or close requests, parsing results and more.

When logging something new, you can use three functions:
- sc_log: for logging everything that is not an error or warning
- sc_error_log: for logging internal errors. It usess fprintf(stderr, format) under the hood, so use it as you would use fprintf(stderr).
- sc_perror: for logging external errors with errno, as it uses perror() under the hood.

These functions take the following arguments:
void sc_log(sc_conn_mgr *mgr, int ll, const char *format, ...);
mgr: the connection manager for the running server. Used to get the current log level.
ll: the log level that will be used for the logging action.
format, ...: The formatted string to be logged.
P.S.: The perror function doesn't support formatted output, so the sc_perror function also doesn't support it.

The ll you pass to the function should be in what level you want to log it. If it is a very specific thing you only want to log when using LL_DEBUG, pass LL_DEBUG to it. If it is something essential, and you want it to be logged under LL_MINIMAL, LL_NORMAL and LL_DEBUG, pass LL_MINIMAL to it, and so on.


