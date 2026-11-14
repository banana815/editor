#ifndef NOTELAB_TEST_EMSCRIPTEN_STUB_H
#define NOTELAB_TEST_EMSCRIPTEN_STUB_H

/* Host-side queue tests only: declare each JavaScript bridge as a C function.
 * The test driver supplies those functions and controls when a flush finishes.
 * This does not emulate Emscripten, IndexedDB, or browser lifecycle behavior.
 * Include this directory only in the persistence queue test target. */
#define EMSCRIPTEN_KEEPALIVE
#define EM_JS(result, name, parameters, ...) result name parameters

#endif
