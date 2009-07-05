/* Yash: yet another shell */
/* editing.c: main editing module */
/* (C) 2007-2009 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "../common.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#if HAVE_GETTEXT
# include <libintl.h>
#endif
#if YASH_ENABLE_ALIAS
# include "../alias.h"
#endif
#include "../exec.h"
#include "../expand.h"
#include "../history.h"
#include "../job.h"
#include "../option.h"
#include "../path.h"
#include "../plist.h"
#include "../redir.h"
#include "../strbuf.h"
#include "../util.h"
#include "../wfnmatch.h"
#include "../yash.h"
#include "display.h"
#include "editing.h"
#include "keymap.h"
#include "lineedit.h"
#include "terminfo.h"


/* The main buffer where the command line is edited. */
xwcsbuf_T le_main_buffer;
/* The position of the cursor on the command line. */
/* 0 <= le_main_index <= le_main_buffer.length */
size_t le_main_index;

/* The history entry that is being edited now.
 * When we're editing no history entry, `main_history_entry' is a pointer to
 * `histlist'. */
static const histentry_T *main_history_entry;
/* The original value of `main_history_entry', converted into a wide string. */
static wchar_t *main_history_value;

/* The direction of currently-performed command history search. */
enum le_search_direction le_search_direction;
/* Supplementary buffer used in command history search. */
/* When search is not being performed, the `contents' member is NULL. */
xwcsbuf_T le_search_buffer;
/* The search result for the current value of `le_search_buffer'.
 * If there is no match, `le_search_result' is `Histlist'. */
const histentry_T *le_search_result;
/* The search string and the direction of the last search. */
static struct {
    enum le_search_direction direction;
    wchar_t *value;
} last_search;

/* The last executed command and the currently executing command. */
static struct command {
    le_command_func_T *func;
    wchar_t arg;
} last_command, current_command;

/* The keymap state. */
static struct state {
    struct {
	/* When count is not specified, `sign' and `abs' is 0.
	 * Otherwise, `sign' is 1 or -1.
	 * When the negative sign is specified but digits are not, `abs' is 0.*/
	int sign;
	unsigned abs;
	int multiplier;
#define COUNT_ABS_MAX 999999999
    } count;
    enum motion_expect_command {
	MEC_NONE, MEC_COPY, MEC_KILL, MEC_CHANGE, MEC_COPYCHANGE,
    } pending_command_motion;
    le_command_func_T *pending_command_char;
} state;

/* The last executed editing command and the then state. */
/* `last_edit_command' is valid iff `.command.func' is non-null. */
static struct {
    struct command command;
    struct state state;
} last_edit_command;

/* The last executed find/till command and the then state. */
/* `last_find_command' is valid iff `.func' is non-null. */
static struct command last_find_command;

/* If true, characters are overwritten rather than inserted. */
static bool overwrite = false;

/* History of the edit line between editing commands. */
static plist_T undo_history;
/* Index of the current state in the history.
 * If the current state is the newest, the index is `undo_history.length'. */
static size_t undo_index;
/* The history entry that is saved in the undo history. */
static const histentry_T *undo_history_entry;
/* The index that is to be the value of the `index' member of the next undo
 * history entry. */
static size_t undo_save_index;
/* Structure of history entries */
struct undo_history {
    size_t index;        /* index of the cursor */
    wchar_t contents[];  /* contents of the edit line */
};

#define KILL_RING_SIZE 32  /* must be power of 2 */
/* The kill ring */
static wchar_t *kill_ring[KILL_RING_SIZE];
/* The index of the element to which next killed string is assigned. */
static size_t next_kill_index = 0;
/* The index of the element which was put last. */
static size_t last_put_elem = 0;  /* < KILL_RING_SIZE */
/* The position and length of the last-put string. */
static size_t last_put_range_start, last_put_range_length;


static void reset_state(void);
static int get_count(int default_value);
static void save_current_edit_command(void);
static void save_current_find_command(void);
static void save_undo_history(void);
static void maybe_save_undo_history(void);
static void exec_motion_command(size_t index, bool inclusive);
static void add_to_kill_ring(const wchar_t *s, size_t n)
    __attribute__((nonnull));

static bool alert_if_first(void);
static bool alert_if_last(void);
static void move_cursor_forward_char(int offset);
static void move_cursor_backward_char(int offset);
static void move_cursor_forward_bigword(int count);
static void move_cursor_backward_bigword(int count);
static size_t next_bigword_index(const wchar_t *s, size_t i)
    __attribute__((nonnull));
static size_t next_end_of_bigword_index(
	const wchar_t *s, size_t i, bool progress)
    __attribute__((nonnull));
static size_t previous_bigword_index(const wchar_t *s, size_t i)
    __attribute__((nonnull));
static void move_cursor_forward_viword(int count);
static bool need_cw_treatment(void)
    __attribute__((pure));
static void move_cursor_backward_viword(int count);
static size_t next_viword_index(const wchar_t *s, size_t i)
    __attribute__((nonnull));
static size_t next_end_of_viword_index(
	const wchar_t *s, size_t i, bool progress)
    __attribute__((nonnull));
static size_t previous_viword_index(const wchar_t *s, size_t i)
    __attribute__((nonnull));
static void move_cursor_forward_nonword(int count);
static void move_cursor_backward_word(int count);
static size_t next_nonword_index(const wchar_t *s, size_t i)
    __attribute__((nonnull));
static size_t previous_word_index(const wchar_t *s, size_t i)
    __attribute__((nonnull));

static void delete_semiword_backward(bool kill);
static inline bool is_blank_or_punct(wchar_t c)
    __attribute__((pure));
static void put_killed_string(bool after_cursor, bool cursor_on_last_char);
static void insert_killed_string(
	bool after_cursor, bool cursor_on_last_char, size_t index, bool clear);
static void cancel_undo(int offset);

static void vi_find(wchar_t c);
static void vi_find_rev(wchar_t c);
static void vi_till(wchar_t c);
static void vi_till_rev(wchar_t c);
static void exec_find(wchar_t c, int count, bool till);
static size_t find_nth_occurence(wchar_t c, int n);
static void vi_replace_char(wchar_t c);
static void exec_edit_command(enum motion_expect_command cmd);
static void exec_edit_command_to_eol(enum motion_expect_command cmd);
static void vi_exec_alias(wchar_t c);
struct xwcsrange { const wchar_t *start, *end; };
static struct xwcsrange get_next_bigword(const wchar_t *s)
    __attribute__((nonnull));
static struct xwcsrange get_prev_bigword(
	const wchar_t *beginning, const wchar_t *s)
    __attribute__((nonnull));

static void go_to_history_absolute(const histentry_T *e, bool cursorend)
    __attribute__((nonnull));
static void go_to_history_relative(int offset, bool cursorend);
static void go_to_history(const histentry_T *e, bool cursorend)
    __attribute__((nonnull));

static bool update_last_search_value(void)
    __attribute__((pure));
static void update_search(void);
static void perform_search(const wchar_t *pattern, enum le_search_direction dir)
    __attribute__((nonnull));
static void search_again(enum le_search_direction dir);

#define ALERT_AND_RETURN_IF_PENDING                     \
    do if (state.pending_command_motion != MEC_NONE)    \
	{ cmd_alert(L'\0'); return; }                   \
    while (0)


/* Initializes the editing module before starting editing. */
void le_editing_init(void)
{
    wb_init(&le_main_buffer);
    le_main_index = 0;
    main_history_entry = Histlist;
    main_history_value = xwcsdup(L"");

    switch (shopt_lineedit) {
	case shopt_vi:
	    le_set_mode(LE_MODE_VI_INSERT);
	    break;
	case shopt_emacs:
	    le_set_mode(LE_MODE_EMACS);
	    break;
	default:
	    assert(false);
    }

    last_command.func = 0;
    last_command.arg = L'\0';

    start_using_history();
    pl_init(&undo_history);
    undo_index = 0;
    undo_save_index = le_main_index;
    undo_history_entry = Histlist;
    save_undo_history();

    reset_state();
    overwrite = false;
}

/* Finalizes the editing module when editing is finished.
 * Returns the content of the main buffer, which must be freed by the caller. */
wchar_t *le_editing_finalize(void)
{
    assert(le_search_buffer.contents == NULL);

    recfree(pl_toary(&undo_history), free);

    end_using_history();
    free(main_history_value);

    wb_wccat(&le_main_buffer, L'\n');
    return wb_towcs(&le_main_buffer);
}

/* Invokes the specified command. */
void le_invoke_command(le_command_func_T *cmd, wchar_t arg)
{
    current_command.func = cmd;
    current_command.arg = arg;

    cmd(arg);

    last_command = current_command;

    if (le_current_mode == &le_modes[LE_MODE_VI_COMMAND]) {
	if (le_main_index > 0 && le_main_index == le_main_buffer.length) {
	    le_main_index--;
	}
    }
    le_display_reposition_cursor();
}

/* Resets `state.count'. */
void reset_state(void)
{
    state.count.sign = 0;
    state.count.abs = 0;
    state.count.multiplier = 1;
    state.pending_command_motion = MEC_NONE;
    state.pending_command_char = 0;
}

/* Returns the count value.
 * If the count is not set, returns the `default_value'. */
int get_count(int default_value)
{
    long long result;

    if (state.count.sign == 0)
	result = (long long) default_value * state.count.multiplier;
    else if (state.count.sign < 0 && state.count.abs == 0)
	result = (long long) -state.count.multiplier;
    else
	result = (long long) state.count.abs * state.count.sign *
	    state.count.multiplier;

    if (result < -COUNT_ABS_MAX)
	result = -COUNT_ABS_MAX;
    else if (result > COUNT_ABS_MAX)
	result = COUNT_ABS_MAX;
    return result;
}

/* Saves the currently executing command and the current state in
 * `last_edit_command' if we are not redoing and the mode is not "vi insert". */
void save_current_edit_command(void)
{
    if (current_command.func != cmd_redo
	    && le_current_mode != &le_modes[LE_MODE_VI_INSERT]) {
	last_edit_command.command = current_command;
	last_edit_command.state = state;
    }
}

/* Saves the currently executing command and the current state in
 * `last_find_command' if we are not redoing/refinding. */
void save_current_find_command(void)
{
    if (current_command.func != cmd_vi_refind
	    && current_command.func != cmd_vi_refind_rev
	    && current_command.func != cmd_redo)
	last_find_command = current_command;
}

/* Saves the current contents of the edit line to the undo history.
 * History entries at the current `undo_index' and newer are removed before
 * saving the current. If `undo_history_entry' is different from
 * `main_history_entry', all undo history entries are removed. */
void save_undo_history(void)
{
    for (size_t i = undo_index; i < undo_history.length; i++)
	free(undo_history.contents[i]);
    pl_remove(&undo_history, undo_index, SIZE_MAX);

    struct undo_history *e = xmalloc(sizeof *e +
	    (le_main_buffer.length + 1) * sizeof *e->contents);
    e->index = le_main_index;
    wcscpy(e->contents, le_main_buffer.contents);
    pl_add(&undo_history, e);
    assert(undo_index == undo_history.length - 1);
    undo_history_entry = main_history_entry;
}

/* Calls `save_undo_history' if the current contents of the edit line is not
 * saved. */
void maybe_save_undo_history(void)
{
    assert(undo_index <= undo_history.length);
    size_t save_undo_save_index = undo_save_index;
    undo_save_index = le_main_index;
    if (undo_history_entry == main_history_entry) {
	if (undo_index < undo_history.length) {
	    struct undo_history *h = undo_history.contents[undo_index];
	    if (wcscmp(le_main_buffer.contents, h->contents) == 0) {
		h->index = le_main_index;
		return;
	    }
	    undo_index++;
	}
    } else {
	if (wcscmp(le_main_buffer.contents, main_history_value) == 0)
	    return;

	/* The contents of the buffer has been changed from the value of the
	 * history entry, but it's not yet saved in the undo history. We first
	 * save the original history value and then save the current buffer
	 * contents. */
	struct undo_history *h;
	pl_clear(&undo_history, free);
	h = xmalloc(sizeof *h +
		(wcslen(main_history_value) + 1) * sizeof *h->contents);
	assert(save_undo_save_index <= wcslen(main_history_value));
	h->index = save_undo_save_index;
	wcscpy(h->contents, main_history_value);
	pl_add(&undo_history, h);
	undo_index = 1;
    }
    save_undo_history();
}

/* Applies the current pending editing command to the range between the current
 * cursor index and the given `index'. If no editing command is pending, simply
 * moves the cursor to the `index'. */
/* This function is used for all cursor-moving commands, even when not in the
 * vi mode. */
void exec_motion_command(size_t index, bool inclusive)
{
    assert(index <= le_main_buffer.length);

    maybe_save_undo_history();

    size_t start_index, end_index;
    if (le_main_index <= index)
	start_index = le_main_index, end_index = index;
    else
	start_index = index, end_index = le_main_index;
    if (inclusive && end_index < le_main_buffer.length)
	end_index++;
    switch (state.pending_command_motion) {
	case MEC_NONE:
	    le_main_index = index;
	    break;
	case MEC_COPY:
	    add_to_kill_ring(le_main_buffer.contents + start_index,
		    end_index - start_index);
	    break;
	case MEC_KILL:
	    save_current_edit_command();
	    add_to_kill_ring(le_main_buffer.contents + start_index,
		    end_index - start_index);
	    wb_remove(&le_main_buffer, start_index, end_index - start_index);
	    le_main_index = start_index;
	    le_display_reprint_buffer(start_index, false);
	    break;
	case MEC_COPYCHANGE:
	    add_to_kill_ring(le_main_buffer.contents + start_index,
		    end_index - start_index);
	    /* falls thru */
	case MEC_CHANGE:
	    save_current_edit_command();
	    wb_remove(&le_main_buffer, start_index, end_index - start_index);
	    le_main_index = start_index;
	    le_display_reprint_buffer(start_index, false);
	    le_set_mode(LE_MODE_VI_INSERT);
	    overwrite = false;
	    break;
	default:
	    assert(false);
    }
    reset_state();
}

/* Adds the specified string to the kill ring.
 * The maximum number of characters that are added is specified by `n'. */
void add_to_kill_ring(const wchar_t *s, size_t n)
{
    if (n > 0 && s[0] != L'\0') {
	free(kill_ring[next_kill_index]);
	kill_ring[next_kill_index] = xwcsndup(s, n);
	next_kill_index = (next_kill_index + 1) % KILL_RING_SIZE;
    }
}


/********** Basic Commands **********/

/* Does nothing. */
void cmd_noop(wchar_t c __attribute__((unused)))
{
    reset_state();
}

/* Same as `cmd_noop', but causes alert. */
void cmd_alert(wchar_t c __attribute__((unused)))
{
    le_alert();
    reset_state();
}

/* Inserts one character in the buffer.
 * If the count is set, inserts `count' times.
 * If `overwrite' is true, overwrites the character instead of inserting. */
void cmd_self_insert(wchar_t c)
{
    ALERT_AND_RETURN_IF_PENDING;

    if (c != L'\0') {
	int count = get_count(1);
	size_t old_index = le_main_index;

	while (--count >= 0)
	    if (overwrite && le_main_index < le_main_buffer.length)
		le_main_buffer.contents[le_main_index++] = c;
	    else
		wb_ninsert_force(&le_main_buffer, le_main_index++, &c, 1);
	le_display_reprint_buffer(old_index,
		!overwrite && le_main_index == le_main_buffer.length);
	reset_state();
    } else {
	cmd_alert(L'\0');
    }
}

/* Sets the `le_next_verbatim' flag.
 * The next character will be input to the main buffer even if it's a special
 * character. */
void cmd_expect_verbatim(wchar_t c __attribute__((unused)))
{
    le_next_verbatim = true;
}

/* Inserts the tab character. */
void cmd_insert_tab(wchar_t c __attribute__((unused)))
{
    cmd_self_insert(L'\t');
}

/* Adds the specified digit `c' to the accumulating argument. */
/* If `c' is not a digit, does nothing. */
void cmd_digit_argument(wchar_t c)
{
    if (L'0' <= c && c <= L'9') {
	if (state.count.abs > COUNT_ABS_MAX / 10) {
	    cmd_alert(L'\0');  // argument too large
	    return;
	}
	if (state.count.sign == 0)
	    state.count.sign = 1;
	state.count.abs = state.count.abs * 10 + (unsigned) (c - L'0');
    } else if (c == L'-') {
	if (state.count.sign == 0)
	    state.count.sign = -1;
	else
	    state.count.sign = -state.count.sign;
    }
}

/* If the count is not set, moves the cursor to the beginning of line.
 * Otherwise, adds the given digit to the count. */
void cmd_bol_or_digit(wchar_t c)
{
    if (state.count.sign == 0)
	cmd_beginning_of_line(c);
    else
	cmd_digit_argument(c);
}

/* Accepts the current line.
 * `le_state' is set to LE_STATE_DONE and `le_readline' returns. */
void cmd_accept_line(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    cmd_srch_accept_search(L'\0');
    le_editstate = LE_EDITSTATE_DONE;
    reset_state();
}

/* Aborts the current line.
 * `le_state' is set to LE_STATE_INTERRUPTED and `le_readline' returns. */
void cmd_abort_line(wchar_t c __attribute__((unused)))
{
    cmd_srch_abort_search(L'\0');
    le_editstate = LE_EDITSTATE_INTERRUPTED;
    reset_state();
}

/* If the edit line is empty, sets `le_state' to LE_STATE_ERROR (return EOF).
 * Otherwise, causes alert. */
void cmd_eof_if_empty(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    if (le_main_buffer.length == 0) {
	le_editstate = LE_EDITSTATE_ERROR;
	reset_state();
    } else {
	cmd_alert(L'\0');
    }
}

/* If the edit line is empty, sets `le_state' to LE_STATE_ERROR (return EOF).
 * Otherwise, deletes the character under the cursor. */
void cmd_eof_or_delete(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    if (le_main_buffer.length == 0) {
	le_editstate = LE_EDITSTATE_ERROR;
	reset_state();
    } else {
	cmd_delete_char(L'\0');
    }
}

/* Inserts a hash sign ('#') at the beginning of the line and accepts the line.
 */
void cmd_accept_with_hash(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    wb_insert(&le_main_buffer, 0, L"#");
    le_display_reprint_buffer(0, false);
    cmd_accept_line(L'\0');
}

/* Changes the editing mode to "vi insert". */
void cmd_setmode_viinsert(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    maybe_save_undo_history();

    le_set_mode(LE_MODE_VI_INSERT);
    reset_state();
    overwrite = false;
}

/* Changes the editing mode to "vi command". */
void cmd_setmode_vicommand(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    maybe_save_undo_history();

    if (le_current_mode == &le_modes[LE_MODE_VI_INSERT])
	if (le_main_index > 0)
	    le_main_index--;
    le_set_mode(LE_MODE_VI_COMMAND);
    reset_state();
    overwrite = false;
}

/* Executes a command that expects a character as an argument. */
void cmd_expect_char(wchar_t c)
{
    if (state.pending_command_char) {
	current_command.func = state.pending_command_char;
	current_command.arg = c;
	state.pending_command_char(c);
    }
}

/* Cancels a command that expects a character as an argument. */
void cmd_abort_expect_char(wchar_t c __attribute__((unused)))
{
    reset_state();
    le_set_mode(LE_MODE_VI_COMMAND);
}

/* Redraw everything. */
void cmd_redraw_all(wchar_t c __attribute__((unused)))
{
    le_display_clear();
    le_restore_terminal();
    le_setupterm(false);
    le_set_terminal();
    le_display_print_all(false);
}


/********** Motion Commands **********/

/* Invokes `cmd_alert' and returns true if the cursor is at the first character.
 */
bool alert_if_first(void)
{
    if (le_main_index > 0)
	return false;

    cmd_alert(L'\0');
    return true;
}

/* Invokes `cmd_alert' and returns true if the cursor is at the last character.
 */
bool alert_if_last(void)
{
    if (le_current_mode == &le_modes[LE_MODE_VI_COMMAND]) {
	if (state.pending_command_motion != MEC_NONE)
	    return false;
	if (le_main_buffer.length > 0
		&& le_main_index < le_main_buffer.length - 1)
	    return false;
    } else {
	if (le_main_index < le_main_buffer.length)
	    return false;
    }

    cmd_alert(L'\0');
    return true;
}

/* Moves forward one character (or `count' characters if the count is set). */
/* exclusive motion command */
void cmd_forward_char(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_forward_char(count);
    else
	move_cursor_backward_char(-count);
}

/* Moves backward one character.
 * If the count is set, moves backward `count' characters. */
/* exclusive motion command */
void cmd_backward_char(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_backward_char(count);
    else
	move_cursor_forward_char(-count);
}

/* Moves the cursor forward by `offset', relative to the current position.
 * The `offset' must not be negative. */
void move_cursor_forward_char(int offset)
{
    assert(offset >= 0);
    if (alert_if_last())
	return;

#if COUNT_ABS_MAX > SIZE_MAX
    if (offset > SIZE_MAX)
	offset = SIZE_MAX;
#endif

    size_t new_index;
    if (le_main_buffer.length - le_main_index < (size_t) offset)
	new_index = le_main_buffer.length;
    else
	new_index = le_main_index + offset;
    exec_motion_command(new_index, false);
}

/* Moves the cursor backward by `offset', relative to the current position.
 * The `offset' must not be negative. */
void move_cursor_backward_char(int offset)
{
    assert(offset >= 0);
    if (alert_if_first())
	return;

    size_t new_index;
#if COUNT_ABS_MAX > SIZE_MAX
    if ((int) le_main_index <= offset)
#else
    if (le_main_index <= (size_t) offset)
#endif
	new_index = 0;
    else
	new_index = le_main_index - offset;
    exec_motion_command(new_index, false);
}

/* Moves forward one bigword (or `count' bigwords if the count is set). */
/* exclusive motion command */
void cmd_forward_bigword(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_forward_bigword(count);
    else
	move_cursor_backward_bigword(-count);
}

/* Moves the cursor to the end of the current bigword (or the next bigword if
 * already at the end). If the count is set, moves to the end of `count'th
 * bigword. */
/* inclusive motion command */
void cmd_end_of_bigword(wchar_t c __attribute__((unused)))
{
    if (alert_if_last())
	return;

    int count = get_count(1);
    size_t new_index = le_main_index;
    while (--count >= 0 && new_index < le_main_buffer.length)
	new_index = next_end_of_bigword_index(
		le_main_buffer.contents, new_index, true);
    exec_motion_command(new_index, true);
}

/* Moves backward one bigword (or `count' bigwords if the count is set). */
/* exclusive motion command */
void cmd_backward_bigword(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_backward_bigword(count);
    else
	move_cursor_forward_bigword(-count);
}

/* Moves the cursor forward `count' bigwords, relative to the current position.
 * If `count' is negative, the cursor is not moved. */
void move_cursor_forward_bigword(int count)
{
    if (alert_if_last())
	return;

    size_t new_index = le_main_index;
    if (!need_cw_treatment()) {
	while (count-- > 0 && new_index < le_main_buffer.length)
	    new_index = next_bigword_index(le_main_buffer.contents, new_index);
	exec_motion_command(new_index, false);
    } else {
	while (count > 1 && new_index < le_main_buffer.length) {
	    new_index = next_bigword_index(le_main_buffer.contents, new_index);
	    count--;
	}
	if (count > 0 && new_index < le_main_buffer.length) {
	    new_index = next_end_of_bigword_index(
		    le_main_buffer.contents, new_index, false);
	}
	exec_motion_command(new_index, true);
    }
}

/* Moves the cursor backward `count' bigwords, relative to the current position.
 * If `count' is negative, the cursor is not moved. */
void move_cursor_backward_bigword(int count)
{
    if (alert_if_first())
	return;

    size_t new_index = le_main_index;
    while (count-- > 0 && new_index > 0)
	new_index = previous_bigword_index(le_main_buffer.contents, new_index);
    exec_motion_command(new_index, false);
}

/* Returns the index of the next bigword in the string `s', counted from the
 * index `i'. The return value is greater than `i' unless `s[i]' is a null
 * character. */
/* A bigword is a sequence of non-blank characters. */
size_t next_bigword_index(const wchar_t *s, size_t i)
{
    while (s[i] != L'\0' && !iswblank(s[i]))
	i++;
    while (s[i] != L'\0' && iswblank(s[i]))
	i++;
    return i;
}

/* Returns the index of the end of a bigword in the string `s', counted from
 * index `i'. If `i' is at the end of a bigword, the end of the next bigword is
 * returned. The return value is greater than `i' unless `s[i]' is a null
 * character. */
size_t next_end_of_bigword_index(const wchar_t *s, size_t i, bool progress)
{
    const size_t init = i;
start:
    if (s[i] == L'\0')
	return i;
    while (s[i] != L'\0' && iswblank(s[i]))
	i++;
    while (s[i] != L'\0' && !iswblank(s[i]))
	i++;
    i--;
    if (i > init || !progress) {
	return i;
    } else {
	i++;
	goto start;
    }
}

/* Returns the index of the previous bigword in the string `s', counted from the
 * index `i'. The return value is less than `i' unless `i' is zero. */
size_t previous_bigword_index(const wchar_t *s, size_t i)
{
    const size_t init = i;
start:
    while (i > 0 && iswblank(s[i]))
	i--;
    while (i > 0 && !iswblank(s[i]))
	i--;
    if (i == 0)
	return i;
    i++;
    if (i < init) {
	return i;
    } else {
	i--;
	goto start;
    }
}

/* Moves forward one viword (or `count' viwords if the count is set). */
/* exclusive motion command */
void cmd_forward_viword(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_forward_viword(count);
    else
	move_cursor_backward_viword(-count);
}

/* Moves the cursor to the end of the current viword (or the next viword if
 * already at the end). If the count is set, moves to the end of the `count'th
 * viword. */
/* inclusive motion command */
void cmd_end_of_viword(wchar_t c __attribute__((unused)))
{
    if (alert_if_last())
	return;

    int count = get_count(1);
    size_t new_index = le_main_index;
    while (--count >= 0 && new_index < le_main_buffer.length)
	new_index = next_end_of_viword_index(
		le_main_buffer.contents, new_index, true);
    exec_motion_command(new_index, true);
}

/* Moves backward one viword (or `count' viwords if the count is set). */
/* exclusive motion command */
void cmd_backward_viword(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_backward_viword(count);
    else
	move_cursor_forward_viword(-count);
}

/* Moves the cursor forward `count' viwords, relative to the current position.
 * If `count' is negative, the cursor is not moved. */
void move_cursor_forward_viword(int count)
{
    if (alert_if_last())
	return;

    size_t new_index = le_main_index;
    if (!need_cw_treatment()) {
	while (count-- > 0 && new_index < le_main_buffer.length)
	    new_index = next_viword_index(le_main_buffer.contents, new_index);
	exec_motion_command(new_index, false);
    } else {
	while (count > 1 && new_index < le_main_buffer.length) {
	    new_index = next_viword_index(le_main_buffer.contents, new_index);
	    count--;
	}
	if (count > 0 && new_index < le_main_buffer.length) {
	    new_index = next_end_of_viword_index(
		    le_main_buffer.contents, new_index, false);
	}
	exec_motion_command(new_index, true);
    }
}

/* Checks if we need a special treatment for the "cw" and "cW" commands. */
bool need_cw_treatment(void)
{
    switch (state.pending_command_motion) {
	case MEC_CHANGE:
	case MEC_COPYCHANGE:
	    return !iswblank(le_main_buffer.contents[le_main_index]);
	default:
	    return false;
    }
}

/* Moves the cursor backward `count' viwords, relative to the current position.
 * If `count' is negative, the cursor is not moved. */
void move_cursor_backward_viword(int count)
{
    if (alert_if_first())
	return;

    size_t new_index = le_main_index;
    while (count-- > 0 && new_index > 0)
	new_index = previous_viword_index(le_main_buffer.contents, new_index);
    exec_motion_command(new_index, false);
}

/* Returns the index of the next viword in the string `s', counted from the
 * index `i'. The return value is greater than `i' unless `s[i]' is a null
 * character. */
/* A viword is a sequence of alphanumeric characters and underscores, or a
 * sequence of other non-blank characters. */
size_t next_viword_index(const wchar_t *s, size_t i)
{
    if (s[i] == L'_' || iswalnum(s[i])) {
	do
	    i++;
	while (s[i] == L'_' || iswalnum(s[i]));
	if (!iswblank(s[i]))
	    return i;
    } else if (!iswblank(s[i])) {
	for (;;) {
	    if (s[i] == L'\0')
		return i;
	    i++;
	    if (s[i] == L'_' || iswalnum(s[i]))
		return i;
	    if (iswblank(s[i]))
		break;
	}
    }
    /* find the first non-blank character */
    while (iswblank(s[++i]));
    return i;
}

/* Returns the index of the end of a viword in the string `s', counted from
 * index `i'.
 * If `progress' is true:
 *   If `i' is at the end of a viword, the end of the next viword is returned.
 *   The return value is greater than `i' unless `s[i]' is a null character.
 * If `progress' is false:
 *   If `i' is at the end of a viword, `i' is returned. */
size_t next_end_of_viword_index(const wchar_t *s, size_t i, bool progress)
{
    const size_t init = i;
start:
    while (iswblank(s[i]))
	i++;
    if (s[i] == L'\0')
	return i;
    if (s[i] == L'_' || iswalnum(s[i])) {
	do
	    i++;
	while (s[i] == L'_' || iswalnum(s[i]));
    } else {
	do
	    i++;
	while (s[i] != L'\0' && s[i] != L'_'
		&& !iswblank(s[i]) && !iswalnum(s[i]));
    }
    i--;
    if (i > init || !progress) {
	return i;
    } else {
	i++;
	goto start;
    }
}

/* Returns the index of the previous viword in the string `s', counted form the
 * index `i'. The return value is less than `i' unless `i' is zero. */
size_t previous_viword_index(const wchar_t *s, size_t i)
{
    const size_t init = i;
start:
    while (i > 0 && iswblank(s[i]))
	i--;
    if (s[i] == L'_' || iswalnum(s[i])) {
	do {
	    if (i == 0)
		return 0;
	    i--;
	} while (s[i] == L'_' || iswalnum(s[i]));
    } else {
	do {
	    if (i == 0)
		return 0;
	    i--;
	} while (s[i] != L'_' && !iswblank(s[i]) && !iswalnum(s[i]));
    }
    i++;
    if (i < init) {
	return i;
    } else {
	i--;
	goto start;
    }
}

/* Moves to the next nonword (or the `count'th nonword if the count is set). */
/* exclusive motion command */
void cmd_forward_nonword(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_forward_nonword(count);
    else
	move_cursor_backward_word(-count);
}

/* Moves backward one word (or `count' words if the count is set). */
/* exclusive motion command */
void cmd_backward_word(wchar_t c __attribute__((unused)))
{
    int count = get_count(1);
    if (count >= 0)
	move_cursor_backward_word(count);
    else
	move_cursor_forward_nonword(-count);
}

/* Moves the cursor to the `count'th nonword, relative to the current position.
 * If `count' is negative, the cursor is not moved. */
void move_cursor_forward_nonword(int count)
{
    size_t new_index = le_main_index;
    while (count-- > 0 && new_index < le_main_buffer.length)
	new_index = next_nonword_index(le_main_buffer.contents, new_index);
    exec_motion_command(new_index, false);
}

/* Moves the cursor backward `count'th word, relative to the current position.
 * If `count' is negative, the cursor is not moved. */
void move_cursor_backward_word(int count)
{
    size_t new_index = le_main_index;
    while (count-- > 0 && new_index > 0)
	new_index = previous_word_index(le_main_buffer.contents, new_index);
    exec_motion_command(new_index, false);
}

/* Returns the index of the next nonword in the string `s', counted from the
 * index `i'. The return value is greater than `i' unless `s[i]' is a null
 * character. */
/* A nonword is a sequence of non-alphanumeric characters. */
size_t next_nonword_index(const wchar_t *s, size_t i)
{
    while (s[i] != L'\0' && !iswalnum(s[i]))
	i++;
    while (s[i] != L'\0' && iswalnum(s[i]))
	i++;
    return i;
}

/* Returns the index of the previous word in the string `s', counted from the
 * index `i'. The return value is less than `i' unless `i' is zero. */
/* A word is a sequence of alphanumeric characters. */
size_t previous_word_index(const wchar_t *s, size_t i)
{
    const size_t init = i;
start:
    while (i > 0 && !iswalnum(s[i]))
	i--;
    while (i > 0 && iswalnum(s[i]))
	i--;
    if (i == 0)
	return i;
    i++;
    if (i < init) {
	return i;
    } else {
	i--;
	goto start;
    }
}

/* Moves the cursor to the beginning of line. */
/* exclusive motion command */
void cmd_beginning_of_line(wchar_t c __attribute__((unused)))
{
    exec_motion_command(0, false);
}

/* Moves the cursor to the end of line. */
/* inclusive motion command */
void cmd_end_of_line(wchar_t c __attribute__((unused)))
{
    exec_motion_command(le_main_buffer.length, true);
}

/* Moves the cursor to the first non-blank character. */
/* exclusive motion command */
void cmd_first_nonblank(wchar_t c __attribute__((unused)))
{
    size_t i = 0;

    while (c = le_main_buffer.contents[i], c != L'\0' && iswblank(c))
	i++;
    exec_motion_command(i, false);
}


/********** Editing Commands **********/

/* Removes the character under the cursor.
 * If the count is set, `count' characters are killed. */
void cmd_delete_char(wchar_t c __attribute__((unused)))
{
    if (state.count.sign == 0) {
	ALERT_AND_RETURN_IF_PENDING;
	save_current_edit_command();
	maybe_save_undo_history();

	if (le_main_index < le_main_buffer.length) {
	    wb_remove(&le_main_buffer, le_main_index, 1);
	    le_display_reprint_buffer(le_main_index, false);
	} else {
	    le_alert();
	}
	reset_state();
    } else {
	cmd_kill_char(L'\0');
    }
}

/* Removes the character behind the cursor.
 * If the count is set, `count' characters are killed. */
void cmd_backward_delete_char(wchar_t c __attribute__((unused)))
{
    if (state.count.sign == 0) {
	ALERT_AND_RETURN_IF_PENDING;
	save_current_edit_command();
	maybe_save_undo_history();

	if (le_main_index > 0) {
	    wb_remove(&le_main_buffer, --le_main_index, 1);
	    le_display_reprint_buffer(le_main_index, false);
	} else {
	    le_alert();
	}
	reset_state();
    } else {
	cmd_backward_kill_char(L'\0');
    }
}

/* Removes the semiword behind the cursor.
 * If the count is set, `count' semiwords are removed.
 * If the cursor is at the beginning of the line, the terminal is alerted. */
void cmd_backward_delete_semiword(wchar_t c __attribute__((unused)))
{
    delete_semiword_backward(false);
}

/* Removes the semiword behind the cursor.
 * If the count is set, `count' semiwords are removed.
 * If `kill' is true, the removed semiword is added to the kill ring.
 * If the cursor is at the beginning of the line, the terminal is alerted. */
/* A "semiword" is a sequence of characters that are not <blank> or <punct>. */
void delete_semiword_backward(bool kill)
{
    ALERT_AND_RETURN_IF_PENDING;
    save_current_edit_command();
    maybe_save_undo_history();

    size_t bound = le_main_index;
    if (le_main_index == 0) {
	cmd_alert(L'\0');
	return;
    }

    for (int count = get_count(1); --count >= 0; ) {
	do {
	    if (bound == 0)
		goto done;
	} while (is_blank_or_punct(le_main_buffer.contents[--bound]));
	do {
	    if (bound == 0)
		goto done;
	} while (!is_blank_or_punct(le_main_buffer.contents[--bound]));
    }
    bound++;
done:
    if (bound < le_main_index) {
	size_t length = le_main_buffer.length - bound;
	if (kill)
	    add_to_kill_ring(le_main_buffer.contents + bound, length);
	wb_remove(&le_main_buffer, bound, length);
	le_main_index = bound;
	le_display_reprint_buffer(le_main_index, false);
    }
    reset_state();
}

bool is_blank_or_punct(wchar_t c)
{
    return iswblank(c) || iswpunct(c);
}

/* Removes all characters in the edit line. */
void cmd_delete_line(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    save_current_edit_command();
    maybe_save_undo_history();

    wb_clear(&le_main_buffer);
    le_main_index = 0;
    le_display_reprint_buffer(0, false);
    reset_state();
}

/* Removes all characters after the cursor. */
void cmd_forward_delete_line(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    save_current_edit_command();
    maybe_save_undo_history();

    if (le_main_index < le_main_buffer.length) {
	wb_remove(&le_main_buffer, le_main_index, SIZE_MAX);
	le_display_reprint_buffer(le_main_index, false);
    }
    reset_state();
}

/* Removes all characters behind the cursor. */
void cmd_backward_delete_line(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    save_current_edit_command();
    maybe_save_undo_history();

    if (le_main_index > 0) {
	wb_remove(&le_main_buffer, 0, le_main_index);
	le_main_index = 0;
	le_display_reprint_buffer(0, false);
    }
    reset_state();
}

/* Kills the character under the cursor.
 * If the count is set, `count' characters are killed. */
void cmd_kill_char(wchar_t c __attribute__((unused)))
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    state.pending_command_motion = MEC_KILL;
    cmd_forward_char(L'\0');
}

/* Kills the character behind the cursor.
 * If the count is set, `count' characters are killed.
 * If the cursor is at the beginning of the line, the terminal is alerted. */
void cmd_backward_kill_char(wchar_t c __attribute__((unused)))
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    state.pending_command_motion = MEC_KILL;
    cmd_backward_char(L'\0');
}

/* Kills the semiword behind the cursor.
 * If the count is set, `count' semiwords are killed.
 * If the cursor is at the beginning of the line, the terminal is alerted. */
void cmd_backward_kill_semiword(wchar_t c __attribute__((unused)))
{
    delete_semiword_backward(true);
}

/* Kills the semiword behind the cursor.
 * If the count is set, `count' semiwords are killed.
 * If the cursor is at the beginning of the line, the terminal is alerted. */
void cmd_backward_kill_bigword(wchar_t c __attribute__((unused)))
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    state.pending_command_motion = MEC_KILL;
    cmd_backward_bigword(L'\0');
}

/* Kills all characters before the cursor. */
void cmd_backward_kill_line(wchar_t c __attribute__((unused)))
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    state.pending_command_motion = MEC_KILL;
    exec_motion_command(0, false);
}

/* Inserts the last-killed string before the cursor.
 * If the count is set, inserts `count' times.
 * The cursor is left on the last character inserted. */
void cmd_put_before(wchar_t c __attribute__((unused)))
{
    put_killed_string(false, true);
}

/* Inserts the last-killed string after the cursor.
 * If the count is set, inserts `count' times.
 * The cursor is left on the last character inserted. */
void cmd_put(wchar_t c __attribute__((unused)))
{
    put_killed_string(true, true);
}

/* Inserts the last-killed string before the cursor.
 * If the count is set, inserts `count' times.
 * The cursor is left after the inserted string. */
void cmd_put_left(wchar_t c __attribute__((unused)))
{
    put_killed_string(false, false);
}

/* Inserts the last-killed text at the current cursor position (`count' times).
 * If `after_cursor' is true, the text is inserted after the current cursor
 * position. Otherwise, before the current position.
 * If `cursor_on_last_char' is true, the cursor is left on the last character
 * inserted. Otherwise, the cursor is left after the inserted text. */
void put_killed_string(bool after_cursor, bool cursor_on_last_char)
{
    ALERT_AND_RETURN_IF_PENDING;
    save_current_edit_command();
    maybe_save_undo_history();

    size_t index = (next_kill_index - 1) % KILL_RING_SIZE;
    if (kill_ring[index] == NULL) {
	cmd_alert(L'\0');
	return;
    }

    insert_killed_string(after_cursor, cursor_on_last_char, index, false);
}

/* Inserts the killed text at the current cursor position (`count' times).
 * If `after_cursor' is true, the text is inserted after the current cursor
 * position. Otherwise, before the current position.
 * If `cursor_on_last_char' is true, the cursor is left on the last character
 * inserted. Otherwise, the cursor is left after the inserted text.
 * The `index' specifies the text in the kill ring to be inserted. If a text
 * does not exist at the specified index in the kill ring, this function does
 * nothing.
 * If `clear' is true, the buffer is always reprinted. Otherwise, the buffer is
 * reprinted only when necessary. */
void insert_killed_string(
	bool after_cursor, bool cursor_on_last_char, size_t index, bool clear)
{
    const wchar_t *s = kill_ring[index];
    if (s == NULL)
	return;

    last_put_elem = index;
    if (after_cursor && le_main_index < le_main_buffer.length)
	le_main_index++;

    size_t offset = le_main_buffer.length - le_main_index;
    size_t old_index = le_main_index;
    for (int count = get_count(1); --count >= 0; )
	wb_insert(&le_main_buffer, le_main_index, s);
    assert(le_main_buffer.length >= offset + 1);

    last_put_range_start = le_main_index;
    le_main_index = le_main_buffer.length - offset;
    last_put_range_length = le_main_index - last_put_range_start;
    if (cursor_on_last_char)
	le_main_index--;

    le_display_reprint_buffer(old_index, !clear && offset == 0);
    reset_state();
}

/* Replaces the string just inserted by `cmd_put_left' with the previously
 * killed string. */
void cmd_put_pop(wchar_t c __attribute__((unused)))
{
    static bool last_success = false;

    ALERT_AND_RETURN_IF_PENDING;
    if ((last_command.func != cmd_put_left
	    && last_command.func != cmd_put
	    && last_command.func != cmd_put_before
	    && (last_command.func != cmd_put_pop || !last_success))
	    || kill_ring[last_put_elem] == NULL) {
	last_success = false;
	cmd_alert(L'\0');
	return;
    }
    last_success = true;
    save_current_edit_command();
    maybe_save_undo_history();

    size_t index = last_put_elem;
    do
	index = (index - 1) % KILL_RING_SIZE;
    while (kill_ring[index] == NULL);

    /* Remove the just inserted text. */
    assert(last_put_range_start <= le_main_buffer.length);
    wb_remove(&le_main_buffer, last_put_range_start, last_put_range_length);
    le_main_index = last_put_range_start;

    insert_killed_string(false, false, index, true);
}

/* Undoes the last editing command. */
void cmd_undo(wchar_t c __attribute__((unused)))
{
    cancel_undo(-get_count(1));
}

/* Undoes all changes to the edit line. */
void cmd_undo_all(wchar_t c __attribute__((unused)))
{
    cancel_undo(-COUNT_ABS_MAX);
}

/* Cancels the last undo. */
void cmd_cancel_undo(wchar_t c __attribute__((unused)))
{
    cancel_undo(get_count(1));
}

/* Cancels all previous undo. */
void cmd_cancel_undo_all(wchar_t c __attribute__((unused)))
{
    cancel_undo(COUNT_ABS_MAX);
}

/* Performs "undo"/"cancel undo".
 * `undo_index' is increased by `offset' and the contents of the history entry
 * of the new index is set to the edit line.
 * `offset' must be between `-COUNT_ABS_MAX' and `COUNT_ABS_MAX'. */
void cancel_undo(int offset)
{
    maybe_save_undo_history();

    if (offset < 0) {
	if (undo_index == 0)
	    goto error;
#if COUNT_ABS_MAX > SIZE_MAX
	if (-offset > (int) undo_index)
#else
	if ((size_t) -offset > undo_index)
#endif
	    undo_index = 0;
	else
	    undo_index += offset;
    } else {
	if (undo_index + 1 >= undo_history.length)
	    goto error;
#if COUNT_ABS_MAX > SIZE_MAX
	if (offset >= (int) (undo_history.length - undo_index))
#else
	if ((size_t) offset >= undo_history.length - undo_index)
#endif
	    undo_index = undo_history.length - 1;
	else
	    undo_index += offset;
    }

    const struct undo_history *entry = undo_history.contents[undo_index];
    wb_replace(&le_main_buffer, 0, SIZE_MAX, entry->contents, SIZE_MAX);
    assert(entry->index <= le_main_buffer.length);
    le_main_index = entry->index;

    le_display_reprint_buffer(0, false);
    reset_state();
    return;

error:
    cmd_alert(L'\0');
    return;
}

/* Redoes the last editing command. */
/* XXX: currently vi's "i" command cannot be redone. */
void cmd_redo(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    if (!last_edit_command.command.func) {
	cmd_alert(L'\0');
	return;
    }

    if (state.count.sign != 0)
	last_edit_command.state.count = state.count;
    state = last_edit_command.state;
    last_edit_command.command.func(last_edit_command.command.arg);
}


/********** Vi-Mode Specific Commands **********/

/* Moves the cursor to the `count'th character in the edit line.
 * If the count is not set, moves to the beginning of the line. */
/* exclusive motion command */
void cmd_vi_column(wchar_t c __attribute__((unused)))
{
    int index = get_count(1) - 1;

    if (index < 0)
	index = 0;
#if COUNT_ABS_MAX > SIZE_MAX
    else if (index > (int) le_main_buffer.length)
#else
    else if ((size_t) index > le_main_buffer.length)
#endif
	index = le_main_buffer.length;
    exec_motion_command(index, false);
}

/* Sets the editing mode to "vi expect" and the pending command to `vi_find'. */
void cmd_vi_find(wchar_t c __attribute__((unused)))
{
    maybe_save_undo_history();

    le_set_mode(LE_MODE_VI_EXPECT);
    state.pending_command_char = vi_find;
}

/* Moves the cursor to the `count'th occurrence of `c' after the current
 * position. */
/* inclusive motion command */
void vi_find(wchar_t c)
{
    exec_find(c, get_count(1), false);
}

/* Sets the editing mode to "vi expect" and the pending command to
 * `vi_find_rev'. */
void cmd_vi_find_rev(wchar_t c __attribute__((unused)))
{
    maybe_save_undo_history();

    le_set_mode(LE_MODE_VI_EXPECT);
    state.pending_command_char = vi_find_rev;
}

/* Moves the cursor to the `count'th occurrence of `c' before the current
 * position. */
/* exclusive motion command */
void vi_find_rev(wchar_t c)
{
    exec_find(c, -get_count(1), false);
}

/* Sets the editing mode to "vi expect" and the pending command to `vi_till'. */
void cmd_vi_till(wchar_t c __attribute__((unused)))
{
    maybe_save_undo_history();

    le_set_mode(LE_MODE_VI_EXPECT);
    state.pending_command_char = vi_till;
}

/* Moves the cursor to the character just before `count'th occurrence of `c'
 * after the current position. */
/* inclusive motion command */
void vi_till(wchar_t c)
{
    exec_find(c, get_count(1), true);
}

/* Sets the editing mode to "vi expect" and the pending command to
 * `vi_till_rev'. */
void cmd_vi_till_rev(wchar_t c __attribute__((unused)))
{
    maybe_save_undo_history();

    le_set_mode(LE_MODE_VI_EXPECT);
    state.pending_command_char = vi_till_rev;
}

/* Moves the cursor to the character just after `count'th occurrence of `c'
 * before the current position. */
/* exclusive motion command */
void vi_till_rev(wchar_t c)
{
    exec_find(c, -get_count(1), true);
}

/* Executes the find/till command. */
void exec_find(wchar_t c, int count, bool till)
{
    le_set_mode(LE_MODE_VI_COMMAND);
    save_current_find_command();

    size_t new_index = find_nth_occurence(c, count);
    if (new_index == SIZE_MAX)
	goto error;
    if (till) {
	if (new_index >= le_main_index) {
	    if (new_index == 0)
		goto error;
	    new_index--;
	} else {
	    if (new_index == le_main_buffer.length)
		goto error;
	    new_index++;
	}
    }
    exec_motion_command(new_index, new_index >= le_main_index);
    return;

error:
    cmd_alert(L'\0');
    return;
}

/* Finds the position of the nth occurrence of `c' in the edit line from the
 * current position. Returns `SIZE_MAX' on failure (no such occurrence). */
size_t find_nth_occurence(wchar_t c, int n)
{
    size_t i = le_main_index;

    if (n == 0) {
	return i;
    } else if (c == L'\0') {
	return SIZE_MAX;  /* no such occurrence */
    } else if (n >= 0) {
	while (n > 0 && i < le_main_buffer.length) {
	    i++;
	    if (le_main_buffer.contents[i] == c)
		n--;
	}
    } else {
	while (n < 0 && i > 0) {
	    i--;
	    if (le_main_buffer.contents[i] == c)
		n++;
	}
    }
    if (n != 0)
	return SIZE_MAX;  /* no such occurrence */
    else
	return i;
}

/* Redoes the last find/till command. */
void cmd_vi_refind(wchar_t c __attribute__((unused)))
{
    if (!last_find_command.func) {
	cmd_alert(L'\0');
	return;
    }

    last_find_command.func(last_find_command.arg);
}

/* Redoes the last find/till command in the reverse direction. */
void cmd_vi_refind_rev(wchar_t c __attribute__((unused)))
{
    if (!last_find_command.func) {
	cmd_alert(L'\0');
	return;
    }

    if (state.count.sign == 0)
	state.count.sign = -1, state.count.abs = 1;
    else if (state.count.sign >= 0)
	state.count.sign = -1;
    else
	state.count.sign = 1;
    last_find_command.func(last_find_command.arg);
}

/* Sets the editing mode to "vi expect" and the pending command to
 * `vi_replace_char'. */
void cmd_vi_replace_char(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    maybe_save_undo_history();

    le_set_mode(LE_MODE_VI_EXPECT);
    state.pending_command_char = vi_replace_char;
}

/* Replaces the character under the cursor with `c'.
 * If the count is set, the `count' characters are replaced. */
void vi_replace_char(wchar_t c)
{
    save_current_edit_command();
    le_set_mode(LE_MODE_VI_COMMAND);

    if (c != L'\0') {
	int count = get_count(1);
	size_t old_index = le_main_index;

	if (--count >= 0 && le_main_index < le_main_buffer.length) {
	    le_main_buffer.contents[le_main_index] = c;
	    while (--count >= 0 && le_main_index < le_main_buffer.length)
		le_main_buffer.contents[++le_main_index] = c;
	}
	le_display_reprint_buffer(old_index, false);
	reset_state();
    } else {
	cmd_alert(L'\0');
    }
}

/* Moves the cursor to the beginning of line and sets the editing mode to
 * "vi insert". */
void cmd_vi_insert_beginning(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    le_main_index = 0;
    cmd_setmode_viinsert(L'\0');
}

/* Moves the cursor by one character and sets the editing mode to "vi insert".
 */
void cmd_vi_append(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    if (le_main_index < le_main_buffer.length)
	le_main_index++;
    cmd_setmode_viinsert(L'\0');
}

/* Moves the cursor to the end of line and sets the editing mode to
 * "vi insert".*/
void cmd_vi_append_end(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    le_main_index = le_main_buffer.length;
    cmd_setmode_viinsert(L'\0');
}

/* Sets the editing mode to "vi insert", with the `overwrite' flag true. */
void cmd_vi_replace(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    cmd_setmode_viinsert(L'\0');
    overwrite = true;
}

/* Changes the case of the character under the cursor and advances the cursor.
 * If the count is set, `count' characters are changed. */
void cmd_vi_change_case(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    save_current_edit_command();
    maybe_save_undo_history();

    size_t old_index = le_main_index;

    if (le_main_index == le_main_buffer.length) {
	cmd_alert(L'\0');
	return;
    }
    for (int count = get_count(1); --count >= 0; ) {
	c = le_main_buffer.contents[le_main_index];
	le_main_buffer.contents[le_main_index]
	    = (iswlower(c) ? towupper : towlower)(c);
	le_main_index++;
	if (le_main_index == le_main_buffer.length)
	    break;
    }
    le_display_reprint_buffer(old_index, false);
    reset_state();
}

/* Sets the pending command to `MEC_COPY'.
 * The count multiplier is set to the current count.
 * If the pending command is already set to `MEC_COPY', the whole line is copied
 * to the kill ring. */
void cmd_vi_yank(wchar_t c __attribute__((unused)))
{
    exec_edit_command(MEC_COPY);
}

/* Copies the content of the edit line from the current position to the end. */
void cmd_vi_yank_to_eol(wchar_t c __attribute__((unused)))
{
    exec_edit_command_to_eol(MEC_COPY);
}

/* Sets the pending command to `MEC_KILL'.
 * The count multiplier is set to the current count. 
 * If the pending command is already set to `MEC_KILL', the whole line is moved
 * to the kill ring. */
void cmd_vi_delete(wchar_t c __attribute__((unused)))
{
    exec_edit_command(MEC_KILL);
}

/* Deletes the content of the edit line from the current position to the end and
 * put it in the kill ring. */
void cmd_vi_delete_to_eol(wchar_t c __attribute__((unused)))
{
    exec_edit_command_to_eol(MEC_KILL);
}

/* Sets the pending command to `MEC_CHANGE'.
 * The count multiplier is set to the current count. 
 * If the pending command is already set to `MEC_CHANGE', the whole line is
 * deleted to the kill ring and the editing mode is set to "vi insert". */
void cmd_vi_change(wchar_t c __attribute__((unused)))
{
    exec_edit_command(MEC_CHANGE);
}

/* Deletes the content of the edit line from the current position to the end and
 * sets the editing mode to "vi insert". */
void cmd_vi_change_to_eol(wchar_t c __attribute__((unused)))
{
    exec_edit_command_to_eol(MEC_CHANGE);
}

/* Deletes all the content of the edit line and sets the editing mode to
 * "vi insert". */
void cmd_vi_change_all(wchar_t c __attribute__((unused)))
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    le_main_index = 0;
    exec_edit_command_to_eol(MEC_CHANGE);
}

/* Sets the pending command to `MEC_COPYCHANGE'.
 * The count multiplier is set to the current count. 
 * If the pending command is already set to `MEC_COPYCHANGE', the whole line is
 * moved to the kill ring and the editing mode is set to "vi insert". */
void cmd_vi_yank_and_change(wchar_t c __attribute__((unused)))
{
    exec_edit_command(MEC_COPYCHANGE);
}

/* Deletes the content of the edit line from the current position to the end,
 * put it in the kill ring, and sets the editing mode to "vi insert". */
void cmd_vi_yank_and_change_to_eol(wchar_t c __attribute__((unused)))
{
    exec_edit_command_to_eol(MEC_COPYCHANGE);
}

/* Deletes all the content of the edit line, put it in the kill ring, and sets
 * the editing mode to "vi insert". */
void cmd_vi_yank_and_change_all(wchar_t c __attribute__((unused)))
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    le_main_index = 0;
    exec_edit_command_to_eol(MEC_COPYCHANGE);
}

/* Executes the specified command. */
void exec_edit_command(enum motion_expect_command cmd)
{
    if (state.pending_command_motion != MEC_NONE) {
	if (state.pending_command_motion == cmd) {
	    size_t old_index = le_main_index;
	    le_main_index = 0;
	    exec_motion_command(le_main_buffer.length, true);
	    if (old_index <= le_main_buffer.length)
		le_main_index = old_index;
	} else {
	    cmd_alert(L'\0');
	}
    } else {
	state.count.multiplier = get_count(1);
	state.count.sign = 0;
	state.count.abs = 0;
	state.pending_command_motion = cmd;
    }
}

/* Executes the specified command on the range from the current cursor position
 * to the end of the line. */
void exec_edit_command_to_eol(enum motion_expect_command cmd)
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    state.pending_command_motion = cmd;
    exec_motion_command(le_main_buffer.length, false);
}

/* Kills the character under the cursor and sets the editing mode to
 * "vi insert". If the count is set, `count' characters are killed. */
void cmd_vi_substitute(wchar_t c __attribute__((unused)))
{
    if (current_command.func != cmd_redo)
	ALERT_AND_RETURN_IF_PENDING;
    state.pending_command_motion = MEC_COPYCHANGE;
    cmd_forward_char(L'\0');
}

/* Appends a space followed by the last bigword from the newest history entry.
 * If the count is specified, the `count'th word is appended.
 * The mode is changed to vi-insert. */
void cmd_vi_append_last_bigword(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    save_current_edit_command();
    maybe_save_undo_history();

    wchar_t *lastcmd = NULL;
    int count = get_count(-1);
    if (count == 0 || histlist.count == 0)
	goto fail;

    struct xwcsrange range;
    lastcmd = malloc_mbstowcs(histlist.Newest->value);
    if (lastcmd == NULL)
	goto fail;
    if (count >= 0) {
	/* find the count'th word */
	range.start = range.end = lastcmd;
	do {
	    struct xwcsrange r = get_next_bigword(range.end);
	    if (r.start == r.end)
		break;
	    range = r;
	} while (--count > 0 && *range.end != L'\0');
    } else {
	/* find the count'th last word */
	range.start = range.end = lastcmd + wcslen(lastcmd);
	do {
	    struct xwcsrange r = get_prev_bigword(lastcmd, range.start);
	    if (r.start == r.end)
		break;
	    range = r;
	} while (++count < 0 && lastcmd < range.start);
    }
    assert(range.start <= range.end);
    if (range.start == range.end)
	goto fail;

    if (le_main_index < le_main_buffer.length)
	le_main_index++;
    size_t oldindex = le_main_index;
    size_t len = range.end - range.start;
    wb_ninsert_force(&le_main_buffer, le_main_index, L" ", 1);
    le_main_index += 1;
    wb_ninsert_force(&le_main_buffer, le_main_index, range.start, len);
    le_main_index += len;
    free(lastcmd);
    le_display_reprint_buffer(oldindex, le_main_index == le_main_buffer.length);
    cmd_setmode_viinsert(L'\0');
    return;

fail:
    free(lastcmd);
    cmd_alert(L'\0');
    return;
}

struct xwcsrange get_next_bigword(const wchar_t *s)
{
    struct xwcsrange result;
    while (iswblank(*s))
	s++;
    result.start = s;
    while (*s != L'\0' && !iswblank(*s))
	s++;
    result.end = s;
    return result;
    /* result.start == result.end if no bigword found */
}

struct xwcsrange get_prev_bigword(const wchar_t *beginning, const wchar_t *s)
{
    struct xwcsrange result;
    assert(beginning <= s);
    do {
	if (beginning == s) {
	    result.start = result.end = s;
	    return result;
	}
    } while (iswblank(*--s));
    result.end = s + 1;
    do {
	if (beginning == s) {
	    result.start = s;
	    return result;
	}
    } while (!iswblank(*--s));
    result.start = s + 1;
    return result;
    /* result.start == result.end if no bigword found */
}

/* Sets the editing mode to "vi expect" and the pending command to
 * `vi_exec_alias'. */
void cmd_vi_exec_alias(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    le_set_mode(LE_MODE_VI_EXPECT);
    state.pending_command_char = vi_exec_alias;
}

/* Appends the value of the alias `_c' to the pre-buffer so that the alias value
 * is interpreted as commands, where `c' in the alias name is the argument of
 * this command. */
void vi_exec_alias(wchar_t c)
{
    le_set_mode(LE_MODE_VI_COMMAND);
    state.pending_command_char = 0;

#if YASH_ENABLE_ALIAS
    wchar_t aliasname[3] = { L'_', c, L'\0', };
    const wchar_t *aliasvalue = get_alias_value(aliasname);
    if (aliasvalue) {
	char *mbaliasvalue = malloc_wcstombs(aliasvalue);
	if (mbaliasvalue) {
	    append_to_prebuffer(mbaliasvalue);
	    return;
	}
    }
#endif
    cmd_alert(L'\0');
}

/* Invokes an external command to edit the current line and accepts the result.
 * If the editor returns a non-zero status, the line is not accepted. */
/* cf. history.c:fc_edit_and_exec_entries */
void cmd_vi_edit_and_accept(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    char *tempfile;
    int fd;
    FILE *f;
    pid_t cpid;
    int savelaststatus;

    fd = create_temporary_file(&tempfile, S_IRUSR | S_IWUSR);
    if (fd < 0) {
	cmd_alert(L'\0');
	return;
    }
    f = fdopen(fd, "w");
    if (f == NULL) {
	xclose(fd);
	cmd_alert(L'\0');
	return;
    }
    le_suspend_readline();

    savelaststatus = laststatus;
    cpid = fork_and_reset(0, true, 0);
    if (cpid < 0) { // fork failed
	fclose(f);
	unlink(tempfile);
	free(tempfile);
	le_resume_readline();
	cmd_alert(L'\0');
    } else if (cpid > 0) {  // parent process
	fclose(f);

	wchar_t **namep = wait_for_child(cpid,
		doing_job_control_now ? cpid : 0,
		doing_job_control_now);
	if (namep)
	    *namep = malloc_wprintf(L"vi %s", tempfile);
	if (laststatus != Exit_SUCCESS)
	    goto end;

	f = fopen(tempfile, "r");
	if (f == NULL) {
	    cmd_alert(L'\0');
	} else {
	    wint_t c;

	    wb_clear(&le_main_buffer);
	    while ((c = fgetwc(f)) != WEOF)
		wb_wccat(&le_main_buffer, (wchar_t) c);
	    fclose(f);

	    /* remove trailing newline */
	    while (le_main_buffer.length > 0 &&
		    le_main_buffer.contents[le_main_buffer.length - 1] == L'\n')
		wb_remove(&le_main_buffer, le_main_buffer.length - 1, 1);

	    le_main_index = le_main_buffer.length;

	    le_editstate = LE_EDITSTATE_DONE;
	    reset_state();
	    // XXX: clear info area
	}

end:
	unlink(tempfile);
	free(tempfile);
	le_resume_readline();
    } else {  // child process
	fwprintf(f, L"%ls\n", le_main_buffer.contents);
	fclose(f);

	wchar_t *command = malloc_wprintf(L"vi %s", tempfile);
	free(tempfile);
	exec_wcs(command, gt("lineedit"), true);
#ifndef NDEBUG
	free(command);
#endif
	assert(false);
    }
}

/* Starts vi-like command history search in the forward direction. */
void cmd_vi_search_forward(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    le_search_direction = FORWARD;
    wb_init(&le_search_buffer);
    le_set_mode(LE_MODE_VI_SEARCH);
    update_search();
}

/* Starts vi-like command history search in the backward direction. */
void cmd_vi_search_backward(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;

    le_search_direction = BACKWARD;
    wb_init(&le_search_buffer);
    le_set_mode(LE_MODE_VI_SEARCH);
    update_search();
}


/********** History-Related Commands **********/

/* Goes to the oldest history entry.
 * If the `count' is specified, goes to the history entry whose number is
 * `count'. If the specified entry is not found, make an alert.
 * The cursor is put at the beginning of line. */
void cmd_oldest_history(wchar_t c __attribute__((unused)))
{
    go_to_history_absolute(histlist.Oldest, false);
}

/* Goes to the newest history entry.
 * If the `count' is specified, goes to the history entry whose number is
 * `count'. If the specified entry is not found, make an alert.
 * The cursor is put at the beginning of line. */
void cmd_newest_history(wchar_t c __attribute__((unused)))
{
    go_to_history_absolute(histlist.Newest, false);
}

/* Goes to the newest history entry.
 * If the `count' is specified, goes to the history entry whose number is
 * `count'. If the specified entry is not found, make an alert.
 * The cursor is put at the beginning of line. */
void cmd_return_history_eol(wchar_t c __attribute__((unused)))
{
    go_to_history_absolute(Histlist, true);
}

/* Goes to the oldest history entry.
 * If the `count' is specified, goes to the history entry whose number is
 * `count'. If the specified entry is not found, make an alert.
 * The cursor is put at the end of line. */
void cmd_oldest_history_eol(wchar_t c __attribute__((unused)))
{
    go_to_history_absolute(histlist.Oldest, true);
}

/* Goes to the newest history entry.
 * If the `count' is specified, goes to the history entry whose number is
 * `count'. If the specified entry is not found, make an alert.
 * The cursor is put at the end of line. */
void cmd_newest_history_eol(wchar_t c __attribute__((unused)))
{
    go_to_history_absolute(histlist.Newest, true);
}

/* Goes to the newest history entry.
 * If the `count' is specified, goes to the history entry whose number is
 * `count'. If the specified entry is not found, make an alert.
 * The cursor is put at the end of line. */
void cmd_return_history(wchar_t c __attribute__((unused)))
{
    go_to_history_absolute(Histlist, false);
}

/* Goes to the specified history entry `e'.
 * If the `count' is specified, goes to the history entry whose number is
 * `count'. If the specified entry is not found, make an alert.
 * If `cursorend' is true, the cursor is put at the end of line; otherwise, at
 * the beginning of line. */
void go_to_history_absolute(const histentry_T *e, bool cursorend)
{
    ALERT_AND_RETURN_IF_PENDING;

    if (state.count.sign == 0) {
	if (histlist.count == 0)
	    goto alert;
    } else {
	int num = get_count(0);
	if (num <= 0)
	    goto alert;
	e = get_history_entry((unsigned) num);
	if (e == NULL)
	    goto alert;
    }
    go_to_history(e, cursorend);
    return;

alert:
    cmd_alert(L'\0');
    return;
}

/* Goes to the `count'th next history entry.
 * The cursor is put at the beginning of line. */
void cmd_next_history(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    go_to_history_relative(get_count(1), false);
}

/* Goes to the `count'th previous history entry.
 * The cursor is put at the beginning of line. */
void cmd_prev_history(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    go_to_history_relative(-get_count(1), false);
}

/* Goes to the `count'th next history entry.
 * The cursor is put at the end of line. */
void cmd_next_history_eol(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    go_to_history_relative(get_count(1), true);
}

/* Goes to the `count'th previous history entry.
 * The cursor is put at the end of line. */
void cmd_prev_history_eol(wchar_t c __attribute__((unused)))
{
    ALERT_AND_RETURN_IF_PENDING;
    go_to_history_relative(-get_count(1), true);
}

/* Goes to the `offset'th next history entry.
 * If `cursorend' is true, the cursor is put at the end of line; otherwise, at
 * the beginning of line. */
void go_to_history_relative(int offset, bool cursorend)
{
    const histentry_T *e = main_history_entry;
    if (offset > 0) {
	do {
	    if (e == Histlist)
		goto alert;
	    e = e->Next;
	} while (--offset > 0);
    } else if (offset < 0) {
	do {
	    e = e->Prev;
	    if (e == Histlist)
		goto alert;
	} while (++offset < 0);
    }
    go_to_history(e, cursorend);
    return;
alert:
    cmd_alert(L'\0');
    return;
}

/* Sets the value of the specified history entry to the main buffer.
 * If `cursorend' is true, the cursor is put at the end of line; otherwise, at
 * the beginning of line. */
void go_to_history(const histentry_T *e, bool cursorend)
{
    maybe_save_undo_history();

    free(main_history_value);
    wb_clear(&le_main_buffer);
    if (e == undo_history_entry && undo_index < undo_history.length) {
	struct undo_history *h = undo_history.contents[undo_index];
	wb_cat(&le_main_buffer, h->contents);
	assert(h->index <= le_main_buffer.length);
	le_main_index = h->index;
    } else if (e != Histlist) {
	wb_mbscat(&le_main_buffer, e->value);
	le_main_index = cursorend ? le_main_buffer.length : 0;
    } else {
	le_main_index = 0;
    }
    main_history_entry = e;
    main_history_value = xwcsdup(le_main_buffer.contents);
    undo_save_index = le_main_index;

    le_display_reprint_buffer(0, false);
    reset_state();
}

/***** History Search Commands *****/

/* Appends the argument character to the search buffer. */
void cmd_srch_self_insert(wchar_t c)
{
    if (le_search_buffer.contents == NULL || c == L'\0') {
	cmd_alert(L'\0');
	return;
    }

    wb_wccat(&le_search_buffer, c);
    update_search();
}

/* Removes the last character from the search buffer.
 * If there are no characters in the buffer, calls `cmd_srch_abort_search' (for
 * vi-like search) or `cmd_alert' (for emacs-like search). */
void cmd_srch_backward_delete_char(wchar_t c __attribute__((unused)))
{
    if (le_search_buffer.contents == NULL) {
	cmd_alert(L'\0');
	return;
    }

    // TODO currently, no emacs-like search
    if (le_search_buffer.length == 0) {
	cmd_srch_abort_search(L'\0');
	return;
    }

    wb_remove(&le_search_buffer, le_search_buffer.length - 1, 1);
    update_search();
}

/* Removes all characters from the search buffer. */
void cmd_srch_backward_delete_line(wchar_t c __attribute__((unused)))
{
    if (le_search_buffer.contents == NULL) {
	cmd_alert(L'\0');
	return;
    }

    wb_clear(&le_search_buffer);
    update_search();
}

/* Finishes the history search with the current result candicate.
 * If no search is being performed, does nothing. */
void cmd_srch_accept_search(wchar_t c __attribute__((unused)))
{
    if (le_search_buffer.contents == NULL)
	return;

    last_search.direction = le_search_direction;
    if (update_last_search_value()) {
	free(last_search.value);
	last_search.value = wb_towcs(&le_search_buffer);
    } else {
	wb_destroy(&le_search_buffer);
    }
    le_search_buffer.contents = NULL;
    le_set_mode(LE_MODE_VI_COMMAND); // TODO assumes vi
    if (le_search_result == Histlist) {
	cmd_alert(L'\0');
	le_display_reprint_buffer(0, false);
    } else {
	go_to_history(le_search_result, false);
	// TODO cursor should be at end for emacs-like search
    }
}

/* Checks if we should update `last_search.value' to the current value of
 * `le_search_buffer'. */
bool update_last_search_value(void)
{
    // XXX assumes vi
    if (le_search_buffer.contents[0] == L'\0')
	return false;
    if (le_search_buffer.contents[0] == L'^'
	    && le_search_buffer.contents[1] == L'\0')
	return false;
    return true;
}

/* Aborts the history search.
 * If no search is being performed, does nothing. */
void cmd_srch_abort_search(wchar_t c __attribute__((unused)))
{
    if (le_search_buffer.contents == NULL)
	return;

    wb_destroy(&le_search_buffer);
    le_search_buffer.contents = NULL;
    le_set_mode(LE_MODE_VI_COMMAND); // XXX assumes vi
    le_display_reprint_buffer(0, false);
    reset_state();
}

/* Re-calculates the search result candidate and prints it. */
void update_search(void)
{
    // TODO currently no emacs-like search

    const wchar_t *pattern = le_search_buffer.contents;
    if (pattern[0] == L'\0') {
	pattern = last_search.value;
	if (pattern == NULL) {
	    le_search_result = Histlist;
	    goto done;
	}
    }

    perform_search(pattern, le_search_direction);
done:
    le_display_reprint_buffer(0, false);
    reset_state();
}

/* Performs history search with the given parameters and updates the result
 * candidate. */
void perform_search(const wchar_t *pattern, enum le_search_direction dir)
{
    const histentry_T *e = main_history_entry;
    char *lpattern = NULL;
    size_t minlen = minlen;
    bool beginning;

    if (dir == FORWARD && e == Histlist)
	goto done;

    if (pattern[0] == L'^') {
	beginning = true;
	pattern++;
	if (pattern[0] == L'\0') {
	    e = Histlist;
	    goto done;
	}
    } else {
	beginning = false;
    }

    if (pattern_has_special_char(pattern, false)) {
	minlen = shortest_match_length(pattern, 0);
    } else {
	lpattern = realloc_wcstombs(unescape(pattern));
	if (lpattern == NULL)
	    goto done;
    }

    for (;;) {
	switch (dir) {
	    case FORWARD:   e = e->Next;  break;
	    case BACKWARD:  e = e->Prev;  break;
	}
	if (e == Histlist)
	    goto done;

	if (lpattern != NULL) {
	    if ((beginning ? matchstrprefix : strstr)(e->value, lpattern))
		goto done;
	} else {
	    wchar_t *wvalue = malloc_mbstowcs(e->value);
	    if (wvalue != NULL) {
		size_t r;
		if (beginning) {
		    r = wfnmatchl(pattern, wvalue, 0, WFNM_SHORTEST, minlen);
		} else {
		    const wchar_t *w = wvalue;
		    assert(w[0] != L'\0');
		    do {
			r = wfnmatchl(pattern, w, 0, WFNM_SHORTEST, minlen);
			if (r != WFNM_NOMATCH)
			    break;
		    } while (*++w != L'\0');
		}
		free(wvalue);
		switch (r) {
		    case WFNM_NOMATCH:  break;
		    case WFNM_ERROR:    e = Histlist;  goto done;
		    default:            goto done;
		}
	    }
	}
    }
done:
    le_search_result = e;
    free(lpattern);
}

/* Redoes the last search. */
void cmd_search_again(wchar_t c __attribute__((unused)))
{
    search_again(last_search.direction);
}

/* Redoes the last search in the reverse direction. */
void cmd_search_again_rev(wchar_t c __attribute__((unused)))
{
    switch (last_search.direction) {
	case FORWARD:   search_again(BACKWARD);  break;
	case BACKWARD:  search_again(FORWARD);   break;
    }
}

/* Redoes the last search in the forward direction. */
void cmd_search_again_forward(wchar_t c __attribute__((unused)))
{
    search_again(FORWARD);
}

/* Redoes the last search in the backward direction. */
void cmd_search_again_backward(wchar_t c __attribute__((unused)))
{
    search_again(BACKWARD);
}

/* Performs command search for the last search pattern in the specified
 * direction. */
void search_again(enum le_search_direction dir)
{
    ALERT_AND_RETURN_IF_PENDING;

    if (last_search.value == NULL) {
	cmd_alert(L'\0');
	return;
    }

    perform_search(last_search.value, dir);
    if (le_search_result == Histlist) {
	cmd_alert(L'\0');
    } else {
	go_to_history(le_search_result, false);
    }
}


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
