// A conversation thread, found by rebuilding who answers whom around a picked message.
//
// Every message may answer an earlier one, on evidence of different strength (after Kummerfeld et
// al., "A Large-Scale Corpus for Conversation Disentanglement", ACL 2019, without the learning):
//   strong  an @[Name] mention (the last thing Name said, and the burst of consecutive messages it
//           ended), a reply to someone who just mentioned the sender, the sender's own previous
//           message, a back-and-forth between two people already linked by a mention;
//   weak    shared words with a message a few steps back.
// A conversation is what strong evidence connects, and it grows in both directions: what the
// picked message answers and what answers it. Weak evidence only adds messages that nothing else
// explains, one step from the conversation, and never joins two conversations. Order is the local
// order of the messages (their sequence), not the senders' clocks; times only break ties.
// Pure, so it is tested on the host.
#pragma once
#include "model/model.h"

namespace ui {

// Message `i` of a conversation, oldest first; false if there is none.
typedef bool (*MessageAt)(void* ctx, int i, model::Message& m);

constexpr int THREAD_MAX = 24;       // messages in a thread
constexpr int THREAD_WINDOW = 128;   // messages looked at (those around the picked one)

// The first @[Name] in `text`, into `out`; false if there is none.
bool firstMention(const char* text, char* out, int n);

// The next @[Name] at or after `*pos` in `text`, into `out`, moving `*pos` past it.
bool nextMention(const char* text, const char** pos, char* out, int n);

// The thread around message `idx` of `count`, as ascending indexes into `out` (at most `max`); `me`
// is the name shown for outgoing messages. `conf`, if given, is 0 for messages joined by strong
// evidence and 1 for those attached on weak evidence. Returns how many: 1 (just the message) if it
// is on its own. Scratch memory is a few KB, taken and released inside the call.
int buildThreadEx(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, uint8_t* conf, int max);
int buildThread(MessageAt at, void* ctx, int count, int idx, const char* me, int* out, int max);

}  // namespace ui
