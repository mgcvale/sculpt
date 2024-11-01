# Sculpt

A Minimal C HTTP server framework providing direct socket access for maximum user control.
A sample application is provided in the app.c file.

# Building

```
mkdir build; cd build
cmake ..
make
./testapp
```

If you are using the clangd LSP for vim, you can run cmake with the `-DCMAKE_EXPORT_COMPILE_COMMANDS=YES` CLI option.
