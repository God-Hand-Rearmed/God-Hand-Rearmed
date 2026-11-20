/* Linux shared libraries of the GS (libpcsx2gs.so, libshaderc_shared.so.1): the static destructors a library
   registered (__cxa_atexit with its __dso_handle) run when it is unloaded. GCC's crtbeginS.o does this
   (__do_global_dtors_aux calls __cxa_finalize); zig's toolchain (cmake/toolchains/linux-portable.cmake) links no
   crtbegin, so without this a dlclose left the destructors registered and exit() called into the unmapped library:
   dyn_shaderc closes libshaderc_shared.so.1 in an atexit handler, and every exit of the portable build crashed. A
   second __cxa_finalize for the same library (GCC builds) finds nothing left to run. */
extern void *__dso_handle __attribute__((visibility("hidden")));
extern void __cxa_finalize(void *dso) __attribute__((weak));

__attribute__((destructor)) static void kamite_dso_finalize(void)
{
    if (__cxa_finalize)
        __cxa_finalize(&__dso_handle);
}
