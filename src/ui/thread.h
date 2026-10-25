// A conversation thread, found by following mentions back: a message that
// says @[Name] answers the last thing Name wrote (and the messages they wrote
// right before it, with nobody else in between); that in turn may answer
// someone else. Pure, so it is tested on the host.
#pragma once
#include "model/model.h"

namespace ui {

// Message `i` of a conversation, oldest first; false if there is none.
typedef bool (*MessageAt)(void* ctx, int i, model::Message& m);

constexpr int THREAD_MAX = 12;       // messages in a thread
constexpr int THREAD_DEPTH = 8;      // replies followed

// The first @[Name] in `text`, into `out`; false if there is none.
bool firstMention(const char* text, char* out, int n);

// The thread that ends at message `idx` of `count`, as ascending indexes into
// `out` (at most `max`); `me` is the name shown for outgoing messages. Returns
// how many: 1 (just the message itself) if it answers no one.
int buildThread(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, int max);

}  // namespace ui
