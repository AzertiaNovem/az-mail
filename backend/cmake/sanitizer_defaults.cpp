// Linked into azmail and azmail_tests when AZMAIL_SANITIZE=ON (see CMakeLists.txt).
//
// container-overflow checks need *every* library touching std containers to be instrumented;
// prebuilt Catch2 / Boost.JSON / Boost.ProgramOptions are not, which yields false positives
// (std::vector/std::string annotated on one side of the boundary only). All other ASan checks
// stay on. ASAN_OPTIONS / UBSAN_OPTIONS from the environment still take precedence.
extern "C" const char* __asan_default_options() { return "detect_container_overflow=0"; }
extern "C" const char* __ubsan_default_options() { return "print_stacktrace=1:halt_on_error=1"; }
