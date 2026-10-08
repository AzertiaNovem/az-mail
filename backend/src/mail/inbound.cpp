// Owner: WP-B
// Inbound routing (C1, C3, C4) and idempotent delivery fan-out (C2, C8, C11, B2).
#include "mail/inbound.hpp"

#include "core/errors.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "mail/attachments.hpp"
#include "mail/fts.hpp"
#include "mail/html_text.hpp"
#include "mail/internal.hpp"
#include "mail/mailbox.hpp"
#include "mail/outbound.hpp"
#include "mail/send_internal.hpp"
#include "mail/threads.hpp"
#include "ws/events.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace azm::mail {
namespace {

namespace json = boost::json;

constexpr int64_t kDayMs = 24LL * 3600 * 1000;
constexpr int64_t kSecondMs = 1000;
constexpr std::size_t kMaxErrorBytes = 500;

// messages.date of an inbound copy: the Date header, else Resend's receipt time, else now.
//  * A Date more than a day after the receipt is bogus (sender clock) → the receipt time.
//  * A Date header has whole-second precision while our own (out) copies carry milliseconds.
//    When the ms receipt time falls inside the header's second it is the same instant, measured
//    more precisely: use it. Otherwise a reply received within the second of the mail it
//    answers sorts BEFORE it (by up to 999 ms), takes over the thread subject (the earliest
//    message's) and is listed above its parent.
int64_t message_date(int64_t header_date, int64_t received_at, int64_t now) {
  int64_t date = header_date > 0 ? header_date : (received_at > 0 ? received_at : now);
  if (received_at > 0 && date > received_at + kDayMs) date = received_at;  // bogus future Date
  if (received_at > 0 && date % kSecondMs == 0 && received_at > date && received_at < date + kSecondMs)
    date = received_at;
  return date;
}

struct AddressHit {
  int64_t id = 0;
  std::string email;
  bool is_user = false;
  std::optional<int64_t> user_id;
};

// Address row for an email: exact (case-insensitive) first, then with the +tag stripped.
std::optional<AddressHit> find_address(db::Conn& c, std::string_view email) {
  const std::string exact = normalize_email(email);
  const std::string stripped = normalize_email(email, true);
  for (const std::string* key : {&exact, &stripped}) {
    if (key->empty()) continue;
    auto s = c.prepare("SELECT id, email, kind, user_id FROM addresses WHERE email = ?");
    s.bind_all(*key);
    if (s.step()) return AddressHit{s.i64(0), s.text(1), s.text(2) == "user", s.opt_i64(3)};
    if (key == &exact && exact == stripped) break;
  }
  return std::nullopt;
}

bool user_active(db::Conn& c, int64_t user_id) {
  return c.scalar<int64_t>("SELECT 1 FROM users WHERE id = ? AND disabled = 0", user_id).has_value();
}

std::vector<int64_t> active_members(db::Conn& c, int64_t alias_id) {
  std::vector<int64_t> out;
  auto s = c.prepare(
      "SELECT am.user_id FROM alias_members am JOIN users u ON u.id = am.user_id "
      "WHERE am.alias_id = ? AND u.disabled = 0 ORDER BY am.user_id");
  s.bind_all(alias_id);
  while (s.step()) out.push_back(s.i64(0));
  return out;
}

bool deliverable(db::Conn& c, const AddressHit& a) {
  if (a.is_user) return a.user_id && user_active(c, *a.user_id);
  return !active_members(c, a.id).empty();
}

std::string lower_trim(std::optional<std::string> v) { return v ? to_lower_ascii(trim(*v)) : std::string(); }

// Basename without control characters, ≤ 255 bytes, "attachment" when nothing is left.
std::string clean_filename(std::string_view raw) {
  std::string name = utf8_sanitize(raw);
  if (const auto sep = name.find_last_of("/\\"); sep != std::string::npos) name = name.substr(sep + 1);
  std::string out;
  for (unsigned char ch : name)
    if (ch >= 0x20 && ch != 0x7f) out.push_back(static_cast<char>(ch));
  out = utf8_truncate(trim(out), 255);
  out = std::string(trim(out));
  return out.empty() ? "attachment" : out;
}

// Always valid UTF-8: a stored JSON column must stay parseable whatever the sender put in its
// headers (review R1).
json::array strings_json(const std::vector<std::string>& v) {
  json::array a;
  for (const auto& s : v) a.emplace_back(json::string(utf8_sanitize(s)));
  return a;
}

json::value opt_json_string(const std::optional<std::string>& v) {
  return v ? json::value(json::string(utf8_sanitize(*v))) : json::value();
}

// The outbound a loopback copy belongs to, with the identity it was sent from.
struct LoopOutbound {
  int64_t id = 0;
  int64_t sender = 0;
  std::optional<std::string> message_id;
};

std::optional<LoopOutbound> find_loop_outbound(db::Conn& c, std::string_view where_col, std::string_view value,
                                               std::string_view from_email) {
  auto s = c.prepare(
      "SELECT o.id, o.sender_user_id, o.message_id_header, a.email FROM outbound o "
      "JOIN addresses a ON a.id = o.from_address_id WHERE o." + std::string(where_col) + " = ?");
  s.bind_all(value);
  if (!s.step()) return std::nullopt;
  // The From must be the identity the outbound was sent from: X-AzMail-Ref and Message-ID are
  // visible to every recipient, so they alone must not mark forged mail as our own.
  if (normalize_email(s.text(3)) != normalize_email(from_email)) return std::nullopt;
  return LoopOutbound{s.i64(0), s.i64(1), s.opt_text(2)};
}

// Review R7/SEC-3: X-AzMail-Ref (and the From it was sent with) is visible to every recipient
// of our mail, so a matching ref alone proves nothing. It vouches for the mail being our own
// loopback — clearing spoofed_internal — only when every local envelope recipient was a
// recipient of that send (its frozen To/Cc/Bcc) and, once the send's Message-ID is known, the
// mail carries exactly that id.
bool ref_verified(db::Conn& c, const LoopOutbound& o, const std::vector<std::string>& envelope,
                  const std::optional<std::string>& msgid) {
  if (o.message_id && (!msgid || *msgid != *o.message_id)) return false;
  const auto payload = c.scalar<std::string>("SELECT payload_json FROM outbound WHERE id = ?", o.id);
  if (!payload) return false;
  std::set<std::string> rcpts;
  try {
    const detail::FrozenPayload p = detail::payload_from_json(*payload);
    for (const auto* list : {&p.to, &p.cc, &p.bcc})
      for (const auto& formatted : *list)
        if (auto a = azm::parse_address(formatted)) rcpts.insert(normalize_email(a->email, true));
  } catch (const detail::PayloadCorrupt&) {
    return false;
  }
  bool any_local = false;
  for (const auto& e : envelope) {
    if (!detail::is_local_domain(c, e)) continue;
    any_local = true;
    if (!rcpts.contains(normalize_email(e, true))) return false;
  }
  return any_local;
}

// DKIM or DMARC passed (and DMARC did not fail): the mail was really sent by its From domain.
bool authenticated(const AuthResults& auth) {
  const std::string dkim = lower_trim(auth.dkim), dmarc = lower_trim(auth.dmarc);
  return (dkim == "pass" || dmarc == "pass") && dmarc != "fail";
}

std::optional<std::string> clean_msgid(const std::optional<std::string>& raw) {
  if (!raw) return std::nullopt;
  std::string id = sanitize_message_id(*raw);
  if (id.empty()) return std::nullopt;
  return id;
}

}  // namespace

// =============================================================================================
// Routing
// =============================================================================================

std::vector<LocalRecipient> resolve_local_recipients(db::Conn& c, std::span<const std::string> emails) {
  std::vector<LocalRecipient> out;
  std::set<int64_t> seen;
  for (const auto& e : emails) {
    if (!detail::is_local_domain(c, e)) continue;
    const auto hit = find_address(c, e);
    if (!hit) continue;
    if (hit->is_user) {
      if (hit->user_id && user_active(c, *hit->user_id) && seen.insert(*hit->user_id).second)
        out.push_back({*hit->user_id, hit->id, hit->email, false});
    } else {
      for (int64_t member : active_members(c, hit->id))
        if (seen.insert(member).second) out.push_back({member, hit->id, hit->email, true});
    }
  }
  return out;
}

std::vector<std::string> unknown_local_recipients(db::Conn& c, std::span<const std::string> emails) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& e : emails) {
    if (!detail::is_local_domain(c, e)) continue;
    const auto hit = find_address(c, e);
    // A disabled user or an alias without active members would also lose the mail silently (C4).
    if (hit && deliverable(c, *hit)) continue;
    if (seen.insert(normalize_email(e)).second) out.emplace_back(trim(e));
  }
  return out;
}

std::vector<std::string> spam_warnings(const AuthResults& auth, bool from_is_local, bool azmail_ref_matches) {
  std::vector<std::string> out;
  const std::string dkim = lower_trim(auth.dkim), dmarc = lower_trim(auth.dmarc);
  if (dmarc == "fail") out.emplace_back(kWarnDmarcFail);
  if (from_is_local && dkim != "pass" && dmarc != "pass" && !azmail_ref_matches)
    out.emplace_back(kWarnSpoofedInternal);
  return out;
}

// =============================================================================================
// inbound_emails state
// =============================================================================================

int64_t record_inbound_pending(db::Tx& tx, std::string_view resend_id, InboundSource source, int64_t now_ms) {
  if (now_ms <= 0) now_ms = azm::now_ms();
  tx.run(
      "INSERT OR IGNORE INTO inbound_emails(resend_id, state, source, created_at, updated_at) "
      "VALUES(?, 'pending', ?, ?, ?)",
      resend_id, to_string(source), now_ms, now_ms);
  return *tx.scalar<int64_t>("SELECT id FROM inbound_emails WHERE resend_id = ?", resend_id);
}

void mark_inbound_failed(db::Tx& tx, std::string_view resend_id, std::string_view error, int64_t now_ms) {
  if (now_ms <= 0) now_ms = azm::now_ms();
  const std::string err = utf8_truncate(utf8_sanitize(error), kMaxErrorBytes);
  // A delivered email never goes back to failed (a late duplicate job must not undo it).
  tx.run(
      "INSERT INTO inbound_emails(resend_id, state, source, error, created_at, updated_at) "
      "VALUES(?, 'failed', 'webhook', ?, ?, ?) ON CONFLICT(resend_id) DO UPDATE SET state = 'failed', "
      "error = excluded.error, updated_at = excluded.updated_at WHERE inbound_emails.state <> 'delivered'",
      resend_id, err, now_ms, now_ms);
}

std::optional<InboundState> inbound_state(db::Conn& c, std::string_view resend_id) {
  if (auto s = c.scalar<std::string>("SELECT state FROM inbound_emails WHERE resend_id = ?", resend_id))
    return parse_inbound_state(*s);
  return std::nullopt;
}

// =============================================================================================
// Delivery
// =============================================================================================

DeliveryResult deliver_inbound(db::Tx& tx, const InboundEmail& email, const DeliveryOptions& opts) {
  if (email.resend_id.empty()) throw std::invalid_argument("deliver_inbound: empty resend_id");
  const int64_t now = opts.now_ms > 0 ? opts.now_ms : azm::now_ms();
  db::Conn& c = tx.conn();
  DeliveryResult res;

  // 1. inbound_emails (C8): a fully delivered email is never delivered twice.
  tx.run(
      "INSERT OR IGNORE INTO inbound_emails(resend_id, state, source, created_at, updated_at) "
      "VALUES(?, 'pending', ?, ?, ?)",
      email.resend_id, to_string(email.source), now, now);
  {
    auto s = c.prepare("SELECT id, state FROM inbound_emails WHERE resend_id = ?");
    s.bind_all(email.resend_id);
    s.step();
    res.inbound_id = s.i64(0);
    if (s.text(1) == "delivered") {
      res.state = DeliveryResult::State::Duplicate;
      return res;
    }
  }
  if (email.raw) register_blob(tx, *email.raw, now);
  for (const auto& a : email.attachments) register_blob(tx, a.blob, now);

  // Message-IDs come from the sender (raw header bytes, or Resend's JSON): every one that is
  // stored, looked up or sent back later passes sanitize_message_id — whatever the producer of
  // the InboundEmail did (review R1/SEC-8).
  std::vector<std::string> refs;  // In-Reply-To + References, sanitized, deduplicated
  {
    std::set<std::string> seen;
    auto add = [&](const std::string& r) {
      std::string id = sanitize_message_id(r);
      if (!id.empty() && seen.insert(id).second) refs.push_back(std::move(id));
    };
    for (const auto& r : email.references) add(r);
    if (email.in_reply_to) add(*email.in_reply_to);
  }
  const std::optional<std::string> msgid = clean_msgid(email.message_id);
  const std::optional<std::string> in_reply_to = clean_msgid(email.in_reply_to);
  std::vector<std::string> references;  // header order, for replies' References chains
  for (const auto& r : email.references)
    if (std::string id = sanitize_message_id(r); !id.empty()) references.push_back(std::move(id));

  {
    json::object meta;
    meta["in_reply_to"] = in_reply_to ? json::value(*in_reply_to) : json::value();
    meta["references"] = strings_json(references);
    meta["x_azmail_ref"] = opt_json_string(email.x_azmail_ref);
    meta["auto_submitted"] = opt_json_string(email.auto_submitted);
    meta["html_format"] = utf8_sanitize(email.html_format);
    meta["attachments"] = static_cast<int64_t>(email.attachments.size());
    tx.run(
        "UPDATE inbound_emails SET message_id_header = ?, from_email = ?, subject = ?, received_at = ?, "
        "raw_sha256 = ?, meta_json = ?, recipients_json = ?, error = NULL, updated_at = ? WHERE id = ?",
        msgid, email.from.email, detail::clean_subject(email.subject),
        email.received_at > 0 ? std::optional<int64_t>(email.received_at) : std::nullopt,
        email.raw ? std::optional<std::string>(to_lower_ascii(email.raw->sha256)) : std::nullopt,
        json::serialize(meta), json::serialize(strings_json(email.received_for)), now, res.inbound_id);
  }

  // 2. Routing (C1): the envelope is the truth; headers only when Resend gave no envelope.
  std::vector<std::string> envelope;
  for (const auto& r : email.received_for) {
    const std::string_view v = trim(r);
    if (v.empty()) continue;
    if (v.find('<') != std::string_view::npos)  // tolerate "Name <addr>" forms
      if (auto a = azm::parse_address(v)) {
        envelope.push_back(a->email);
        continue;
      }
    envelope.emplace_back(v);
  }
  if (envelope.empty())
    for (const auto* list : {&email.to, &email.cc})
      for (const auto& a : *list) envelope.push_back(a.email);
  std::vector<LocalRecipient> recipients = resolve_local_recipients(c, envelope);
  if (recipients.empty() && opts.unroutable_to_admins) {
    // C4 policy: mail for unknown LOCAL addresses goes to the admins (foreign domains stay unroutable).
    const auto first_local = std::find_if(envelope.begin(), envelope.end(),
                                          [&](const std::string& e) { return detail::is_local_domain(c, e); });
    if (first_local != envelope.end()) {
      auto s = c.prepare("SELECT id FROM users WHERE is_admin = 1 AND disabled = 0 ORDER BY id");
      while (s.step()) recipients.push_back({s.i64(0), 0, normalize_email(*first_local), false});
    }
  }
  if (recipients.empty()) {
    tx.run("UPDATE inbound_emails SET state = 'unroutable', updated_at = ? WHERE id = ?", now, res.inbound_id);
    res.state = DeliveryResult::State::Unroutable;
    return res;
  }

  // 3. Our own mail looping back (C3, B2) and spoofing rules (C11).
  std::optional<LoopOutbound> by_ref;
  if (email.x_azmail_ref && !trim(*email.x_azmail_ref).empty())
    by_ref = find_loop_outbound(c, "uuid", trim(*email.x_azmail_ref), email.from.email);
  // The ref chooses the loopback-merge path below (a forged ref can only flag an existing copy
  // of the send as received); it clears spoofed_internal only when verified (R7/SEC-3).
  const bool ref_ok = by_ref && ref_verified(c, *by_ref, envelope, msgid);
  // Capture source (c) of B2 only from mail that is provably ours: verified ref AND DKIM/DMARC
  // pass — a forged mail must never decide our Message-ID (and with it which messages the late
  // loopback cleanup folds away).
  if (by_ref && !by_ref->message_id && msgid && ref_ok && authenticated(email.auth)) {
    set_outbound_message_id(tx, by_ref->id, *msgid);
    by_ref->message_id = msgid;
  }
  std::optional<LoopOutbound> loop = by_ref;
  if (!loop && msgid) loop = find_loop_outbound(c, "message_id_header", *msgid, email.from.email);

  const std::vector<std::string> warnings =
      spam_warnings(email.auth, detail::is_local_domain(c, email.from.email), ref_ok);
  const bool is_spam = !warnings.empty();
  const std::string warnings_json = json::serialize(strings_json(warnings));

  const std::string subject = detail::clean_subject(email.subject);
  const int64_t date = message_date(email.date, email.received_at, now);
  const std::string snippet =
      email.text && !trim(*email.text).empty() ? make_snippet(*email.text, false) : make_snippet(email.html.value_or(""), true);
  std::vector<std::string> participants{normalize_email(email.from.email)};
  std::set<std::string> header_rcpts;  // To/Cc, normalized with +tags stripped (BCC rule, C2)
  for (const auto* list : {&email.to, &email.cc})
    for (const auto& a : *list) {
      participants.push_back(normalize_email(a.email));
      header_rcpts.insert(normalize_email(a.email, true));
    }
  int64_t size_bytes = email.raw ? email.raw->size : 0;
  if (size_bytes == 0) {
    size_bytes = static_cast<int64_t>(email.html.value_or("").size() + email.text.value_or("").size());
    for (const auto& a : email.attachments) size_bytes += a.blob.size;
  }

  struct AttRow {
    const InboundAttachment* src;
    std::string filename, content_type;
    std::optional<std::string> content_id;
    bool is_inline;
  };
  std::vector<AttRow> atts;
  for (const auto& a : email.attachments) {
    AttRow r{&a, clean_filename(a.filename), detail::normalize_mime(a.content_type), std::nullopt, false};
    if (r.content_type.empty()) r.content_type = "application/octet-stream";
    if (a.content_id) {  // a Content-ID is forwarded to Resend later: header-safe only (SEC-8)
      std::string cid = sanitize_message_id(*a.content_id);
      if (!cid.empty()) r.content_id = std::move(cid);
    }
    // Inline = a part the HTML can reference: Content-ID plus inline disposition, or a cid: use.
    r.is_inline = r.content_id && (iequals(trim(a.disposition), "inline") ||
                                   (email.html && detail::html_references_cid(*email.html, *r.content_id)));
    atts.push_back(std::move(r));
  }
  const bool has_attachments = std::any_of(atts.begin(), atts.end(), [](const AttRow& a) { return !a.is_inline; });

  // 4. Fan-out, one copy per owner.
  for (const auto& rcpt : recipients) {
    const int64_t owner = rcpt.user_id;
    if (loop) {
      auto s = c.prepare(
          "SELECT id, thread_id, delivered_to FROM messages WHERE outbound_id = ? AND owner_id = ? AND "
          "direction = 'out' AND is_draft = 0 ORDER BY is_shared_copy, id LIMIT 1");
      s.bind_all(loop->id, owner);
      if (s.step()) {
        const int64_t mid = s.i64(0), thread = s.i64(1);
        const bool merged_before = s.opt_text(2).has_value();
        s.reset();
        if (merged_before) {
          // Another part of a split delivery already merged into this copy (sender and shared
          // copies are created with delivered_to NULL): it must not come back unread or as new
          // mail (review R11).
          res.copies.push_back({owner, mid, thread, rcpt.delivered_to, true, false, true});
          continue;
        }
        // The owner already holds our outbound copy: it lands in the inbox instead of a duplicate.
        const bool not_sender = owner != loop->sender;
        tx.run(
            "UPDATE messages SET in_inbox = 1, is_read = CASE WHEN ? THEN 0 ELSE is_read END, "
            "delivered_to = COALESCE(delivered_to, ?), updated_at = ? WHERE id = ?",
            not_sender, rcpt.delivered_to, now, mid);
        recompute_thread(tx, owner, thread);
        detail::emit_threads_changed(tx, owner, {thread});
        if (not_sender) {
          const Address from{tx.scalar<std::string>("SELECT from_name FROM messages WHERE id = ?", mid).value_or(""),
                             tx.scalar<std::string>("SELECT from_email FROM messages WHERE id = ?", mid).value_or("")};
          tx.emit(owner, std::string(ws::events::kMailNew),
                  ws::mail_new_payload(thread, mid, from, subject, snippet, true, false));
        }
        res.copies.push_back({owner, mid, thread, rcpt.delivered_to, true, false, true});
        continue;
      }
    }

    ThreadingKeys keys;
    keys.message_id = msgid;
    keys.refs = refs;
    keys.subject = subject;
    keys.participants = participants;
    keys.date = date;
    int64_t thread = assign_thread(tx, owner, keys);

    // BCC privacy (C2): never Resend's bcc[]; just "密送给我" when the owner is not in To/Cc.
    bool in_headers = header_rcpts.contains(normalize_email(rcpt.delivered_to, true));
    if (!in_headers)
      for (const auto& mine : detail::owner_addresses(c, owner))
        if (header_rcpts.contains(normalize_email(mine, true))) {
          in_headers = true;
          break;
        }
    const std::string bcc_json =
        in_headers ? "[]" : detail::addresses_to_json(std::vector<Address>{Address{"", rcpt.delivered_to}});

    tx.run(
        "INSERT OR IGNORE INTO messages(owner_id, thread_id, direction, is_draft, inbound_id, from_name, from_email, "
        "to_json, cc_json, bcc_json, reply_to_json, delivered_to, subject, snippet, date, message_id_header, "
        "in_reply_to, has_attachments, size_bytes, is_read, in_inbox, is_spam, auth_spf, auth_dkim, auth_dmarc, "
        "warnings_json, created_at, updated_at) "
        "VALUES(?, ?, 'in', 0, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?, ?, ?)",
        owner, thread, res.inbound_id, email.from.name, email.from.email, detail::addresses_to_json(email.to),
        detail::addresses_to_json(email.cc), bcc_json, detail::addresses_to_json(email.reply_to), rcpt.delivered_to,
        subject, snippet, date, msgid, in_reply_to, has_attachments, size_bytes, !is_spam, is_spam, email.auth.spf, email.auth.dkim, email.auth.dmarc,
        warnings_json, now, now);
    if (tx.changes() == 0) {
      // Split delivery / re-run (UNIQUE(owner, inbound_id), messages_in_msgid): drop the thread
      // assign_thread may have created for it.
      recompute_thread(tx, owner, thread);
      continue;
    }
    const int64_t mid = tx.last_insert_id();
    tx.run("INSERT INTO message_bodies(message_id, html, text, quoted_html) VALUES(?, ?, ?, NULL)", mid, email.html,
           email.text);
    for (const auto& r : refs)
      tx.run("INSERT OR IGNORE INTO message_refs(message_id, owner_id, ref) VALUES(?,?,?)", mid, owner, r);
    for (const auto& a : atts)
      tx.run(
          "INSERT INTO attachments(owner_id, message_id, blob_sha256, filename, content_type, size, content_id, "
          "is_inline, resend_attachment_id, created_at) VALUES(?,?,?,?,?,?,?,?,?,?)",
          owner, mid, to_lower_ascii(a.src->blob.sha256), a.filename, a.content_type, a.src->blob.size, a.content_id,
          a.is_inline,
          a.src->resend_attachment_id.empty() ? std::nullopt : std::optional<std::string>(a.src->resend_attachment_id),
          now);
    if (msgid) adopt_referencing(tx, owner, thread, *msgid);
    thread = tx.scalar<int64_t>("SELECT thread_id FROM messages WHERE id = ?", mid).value_or(thread);
    recompute_thread(tx, owner, thread);
    fts_reindex(tx, mid);
    if (!is_spam) upsert_contact(tx, owner, email.from, 0.2, now);
    tx.emit(owner, std::string(ws::events::kMailNew),
            ws::mail_new_payload(thread, mid, email.from, subject, snippet, !is_spam, is_spam));
    res.copies.push_back({owner, mid, thread, rcpt.delivered_to, !is_spam, is_spam, false});
  }

  tx.run("UPDATE inbound_emails SET state = 'delivered', error = NULL, updated_at = ? WHERE id = ?", now,
         res.inbound_id);
  res.state = res.copies.empty() ? DeliveryResult::State::Duplicate : DeliveryResult::State::Delivered;
  return res;
}

}  // namespace azm::mail
