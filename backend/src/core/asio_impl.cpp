// Separate-compilation translation unit for Boost.Asio (+SSL) and Boost.Beast.
// Every other TU sees BOOST_ASIO_SEPARATE_COMPILATION / BOOST_BEAST_SEPARATE_COMPILATION
// (set PUBLIC on azmail_lib) and only declarations; the definitions live here exactly once.
// This file is excluded from the precompiled header (see CMakeLists.txt).
#include <boost/asio/impl/src.hpp>
#include <boost/asio/ssl/impl/src.hpp>
#include <boost/beast/src.hpp>
