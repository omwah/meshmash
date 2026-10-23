"""Which emoji each coverage tier carries, most useful first.

Tokens are whitespace-separated and each is exactly one emoji sequence (a
variation selector is optional; keys are compared without U+FE0F). A profile
takes its tiers in order until its byte budget is spent (scripts/gen_emoji.py),
so put what people send most near the top of a tier.

`core` is what the Cardputer carries: everyday reactions and faces, the hearts
and signs, and the things a mesh radio user talks about (signal, battery,
position, weather, getting somewhere). `common` adds to that for the T-Deck.
Flags, skin tones and joined sequences are listed only where they are common.
"""

CORE = """
😀 😃 😄 😁 😆 😅 😂 🤣 😊 🙂 😉 😍 🥰 😘 😋 😎 🤔 😐 😴 😢 😭 😡 🤯 😱 🥺 🙄 😬 🤗 🥳 😇 🤩 😜 😏 😮
👍 👎 👋 👏 🙏 💪 ✌️ 🤝 👌 🤞 🫡 🙌 👍🏽 👋🏿
❤️ 💙 💚 💛 🧡 💜 🖤 💔 💕 ✨ 🔥 ⭐ 🎉 💯 ✅ ❌ ⚠️ ❓ ❗
📡 📶 🛰️ 🔋 🪫 ⚡ 🔌 📍 🗺️ 📻 📱 🧭 🔦 🚨
☀️ 🌧️ ⛈️ ❄️ 🌈 🌲 ⛰️ 🌊
🏠 🚗 🚙 🏕️ 🚲 🚶 ☕ 🍺 🍕 🐶
🇺🇸 🇨🇦 🇯🇵
👨‍👩‍👧
🪶 🧿
"""

COMMON = """
😌 😔 😒 😞 😟 😤 😩 😫 🥲 😳 🤪 😝 🤤 🥱 😵 🤒 🤕 🤢 🤮 🥵 🥶 😈 💀 👻 🤖 🙈 🙉 🙊 😺 😹 😻
👊 ✊ 🤙 🤘 🤟 ☝️ 👆 👇 👉 👈 ✋ 🖐️ 🖖 🤚 🫶 🫂 🙋 🙇 🤷 🤦 💁 🤲 ✍️ 👀 👂 🧠
💖 💗 💓 💞 💘 💝 🤍 🤎 💥 💫 💦 💨 💤 🎊 🎈 🎁 🏆 🥇 ☑️ ✔️ ➕ ➖ ➡️ ⬅️ ⬆️ ⬇️ 🔴 🟠 🟡 🟢 🔵 🟣
#️⃣ *️⃣ 0️⃣ 1️⃣ 2️⃣ 3️⃣ 4️⃣ 5️⃣ 6️⃣ 7️⃣ 8️⃣ 9️⃣ 🔟
📞 ☎️ 📲 💻 🖥️ ⌨️ 🖨️ 🔭 🔬 💡 🔧 🔨 🛠️ ⚙️ 🔩 🧰 🧲 🔒 🔓 🔑 📦 📧 ✉️ 📝 📖 📅 ⏰ ⌚ ⏳ 🔔 🔕 🔊 🔇 📢 🎧 🎵 📷 🔍
🔋 🚧 🛑 ⛔ 🚫 ℹ️ 📌 🧱 🪜
🌤️ ⛅ ☁️ 🌩️ 🌪️ 🌫️ 💧 ☔ ⚡ 🌙 ⭐ 🌟 🌞 🌡️ 🌋 🏔️ 🌴 🌵 🌳 🌻 🌹 🍀 🍁 🍂 🌍 🌎 🌏
🚓 🚑 🚒 🚐 🚚 🚜 🏍️ 🚌 ✈️ 🚁 🚀 ⛵ 🚤 🚂 🛶 ⛺ 🏡 🏢 🏥 🏫 ⛪ 🌉 🏖️ ⛽ 🅿️ 🚏
🍔 🍟 🌭 🥪 🌮 🍎 🍌 🍉 🍇 🍓 🥕 🌽 🍞 🧀 🥚 🍳 🥓 🍖 🍗 🍩 🍪 🎂 🍫 🍦 🍷 🍻 🥂 🍵 🥤 💧
🐱 🐭 🐹 🐰 🦊 🐻 🐼 🐨 🐯 🦁 🐮 🐷 🐸 🐵 🐔 🐧 🐦 🦆 🦅 🦉 🐝 🦋 🐢 🐍 🐙 🐬 🐳 🐟 🦈 🐴
⚽ 🏀 🏈 ⚾ 🎾 🏐 🎱 🏓 ⛳ 🎣 🏂 ⛷️ 🚴 🧗 🏃 🏊 🎮 🎲 🎯 🧩
🇬🇧 🇩🇪 🇫🇷 🇪🇸 🇮🇹 🇲🇽 🇧🇷 🇦🇺 🇳🇿 🇳🇱 🇸🇪 🇳🇴 🇨🇭 🇮🇪 🇮🇳 🇰🇷 🇨🇳 🇺🇦 🇵🇱 🇿🇦 🏳️‍🌈 🏴‍☠️ 🏁 🚩
👩‍💻 👨‍💻 ❤️‍🔥 👩‍❤️‍👨 👨‍👩‍👧‍👦 👩‍👩‍👧 👨‍👨‍👦 🧑‍🤝‍🧑 🧑‍🚀 👩‍🔧 👨‍🔧 🧑‍🌾 🐻‍❄️ 🐕‍🦺
👍🏻 👍🏼 👍🏾 👍🏿 👋🏻 👋🏼 👋🏽 👋🏾 🙏🏻 🙏🏼 🙏🏽 🙏🏾 🙏🏿 👏🏻 👏🏼 👏🏽 👏🏾 👏🏿 💪🏻 💪🏼 💪🏽 💪🏾 💪🏿
©️ ®️ ™️ ‼️ ⁉️ ➰ ♻️ ☮️ ☯️ ♾️ ⚛️ ☢️ ☣️ ⚜️ 🔱 🆘 🆗 🆒 🆕 🆓
"""

TIERS = {"core": CORE, "common": COMMON}


def tokens(tier_names):
    """Sequences of the named tiers in order, as tuples of code points
    without U+FE0F, first occurrence wins."""
    seen, out = set(), []
    for name in tier_names:
        for tok in TIERS[name].split():
            key = tuple(ord(c) for c in tok if ord(c) != 0xFE0F)
            if key not in seen:
                seen.add(key)
                out.append(key)
    return out
