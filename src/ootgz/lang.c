/* Languages (lang.h). There's no directory listing among our imports, so the files are found by
 * trying the codes below; each file names its language ("language"). A loaded file is kept whole in
 * memory: its strings are decoded in place and the translations looked up by the MurmurHash3 of the
 * English. The reader takes every "key": "string" pair in the file, at any depth, so the layout is
 * forgiving: "language" names it and every other pair is a translation. */
#include "lang.h"

#include "gamedata.h"
#include "sdfs.h"
#include "sys.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);

#define LANG_DIR SD_DIR "/lang"
#define OLD_LANG_DIR SD_OLD_DIR "/lang" /* before the rename */
#define MAX_FILE (256 * 1024)
#define TABLE_SIZE 4096 /* open addressing, a power of two */
#define TABLE_MASK (TABLE_SIZE - 1)
#define LANG_NAME_LEN 24 /* room for a language's name, as shown in the menu */

static const char *const codes[] = { "es", "fr", "de", "it", "pt", "nl", "pl", "sv", "da", "no", "fi", "cs",
                                     "hu", "tr", "ro", "ca", "id", "custom" };
#define N_CODES ((int)(sizeof(codes) / sizeof(*codes)))

/* The languages found, as the menu lists them: 0 is English, the others have a file. */
static const char *names[N_CODES + 1] = { "english" };
static char name_storage[N_CODES + 1][LANG_NAME_LEN];
static int code_index[N_CODES + 1]; /* names[i], i > 0: codes[code_index[i]] */
static int n_languages = 1;
static int current;

static char *loaded_file; /* the current language's file, its strings decoded in place */
static struct { uint32_t hash; const char *value; } table[TABLE_SIZE];
static int n_strings;

static bool same_string(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* --- reading ------------------------------------------------------------------------------------ */

/* The language file for `code` (the new folder, else the old one), null-terminated and allocated;
 * 0 if missing, empty or too big. */
static char *read_file(const char *code)
{
  char path[64];
  snprintf(path, sizeof(path), LANG_DIR "/%s.json", code);
  fs_file_t file;
  if (fs_open(&file, path, FS_READ) != 0) {
    snprintf(path, sizeof(path), OLD_LANG_DIR "/%s.json", code);
    if (fs_open(&file, path, FS_READ) != 0)
      return 0;
  }
  int64_t size = 0;
  fs_size(&size, file);
  char *buf = 0;
  if (size > 0 && size < MAX_FILE)
    buf = aligned_alloc(16, (size_t)size + 1);
  if (buf) {
    size_t got = 0;
    fs_read(&got, file, 0, buf, (size_t)size);
    buf[got] = 0;
  }
  fs_close(file);
  return buf;
}

/* U+00C0..U+00FF without its accent (the font has ASCII only) */
static const char latin1[65] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

/* A code point as one ASCII character: itself, an unaccented Latin-1 letter, or '?'. */
static char fold(unsigned code_point)
{
  if (code_point < 0x80)
    return (char)code_point;
  if (code_point >= 0xC0 && code_point <= 0xFF)
    return latin1[code_point - 0xC0];
  return '?';
}

static unsigned hex_digit(char c)
{
  if (c >= '0' && c <= '9')
    return (unsigned)(c - '0');
  c |= 0x20;
  return c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 0;
}

/* p just after an opening quote: decodes the string where it is (it only gets shorter), returns the
 * position after the closing quote, or 0 if there isn't one */
static char *decode(char *p)
{
  char *out = p;
  while (*p && *p != '"') {
    unsigned char c = (unsigned char)*p++;
    if (c == '\\') {
      char escape = *p ? *p++ : 0;
      if (escape == 'u') { /* \uXXXX */
        unsigned code_point = 0;
        for (int i = 0; i < 4 && *p; i++, p++)
          code_point = code_point * 16 + hex_digit(*p);
        *out++ = fold(code_point);
      } else {
        *out++ = escape == 'n' || escape == 't' ? ' ' : escape;
      }
    } else if (c < 0x80) {
      *out++ = (char)c;
    } else { /* UTF-8: a lead byte, then up to three continuation bytes */
      int continuation_bytes = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
      unsigned code_point = c & (0x3Fu >> continuation_bytes);
      for (int i = 0; i < continuation_bytes && ((unsigned char)*p & 0xC0) == 0x80; i++)
        code_point = code_point << 6 | ((unsigned char)*p++ & 0x3F);
      *out++ = fold(code_point);
    }
  }
  if (*p != '"')
    return 0;
  *out = 0;
  return p + 1;
}

/* Calls pair(key, value) for each "key": "value" in the text (decoding it in place). */
static void each_pair(char *p, void (*pair)(const char *key, const char *value, void *ctx), void *ctx)
{
  const char *key = 0;
  bool after_colon = false;
  while (*p) {
    char c = *p++;
    if (c == '"') {
      char *string = p;
      char *end = decode(p);
      if (!end)
        return;
      p = end;
      if (key && after_colon) {
        pair(key, string, ctx);
        key = 0;
      } else {
        key = string;
      }
      after_colon = false;
    } else if (c == ':') {
      after_colon = key != 0;
    } else if (c == ',' || c == '{' || c == '}' || c == '[' || c == ']') {
      key = 0;
      after_colon = false;
    }
  }
}

/* --- the table ---------------------------------------------------------------------------------- */

/* each_pair callback. With ctx (a LANG_NAME_LEN buffer) it only takes the "language" name;
 * without, it adds every translation to the table. */
static void add(const char *key, const char *value, void *ctx)
{
  if (same_string(key, "language")) {
    if (ctx)
      snprintf((char *)ctx, LANG_NAME_LEN, "%s", value);
    return;
  }
  if (!value[0] || ctx) /* empty: stays English */
    return;
  uint32_t hash = murmur3(key);
  unsigned slot = hash & TABLE_MASK;
  for (unsigned probes = 0; probes < TABLE_SIZE; probes++) {
    if (!table[slot].value || table[slot].hash == hash) {
      n_strings += !table[slot].value;
      table[slot].hash = hash;
      table[slot].value = value;
      return;
    }
    slot = (slot + 1) & TABLE_MASK;
  }
}

const char *T(const char *english)
{
  if (!loaded_file || !english || !english[0])
    return english;
  uint32_t hash = murmur3(english);
  unsigned slot = hash & TABLE_MASK;
  for (unsigned probes = 0; probes < TABLE_SIZE && table[slot].value; probes++) {
    if (table[slot].hash == hash)
      return table[slot].value;
    slot = (slot + 1) & TABLE_MASK;
  }
  return english;
}

/* the conversions of a format, in order ("%-12s %d" -> "sd") */
static void conversions(const char *format, char *out, int len)
{
  int n = 0;
  for (; *format && n < len - 1; format++) {
    if (*format != '%')
      continue;
    format++;
    /* skip flags, width, precision and length modifiers to the conversion letter */
    while (*format && !((*format >= 'a' && *format <= 'z' && *format != 'l' && *format != 'h' && *format != 'z') ||
                        *format == '%' || *format == 'X')) {
      if (*format == '*' && n < len - 2) /* takes an argument too */
        out[n++] = '*';
      format++;
    }
    if (!*format)
      break;
    if (*format != '%')
      out[n++] = *format;
  }
  out[n] = 0;
}

const char *TF(const char *english)
{
  const char *translated = T(english);
  if (translated == english)
    return translated;
  char english_conversions[16], translated_conversions[16];
  conversions(english, english_conversions, sizeof(english_conversions));
  conversions(translated, translated_conversions, sizeof(translated_conversions));
  return same_string(english_conversions, translated_conversions) ? translated : english;
}

/* --- languages ---------------------------------------------------------------------------------- */

void lang_init(void)
{
  n_languages = 1;
  for (int c = 0; c < N_CODES; c++) {
    char *file = read_file(codes[c]);
    if (!file)
      continue;
    /* the name defaults to the code if the file doesn't give one */
    snprintf(name_storage[n_languages], sizeof(name_storage[n_languages]), "%s", codes[c]);
    each_pair(file, add, name_storage[n_languages]); /* only the name */
    free(file);
    names[n_languages] = name_storage[n_languages];
    code_index[n_languages++] = c;
  }
  log_printf("lang: %d language file%s found in " LANG_DIR, n_languages - 1, n_languages == 2 ? "" : "s");
}

int lang_count(void) { return n_languages; }
const char *const *lang_names(void) { return names; }
int lang_current(void) { return current; }
const char *lang_code(void) { return current ? codes[code_index[current]] : "en"; }

bool lang_select(int i)
{
  if (i < 0 || i >= n_languages)
    return false;
  char *old_file = loaded_file;
  loaded_file = 0;
  memset(table, 0, sizeof(table));
  n_strings = 0;
  free(old_file);
  current = 0;
  if (i == 0)
    return true;
  char *file = read_file(codes[code_index[i]]);
  if (!file) {
    log_printf("lang: couldn't read %s.json", codes[code_index[i]]);
    return false;
  }
  each_pair(file, add, 0);
  loaded_file = file;
  current = i;
  log_printf("lang: %s, %d strings", names[i], n_strings);
  return true;
}

bool lang_select_code(const char *code)
{
  for (int i = 1; i < n_languages; i++)
    if (same_string(codes[code_index[i]], code))
      return lang_select(i);
  return lang_select(0);
}
