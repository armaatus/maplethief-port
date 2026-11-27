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
// The texts are UTF-8. The overlays' Canvas draws accented Latin and Hangul from the bundled
// fonts (#372, ADR 0206); the lines drawn with SDL's debug font (the price line), which has ASCII
// only, fold their text with ascii_fold ("Pötion" -> "Potion", a character with no ASCII form ->
// '?'). A translation whose places ({0}, ...) are not the English's is not used (logged once). Logs
// stay English. Without a pack every text is the English literal itself, so the overlays draw
// exactly what they drew before the table.
//
// The menu language (#373, ADR 0222): the port ships its own translations, platform/lang/<tag>/
// (a text pack of our own text only: textpack.strings and Port.strings), embedded at build time.
// set_menu_language picks one; a text is then the open pack's, else the menu language's, else
// the English. The menu language's texts live until the process ends, so a text drawn before a
// change of language stays valid. menu_language = auto follows the system's preferred
// languages (system_menu_language), so the first start, before settings.ini exists, is in the
// player's language from its first screen.
#ifndef MAPLETHIEF_PLATFORM_PORT_TEXT_H
#define MAPLETHIEF_PLATFORM_PORT_TEXT_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace maplethief::frontend {

enum class MenuLanguage; // settings.h

enum class Txt : std::uint16_t {
#define MT_TEXT(id, english) id,
#include "port_text.def"
#undef MT_TEXT
    kCount
};

// The text of `id`: the open text pack's Port.strings value or the English.
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

// A shipped translation's files (generated from platform/lang/<tag>/ by platform/CMakeLists.txt).
struct ShippedLanguageFiles {
    const char *tag;           // "nl", "ko": the folder's name, a BCP 47 language tag
    std::string_view manifest; // textpack.strings
    std::string_view port;     // Port.strings
};
std::vector<ShippedLanguageFiles> shipped_port_language_files();

// A shipped translation, parsed and checked against the table once (on first use).
struct ShippedLanguage {
    std::string tag;
    std::string name;        // the manifest's name, as the language calls itself
    std::string description; // the manifest's: says "Draft" until a speaker has reviewed it
    std::string error;       // why the files were refused ("" when they were not)
    std::size_t entries = 0;
    // Its texts by Port.strings key, those whose places ({0}, ...) are the English's.
    std::vector<std::pair<std::string, std::string>> texts;
    // Its keys whose places differ from the English's, and keys the port has no text for.
    std::vector<std::string> wrong_places;
    std::vector<std::string> unknown;
};
const std::vector<ShippedLanguage> &shipped_port_languages();

// Every Port.strings key with its English (the table, the registry's labels and help, the
// menu actions' fields), in the export's order.
const std::vector<std::pair<std::string, std::string>> &port_text_rows();

// The menu languages: "en" first, then the shipped tags in order.
std::vector<std::string> menu_languages();
// Sets the menu language: "en" or a shipped tag (one whose files were refused is English).
// False, and nothing changes, for any other tag. Logs a change.
bool set_menu_language(std::string_view tag);
// The menu language now ("en" until set).
const std::string &menu_language();
// The first of the system's preferred languages (SDL_GetPreferredLocales, which the hint
// SDL_PREFERRED_LOCALES overrides) that is a menu language, by its language subtag ("nl_BE"
// is "nl"); "en" if none is.
std::string system_menu_language();
// The menu_language setting applied: its language, auto as system_menu_language. Logs the
// choice. Returns the tag set.
std::string apply_menu_language(MenuLanguage setting);
// A menu language's name as it calls itself ("English", "Nederlands"; the manifest's name),
// the tag itself for one the port does not ship; "auto" as tr(Txt::menu_language_auto) with the
// system's.
std::string menu_language_name(std::string_view tag);

// UTF-8 -> ASCII for SDL's debug font: accented Latin letters lose their accents ("é" -> "e",
// "ß" -> "ss", "Æ" -> "AE"), typographic quotes and dashes become ' " -, "…" becomes "...", a
// no-break space a space; anything else (and any byte that is not UTF-8) becomes '?'.
std::string ascii_fold(std::string_view utf8);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PORT_TEXT_H
