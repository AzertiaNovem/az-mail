// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/threads.hpp"

#include "core/errors.hpp"

namespace azm::mail {

int64_t assign_thread(db::Tx&, int64_t, const ThreadingKeys&) {
  throw NotImplemented("mail::assign_thread");
}
int adopt_referencing(db::Tx&, int64_t, int64_t, std::string_view) {
  throw NotImplemented("mail::adopt_referencing");
}
int64_t merge_threads(db::Tx&, int64_t, int64_t, int64_t) {
  throw NotImplemented("mail::merge_threads");
}
bool recompute_thread(db::Tx&, int64_t, int64_t) { throw NotImplemented("mail::recompute_thread"); }
std::string normalize_subject(std::string_view) { throw NotImplemented("mail::normalize_subject"); }
bool has_reply_prefix(std::string_view) { throw NotImplemented("mail::has_reply_prefix"); }
std::vector<int64_t> threads_for_refs(db::Conn&, int64_t, std::span<const std::string>) {
  throw NotImplemented("mail::threads_for_refs");
}

}  // namespace azm::mail
