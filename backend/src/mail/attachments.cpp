// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/attachments.hpp"

#include "core/errors.hpp"

namespace azm::mail {

void register_blob(db::Tx&, const BlobRef&, int64_t) { throw NotImplemented("mail::register_blob"); }
AttachmentView create_upload(db::Tx&, const SignedUrls&, int64_t, const BlobRef&, std::string_view,
                             std::string_view, bool, int64_t) {
  throw NotImplemented("mail::create_upload");
}
std::optional<AttachmentRecord> find_attachment(db::Conn&, int64_t, int64_t) {
  throw NotImplemented("mail::find_attachment");
}
std::vector<AttachmentRecord> message_attachments(db::Conn&, int64_t, int64_t) {
  throw NotImplemented("mail::message_attachments");
}
std::optional<RawRef> find_raw(db::Conn&, int64_t, int64_t) { throw NotImplemented("mail::find_raw"); }
int purge_orphan_uploads(db::Tx&, int64_t, int) { throw NotImplemented("mail::purge_orphan_uploads"); }
std::vector<BlobRef> unreferenced_blobs(db::Conn&, int64_t, int) {
  throw NotImplemented("mail::unreferenced_blobs");
}
bool is_blob_unreferenced(db::Conn&, std::string_view) {
  throw NotImplemented("mail::is_blob_unreferenced");
}
bool forget_blob_if_unreferenced(db::Tx&, std::string_view) {
  throw NotImplemented("mail::forget_blob_if_unreferenced");
}

}  // namespace azm::mail
