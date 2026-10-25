// Entry points to every screen. Screens are file-local singletons; other
// screens open them through these functions.
#pragma once
#include "ui/app.h"

#ifndef MC_UI_REMOTE
#define MC_UI_REMOTE 0   // remote node mode (set by builds with Bluetooth)
#endif

namespace ui { namespace screens {

Screen* home();
Screen* boot();
void openThread(App& a, model::ThreadId t);
// Vim keys: back to the conversation open before this one (Ctrl-o), and to
// the next or previous one with unread messages (]u, [u).
void jumpBack(App& a);
bool draftPreview(model::ThreadId t, char* out, int n);   // a half-written message waiting in a chat
void jumpUnread(App& a, int dir);
// If the conversation is on top, start writing in it.
void startCompose(App& a);
// The touch shortcut bar: Home on one of its tabs, or the main menu.
enum { SC_CHATS, SC_CONTACTS, SC_NODES, SC_CHANNELS, SC_MENU, SC_COUNT };
void shortcut(App& a, int sc);
// The shortcut showing now, -1 if none.
int shortcutShowing(App& a);
// The open chat's messages from the "New" line down (-1: no line). For tests.
int chatNewCount(App& a);
void openContact(App& a, int contactIdx);
void openChannel(App& a, int channelIdx);
void openAdmin(App& a, int contactIdx);
void openMainMenu(App& a);
bool mainMenuOpen(App& a);
// Going to another tab from inside the menu (Diagnostics, Settings...): remember where it was,
// so the Menu button, or the menu key, comes back to the same screen.
void menuLeaving(App& a);
void openSettings(App& a);
void openRadio(App& a);
void openDiag(App& a);
void openPacketLog(App& a);
void openGps(App& a);
void openHeard(App& a, bool repeatersOnly);
void openBackup(App& a);
void openJoinChannel(App& a);
#if MC_UI_REMOTE
void openRemote(App& a);   // remote mode: the link to the node
#endif
// Scrollable text page (results, keys, help).
void openText(App& a, const char* title, const char* body);

}}  // namespace ui::screens
