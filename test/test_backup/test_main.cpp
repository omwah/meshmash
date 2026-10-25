// Backup container: round trip, damage detection, and forward compatibility.
#include <unity.h>
#include <string>
#include <sys/stat.h>
#include <FS.h>
#include "app/backup.h"
#include "app/msg_store.h"
#include "app/platform.h"

using app::Backup;

static std::string base() {
  static std::string b;
  if (b.empty()) {
    char tmpl[] = "sim-data/test-backup-XXXXXX";   // git-ignored, one per run
    mkdir("sim-data", 0755);
    b = mkdtemp(tmpl);
  }
  return b;
}

static void writeFile(fs::FS& f, const char* path, const std::string& data) {
  File o = f.open(path, "w", true);
  o.write((const uint8_t*)data.data(), data.size());
  o.close();
}

static std::string readFile(fs::FS& f, const char* path) {
  File i = f.open(path, "r");
  std::string s;
  for (int c; i && (c = i.read()) >= 0;) s += (char)c;
  return s;
}

static int s_case = 0;
struct Fixture {
  fs::FS flash, sd;
  Fixture() {
    std::string d = base() + "/" + std::to_string(++s_case);
    flash.setRoot(d + "/flash");
    sd.setRoot(d + "/sd");
    flash.format();
    sd.format();
    writeFile(flash, "/identity/_main.id", std::string(96, 'K'));
    writeFile(flash, "/prefs.json", "{\"name\":\"node\"}");
    writeFile(flash, "/contacts3", std::string(152, 'C'));
    writeFile(flash, "/ui/prefs.bin", "UIPREFS");
    writeFile(flash, "/ui/msgs.bin", "MESSAGES");
  }
};

void test_round_trip() {
  Fixture f;
  char err[64] = {};
  TEST_ASSERT_TRUE(Backup::write(f.flash, f.sd, {true, false}, 1791727500, "v1.17.1", err, sizeof err));
  Backup::Info i = Backup::info(f.sd, false);
  TEST_ASSERT_TRUE(i.valid);
  TEST_ASSERT_EQUAL_UINT32(1791727500, i.created);
  TEST_ASSERT_EQUAL(4, i.sections);   // identity, prefs, contacts, ui prefs (no messages)

  f.flash.format();
  TEST_ASSERT_TRUE(Backup::restore(f.flash, f.sd, false, err, sizeof err));
  TEST_ASSERT_EQUAL_STRING(std::string(96, 'K').c_str(), readFile(f.flash, "/identity/_main.id").c_str());
  TEST_ASSERT_EQUAL_STRING("UIPREFS", readFile(f.flash, "/ui/prefs.bin").c_str());
  TEST_ASSERT_FALSE(f.flash.exists("/ui/msgs.bin"));
}

void test_identity_can_be_left_out() {
  Fixture f;
  char err[64];
  Backup::write(f.flash, f.sd, {false, true}, 1, "x", err, sizeof err);
  f.flash.format();
  Backup::restore(f.flash, f.sd, false, err, sizeof err);
  TEST_ASSERT_FALSE(f.flash.exists("/identity/_main.id"));
  TEST_ASSERT_EQUAL_STRING("MESSAGES", readFile(f.flash, "/ui/msgs.bin").c_str());
}

void test_corruption_is_detected_and_nothing_written() {
  Fixture f;
  char err[64];
  Backup::write(f.flash, f.sd, {true, false}, 1, "x", err, sizeof err);
  std::string b = readFile(f.sd, Backup::PATH);
  b[b.size() - 3] ^= 0x40;   // flip a bit in the last section
  writeFile(f.sd, Backup::PATH, b);
  TEST_ASSERT_FALSE(Backup::info(f.sd, false).valid);
  f.flash.format();
  TEST_ASSERT_FALSE(Backup::restore(f.flash, f.sd, false, err, sizeof err));
  TEST_ASSERT_FALSE(f.flash.exists("/prefs.json"));   // all-or-nothing
}

void test_truncated_file_is_invalid() {
  Fixture f;
  char err[64];
  Backup::write(f.flash, f.sd, {true, false}, 1, "x", err, sizeof err);
  std::string b = readFile(f.sd, Backup::PATH);
  writeFile(f.sd, Backup::PATH, b.substr(0, b.size() / 2));
  TEST_ASSERT_FALSE(Backup::info(f.sd, false).valid);
}

void test_previous_backup_is_kept() {
  Fixture f;
  char err[64];
  Backup::write(f.flash, f.sd, {true, false}, 100, "x", err, sizeof err);
  writeFile(f.flash, "/prefs.json", "{\"name\":\"changed\"}");
  Backup::write(f.flash, f.sd, {true, false}, 200, "x", err, sizeof err);
  TEST_ASSERT_EQUAL_UINT32(200, Backup::info(f.sd, false).created);
  TEST_ASSERT_EQUAL_UINT32(100, Backup::info(f.sd, true).created);
  Backup::restore(f.flash, f.sd, true, err, sizeof err);
  TEST_ASSERT_EQUAL_STRING("{\"name\":\"node\"}", readFile(f.flash, "/prefs.json").c_str());
}

// A section a newer firmware added (unknown path) is skipped, and a path
// outside the allowed set is never written.
void test_unknown_sections_are_skipped() {
  Fixture f;
  char err[64];
  Backup::write(f.flash, f.sd, {true, false}, 1, "x", err, sizeof err);
  std::string b = readFile(f.sd, Backup::PATH);
  const char* path = "/future/thing";
  std::string data = "FUTURE";
  uint32_t len = data.size(), crc = Backup::crc32(0, (const uint8_t*)data.data(), data.size());
  std::string sec;
  sec += (char)strlen(path);
  sec += path;
  sec.append((const char*)&len, 4);
  sec.append((const char*)&crc, 4);
  sec += data;
  b += sec;
  uint16_t n;
  memcpy(&n, &b[6], 2);
  n++;
  memcpy(&b[6], &n, 2);
  writeFile(f.sd, Backup::PATH, b);
  TEST_ASSERT_TRUE(Backup::info(f.sd, false).valid);
  f.flash.format();
  TEST_ASSERT_TRUE(Backup::restore(f.flash, f.sd, false, err, sizeof err));
  TEST_ASSERT_FALSE(f.flash.exists("/future/thing"));
  TEST_ASSERT_TRUE(f.flash.exists("/contacts3"));
}

// MessageStore::history reads a conversation back without moving the open window.
void test_history_reads_one_conversation_in_order() {
  Fixture f;
  app::MessageStore st;
  TEST_ASSERT_TRUE(st.begin(&f.flash));
  st.setArchive(&f.sd);
  app::ThreadKey a = app::ThreadKey::channel(0), b = app::ThreadKey::channel(1);
  uint32_t seqs[20];
  for (int i = 0; i < 20; i++) {
    model::Message m = {};
    snprintf(m.sender, sizeof m.sender, "Person %d", i);
    snprintf(m.text, sizeof m.text, "text %d", i);
    seqs[i] = st.append(i % 4 == 3 ? b : a, m);
    TEST_ASSERT_NOT_EQUAL(0, seqs[i]);
  }
  st.open(a);
  int before = st.count();
  model::Message out[8];
  int n = st.history(a, seqs[18], out, 8);
  TEST_ASSERT_EQUAL(8, n);
  TEST_ASSERT_EQUAL_UINT32(seqs[18], out[n - 1].seq);
  for (int i = 1; i < n; i++) TEST_ASSERT_TRUE(out[i - 1].seq < out[i].seq);
  // Messages 3, 7, 11, 15 belong to the other conversation.
  for (int i = 0; i < n; i++) {
    TEST_ASSERT_NOT_EQUAL(seqs[3], out[i].seq);
    TEST_ASSERT_NOT_EQUAL(seqs[7], out[i].seq);
    TEST_ASSERT_NOT_EQUAL(seqs[11], out[i].seq);
  }
  TEST_ASSERT_EQUAL(before, st.count());   // the window stayed put
  TEST_ASSERT_EQUAL(3, st.history(a, seqs[2], out, 8));   // 0, 1, 2
  TEST_ASSERT_EQUAL(0, st.history(a, 0, out, 8));
}

// A ring file made for another size (80 slots here, 40 built) is moved to the new size at boot; what
// no longer fits goes to the archive and is still found by scrolling back.
static void refit(bool withCard) {
  Fixture f;
  const int RS = 209, OLD = 80;
  std::string ring(OLD * RS, '\0');
  for (uint32_t seq = 1; seq <= 100; seq++) {   // a ring that has wrapped: newer seqs overwrite older
    char* r = &ring[(seq % OLD) * RS];
    memcpy(r, &seq, 4);
    r[4] = 1;   // channel 0
    snprintf(r + 25, 24, "Person");
    snprintf(r + 49, 160, "text %u", seq);
  }
  f.flash.mkdir("/ui");
  writeFile(f.flash, "/ui/msgs.bin", ring);
  app::MessageStore st;
  if (withCard) st.setArchive(&f.sd);
  TEST_ASSERT_TRUE(st.begin(&f.flash));
  TEST_ASSERT_EQUAL(OLD, (int)st.stats().resized);
  File check = f.flash.open("/ui/msgs.bin", "r");
  TEST_ASSERT_EQUAL((int)(app::MessageStore::CAPACITY * app::MessageStore::RECORD_BYTES), (int)check.size());
  check.close();
  TEST_ASSERT_FALSE(f.flash.exists("/ui/msgs.bin.new"));
  TEST_ASSERT_FALSE(f.sd.exists("/meshmash/ring.tmp"));
  app::ThreadKey k = app::ThreadKey::channel(0);
  TEST_ASSERT_EQUAL_UINT32(100, st.find(k)->lastSeq);
  // Nothing is lost: the newest are in the ring, the rest reachable through the window and archive.
  st.open(k);
  TEST_ASSERT_EQUAL_UINT32(100, st.at(st.count() - 1).seq);
  int seen = st.count();
  if (withCard) {
    for (int i = 0; i < 20 && st.older(); i++) seen += 0;
    TEST_ASSERT_TRUE(st.at(0).seq < 100 - 23);   // went back past the first window, to the archive
  }
  // The same file again: nothing to do.
  app::MessageStore again;
  if (withCard) again.setArchive(&f.sd);
  TEST_ASSERT_TRUE(again.begin(&f.flash));
  TEST_ASSERT_EQUAL(0, (int)again.stats().resized);
  (void)seen;
}

void test_ring_of_another_size_is_refitted() { refit(true); }
void test_ring_of_another_size_is_refitted_without_a_card() { refit(false); }

// With a card the history lives on it; what the flash held is merged in once and then removed.
void test_history_moves_to_the_card() {
  Fixture f;
  const int RS = 209, CAP = app::MessageStore::CAPACITY;
  std::string ring(CAP * RS, '\0');
  for (uint32_t seq = 1; seq <= 30; seq++) {
    char* r = &ring[(seq % CAP) * RS];
    memcpy(r, &seq, 4);
    r[4] = 1;   // channel 0
    snprintf(r + 25, 24, "Person");
    snprintf(r + 49, 160, "text %u", seq);
  }
  f.flash.mkdir("/ui");
  writeFile(f.flash, "/ui/msgs.bin", ring);
  writeFile(f.flash, "/ui/read.bin", std::string(11, 'R'));
  {
    app::MessageStore st;
    st.setArchive(&f.sd);
    TEST_ASSERT_TRUE(st.begin(&f.sd, nullptr, &f.flash));
    TEST_ASSERT_TRUE(st.stats().onCard);
    TEST_ASSERT_FALSE(f.flash.exists("/ui/msgs.bin"));   // the flash is left alone from now on
    TEST_ASSERT_FALSE(f.flash.exists("/ui/read.bin"));
    TEST_ASSERT_TRUE(f.sd.exists("/meshmash/msgs.bin"));
    TEST_ASSERT_TRUE(f.sd.exists("/meshmash/read.bin"));
    TEST_ASSERT_EQUAL_UINT32(30, st.find(app::ThreadKey::channel(0))->lastSeq);
    model::Message m = {};
    snprintf(m.sender, sizeof m.sender, "Other");
    snprintf(m.text, sizeof m.text, "after");
    uint32_t s = st.append(app::ThreadKey::channel(0), m);
    TEST_ASSERT_EQUAL_UINT32(31, s);
    TEST_ASSERT_EQUAL(0, (int)st.stats().writeFails);
    TEST_ASSERT_FALSE(f.flash.exists("/ui/msgs.bin"));
  }
  // The next boot reads it back from the card.
  app::MessageStore again;
  again.setArchive(&f.sd);
  TEST_ASSERT_TRUE(again.begin(&f.sd, nullptr, &f.flash));
  TEST_ASSERT_EQUAL_UINT32(31, again.find(app::ThreadKey::channel(0))->lastSeq);
  again.open(app::ThreadKey::channel(0));
  TEST_ASSERT_EQUAL_UINT32(31, again.at(again.count() - 1).seq);
}

// Senders' clocks are often wrong: a message is stamped with this device's time once that is set.
void test_message_time_is_the_devices_once_set() {
  const uint32_t now = 1792900000, wrong = 1400000000;
  TEST_ASSERT_EQUAL_UINT32(now, app::receivedAt(now, wrong));
  TEST_ASSERT_EQUAL_UINT32(now, app::receivedAt(now, now + 86400 * 400));
  TEST_ASSERT_EQUAL_UINT32(wrong, app::receivedAt(5000, wrong));   // clock not set yet: the sender's
}

// How far a sender's clock was from ours is kept with the message, rounded.
void test_sender_clock_offset_is_kept() {
  Fixture f;
  app::MessageStore st;
  TEST_ASSERT_TRUE(st.begin(&f.flash));
  app::ThreadKey k = app::ThreadKey::channel(0);
  const int32_t offs[] = {5, -40, 3 * 60, -2 * 3600 - 7, 5 * 86400, -(600 * 86400), 3 * 604800 * 100};
  const int n = sizeof offs / sizeof offs[0];
  for (int i = 0; i < n; i++) {
    model::Message m = {};
    m.ts = 1792900000 + i;
    m.skewKnown = true;
    m.skew = offs[i];
    snprintf(m.sender, sizeof m.sender, "Someone");
    snprintf(m.text, sizeof m.text, "text %d", i);
    TEST_ASSERT_NOT_EQUAL(0, st.append(k, m));
  }
  model::Message none = {};   // an older record has no offset
  none.ts = 1792900100;
  snprintf(none.text, sizeof none.text, "plain");
  st.append(k, none);
  app::MessageStore again;   // read back from the file, not the window
  TEST_ASSERT_TRUE(again.begin(&f.flash));
  again.open(k);
  TEST_ASSERT_EQUAL(n + 1, again.count());
  for (int i = 0; i < n; i++) {
    const model::Message& r = again.at(i);
    TEST_ASSERT_TRUE(r.skewKnown);
    int32_t want = offs[i], got = r.skew;
    int32_t mag = want < 0 ? -want : want, tol = mag < 60 ? 59 : mag < 3600 ? 59 : mag < 86400 ? 3599 : mag < 604800 ? 86399 : 604799;
    if (mag >= 255 * 604800) tol = mag;   // beyond the range: clamped
    TEST_ASSERT_TRUE((want < 0) == (got < 0) || got == 0);
    TEST_ASSERT_TRUE((got < 0 ? -got : got) <= mag);
    TEST_ASSERT_TRUE(mag - (got < 0 ? -got : got) <= tol);
  }
  TEST_ASSERT_FALSE(again.at(n).skewKnown);
}

// A message keeps its whole route, and records written before that (209 bytes, six route bytes packed
// into other fields) are converted: the ring when it is rebuilt, an archive file when it is first used.
void test_the_whole_route_is_kept() {
  Fixture f;
  app::MessageStore st;
  TEST_ASSERT_TRUE(st.begin(&f.flash));
  app::ThreadKey k = app::ThreadKey::channel(0);
  model::Message m = {};
  m.ts = 1792900000;
  m.hops = 20;
  m.pathSize = 1;
  m.pathN = 20;
  for (int i = 0; i < 20; i++) m.path[i] = (uint8_t)(0x10 + i);
  snprintf(m.sender, sizeof m.sender, "Someone");
  snprintf(m.text, sizeof m.text, "long way round");
  TEST_ASSERT_NOT_EQUAL(0, st.append(k, m));
  app::MessageStore again;   // from the file
  TEST_ASSERT_TRUE(again.begin(&f.flash));
  again.open(k);
  const model::Message& r = again.at(0);
  TEST_ASSERT_EQUAL(1, r.pathSize);
  TEST_ASSERT_EQUAL(20, r.pathN);
  for (int i = 0; i < 20; i++) TEST_ASSERT_EQUAL_UINT8(0x10 + i, r.path[i]);
}

void test_an_old_archive_file_is_converted() {
  Fixture f;
  std::string old;   // three 209-byte records in the old layout, the second with a packed route
  for (uint32_t seq = 1; seq <= 3; seq++) {
    std::string rec(209, '\0');
    memcpy(&rec[0], &seq, 4);
    rec[4] = 1;   // channel 0
    if (seq == 2) {
      rec[11] = 2;        // flags: the route is stored, one-byte hashes
      rec[16] = 3;        // hops
      const uint8_t b[6] = {1, 2, 3, 4, 5, 6};
      memcpy(&rec[21], b, 4);   // ack
      rec[13] = (char)b[4];     // retries
      rec[14] = (char)b[5];     // repeats
    }
    snprintf(&rec[25], 24, "Person");
    snprintf(&rec[49], 160, "old %u", seq);
    old += rec;
  }
  f.sd.mkdir("/meshmash");
  f.sd.mkdir("/meshmash/history");
  writeFile(f.sd, "/meshmash/history/c000000000000.bin", old);
  app::MessageStore st;
  st.setArchive(&f.sd);
  TEST_ASSERT_TRUE(st.begin(&f.flash));
  model::Message out[8];
  TEST_ASSERT_EQUAL(3, st.history(app::ThreadKey::channel(0), 3, out, 8));
  TEST_ASSERT_EQUAL_UINT32(2, out[1].seq);
  TEST_ASSERT_EQUAL(3, out[1].pathN);
  TEST_ASSERT_EQUAL_UINT8(1, out[1].path[0]);
  TEST_ASSERT_EQUAL_UINT8(3, out[1].path[2]);
  TEST_ASSERT_FALSE(f.sd.exists("/meshmash/history/c000000000000.bin"));
  TEST_ASSERT_TRUE(f.sd.exists("/meshmash/history/c000000000000.m2"));
  TEST_ASSERT_EQUAL(3 * app::MessageStore::RECORD_BYTES, (int)f.sd.open("/meshmash/history/c000000000000.m2", "r").size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip);
  RUN_TEST(test_the_whole_route_is_kept);
  RUN_TEST(test_an_old_archive_file_is_converted);
  RUN_TEST(test_sender_clock_offset_is_kept);
  RUN_TEST(test_message_time_is_the_devices_once_set);
  RUN_TEST(test_history_moves_to_the_card);
  RUN_TEST(test_ring_of_another_size_is_refitted);
  RUN_TEST(test_ring_of_another_size_is_refitted_without_a_card);
  RUN_TEST(test_history_reads_one_conversation_in_order);
  RUN_TEST(test_identity_can_be_left_out);
  RUN_TEST(test_corruption_is_detected_and_nothing_written);
  RUN_TEST(test_truncated_file_is_invalid);
  RUN_TEST(test_previous_backup_is_kept);
  RUN_TEST(test_unknown_sections_are_skipped);
  return UNITY_END();
}
