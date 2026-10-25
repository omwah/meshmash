#include "ui/thread.h"
#include <string.h>
#include <strings.h>

namespace ui {

bool firstMention(const char* text, char* out, int n) {
  const char* p = strstr(text, "@[");
  if (!p) return false;
  p += 2;
  const char* e = strchr(p, ']');
  if (!e || e == p) return false;
  int len = (int)(e - p);
  if (len >= n) len = n - 1;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}

namespace {

// Who wrote it: outgoing messages carry no name.
bool writtenBy(const model::Message& m, const char* name, const char* me) {
  return strcasecmp(m.outgoing ? me : m.sender, name) == 0;
}

}  // namespace

int buildThread(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, int max) {
  if (idx < 0 || idx >= count || max < 1) return 0;
  // Collected newest first, then reversed.
  int rev[THREAD_MAX];
  int n = 0;
  rev[n++] = idx;
  int cur = idx;
  for (int depth = 0; depth < THREAD_DEPTH; depth++) {
    model::Message m;
    char name[model::NAME_LEN];
    if (!at(ctx, cur, m) || !firstMention(m.text, name, sizeof name)) break;
    int j = cur - 1;
    for (; j >= 0; j--) {
      if (at(ctx, j, m) && writtenBy(m, name, me)) break;
    }
    if (j < 0) break;
    // Their message, and the ones they sent right before it.
    int k = j;
    while (k > 0 && at(ctx, k - 1, m) && writtenBy(m, name, me)) k--;
    if (n + (j - k + 1) > max || n + (j - k + 1) > THREAD_MAX) break;
    for (int i = j; i >= k; i--) rev[n++] = i;
    cur = k;
  }
  for (int i = 0; i < n; i++) out[i] = rev[n - 1 - i];
  return n;
}

}  // namespace ui
