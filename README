## Building

1. Download and build chromium.
2. Add plugin to ppapi_test.gypi

```
    {
      # GN version: //ppapi/examples/window_stream
      'target_name': 'ppapi_window_stream',
      'dependencies': [
        'ppapi_example_skeleton',
        'ppapi.gyp:ppapi_cpp',
      ],
      'sources': [
        '../../../window_stream/window_stream.cc',
      ],
    },
```

3. Cd to chromium src\
4. Rebuild projects: build\gyp_chromium -Dcomponent=shared_library
5. Compile: ninja -C out\Release ppapi_window_stream

## License

![CC BY 4.0](https://i.creativecommons.org/l/by/4.0/88x31.png)
This work is licensed under the Creative Commons Attribution 4.0 International License. To view a copy of this license, visit http://creativecommons.org/licenses/by/4.0/ or send a letter to Creative Commons, PO Box 1866, Mountain View, CA 94042, USA.
