// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/fts.hpp"

#include "core/errors.hpp"

namespace azm::mail {

void fts_reindex(db::Tx&, int64_t) { throw NotImplemented("mail::fts_reindex"); }
int64_t fts_rebuild_all(db::Pool&, int, const std::function<void(int64_t, int64_t)>&) {
  throw NotImplemented("mail::fts_rebuild_all");
}
std::string fts_quote(std::string_view) { throw NotImplemented("mail::fts_quote"); }

}  // namespace azm::mail
