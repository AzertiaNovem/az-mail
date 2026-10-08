// Owner: WP-B — list_threads (folders, labels, cursors), thread actions with aggregates per
// folder, get_thread / get_message rendering, counts, IDOR.
#include "core/errors.hpp"
#include "mail/mailbox.hpp"
#include "mail/serde.hpp"
#include "mail_fixtures.hpp"
#include "test_support.hpp"
#include "ws/events.hpp"

#include <boost/json/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::mailfx;

namespace {

struct Fx {
  test::TestServices ts;
  int64_t alice = 0, bob = 0, alice_addr = 0, support_addr = 0;
  const Address me{"Alice", "alice@team.example"};
  const Address carol{"Carol", "carol@ext.example"};

  Fx() {
    ts.db.write([&](db::Tx& tx) {
      alice = test::seed_user(tx, "alice@team.example", false, "Alice");
      bob = test::seed_user(tx, "bob@team.example", false, "Bob");
      alice_addr = test::address_id(tx.conn(), "alice@team.example");
      support_addr = test::seed_alias(tx, "support@team.example", {{alice, true}, {bob, false}});
    });
  }

  Inserted add(Msg m) {
    if (m.owner == 0) m.owner = alice;
    return ts.db.write([&](db::Tx& tx) { return insert_message(tx, m); });
  }
  Msg inbound(std::string subject, int64_t date, std::optional<int64_t> thread = {}) const {
    Msg m;
    m.owner = alice;
    m.from = carol;
    m.to = {me};
    m.subject = std::move(subject);
    m.date = date;
    m.thread_id = thread;
    return m;
  }
  ThreadPage list(ThreadQuery q, int64_t owner = 0) {
    return ts.db.read([&](db::Conn& c) { return list_threads(c, owner ? owner : alice, q); });
  }
  ThreadPage folder(Folder f, int limit = 50) {
    ThreadQuery q;
    q.folder = f;
    q.limit = limit;
    return list(q);
  }
  std::vector<int64_t> ids(Folder f) {
    std::vector<int64_t> out;
    for (const auto& it : folder(f).items) out.push_back(it.id);
    return out;
  }
  std::vector<int64_t> act(std::vector<int64_t> tids, ThreadAction a, std::optional<int64_t> label = {},
                           int64_t owner = 0) {
    return ts.db.write(
        [&](db::Tx& tx) { return apply_thread_action(tx, owner ? owner : alice, tids, a, label); });
  }
  Agg agg(int64_t t) {
    return ts.db.read([&](db::Conn& c) { return aggregates(c, t); }).value();
  }
  Counts cnt(int64_t owner = 0) {
    return ts.db.read([&](db::Conn& c) { return counts(c, owner ? owner : alice); });
  }
};

}  // namespace

TEST_CASE("list_threads: folders and ordering", "[thread_list]") {
  Fx f;
  const auto a = f.add(f.inbound("A", kT0));
  const auto b = f.add(f.inbound("B", kT0 + 1000));
  Msg s = f.inbound("Spam", kT0 + 2000);
  s.is_spam = true;
  s.in_inbox = false;
  const auto sp = f.add(s);
  Msg t = f.inbound("Trashed", kT0 + 500);
  t.trashed_at = kT0 + 9000;
  const auto tr = f.add(t);

  auto inbox = f.folder(Folder::Inbox);
  REQUIRE(inbox.items.size() == 2);
  CHECK(inbox.items[0].id == b.thread_id);  // newest first
  CHECK(inbox.items[1].id == a.thread_id);
  CHECK(inbox.total == 2);
  CHECK_FALSE(inbox.next_cursor);
  CHECK(inbox.items[0].unread);
  CHECK(inbox.items[0].in_inbox);
  CHECK(inbox.items[0].last_at == kT0 + 1000);

  CHECK(f.ids(Folder::All) == std::vector<int64_t>{b.thread_id, a.thread_id});
  CHECK(f.ids(Folder::Spam) == std::vector<int64_t>{sp.thread_id});
  CHECK(f.ids(Folder::Trash) == std::vector<int64_t>{tr.thread_id});
  CHECK(f.folder(Folder::Spam).items[0].last_at == kT0 + 2000);  // spam_last_at
  CHECK(f.folder(Folder::Trash).items[0].last_at == kT0 + 500);  // trash_last_at
  CHECK(f.folder(Folder::Trash).items[0].unread);                 // computed for trash
  CHECK(f.ids(Folder::Starred).empty());
  CHECK(f.ids(Folder::Sent).empty());
  CHECK(f.ids(Folder::Drafts).empty());
  CHECK(f.ids(Folder::Scheduled).empty());
  // No selector → inbox.
  CHECK(f.list(ThreadQuery{}).items.size() == 2);
  // Another owner sees nothing (IDOR).
  ThreadQuery q;
  q.folder = Folder::All;
  CHECK(f.list(q, f.bob).items.empty());
  CHECK(f.list(q, f.bob).total == 0);
}

TEST_CASE("folder list queries use the partial indexes (C6)", "[thread_list]") {
  Fx f;
  auto plan = [&](std::string_view sql) {
    return f.ts.db.read([&](db::Conn& c) {
      auto s = c.prepare("EXPLAIN QUERY PLAN " + std::string(sql));
      int idx = 1;
      for (const char* p = sql.data(); *p; ++p)
        if (*p == '?') s.bind(idx++, int64_t{1});
      std::string out;
      while (s.step()) out += s.text(3) + "\n";
      return out;
    });
  };
  // Same SQL shapes as list_threads (folder predicate spelled like the index WHERE clause).
  for (auto [pred, key, index] : {std::tuple{"inbox_count>0", "last_at", "threads_inbox"},
                                  {"msg_count+draft_count>0", "last_at", "threads_all"},
                                  {"starred_count>0", "last_at", "threads_starred"},
                                  {"sent_count>0", "last_at", "threads_sent"},
                                  {"draft_count>0", "last_at", "threads_drafts"},
                                  {"scheduled_count>0", "last_at", "threads_scheduled"},
                                  {"spam_count>0", "spam_last_at", "threads_spam"},
                                  {"trash_count>0", "trash_last_at", "threads_trash"}}) {
    const std::string first = std::string("SELECT id FROM threads WHERE owner_id = ? AND ") + pred +
                              " ORDER BY " + key + " DESC, id DESC LIMIT ?";
    const std::string next = std::string("SELECT id FROM threads WHERE owner_id = ? AND ") + pred + " AND (" +
                             key + ", id) < (?, ?) ORDER BY " + key + " DESC, id DESC LIMIT ?";
    const std::string p1 = plan(first), p2 = plan(next);
    INFO(first << "\n" << p1 << next << "\n" << p2);
    CHECK(p1.find(index) != std::string::npos);
    CHECK(p2.find(index) != std::string::npos);
    CHECK(p1.find("TEMP B-TREE") == std::string::npos);  // no sort step
    CHECK(p2.find("TEMP B-TREE") == std::string::npos);
  }
}

TEST_CASE("list_threads: keyset cursor pagination is stable", "[thread_list]") {
  Fx f;
  std::vector<int64_t> threads;
  for (int i = 0; i < 25; ++i) {
    // Two threads share each timestamp to exercise the id tie-breaker.
    threads.push_back(f.add(f.inbound("T" + std::to_string(i), kT0 + (i / 2) * 1000)).thread_id);
  }
  std::vector<int64_t> seen;
  std::optional<std::string> cursor;
  int pages = 0;
  for (;;) {
    ThreadQuery q;
    q.folder = Folder::Inbox;
    q.limit = 10;
    q.cursor = cursor;
    const auto p = f.list(q);
    CHECK(p.total == (pages == 0 ? 25 : 26));
    for (const auto& it : p.items) seen.push_back(it.id);
    ++pages;
    if (pages == 1) {
      // A thread arriving between page loads appears on top, never shifting later pages.
      f.add(f.inbound("late", kT0 + 100'000));
    }
    if (!p.next_cursor) break;
    cursor = p.next_cursor;
    REQUIRE(pages < 10);
  }
  CHECK(pages == 3);
  REQUIRE(seen.size() == 25);
  std::set<int64_t> uniq(seen.begin(), seen.end());
  CHECK(uniq.size() == 25);
  std::vector<int64_t> expected = threads;
  std::sort(expected.begin(), expected.end(), [&](int64_t x, int64_t y) {
    const auto ax = f.agg(x), ay = f.agg(y);
    return ax.last_at != ay.last_at ? ax.last_at > ay.last_at : x > y;
  });
  CHECK(seen == expected);

  // Limits are clamped (1..100).
  ThreadQuery q;
  q.limit = 0;
  CHECK(f.list(q).items.size() == 1);
  q.limit = 1000;
  CHECK(f.list(q).items.size() == 26);
}

TEST_CASE("list_threads: malformed cursor → 400 invalid_field", "[thread_list]") {
  Fx f;
  for (std::string bad : {"!!!", "bm90LWEtY3Vyc29y" /* not-a-cursor */, "MTIz" /* "123" */, "LTE6LTE" /* -1:-1 */}) {
    ThreadQuery q;
    q.cursor = bad;
    try {
      f.list(q);
      FAIL("expected ApiError for cursor " << bad);
    } catch (const ApiError& e) {
      CHECK(e.status == 400);
      CHECK(e.code == "invalid_field");
      CHECK(e.details.at("field") == "cursor");
    }
  }
  ThreadQuery empty;
  empty.cursor = "";  // empty = first page
  CHECK_NOTHROW(f.list(empty));
}

TEST_CASE("list_threads: label view, totals and 404 for foreign labels", "[thread_list]") {
  Fx f;
  int64_t work = 0, bobs = 0;
  f.ts.db.write([&](db::Tx& tx) {
    work = insert_label(tx, f.alice, "Work");
    bobs = insert_label(tx, f.bob, "Bobs");
  });
  Msg m = f.inbound("labeled", kT0);
  m.labels = {work};
  const auto a = f.add(m);
  Msg t = f.inbound("labeled but trashed", kT0 + 1);
  t.labels = {work};
  t.trashed_at = kT0;
  f.add(t);
  f.add(f.inbound("unlabeled", kT0 + 2));

  ThreadQuery q;
  q.label_id = work;
  const auto p = f.list(q);
  REQUIRE(p.items.size() == 1);
  CHECK(p.items[0].id == a.thread_id);
  CHECK(p.items[0].label_ids == std::vector<int64_t>{work});
  CHECK(p.total == 1);

  q.label_id = bobs;
  CHECK_THROWS_AS(f.list(q), ApiError);
  q.label_id = 999;
  try {
    f.list(q);
    FAIL("expected 404");
  } catch (const ApiError& e) {
    CHECK(e.status == 404);
  }
}

TEST_CASE("list_threads: decorations (participants, previews, status, schedule)", "[thread_list]") {
  Fx f;
  int64_t ob_delivered = 0, ob_sched = 0;
  f.ts.db.write([&](db::Tx& tx) {
    ob_delivered = insert_outbound(tx, f.alice, f.alice_addr, "delivered");
    ob_sched = insert_outbound(tx, f.alice, f.alice_addr, "scheduled", kT0 + 5 * kDay);
  });
  Msg m = f.inbound("Files", kT0);
  m.atts = {Att{.filename = "a.pdf"}, Att{.filename = "b.docx", .content_type = "application/msword"},
            Att{.filename = "inline.png", .content_type = "image/png", .content_id = "i@x", .is_inline = true},
            Att{.filename = "c.zip"}, Att{.filename = "d.txt"}};
  const auto r = f.add(m);
  Msg reply;
  reply.owner = f.alice;
  reply.thread_id = r.thread_id;
  reply.direction = "out";
  reply.from = f.me;
  reply.to = {f.carol};
  reply.subject = "Re: Files";
  reply.is_read = true;
  reply.in_inbox = false;
  reply.date = kT0 + 1000;
  reply.outbound_id = ob_delivered;
  f.add(reply);
  Msg later = reply;
  later.outbound_id = ob_sched;
  later.date = kT0 + 2000;
  f.add(later);

  const auto p = f.folder(Folder::Inbox);
  REQUIRE(p.items.size() == 1);
  const auto& it = p.items[0];
  CHECK(it.message_count == 3);
  CHECK(it.has_attachments);
  REQUIRE(it.attachments_preview.size() == 3);  // max 3, inline excluded
  CHECK(it.attachments_preview[0].filename == "a.pdf");
  CHECK(it.attachments_preview[1].filename == "b.docx");
  CHECK(it.attachments_preview[1].content_type == "application/msword");
  CHECK(it.attachments_preview[2].filename == "c.zip");
  CHECK(it.latest_status == OutboundStatus::Scheduled);
  CHECK(it.scheduled_at == kT0 + 5 * kDay);
  REQUIRE(it.participants.size() == 2);
  CHECK(it.participants[0].email == "carol@ext.example");
  CHECK_FALSE(it.participants[0].is_me);
  CHECK(it.participants[0].unread);
  CHECK(it.participants[1].email == "alice@team.example");
  CHECK(it.participants[1].is_me);
  CHECK(f.ids(Folder::Sent) == std::vector<int64_t>{r.thread_id});
  CHECK(f.ids(Folder::Scheduled) == std::vector<int64_t>{r.thread_id});
}

TEST_CASE("thread actions update aggregates and folders", "[thread_list][actions]") {
  Fx f;
  const auto a = f.add(f.inbound("A", kT0));
  Msg a2 = f.inbound("Re: A", kT0 + 1000, a.thread_id);
  const auto ra2 = f.add(a2);
  const int64_t t = a.thread_id;
  f.ts.notifier.clear();

  SECTION("archive / inbox") {
    CHECK(f.act({t}, ThreadAction::Archive) == std::vector<int64_t>{t});
    CHECK(f.agg(t).inbox_count == 0);
    CHECK(f.ids(Folder::Inbox).empty());
    CHECK(f.ids(Folder::All) == std::vector<int64_t>{t});
    f.act({t}, ThreadAction::Inbox);
    CHECK(f.agg(t).inbox_count == 2);
    CHECK(f.ids(Folder::Inbox) == std::vector<int64_t>{t});
    const auto evs = f.ts.notifier.events_of(ws::events::kThreadsChanged);
    REQUIRE(evs.size() == 2);
    CHECK(evs[0].data.at("thread_ids").as_array()[0].as_int64() == t);
  }
  SECTION("read / unread") {
    f.act({t}, ThreadAction::Read);
    CHECK(f.agg(t).unread_count == 0);
    CHECK(f.agg(t).inbox_unread == 0);
    CHECK(f.cnt().inbox_unread == 0);
    CHECK_FALSE(f.folder(Folder::Inbox).items[0].unread);
    f.act({t}, ThreadAction::Unread);
    CHECK(f.agg(t).unread_count == 2);
    CHECK(f.cnt().inbox_unread == 1);  // thread count, not message count
  }
  SECTION("star marks only the latest message; unstar clears all") {
    f.act({t}, ThreadAction::Star);
    CHECK(f.agg(t).starred_count == 1);
    f.ts.db.read([&](db::Conn& c) {
      CHECK(c.scalar<int64_t>("SELECT is_starred FROM messages WHERE id=?", ra2.message_id) == 1);
      CHECK(c.scalar<int64_t>("SELECT is_starred FROM messages WHERE id=?", a.message_id) == 0);
    });
    CHECK(f.ids(Folder::Starred) == std::vector<int64_t>{t});
    f.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE messages SET is_starred=1 WHERE thread_id=?", t); });
    f.act({t}, ThreadAction::Unstar);
    CHECK(f.agg(t).starred_count == 0);
    CHECK(f.ids(Folder::Starred).empty());
  }
  SECTION("trash / restore / delete_forever") {
    f.act({t}, ThreadAction::Trash);
    Agg g = f.agg(t);
    CHECK(g.trash_count == 2);
    CHECK(g.msg_count == 0);
    CHECK(g.inbox_count == 0);
    CHECK(g.last_at == 0);
    CHECK(g.trash_last_at == kT0 + 1000);
    CHECK(f.ids(Folder::Inbox).empty());
    CHECK(f.ids(Folder::All).empty());
    CHECK(f.ids(Folder::Trash) == std::vector<int64_t>{t});
    f.act({t}, ThreadAction::Restore);
    g = f.agg(t);
    CHECK(g.trash_count == 0);
    CHECK(g.inbox_count == 2);  // back where it was
    // delete_forever only removes trashed/spam messages.
    CHECK(f.act({t}, ThreadAction::DeleteForever) == std::vector<int64_t>{t});
    CHECK(f.agg(t).msg_count == 2);
    f.act({t}, ThreadAction::Trash);
    f.act({t}, ThreadAction::DeleteForever);
    f.ts.db.read([&](db::Conn& c) {
      CHECK(count(c, "SELECT COUNT(*) FROM threads") == 0);
      CHECK(count(c, "SELECT COUNT(*) FROM messages") == 0);
      CHECK(count(c, "SELECT COUNT(*) FROM message_fts") == 0);  // trigger keeps FTS in sync
    });
  }
  SECTION("spam / not_spam") {
    f.act({t}, ThreadAction::Spam);
    Agg g = f.agg(t);
    CHECK(g.spam_count == 2);
    CHECK(g.spam_unread == 2);
    CHECK(g.inbox_count == 0);
    CHECK(f.ids(Folder::Spam) == std::vector<int64_t>{t});
    CHECK(f.cnt().spam_unread == 1);
    f.act({t}, ThreadAction::NotSpam);
    g = f.agg(t);
    CHECK(g.spam_count == 0);
    CHECK(g.inbox_count == 2);
    CHECK(f.ids(Folder::Inbox) == std::vector<int64_t>{t});
  }
  SECTION("inbox from trash and spam moves the thread back") {
    f.act({t}, ThreadAction::Spam);
    f.act({t}, ThreadAction::Inbox);
    CHECK(f.ids(Folder::Inbox) == std::vector<int64_t>{t});
    f.act({t}, ThreadAction::Trash);
    f.act({t}, ThreadAction::Inbox);
    CHECK(f.ids(Folder::Inbox) == std::vector<int64_t>{t});
    CHECK(f.agg(t).trash_count == 0);
  }
  SECTION("labels") {
    int64_t work = 0, foreign = 0;
    f.ts.db.write([&](db::Tx& tx) {
      work = insert_label(tx, f.alice, "Work");
      foreign = insert_label(tx, f.bob, "Bob");
    });
    f.act({t}, ThreadAction::AddLabel, work);
    CHECK(f.folder(Folder::Inbox).items[0].label_ids == std::vector<int64_t>{work});
    Counts c = f.cnt();
    CHECK(c.labels.at(work).total == 1);
    CHECK(c.labels.at(work).unread == 1);
    ThreadQuery q;
    q.label_id = work;
    CHECK(f.list(q).items.size() == 1);
    f.act({t}, ThreadAction::RemoveLabel, work);
    CHECK(f.folder(Folder::Inbox).items[0].label_ids.empty());
    CHECK(f.cnt().labels.at(work).total == 0);
    // Missing label id → 400; foreign label → 404.
    try {
      f.act({t}, ThreadAction::AddLabel);
      FAIL("expected 400");
    } catch (const ApiError& e) {
      CHECK(e.status == 400);
      CHECK(e.details.at("field") == "label_id");
    }
    try {
      f.act({t}, ThreadAction::AddLabel, foreign);
      FAIL("expected 404");
    } catch (const ApiError& e) {
      CHECK(e.status == 404);
    }
  }
  SECTION("foreign and unknown thread ids are skipped (IDOR)") {
    const auto bobs = f.add([&] {
      Msg m = f.inbound("bob's", kT0);
      m.owner = f.bob;
      return m;
    }());
    f.ts.notifier.clear();
    CHECK(f.act({bobs.thread_id, 999999}, ThreadAction::Trash).empty());
    CHECK(f.ts.notifier.events_of(ws::events::kThreadsChanged).empty());
    CHECK(f.agg(bobs.thread_id).trash_count == 0);
    // Duplicates are applied once.
    CHECK(f.act({t, t, bobs.thread_id}, ThreadAction::Read) == std::vector<int64_t>{t});
  }
}

TEST_CASE("list_threads: to_preview lists the newest sent message's recipients", "[thread_list]") {
  Fx f;
  int64_t ob1 = 0, ob2 = 0;
  f.ts.db.write([&](db::Tx& tx) {
    ob1 = insert_outbound(tx, f.alice, f.alice_addr, "delivered");
    ob2 = insert_outbound(tx, f.alice, f.alice_addr, "delivered");
  });
  const Address dave{"Dave", "dave@ext.example"};
  Msg first;
  first.owner = f.alice;
  first.direction = "out";
  first.from = f.me;
  first.to = {f.carol};
  first.subject = "Plan";
  first.is_read = true;
  first.in_inbox = false;
  first.date = kT0;
  first.outbound_id = ob1;
  const auto r = f.add(first);
  // The newest outbound message decides: To + Cc, deduplicated (case-insensitive), me marked.
  Msg second = first;
  second.thread_id = r.thread_id;
  second.to = {dave, Address{"", "CAROL@ext.example"}};
  second.cc = {f.carol, f.me};
  second.date = kT0 + 1000;
  second.outbound_id = ob2;
  f.add(second);
  // A draft and an inbound reply do not count.
  Msg draft = first;
  draft.thread_id = r.thread_id;
  draft.is_draft = true;
  draft.outbound_id.reset();
  draft.to = {Address{"Eve", "eve@ext.example"}};
  draft.date = kT0 + 3000;
  f.add(draft);
  f.add(f.inbound("Re: Plan", kT0 + 2000, r.thread_id));

  const auto sent = f.folder(Folder::Sent);
  REQUIRE(sent.items.size() == 1);
  const auto& tp = sent.items[0].to_preview;
  REQUIRE(tp.size() == 3);
  CHECK(tp[0].email == "dave@ext.example");
  CHECK(tp[0].name == "Dave");
  CHECK_FALSE(tp[0].is_me);
  CHECK(tp[1].email == "CAROL@ext.example");
  CHECK(tp[2].email == "alice@team.example");
  CHECK(tp[2].is_me);

  // Wire format: additive `to_preview: [{name, email, is_me}]`.
  const auto j = to_json(sent.items[0]);
  REQUIRE(j.contains("to_preview"));
  const auto& arr = j.at("to_preview").as_array();
  REQUIRE(arr.size() == 3);
  CHECK(arr[0].as_object().at("email").as_string() == "dave@ext.example");
  CHECK(arr[2].as_object().at("is_me").as_bool());

  // At most 3; Bcc is never exposed.
  Msg third = first;
  third.thread_id = r.thread_id;
  third.to = {Address{"", "a@ext.example"}, Address{"", "b@ext.example"}};
  third.cc = {Address{"", "c@ext.example"}, Address{"", "d@ext.example"}};
  third.bcc = {Address{"", "secret@ext.example"}};
  third.date = kT0 + 4000;
  third.outbound_id = ob2;
  f.add(third);
  const auto sent2 = f.folder(Folder::Sent);
  REQUIRE(sent2.items.size() == 1);
  REQUIRE(sent2.items[0].to_preview.size() == 3);
  for (const auto& p : sent2.items[0].to_preview) CHECK(p.email != "secret@ext.example");
  CHECK(boost::json::serialize(to_json(sent2.items[0])).find("secret@") == std::string::npos);

  // A thread without outbound mail has none, and the wire field is omitted.
  const auto other = f.add(f.inbound("Hello", kT0 + 5000));
  bool found = false;
  for (const auto& it : f.folder(Folder::Inbox).items)
    if (it.id == other.thread_id) {
      found = true;
      CHECK(it.to_preview.empty());
      CHECK_FALSE(to_json(it).contains("to_preview"));
    }
  CHECK(found);
  // Another owner never sees alice's recipients (owner-scoped).
  ThreadQuery bq;
  bq.folder = Folder::Sent;
  CHECK(f.list(bq, f.bob).items.empty());
}

TEST_CASE("drafts and sent folders follow the outbound status", "[thread_list][aggregates]") {
  Fx f;
  int64_t ob = 0;
  f.ts.db.write([&](db::Tx& tx) { ob = insert_outbound(tx, f.alice, f.alice_addr, "queued", {}, kT0 + 5000); });
  Msg d;
  d.owner = f.alice;
  d.direction = "out";
  d.is_draft = true;
  d.from = f.me;
  d.to = {f.carol};
  d.subject = "Draft";
  d.in_inbox = false;
  d.is_read = true;
  const auto r = f.add(d);
  CHECK(f.ids(Folder::Drafts) == std::vector<int64_t>{r.thread_id});
  CHECK(f.ids(Folder::All) == std::vector<int64_t>{r.thread_id});  // msg_count+draft_count>0
  CHECK(f.cnt().drafts == 1);
  CHECK(f.folder(Folder::Drafts).items[0].draft_count == 1);
  // "Send": the draft becomes an out message pointing at a queued outbound.
  f.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE messages SET is_draft=0, outbound_id=? WHERE id=?", ob, r.message_id);
    recompute_thread(tx, f.alice, r.thread_id);
  });
  CHECK(f.ids(Folder::Drafts).empty());
  CHECK(f.ids(Folder::Sent) == std::vector<int64_t>{r.thread_id});
  CHECK(f.folder(Folder::Sent).items[0].latest_status == OutboundStatus::Queued);
  // Undo → canceled: no longer in sent.
  f.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE outbound SET status='canceled' WHERE id=?", ob);
    recompute_thread(tx, f.alice, r.thread_id);
  });
  CHECK(f.ids(Folder::Sent).empty());
}

TEST_CASE("get_thread / get_message rendering", "[thread_list][views]") {
  Fx f;
  int64_t ob = 0;
  std::string raw_sha;
  int64_t inbound_row = 0;
  f.ts.db.write([&](db::Tx& tx) {
    ob = insert_outbound(tx, f.alice, f.support_addr, "queued", {}, kT0 + 5000);
    raw_sha = crypto::sha256_hex("raw eml");
    register_blob(tx, BlobRef{raw_sha, 7, "local"}, kT0);
    inbound_row = insert_inbound_email(tx, "re_1", raw_sha);
  });
  Msg in = f.inbound("Hello", kT0);
  in.inbound_id = inbound_row;
  in.html = "<p>hi <img src=\"cid:logo@x\"></p>";
  in.text = "hi";
  in.atts = {Att{.filename = "logo.png", .content_type = "image/png", .content_id = "logo@x", .is_inline = true},
             Att{.filename = "doc.pdf"}};
  in.delivered_to = "support@team.example";
  in.auth_dmarc = "pass";
  in.warnings_json = "[\"dmarc_fail\"]";
  in.message_id = "in1@ext";
  const auto r = f.add(in);
  // Shared alias copy sent by bob (C3).
  Msg out;
  out.owner = f.alice;
  out.thread_id = r.thread_id;
  out.direction = "out";
  out.from = {"Support", "support@team.example"};
  out.to = {f.carol};
  out.bcc = {{"", "secret@ext.example"}};
  out.subject = "Re: Hello";
  out.date = kT0 + 1000;
  out.is_read = true;
  out.in_inbox = false;
  out.outbound_id = ob;
  out.is_shared_copy = true;
  out.sent_by_user_id = f.bob;
  out.trashed_at = kT0 + 2000;
  const auto ro = f.add(out);
  Msg draft = out;
  draft.is_draft = true;
  draft.outbound_id.reset();
  draft.is_shared_copy = false;
  draft.sent_by_user_id.reset();
  draft.trashed_at.reset();
  draft.date = kT0 + 3000;
  const auto rd = f.add(draft);

  const auto d = f.ts.db.read([&](db::Conn& c) { return get_thread(c, f.ts.urls, f.alice, r.thread_id); });
  REQUIRE(d);
  CHECK(d->subject == "Hello");
  REQUIRE(d->messages.size() == 3);  // includes the trashed copy and the draft
  const MessageView& m0 = d->messages[0];
  CHECK(m0.id == r.message_id);
  CHECK(m0.direction == Direction::In);
  CHECK(m0.html->find("cid:") == std::string::npos);
  CHECK(m0.html->find(f.ts.urls.base_url() + "/api/files/" + std::to_string(r.attachment_ids[0]) + "?d=i") !=
        std::string::npos);
  CHECK(m0.html->find("data-att-id=\"" + std::to_string(r.attachment_ids[0]) + "\"") != std::string::npos);
  CHECK(m0.text == "hi");
  REQUIRE(m0.attachments.size() == 2);
  CHECK(m0.attachments[0].is_inline);
  CHECK(m0.attachments[0].view_url.has_value());
  CHECK(m0.attachments[1].filename == "doc.pdf");
  CHECK(m0.attachments[1].download_url.find("d=a") != std::string::npos);
  REQUIRE(m0.auth);
  CHECK(m0.auth->dmarc == "pass");
  CHECK_FALSE(m0.auth->spf);
  CHECK(m0.warnings == std::vector<std::string>{"dmarc_fail"});
  CHECK(m0.delivered_to == "support@team.example");
  CHECK(m0.message_id_header == "in1@ext");
  REQUIRE(m0.raw_url);
  CHECK(m0.raw_url->find("/api/files/raw/" + std::to_string(r.message_id)) != std::string::npos);
  CHECK_FALSE(m0.outbound);
  CHECK_FALSE(m0.sent_by);

  const MessageView& m1 = d->messages[1];
  CHECK(m1.id == ro.message_id);
  CHECK(m1.trashed);
  CHECK_FALSE(m1.auth);
  CHECK_FALSE(m1.raw_url);
  REQUIRE(m1.sent_by);
  CHECK(m1.sent_by->email == "bob@team.example");
  CHECK(m1.sent_by->name == "Bob");
  REQUIRE(m1.outbound);
  CHECK(m1.outbound->id == ob);
  CHECK(m1.outbound->status == OutboundStatus::Queued);
  CHECK(m1.outbound->undo_until == kT0 + 5000);
  CHECK_FALSE(m1.outbound->sent_at);
  CHECK(m1.bcc.size() == 1);

  CHECK(d->messages[2].id == rd.message_id);
  CHECK(d->messages[2].is_draft);
  CHECK_FALSE(d->messages[2].outbound);

  // get_message: same rendering; foreign / missing → nullopt.
  const auto gm = f.ts.db.read([&](db::Conn& c) { return get_message(c, f.ts.urls, f.alice, r.message_id); });
  REQUIRE(gm);
  CHECK(gm->html == m0.html);
  f.ts.db.read([&](db::Conn& c) {
    CHECK_FALSE(get_message(c, f.ts.urls, f.bob, r.message_id));
    CHECK_FALSE(get_thread(c, f.ts.urls, f.bob, r.thread_id));
    CHECK_FALSE(get_message(c, f.ts.urls, f.alice, 999999));
    CHECK_FALSE(get_thread(c, f.ts.urls, f.alice, 999999));
  });

  // Accepted outbound: sent_at, no undo window.
  f.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE outbound SET status='accepted', accepted_at=? WHERE id=?", kT0 + 6000, ob); });
  const auto acc = f.ts.db.read([&](db::Conn& c) { return get_message(c, f.ts.urls, f.alice, ro.message_id); });
  REQUIRE(acc->outbound);
  CHECK(acc->outbound->status == OutboundStatus::Accepted);
  CHECK_FALSE(acc->outbound->undo_until);
  CHECK(acc->outbound->sent_at == kT0 + 6000);
}

TEST_CASE("patch_message, message_label_ids, message_events", "[thread_list][views]") {
  Fx f;
  int64_t work = 0, foreign = 0, ob = 0;
  f.ts.db.write([&](db::Tx& tx) {
    work = insert_label(tx, f.alice, "Work");
    foreign = insert_label(tx, f.bob, "B");
    ob = insert_outbound(tx, f.alice, f.alice_addr, "delivered");
    tx.run("INSERT INTO delivery_events(outbound_id, type, occurred_at, detail_json, source_key) VALUES(?,?,?,?,?)",
           ob, "email.delivered", kT0 + 20, "{\"to\":[\"c@x\"]}", "svix_2");
    tx.run("INSERT INTO delivery_events(outbound_id, type, occurred_at, detail_json, source_key) VALUES(?,?,?,?,?)",
           ob, "email.sent", kT0 + 10, "not json", "svix_1");
  });
  const auto r = f.add(f.inbound("P", kT0));
  f.ts.notifier.clear();
  MessagePatch p;
  p.is_read = true;
  p.is_starred = true;
  p.add_label_ids = {work};
  f.ts.db.write([&](db::Tx& tx) { patch_message(tx, f.alice, r.message_id, p); });
  Agg g = f.agg(r.thread_id);
  CHECK(g.unread_count == 0);
  CHECK(g.starred_count == 1);
  CHECK(f.ts.db.read([&](db::Conn& c) { return message_label_ids(c, f.alice, r.message_id); }) ==
        std::vector<int64_t>{work});
  CHECK(f.ts.notifier.events_of(ws::events::kThreadsChanged).size() == 1);
  MessagePatch rm;
  rm.remove_label_ids = {work};
  rm.is_starred = false;
  f.ts.db.write([&](db::Tx& tx) { patch_message(tx, f.alice, r.message_id, rm); });
  CHECK(f.ts.db.read([&](db::Conn& c) { return message_label_ids(c, f.alice, r.message_id); }).empty());
  CHECK(f.agg(r.thread_id).starred_count == 0);

  // IDOR / validation.
  MessagePatch bad;
  bad.add_label_ids = {foreign};
  CHECK_THROWS_AS(f.ts.db.write([&](db::Tx& tx) { patch_message(tx, f.alice, r.message_id, bad); }), ApiError);
  CHECK_THROWS_AS(f.ts.db.write([&](db::Tx& tx) { patch_message(tx, f.bob, r.message_id, p); }), ApiError);
  CHECK(f.ts.db.read([&](db::Conn& c) { return message_label_ids(c, f.bob, r.message_id); }).empty());

  // Events: inbound → empty; outbound → ascending; foreign → nullopt.
  Msg o;
  o.owner = f.alice;
  o.direction = "out";
  o.from = f.me;
  o.outbound_id = ob;
  o.is_read = true;
  const auto ro = f.add(o);
  f.ts.db.read([&](db::Conn& c) {
    auto e0 = message_events(c, f.alice, r.message_id);
    REQUIRE(e0);
    CHECK(e0->empty());
    auto e = message_events(c, f.alice, ro.message_id);
    REQUIRE(e);
    REQUIRE(e->size() == 2);
    CHECK((*e)[0].type == "email.sent");
    CHECK((*e)[0].detail.empty());  // malformed detail_json → {}
    CHECK((*e)[1].type == "email.delivered");
    CHECK((*e)[1].detail.at("to").as_array().size() == 1);
    CHECK_FALSE(message_events(c, f.bob, ro.message_id));
  });
}

TEST_CASE("counts", "[thread_list][counts]") {
  Fx f;
  int64_t l1 = 0, l2 = 0, ob = 0;
  f.ts.db.write([&](db::Tx& tx) {
    l1 = insert_label(tx, f.alice, "L1");
    l2 = insert_label(tx, f.alice, "L2");
    ob = insert_outbound(tx, f.alice, f.alice_addr, "scheduled", kT0 + kDay);
  });
  Msg a = f.inbound("a", kT0);
  a.labels = {l1};
  f.add(a);
  Msg b = f.inbound("b", kT0 + 1);
  b.is_read = true;
  b.labels = {l1};
  f.add(b);
  Msg s = f.inbound("s", kT0 + 2);
  s.is_spam = true;
  s.in_inbox = false;
  s.labels = {l2};  // spam does not count for labels
  f.add(s);
  Msg d;
  d.owner = f.alice;
  d.direction = "out";
  d.is_draft = true;
  d.from = f.me;
  d.subject = "d";
  d.in_inbox = false;
  f.add(d);
  Msg sch = d;
  sch.is_draft = false;
  sch.outbound_id = ob;
  sch.subject = "sch";
  f.add(sch);

  const Counts c = f.cnt();
  CHECK(c.inbox_unread == 1);
  CHECK(c.drafts == 1);
  CHECK(c.scheduled == 1);
  CHECK(c.spam_unread == 1);
  REQUIRE(c.labels.size() == 2);
  CHECK(c.labels.at(l1).total == 2);
  CHECK(c.labels.at(l1).unread == 1);
  CHECK(c.labels.at(l2).total == 0);
  CHECK(c.labels.at(l2).unread == 0);
  const Counts other = f.cnt(f.bob);
  CHECK(other.inbox_unread == 0);
  CHECK(other.labels.empty());
}

TEST_CASE("contacts: search and upsert", "[thread_list][contacts]") {
  Fx f;
  f.ts.db.write([&](db::Tx& tx) {
    test::seed_user(tx, "zhang@team.example", false, "张伟");
    test::seed_user(tx, "azhar@team.example", false, "Azhar");
    const int64_t gone = test::seed_user(tx, "zhgone@team.example", false, "Gone");
    tx.run("UPDATE users SET disabled = 1 WHERE id = ?", gone);  // disabled users are not suggested
    upsert_contact(tx, f.alice, {"Carol Ext", "Carol@Ext.Example"}, 1.0, kT0);
    upsert_contact(tx, f.alice, {"", "carol@ext.example"}, 1.0, kT0 + 5);  // name kept, score summed
    upsert_contact(tx, f.alice, {"Zed", "zed@other.example"}, 0.2, kT0);
    upsert_contact(tx, f.alice, {"Me", "alice@team.example"}, 1.0, kT0);  // team address skipped
    upsert_contact(tx, f.alice, {"x", "not-an-email"}, 1.0, kT0);         // invalid skipped
    upsert_contact(tx, f.bob, {"Bob's", "secret@ext.example"}, 5.0, kT0);
  });
  f.ts.db.read([&](db::Conn& c) {
    CHECK(c.scalar<double>("SELECT score FROM contacts WHERE owner_id=? AND email='carol@ext.example'", f.alice) ==
          2.0);
    CHECK(c.scalar<std::string>("SELECT name FROM contacts WHERE owner_id=? AND email='carol@ext.example'", f.alice) ==
          "Carol Ext");
    CHECK(c.scalar<int64_t>("SELECT last_used_at FROM contacts WHERE owner_id=? AND email='carol@ext.example'",
                            f.alice) == kT0 + 5);
    CHECK(count(c, "SELECT COUNT(*) FROM contacts WHERE email='alice@team.example'") == 0);

    auto all = search_contacts(c, f.alice, "", 50);
    // Team users first, then aliases, then contacts by score.
    REQUIRE(all.size() >= 6);
    CHECK(all[0].kind == ContactKind::Team);
    auto pos = [&](std::string_view email) {
      for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].email == email) return static_cast<int>(i);
      return -1;
    };
    CHECK(pos("support@team.example") > pos("zhang@team.example"));
    CHECK(all[static_cast<std::size_t>(pos("support@team.example"))].kind == ContactKind::Alias);
    CHECK(pos("carol@ext.example") < pos("zed@other.example"));
    CHECK(pos("carol@ext.example") > pos("support@team.example"));
    CHECK(pos("secret@ext.example") == -1);  // bob's contact

    auto z = search_contacts(c, f.alice, "张", 8);
    REQUIRE(z.size() == 1);
    CHECK(z[0].email == "zhang@team.example");
    CHECK(z[0].name == "张伟");
    // Prefix matches rank first within the team directory ("azhar" sorts before "zhang").
    auto zh = search_contacts(c, f.alice, "zh", 8);
    REQUIRE(zh.size() == 2);
    CHECK(zh[0].email == "zhang@team.example");
    CHECK(zh[1].email == "azhar@team.example");
    auto car = search_contacts(c, f.alice, "CAROL", 8);
    REQUIRE(car.size() == 1);
    CHECK(car[0].kind == ContactKind::Contact);
    CHECK(search_contacts(c, f.alice, "%", 8).empty());  // LIKE metachar escaped
    CHECK(search_contacts(c, f.alice, "", 2).size() == 2);
    CHECK(search_contacts(c, f.alice, "", 0).size() == 1);  // clamped to ≥ 1
  });
}

TEST_CASE("purge_trash: retention for trash and spam", "[thread_list][purge]") {
  Fx f;
  const int64_t now = kT0 + 100 * kDay;
  Msg old_trash = f.inbound("old trash", kT0);
  old_trash.trashed_at = now - 31 * kDay;
  const auto r1 = f.add(old_trash);
  Msg new_trash = f.inbound("new trash", kT0);
  new_trash.trashed_at = now - 1 * kDay;
  const auto r2 = f.add(new_trash);
  Msg old_spam = f.inbound("old spam", now - 40 * kDay);
  old_spam.is_spam = true;
  const auto r3 = f.add(old_spam);
  Msg keep = f.inbound("keep", kT0, r1.thread_id);  // shares the thread of the purged message
  const auto r4 = f.add(keep);
  Msg bobs = f.inbound("bob old trash", kT0);
  bobs.owner = f.bob;
  bobs.trashed_at = now - 60 * kDay;
  const auto r5 = f.add(bobs);
  f.ts.notifier.clear();

  // limit 1 → more=true, then the rest.
  auto res = f.ts.db.write([&](db::Tx& tx) { return purge_trash(tx, now, 30, 30, 1); });
  CHECK(res.messages_deleted == 1);
  CHECK(res.more);
  res = f.ts.db.write([&](db::Tx& tx) { return purge_trash(tx, now, 30, 30, 100); });
  CHECK(res.messages_deleted == 2);
  CHECK_FALSE(res.more);
  f.ts.db.read([&](db::Conn& c) {
    auto exists = [&](int64_t id) { return c.scalar<int64_t>("SELECT 1 FROM messages WHERE id=?", id).has_value(); };
    CHECK_FALSE(exists(r1.message_id));
    CHECK(exists(r2.message_id));
    CHECK_FALSE(exists(r3.message_id));
    CHECK(exists(r4.message_id));
    CHECK_FALSE(exists(r5.message_id));
    CHECK_FALSE(c.scalar<int64_t>("SELECT 1 FROM threads WHERE id=?", r3.thread_id).has_value());
  });
  CHECK(f.agg(r1.thread_id).msg_count == 1);
  // threads.changed per owner.
  std::set<int64_t> owners;
  for (const auto& e : f.ts.notifier.events_of(ws::events::kThreadsChanged)) owners.insert(e.user_id);
  CHECK(owners == std::set<int64_t>{f.alice, f.bob});
  // Negative retention disables; nothing left to purge either way.
  res = f.ts.db.write([&](db::Tx& tx) { return purge_trash(tx, now, -1, -1, 100); });
  CHECK(res.messages_deleted == 0);
  res = f.ts.db.write([&](db::Tx& tx) { return purge_trash(tx, now, 0, 30, 100); });  // 0 days: purge now
  CHECK(res.messages_deleted == 1);
}
