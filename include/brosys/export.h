#pragma once

#if defined(BROSYS_STATIC_DEFINE)
    #define BROSYS_API
#elif defined(_WIN32) || defined(__CYGWIN__)
    #if defined(BROSYS_EXPORTS)
        #define BROSYS_API __declspec(dllexport)
    #else
        #define BROSYS_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define BROSYS_API __attribute__((visibility("default")))
    #else
        #define BROSYS_API
    #endif
#endif
