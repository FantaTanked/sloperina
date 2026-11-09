/* Languages: the menu's English text looked up in a translation file on the SD card.
 *
 *   sd:/sloperinaGZ/lang/<code>.json    { "language": "Español", "strings": { "<english>": "<translation>", ... } }
 *
 * (sd:/ootgz/lang/, from before the rename, is read too.)
 * tools/lang_template.py writes lang/template.json with every English string. Strings a file leaves
 * out (or leaves "") stay English. The font is ASCII only (gz's Press Start 2P): accented Latin
 * letters are shown without their accents, and other scripts as '?'. */
#ifndef LANG_H
#define LANG_H

#include <stdbool.h>

const char *T(const char *english); /* the translation, or the English */
/* for printf formats: the translation only if it has the same conversions in the same order */
const char *TF(const char *english_format);

void lang_init(void);               /* after the SD card is mounted: finds the language files */
int  lang_count(void);              /* 1 (English) + the files found */
const char *const *lang_names(void);
int  lang_current(void);
bool lang_select(int i);            /* 0 = English; loads the file */
const char *lang_code(void);        /* for settings.ini: "en", or the file's code */
bool lang_select_code(const char *code);

#endif
