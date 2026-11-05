// The port's own text in one table (#189, ADR 0108): every sentence, label and hint the port's
// overlays show (the settings menu, the preset picker, the controls card, the disconnect pause,
// the save lists, the remap screen, the key pad editor, the price line), in English, which a text
// pack's Port.strings translates (maplethief/textpack.h, docs/formats/text-pack.md).
//
//   port_text.def   the table: MT_TEXT(id, "English"), the key in Port.strings is the id
//   tr(Txt::id)     the text: the pack's, else the English
//   tr(Txt::id, {a, b})  a text with places {0}, {1}, ... filled in
//   tr_key(key, en) a text of a table row that already holds its English (the settings
//                   registry's labels and help, "setting.<key>.label" / ".help"; the menu's
//                   action rows, "action.<key>.label" / ".help" / ".button" / ".confirm")
//
// The overlays draw with the port's 5x7 font and SDL's debug font, which have ASCII only: a
// pack's text is folded to ASCII when it is looked up (ascii_fold: "Pötion" -> "Potion", a
// character with no ASCII form -> '?'). A translation whose places ({0}, ...) are not the
// English's is not used (logged once). Logs stay English. Without a pack every text is the
// English literal itself, so the overlays draw exactly what they drew before the table.
#ifndef MAPLETHIEF_PLATFORM_PORT_TEXT_H
#define MAPLETHIEF_PLATFORM_PORT_TEXT_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace maplethief::frontend {

enum class Txt : std::uint16_t {
#define MT_TEXT(id, english) id,
#include "port_text.def"
#undef MT_TEXT
    kCount
};

// The text of `id`: the open text pack's Port.strings value (folded to ASCII) or the English.
// Valid until reload_port_text().
const char *tr(Txt id);
// The text of `id` with each "{N}" replaced by args[N] (the English's places only).
std::string tr(Txt id, std::initializer_list<std::string_view> args);
// The text of `key` (a registry row's), else `english`. Valid until reload_port_text().
const char *tr_key(const std::string &key, const char *english);

// The table: the key (the id's name) and the English of `id`.
const char *port_text_key(Txt id);
const char *port_text_english(Txt id);

// Forgets the looked-up texts: after a text pack opens or closes.
void reload_port_text();

// UTF-8 -> ASCII for the port's fonts: accented Latin letters lose their accents ("é" -> "e",
// "ß" -> "ss", "Æ" -> "AE"), typographic quotes and dashes become ' " -, "…" becomes "...", a
// no-break space a space; anything else (and any byte that is not UTF-8) becomes '?'.
std::string ascii_fold(std::string_view utf8);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PORT_TEXT_H
