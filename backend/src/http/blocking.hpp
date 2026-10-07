// Owner: WP-A (frozen; implemented inline in WP0 exactly as DESIGN §3)
//
// run_blocking(pool, f): runs the synchronous callable `f` on a blocking thread pool and resumes
// the calling coroutine on ITS OWN executor (the connection strand) with f's result. Exceptions
// thrown by f are rethrown in the caller. This is the only correct hand-off (DESIGN A1):
// `co_await post(pool, use_awaitable)` does NOT move the coroutine onto the pool.
// The callable must not touch the connection's stream.
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <type_traits>
#include <utility>

namespace azm::http {

namespace asio = boost::asio;

template <class F>
asio::awaitable<std::invoke_result_t<F>> run_blocking(asio::thread_pool& pool, F f) {
  using R = std::invoke_result_t<F>;
  co_return co_await asio::co_spawn(
      pool, [f = std::move(f)]() mutable -> asio::awaitable<R> { co_return f(); },
      asio::use_awaitable);
}  // resumes on the caller's strand; exceptions rethrown in caller

}  // namespace azm::http
