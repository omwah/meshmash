# Locked notification preview plan

Status: Draft for review. Split out of `docs/POWER-PLAN.md`; depends on that plan's Phase 2 (screen wake separated from input unlock) but on no sleep work. When implemented, this plan is removed and replaced by as-implemented documentation, as described at the end of `docs/POWER-PLAN.md`.

## Scope

When input is locked and a notification action is set to light the screen, show a dedicated incoming-message preview instead of waking whichever screen was last open. While unlocked, never replace the current screen; keep normal notification behaviour.

## Preview text source

Stored previews are short (`char preview[48]` in `src/model/model.h`; 44 bytes in `src/app/msg_store.h`). At arrival, copy the newest incoming message's sender and full text into a single bounded RAM buffer (about 160 bytes, MeshCore's maximum text length) owned by the notification policy. The preview renders from that buffer; no storage reads while locked. A newer eligible message overwrites it.

## Layout

- Sender/conversation identity and a word-wrapped message preview at the selected text size, using the available display area.
- A small lock icon and a compact board-specific hint: `Press G0 to unlock` (Cardputer) or `Press trackball to unlock` (T-Deck). No separate locked-state message.
- Truncate overflow visibly; no auto-scroll.
- Add layout examples and simulator tests for both devices at Small, Medium, and Large.

## Behaviour

- Input stays locked throughout; showing the preview is not an unlock action. Non-screen notification settings never open or light this screen.
- Preserve the previous screen/navigation state. On expiry, dismiss the preview and return to the dark, locked state. On explicit unlock, dismiss it and restore the previous interactive screen without forwarding the unlock press.
- Showing a preview does not mark the message read.
- A newer eligible message during the preview replaces it and restarts the timer.
- No pre-timeout dimming during the preview.
- Applies even when the normal timeout is "never" and the device was manually locked.

## Reading time

Brysbaert's 2019 meta-analysis (190 studies) puts average adult silent non-fiction reading at 238 words/minute, most adults 175–300 (https://biblio.ugent.be/publication/8647789, DOI 10.1016/j.jml.2019.104047). Use a conservative 200 wpm plus 2 s to notice the preview, minimum 4 s:

```text
seconds = max(4, ceil(2 + visible_words * 60 / 200))
```

- Count only text actually rendered: sender/conversation label and visible message text. Exclude truncated text and the fixed unlock hint (covered by the 2 s margin).
- Render first, then start the timer.
- Count words by spaces normally. When visible text has fewer than one space per 12 glyphs (non-Latin scripts, long unbroken strings, emoji runs), use `visible_glyphs / 6` words instead. Count glyphs as rendered, never bytes; each emoji counts as one glyph.
- Test short, full, truncated, and multi-notification cases on each device/font; verify hidden message length doesn't affect the timer.
