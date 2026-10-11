#ifndef PVIM_CONFIG_H
#define PVIM_CONFIG_H

/*
 * pvim.conf discovery and lookup.
 *
 * The config file is the first existing one of:
 *   - $PVIM_CONFIG
 *   - ~/.config/pvim/pvim.conf      (or $XDG_CONFIG_HOME/pvim/pvim.conf)
 *   - ./pvim.conf
 *   - <executable dir>/pvim.conf and <executable dir>/../pvim.conf
 */

/* First existing pvim.conf, or NULL when none is found. Cached; the result
 * stays valid until pvim_config_free(). */
const char *pvim_config_path(void);

/* Value of the first `key = value` occurrence with surrounding whitespace and
 * any trailing `# comment` removed. Returns a strdup'd string the caller must
 * free, or NULL when the key is absent. */
char *pvim_config_get(const char *key);

/* Return the trimmed, non-empty, non-comment lines inside the `[section]` block
 * of the config file: everything after the section header until the next `[`
 * header. Each returned string is malloc'd and must be freed by the caller.
 * Returns the number of lines (up to `max`), or -1 when the section does not
 * exist. Used for the vim-style `[bindings]` map section. */
int pvim_config_section(const char *section, char **lines, int max);

/* User configuration base directory (~/.config/pvim or the $XDG_CONFIG_HOME
 * variant), malloc'd, or NULL. */
char *pvim_user_config_dir(void);

/* Running executable's directory and its parent ("" when undeterminable). */
const char *pvim_exe_dir(void);
const char *pvim_exe_parent_dir(void);

/* Free resources owned by the config module. Idempotent; also called on exit
 * via atexit(). */
void pvim_config_free(void);

#endif /* PVIM_CONFIG_H */