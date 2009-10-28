/* Yash: yet another shell */
/* sig.c: signal handling */
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


#include "common.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <wchar.h>
#include <wctype.h>
#if HAVE_GETTEXT
# include <libintl.h>
#endif
#include "builtin.h"
#include "exec.h"
#include "expand.h"
#include "job.h"
#include "option.h"
#include "parser.h"
#include "redir.h"
#include "sig.h"
#include "siglist.h"
#include "signum.h"
#include "strbuf.h"
#include "util.h"
#include "yash.h"
#if YASH_ENABLE_LINEEDIT
# include "lineedit/lineedit.h"
#endif


/* About the shell's signal handling:
 *
 * Yash always catches SIGCHLD.
 * When job control is active, SIGTSTP is ignored.
 * If the shell is interactive, SIGTERM and SIGQUIT are ignored, and SIGINT and
 * SIGWINCH are caught.
 * Trapped signals are also caught.
 *
 * SIGQUIT and SIGINT are ignored in an asynchronous list.
 * SIGTSTP is left ignored in a command substitution in a job-control shell.
 *
 * The shell inherits the signal mask from its invoker and commands invoked by
 * the shell also inherit it. (POSIX.1-2008)
 * Signals with the handler installed are almost always blocked to avoid
 * unexpected interruption of system calls. They are unblocked when:
 *  - the shell waits for input
 *  - the shell waits for a child process to finish
 *  - the shell handles traps.
 *
 * SIGTTOU is blocked in `put_foreground' and unblocked in `ensure_foreground'.
 * All signals are blocked to avoid race conditions when the shell forks. */


static void set_special_handler(int signum, void (*handler)(int signum));
static void reset_special_handler(
	int signum, void (*handler)(int signum), bool leave);
static void sig_handler(int signum);
static void handle_sigchld(void);
static bool set_trap(int signum, const wchar_t *command);
static bool is_ignored(int signum);
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
static inline void handle_sigwinch(void);
#endif


/* Checks if there exists a process with the specified process ID, which should
 * be positive. */
bool process_exists(pid_t pid)
{
    return kill(pid, 0) >= 0 || errno != ESRCH;
}

/* Returns the name of the signal with the specified number.
 * The returned name doesn't have a "SIG"-prefix.
 * "?" is returned for an unknown signal number.
 * The returned string is valid until the next call to this function. */
const char *get_signal_name(int signum)
{
    if (signum == 0)
	return "EXIT";
#if defined SIGRTMIN && defined SIGRTMAX
    int sigrtmin = SIGRTMIN, sigrtmax = SIGRTMAX;
    if (sigrtmin <= signum && signum <= sigrtmax) {
	static char *name = NULL;
	if (signum == sigrtmin)
	    return "RTMIN";
	if (signum == sigrtmax)
	    return "RTMAX";
	int range = sigrtmax - sigrtmin, diff = signum - sigrtmin;
	free(name);
	if (diff <= range / 2)
	    name = malloc_printf("RTMIN+%d", diff);
	else
	    name = malloc_printf("RTMAX-%d", sigrtmax - signum);
	return name;
    }
#endif
    for (const signal_T *s = signals; s->no; s++)
	if (s->no == signum)
	    return s->name;
    return "?";
}

/* Returns the number of a signal whose name is `name'.
 * Returns 0 for "EXIT" and -1 for an unknown name.
 * `name' should be all in uppercase. */
int get_signal_number(const char *name)
{
    if (isdigit(name[0])) {  /* parse a decimal integer */
	int signum;
	if (!xstrtoi(name, 10, &signum) || signum < 0)
	    return -1;

	/* check if `signum' is a valid signal */
	if (signum == 0)
	    return 0;
#if defined SIGRTMIN && defined SIGRTMAX
	if (SIGRTMIN <= signum && signum <= SIGRTMAX)
	    return signum;
#endif
	for (const signal_T *s = signals; s->no; s++)
	    if (s->no == signum)
		return signum;
	return -1;
    }

    if (strcmp(name, "EXIT") == 0)
	return 0;
    if (strncmp(name, "SIG", 3) == 0)
	name += 3;
    for (const signal_T *s = signals; s->no; s++)
	if (strcmp(name, s->name) == 0)
	    return s->no;
#if defined SIGRTMIN && defined SIGRTMAX
    if (strncmp(name, "RTMIN", 5) == 0) {
	int sigrtmin = SIGRTMIN;
	name += 5;
	if (name[0] == '\0') {
	    return sigrtmin;
	} else if (name[0] == '+') {
	    int num;
	    if (!xstrtoi(name, 10, &num)
		    || num < 0 || SIGRTMAX - sigrtmin < num)
		return -1;
	    return sigrtmin + num;
	}
    } else if (strncmp(name, "RTMAX", 5) == 0) {
	int sigrtmax = SIGRTMAX;
	name += 5;
	if (name[0] == '\0') {
	    return sigrtmax;
	} else if (name[0] == '-') {
	    int num;
	    if (!xstrtoi(name, 10, &num)
		    || num > 0 || SIGRTMIN - sigrtmax > num)
		return -1;
	    return sigrtmax + num;
	}
    }
#endif
    return -1;
}

/* Returns the number of a signal whose name is `name'.
 * Returns 0 for "EXIT" and -1 for an unknown name.
 * The given string is converted into uppercase. */
int get_signal_number_w(wchar_t *name)
{
    for (wchar_t *n = name; *n; n++)
	*n = towupper(*n);
    char *mname = malloc_wcstombs(name);
    if (!mname)
	return -1;
    int result = get_signal_number(mname);
    free(mname);
    return result;
}


/* set to true when any trap other than ignore is set */
bool any_trap_set = false;

/* flag to indicate a signal is caught. */
static volatile sig_atomic_t any_signal_received = false;

/* the signal for which trap is currently executed */
static int handled_signal = -1;
/* set to true when the EXIT trap is executed */
static bool exit_handled = false;

/* flags to indicate a signal is caught. */
static volatile sig_atomic_t signal_received[MAXSIGIDX];
/* commands to be executed when a signal is trapped (caught). */
static wchar_t *trap_command[MAXSIGIDX];
/* These arrays are indexed by `sigindex'. The index 0 is for the EXIT trap. */

/* `signal_received' and `trap_command' for real-time signals. */
#if defined SIGRTMIN && defined SIGRTMAX
# if RTSIZE == 0
#  error "RTSIZE == 0"
# endif
static volatile sig_atomic_t rtsignal_received[RTSIZE];
static wchar_t *rttrap_command[RTSIZE];
#endif

/* The signal mask the shell inherited on invocation.
 * This mask is inherited by commands the shell invokes.
 * When a signal's trap is set, the signal is removed from this mask. */
static sigset_t original_sigmask;
/* Set of signals whose handler was SIG_IGN when the shell is invoked but
 * currently is substituted with the shell's handler.
 * The handler of these signals must be reset to SIG_IGN before the shell
 * invokes another command so that the command inherits SIG_IGN as the handler.
 * A signal is added to this set also when its trap handler is set to "ignore."
 */
static sigset_t ignored_signals;
/* Set of signals whose trap is set to other than "ignore".
 * These signals are almost always blocked. */
static sigset_t trapped_signals;
/* Set of signals that are in `original_sigmask' but not in `trapped_signals' */
static sigset_t accept_sigmask;

/* This flag is set to true as well as `signal_received[sigindex(SIGCHLD/INT)]'
 * when SIGCHLD/SIGINT is caught.
 * This flag is used for job control while `signal_received[...]' is for trap
 * handling. */
static volatile sig_atomic_t sigchld_received, sigint_received;
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
/* This flag is set to true as well as `signal_received[sigindex(SIGWINCH)]'
 * when SIGWINCH is caught.
 * This flag is used by line-editing. */
static volatile sig_atomic_t sigwinch_received;
#endif

/* true iff SIGCHLD is handled. */
static bool main_handler_set = false;
/* true iff SIGTSTP is ignored. */
static bool job_handler_set = false;
/* true iff SIGTERM/SIGINT/SIGQUIT/SIGWINCH are ignored/handled. */
static bool interactive_handlers_set = false;

/* Initializes the signal module. */
void init_signal(void)
{
    sigemptyset(&original_sigmask);
    sigemptyset(&ignored_signals);
    sigemptyset(&trapped_signals);
    sigprocmask(SIG_SETMASK, NULL, &original_sigmask);
    accept_sigmask = original_sigmask;
}

/* Installs signal handlers used by the shell according to the current settings.
 */
void set_signals(void)
{
    sigset_t block = trapped_signals;

    if (!job_handler_set && doing_job_control_now) {
	job_handler_set = true;
	set_special_handler(SIGTSTP, SIG_IGN);
    }

    if (!interactive_handlers_set && is_interactive_now) {
	interactive_handlers_set = true;
	sigaddset(&block, SIGINT);
	set_special_handler(SIGINT, sig_handler);
	set_special_handler(SIGTERM, SIG_IGN);
	set_special_handler(SIGQUIT, SIG_IGN);
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
	sigaddset(&block, SIGWINCH);
	set_special_handler(SIGWINCH, sig_handler);
#endif
    }

    if (!main_handler_set) {
	main_handler_set = true;
	sigaddset(&block, SIGCHLD);
	set_special_handler(SIGCHLD, sig_handler);
    }

    sigprocmask(SIG_BLOCK, &block, NULL);
}

/* Restores the original signal handlers for the signals used by the shell.
 * If `leave' is true, the current process is assumed to be about to exec:
 * The handler may be left unchanged if the handler is supposed to be reset
 * during exec. The signal settings for SIGCHLD is restored.
 * If `leave' is false, the settings for SIGCHLD are not restored. */
void restore_signals(bool leave)
{
    if (job_handler_set) {
	job_handler_set = false;
	reset_special_handler(SIGTSTP, SIG_IGN, leave);
    }
    if (interactive_handlers_set) {
	interactive_handlers_set = false;
	reset_special_handler(SIGINT, sig_handler, leave);
	reset_special_handler(SIGTERM, SIG_IGN, leave);
	reset_special_handler(SIGQUIT, SIG_IGN, leave);
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
	reset_special_handler(SIGWINCH, sig_handler, leave);
#endif
    }
    if (main_handler_set) {
	sigset_t ss = original_sigmask;
	if (leave) {
	    main_handler_set = false;
	    reset_special_handler(SIGCHLD, sig_handler, leave);
	} else {
	    sigaddset(&ss, SIGCHLD);
	}
	sigprocmask(SIG_SETMASK, &ss, NULL);
    }
}

/* Re-sets the signal handler of SIGTSTP according to the current
 * `doing_job_control_now' and `job_handler_set'. */
void reset_job_signals(void)
{
    if (doing_job_control_now && !job_handler_set) {
	job_handler_set = true;
	set_special_handler(SIGTSTP, SIG_IGN);
    } else if (!doing_job_control_now && job_handler_set) {
	job_handler_set = false;
	reset_special_handler(SIGTSTP, SIG_IGN, false);
    }
}

/* Sets the signal handler of `signum' to `handler', which must be either
 * SIG_IGN or `sig_handler'.
 * If the old handler is SIG_IGN, `signum' is added to `ignored_signals'.
 * If `handler' is SIG_IGN and the trap for the signal is set, the signal
 * handler is not changed. */
/* Note that this function does not block or unblock the specified signal. */
void set_special_handler(int signum, void (*handler)(int signum))
{
    const wchar_t *trap = trap_command[sigindex(signum)];
    if (trap != NULL && trap[0] != L'\0')
	return;  /* The signal handler must have been set. */

    struct sigaction action, oldaction;
    action.sa_flags = 0;
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    sigemptyset(&oldaction.sa_mask);
    if (sigaction(signum, &action, &oldaction) >= 0)
	if (oldaction.sa_handler == SIG_IGN)
	    sigaddset(&ignored_signals, signum);
}

/* Resets the signal handler of `signum' to what external commands should
 * inherit from the shell. The handler that have been passed to
 * `set_special_handler' must be passed as `handler'. If `leave' is true, the
 * current process is assumed to be about to exec: the handler may be left
 * unchanged if the handler is supposed to be reset during exec. */
void reset_special_handler(int signum, void (*handler)(int signum), bool leave)
{
    struct sigaction action;
    if (sigismember(&ignored_signals, signum))
	action.sa_handler = SIG_IGN;
    else if (trap_command[sigindex(signum)])
	return;
    else
	action.sa_handler = SIG_DFL;
    if (leave && handler != SIG_IGN)
	handler = SIG_DFL;
    if (handler != action.sa_handler) {
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);
	sigaction(signum, &action, NULL);
    }
}

/* Unblocks SIGINT so that system calls can be interrupted.
 * First, this function is called with `onoff' of true and SIGINT is unblocked.
 * Later, this function must be called again with `onoff' of false and SIGINT
 * is re-blocked.
 * This function is effective only if the shell is interactive. */
void set_interruptible_by_sigint(bool onoff)
{
    if (interactive_handlers_set) {
	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, SIGINT);
	sigprocmask(onoff ? SIG_UNBLOCK : SIG_BLOCK, &ss, NULL);
    }
}

/* Sets the action of SIGQUIT and SIGINT to ignoring the signals
 * to prevent an asynchronous job from being killed by these signals. */
void ignore_sigquit_and_sigint(void)
{
    struct sigaction action;

    if (!is_interactive_now) {
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	action.sa_handler = SIG_IGN;
	sigaction(SIGQUIT, &action, NULL);
	sigaction(SIGINT, &action, NULL);
    }  /* Don't set the handers if interactive because they are reset when
	  `restore_signals' is called later. */
    sigaddset(&ignored_signals, SIGQUIT);
    sigaddset(&ignored_signals, SIGINT);
}

/* Sets the action of SIGTSTP to ignoring the signal
 * to prevent a command substitution process from being stopped by SIGTSTP.
 * `doing_job_control_now' must be true. */
void ignore_sigtstp(void)
{
    assert(doing_job_control_now);
    /* Don't set the hander now because it is reset when `restore_signals' is
     * called later.
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = SIG_IGN;
    sigaction(SIGTSTP, &action, NULL);
    */
    sigaddset(&ignored_signals, SIGTSTP);
}

/* Sends SIGSTOP to the shell process.
 * Returns true iff successful. `errno' is set on failure. */
bool send_sigstop_to_myself(void)
{
    return kill(0, SIGSTOP) == 0;
}

/* general signal handler */
void sig_handler(int signum)
{
    any_signal_received = true;
#if defined SIGRTMIN && defined SIGRTMAX
    int sigrtmin = SIGRTMIN;
    if (sigrtmin <= signum && signum <= SIGRTMAX) {
	size_t index = signum - sigrtmin;
	if (index < RTSIZE)
	    rtsignal_received[index] = true;
    } else
#endif
    {
	signal_received[sigindex(signum)] = true;

	switch (signum) {
	    case SIGCHLD:   sigchld_received  = true;   break;
	    case SIGINT:    sigint_received   = true;   break;
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
	    case SIGWINCH:  sigwinch_received = true;   break;
#endif
	}
    }
}

/* Accepts currently pending signals and calls `handle_sigchld' and
 * `handle_traps'. */
void handle_signals(void)
{
    sigset_t ss = accept_sigmask, savess;
    sigdelset(&ss, SIGCHLD);
    if (interactive_handlers_set)
	sigdelset(&ss, SIGINT);
    sigemptyset(&savess);
    sigprocmask(SIG_SETMASK, &ss, &savess);
    sigprocmask(SIG_SETMASK, &savess, NULL);

    handle_sigchld();
    handle_traps();
}

/* Waits for SIGCHLD to be caught and call `handle_sigchld'.
 * If SIGCHLD is already caught, this function doesn't wait.
 * If `interruptible' is true, this function can be canceled by SIGINT.
 * If `return_on_trap' is true, this function returns immediately after trap
 * actions are performed. Otherwise, traps are not handled.
 * Returns the signal number if interrupted, or zero if successful. */
int wait_for_sigchld(bool interruptible, bool return_on_trap)
{
    int result = 0;

    sigset_t ss = accept_sigmask;
    sigdelset(&ss, SIGCHLD);
    if (interruptible)
	sigdelset(&ss, SIGINT);

    for (;;) {
	if (return_on_trap && ((result = handle_traps()) != 0))
	    break;
	if (interruptible && sigint_received)
	    break;
	if (sigchld_received)
	    break;
	if (sigsuspend(&ss) < 0) {
	    if (errno != EINTR) {
		xerror(errno, "sigsuspend");
		break;
	    }
	}
    }

    if (interruptible && sigint_received)
	result = SIGINT;
    handle_sigchld();
    return result;
}

/* Waits for the specified file descriptor to be available for reading.
 * `handle_sigchld' and `handle_sigwinch' are called to handle SIGCHLD and
 * SIGWINCH.
 * If `trap' is true, traps are also handled while waiting.
 * The maximum time length of wait is specified by `timeout' in milliseconds.
 * If `timeout' is negative, the time length is unlimited.
 * If the wait is interrupted by a signal, this function will re-wait for the
 * specified timeout, which means that this function may wait for a time length
 * longer than the specified timeout.
 * This function returns true if the input is ready, and false if an error
 * occurred or it timed out. */
bool wait_for_input(int fd, bool trap, int timeout)
{
    sigset_t ss;
    struct timespec to;
    struct timespec *top;

    ss = accept_sigmask;
    sigdelset(&ss, SIGCHLD);
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
    if (interactive_handlers_set)
	sigdelset(&ss, SIGWINCH);
#endif

    if (timeout < 0) {
	top = NULL;
    } else {
	to.tv_sec  = timeout / 1000;
	to.tv_nsec = timeout % 1000 * 1000000;
	top = &to;
    }

    for (;;) {
	handle_sigchld();
	if (trap)
	    handle_traps();
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
	handle_sigwinch();
#endif

	fd_set fdset;
	FD_ZERO(&fdset);
	FD_SET(fd, &fdset);
	if (pselect(fd + 1, &fdset, NULL, NULL, top, &ss) >= 0) {
	    return FD_ISSET(fd, &fdset);
	} else {
	    if (errno != EINTR) {
		xerror(errno, "pselect");
		return false;
	    }
	}
    }
}

/* Handles SIGCHLD if caught. */
void handle_sigchld(void)
{
    if (sigchld_received) {
        sigchld_received = false;
        do_wait();
#if YASH_ENABLE_LINEEDIT
	if ((shopt_notify || shopt_notifyle) && le_state == LE_STATE_ACTIVE) {
	    le_suspend_readline();
	    print_job_status_all();
	    le_resume_readline();
	} else
#endif
	if (shopt_notify) {
	    sigset_t ss, savess;
	    sigemptyset(&ss);
	    sigaddset(&ss, SIGTTOU);
	    sigemptyset(&savess);
	    sigprocmask(SIG_BLOCK, &ss, &savess);
	    print_job_status_all();
	    sigprocmask(SIG_SETMASK, &savess, NULL);
	}
    }
}

/* Executes trap commands for trapped signals if any.
 * There must not be an active job when this function is called.
 * Returns the signal number if any handler is executed, zero otherwise.
 * Note that, if more than one signal is caught, only one of their numbers is
 * returned. */
int handle_traps(void)
{
    /* Signal handler execution is not reentrant because the value of
     * `savelaststatus' would be lost. But the EXIT is the only exception:
     * The EXIT trap may be executed inside another trap. */
    if (!any_trap_set || !any_signal_received || handled_signal >= 0)
	return false;

#if YASH_ENABLE_LINEEDIT
    le_suspend_readline();
#endif

    int signum = 0;
    struct parsestate_T *state = NULL;
    savelaststatus = laststatus;

    do {
	/* we reset this before executing signal commands to avoid race */
	any_signal_received = false;

	for (const signal_T *s = signals; s->no; s++) {
	    size_t i = sigindex(s->no);
	    if (signal_received[i]) {
		signal_received[i] = false;
		wchar_t *command = trap_command[i];
		if (command && command[0]) {
		    if (!state)
			state = save_parse_state();
		    signum = handled_signal = s->no;
		    exec_wcs(command, "trap", false);
		    laststatus = savelaststatus;
		    if (command != trap_command[i])
			free(command);
		}
	    }
	}
#if defined SIGRTMIN && defined SIGRTMAX
	int sigrtmin = SIGRTMIN, range = SIGRTMAX - sigrtmin + 1;
	if (range > RTSIZE)
	    range = RTSIZE;
	for (int i = 0; i < range; i++) {
	    if (rtsignal_received[i]) {
		rtsignal_received[i] = false;
		wchar_t *command = rttrap_command[i];
		if (command && command[0]) {
		    if (!state)
			state = save_parse_state();
		    signum = handled_signal = sigrtmin + i;
		    exec_wcs(command, "trap", false);
		    laststatus = savelaststatus;
		    if (command != rttrap_command[i])
			free(command);
		}
	    }
	}
#endif
    } while (any_signal_received);

#if YASH_ENABLE_LINEEDIT
    if (shopt_notifyle && le_state == LE_STATE_SUSPENDED)
	print_job_status_all();
    le_resume_readline();
#endif

    savelaststatus = -1;
    handled_signal = -1;
    if (state)
	restore_parse_state(state);
    return signum;
}

/* Executes the EXIT trap if any. */
void execute_exit_trap(void)
{
    wchar_t *command = trap_command[sigindex(0)];
    if (command) {
	/* don't have to save state because no commands are executed any more */
	// struct parsestate_T *state = save_parse_state();
	assert(!exit_handled);
	exit_handled = true;
	savelaststatus = laststatus;
	exec_wcs(command, "EXIT trap", false);
	savelaststatus = -1;
	// restore_parse_state(state);
#ifndef NDEBUG
	if (command != trap_command[sigindex(0)])
	    free(command);
#endif
    }
}

/* Sets trap for the signal `signum' to `command'.
 * If `command' is NULL, the trap is reset to the default.
 * If `command' is an empty string, the trap is set to SIG_IGN.
 * This function may call `get_signal_name'.
 * Returns true iff successful. */
bool set_trap(int signum, const wchar_t *command)
{
    if (signum == SIGKILL || signum == SIGSTOP) {
	xerror(0, Ngt("SIG%s: cannot be trapped"),
		signum == SIGKILL ? "KILL" : "STOP");
	return false;
    }

    wchar_t **commandp;
    volatile sig_atomic_t *receivedp;
#if defined SIGRTMIN && defined SIGRTMAX
    int sigrtmin = SIGRTMIN;
    if (sigrtmin <= signum && signum <= SIGRTMAX) {
	size_t index = signum - sigrtmin;
	if (index < RTSIZE) {
	    commandp = &rttrap_command[index];
	    receivedp = &rtsignal_received[index];
	} else {
	    xerror(0, Ngt("SIG%s: unsupported real-time signal"),
		    get_signal_name(signum));
	    return false;
	}
    } else
#endif
    {
	size_t index = sigindex(signum);
	commandp = &trap_command[index];
	receivedp = &signal_received[index];
    }

    if (!is_interactive && *commandp == NULL && is_ignored(signum)) {
	/* Signals that were ignored on entry to a non-interactive shell cannot
	 * be trapped or reset. (POSIX) */
#if FIXED_SIGNAL_AS_ERROR
	xerror(0, Ngt("SIG%s: cannot be reset"),
		get_signal_name(signum));
	return false;
#else
	return true;
#endif
    }

    /* If `*commandp' is currently executed, we must not free it. */
    if (signum != handled_signal && (signum != 0 || !exit_handled))
	free(*commandp);
    if (command) {
	if (command[0] != L'\0')
	    any_trap_set = true;
	*commandp = xwcsdup(command);
    } else {
	*commandp = NULL;
    }
    *receivedp = false;
    if (signum == 0)
	return true;

    struct sigaction action;
    if (command == NULL)
	action.sa_handler = SIG_DFL;
    else if (command[0] == L'\0')
	action.sa_handler = SIG_IGN;
    else
	action.sa_handler = sig_handler;

    if (action.sa_handler == SIG_IGN) {
	sigaddset(&ignored_signals, signum);
    } else {
	sigdelset(&ignored_signals, signum);
    }
    if (action.sa_handler == sig_handler) {
	sigdelset(&original_sigmask, signum);
	sigaddset(&trapped_signals, signum);
	sigdelset(&accept_sigmask, signum);
    } else {
	sigdelset(&trapped_signals, signum);
    }

    switch (signum) {
	case SIGCHLD:
	    /* SIGCHLD's signal handler is always `sig_handler' */
	    return true;
	case SIGINT:
#if YASH_ENABLE_LINEEDIT && defined(SIGWINCH)
	case SIGWINCH:
#endif
	    /* SIGINT and SIGWINCH's signal handler is always `sig_handler'
	     * when interactive */
	    if (interactive_handlers_set)
		return true;
	    break;
	case SIGTSTP:
	    if (job_handler_set)
		goto default_ignore;
	    break;
	case SIGTERM:
	case SIGQUIT:
	    if (interactive_handlers_set)
		goto default_ignore;
	    break;
default_ignore:
	    if (action.sa_handler == SIG_DFL)
		action.sa_handler = SIG_IGN;
	    break;
    }

    if (action.sa_handler == sig_handler)
	sigprocmask(SIG_BLOCK, &trapped_signals, NULL);

    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(signum, &action, NULL) >= 0) {
	return true;
    } else {
	int saveerrno = errno;
	xerror(saveerrno, "sigaction(SIG%s)", get_signal_name(signum));
	return false;
    }
}

/* Checks if the specified signal is ignored.
 * Asserts the shell is not interactive. */
bool is_ignored(int signum)
{
    assert(!is_interactive_now);

    if (doing_job_control_now && signum == SIGTSTP)
	return sigismember(&ignored_signals, signum);

    struct sigaction action;
    sigemptyset(&action.sa_mask);
    return sigaction(signum, NULL, &action) >= 0
	&& action.sa_handler == SIG_IGN;
}

/* Clears all traps except that are set to SIG_IGN. */
void clear_traps(void)
{
    if (!any_trap_set && !any_signal_received)
	return;

    {
	size_t index = sigindex(0);
	wchar_t *command = trap_command[index];
	if (command && command[0])
	    set_trap(0, NULL);
	signal_received[index] = false;
    }
    for (const signal_T *s = signals; s->no; s++) {
	size_t index = sigindex(s->no);
	wchar_t *command = trap_command[index];
	if (command && command[0])
	    set_trap(s->no, NULL);
	signal_received[index] = false;
    }
#if defined SIGRTMIN && defined SIGRTMAX
    for (int sigrtmin = SIGRTMIN, i = 0; i < RTSIZE; i++) {
	wchar_t *command = rttrap_command[i];
	if (command && command[0])
	    set_trap(sigrtmin + i, NULL);
	rtsignal_received[i] = false;
    }
#endif
    any_trap_set = false, any_signal_received = false;
}

/* Tests the `sigint_received' flag. Returns true only if interactive. */
_Bool is_interrupted(void)
{
    return is_interactive_now && sigint_received;
}

/* Sets the `sigint_received' flag. */
void set_interrupted(void)
{
    sigint_received = true;
}

/* Resets the `sigint_received' flag. */
void reset_sigint(void)
{
    if (interactive_handlers_set) {
	sigset_t ss, savess;
	sigemptyset(&ss);
	sigemptyset(&savess);
	sigaddset(&ss, SIGINT);
	sigprocmask(SIG_UNBLOCK, &ss, &savess);
	sigprocmask(SIG_SETMASK, &savess, NULL);
    }
    sigint_received = false;
}

#if YASH_ENABLE_LINEEDIT
#ifdef SIGWINCH

/* If SIGWINCH has been caught and line-editing is currently active, cause
 * line-editing to redraw the display. */
void handle_sigwinch(void)
{
    if (sigwinch_received)
	le_display_size_changed();
}

#endif /* defined(SIGWINCH) */

/* Resets the `sigwinch_received' flag. */
void reset_sigwinch(void)
{
#ifdef SIGWINCH
    sigwinch_received = false;
#endif
}

#endif /* YASH_ENABLE_LINEEDIT */


/********** Builtin **********/

static bool print_trap(const char *signame, const wchar_t *command)
    __attribute__((nonnull(1)));
static bool print_signal(int signum, const char *name, bool verbose);
static bool signal_job(int signum, const wchar_t *jobname)
    __attribute__((nonnull));

/* The "trap" builtin */
int trap_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"print", xno_argument, L'p', },
#if YASH_ENABLE_HELP
	{ L"help",  xno_argument, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    wchar_t opt;
    bool print = false;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv,
		    posixly_correct ? L"" : L"p",
		    long_options, NULL))) {
	switch (opt) {
	    case L'p':
		print = true;
		break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		goto print_usage;
	}
    }

    if (xoptind == argc) {
	/* print all traps */
	sigset_t ss;
	sigemptyset(&ss);
	print_trap("EXIT", trap_command[sigindex(0)]);
	for (const signal_T *s = signals; s->no; s++) {
	    if (!sigismember(&ss, s->no)) {
		sigaddset(&ss, s->no);
		if (!print_trap(s->name, trap_command[sigindex(s->no)]))
		    return Exit_FAILURE;
	    }
	}
#if defined SIGRTMIN && defined SIGRTMAX
	int sigrtmin = SIGRTMIN, sigrtmax = SIGRTMAX;
	for (int i = 0; i < RTSIZE; i++) {
	    if (sigrtmin + i > sigrtmax)
		break;
	    if (!print_trap(get_signal_name(sigrtmin + i), rttrap_command[i]))
		return Exit_FAILURE;
	}
#endif
	return Exit_SUCCESS;
    } else if (print) {
	/* print specified traps */
	bool ok = true;
#if defined SIGRTMIN && defined SIGRTMAX
	int sigrtmin = SIGRTMIN, sigrtmax = SIGRTMAX;
#endif
	do {
	    wchar_t *wname = ARGV(xoptind);
	    for (wchar_t *w = wname; *w; w++)
		*w = towupper(*w);
	    char *name = malloc_wcstombs(wname);
	    if (!name)
		name = "";
	    int signum = get_signal_number(name);
	    if (signum < 0) {
		xerror(0, Ngt("%ls: no such signal"), wname);
	    } else {
#if defined SIGRTMIN && defined SIGRTMAX
		if (sigrtmin <= signum && signum <= sigrtmax) {
		    int index = signum - sigrtmin;
		    if (index < RTSIZE)
			ok = print_trap(get_signal_name(signum),
				rttrap_command[index]);
		} else
#endif
		{
		    ok = print_trap(name, trap_command[sigindex(signum)]);
		}
	    }
	    free(name);
	} while (ok && ++xoptind < argc);
	return ok ? Exit_SUCCESS : Exit_FAILURE;
    }

    bool err = false;
    const wchar_t *command;

    /* check if the first operand is an integer */
    wchar_t *end;
    errno = 0;
    wcstoul(ARGV(xoptind), &end, 10);
    if (ARGV(xoptind)[0] != L'\0' && *end == L'\0') {
	command = NULL;
	goto set_traps;
    } else {
	command = ARGV(xoptind++);
	if (xoptind == argc)
	    goto print_usage;
	if (wcscmp(command, L"-") == 0)
	    command = NULL;
    }

set_traps:
    do {
	wchar_t *name = ARGV(xoptind);
	int signum = get_signal_number_w(name);
	if (signum < 0) {
	    xerror(0, Ngt("%ls: no such signal"), name);
	} else {
	    err |= !set_trap(signum, command);
	}
    } while (++xoptind < argc);
    return err ? Exit_FAILURE : Exit_SUCCESS;

print_usage:
    if (posixly_correct)
	fprintf(stderr,
		Ngt("Usage:  trap [action signal...]\n"
		    "        trap signum [signal...]\n"));
    else
	fprintf(stderr,
		Ngt("Usage:  trap [action signal...]\n"
		    "        trap signum [signal...]\n"
		    "        trap -p [signal...]\n"));
    SPECIAL_BI_ERROR;
    return Exit_ERROR;
}

/* Prints a trap command to stdout that can be used to restore the current
 * signal handler for the specified signal.
 * If the `command' is NULL, this function does nothing.
 * Otherwise, the `command' is properly single-quoted and printed.
 * Returns true iff successful (no error). */
bool print_trap(const char *signame, const wchar_t *command)
{
    if (command) {
	wchar_t *q = quote_sq(command);
	int r = printf("trap -- %ls %s\n", q, signame);
	if (r < 0)
	    xerror(errno, Ngt("cannot print to standard output"));
	free(q);
	if (r < 0)
	    return false;
    }
    return true;
}

#if YASH_ENABLE_HELP
const char trap_help[] = Ngt(
"trap - set signal handler\n"
"\ttrap [action signal...]\n"
"\ttrap signum [signal...]\n"
"\ttrap -p [signal...]\n"
"Sets the signal handler of the specified <signal>s to <action>.\n"
"When the shell receives the signal, the corresponding action is executed as\n"
"if by the \"eval\" command.\n"
"If <action> is an empty string, no actions are taken and the signal is\n"
"silently ignored when the signal is received.\n"
"If <action> is \"-\", the signal handler is reset to the default.\n"
"If the first operand is a non-negative integer, the operand is considered as\n"
"a signal specification and <action> is assumed to be \"-\".\n"
"If the -p (--print) option is specified, the actions for the specified\n"
"<signal>s are printed. This option is not available in POSIXly correct mode.\n"
"Without any operands, all signal handlers currently set are printed.\n"
);
#endif

/* The "kill" builtin, which accepts the following options:
 * -s SIG: specifies the signal to send
 * -n num: specifies the signal to send by number
 * -l: prints signal info
 * -v: prints signal info verbosely */
int kill_builtin(int argc, void **argv)
{
#if YASH_ENABLE_HELP
    if (!posixly_correct && argc == 2 && wcscmp(ARGV(1), L"--help") == 0)
	return print_builtin_help(ARGV(0));
#endif

    wchar_t opt;
    int signum = SIGTERM;
    bool list = false, verbose = false;
    bool err = false;

    xoptind = 0, xopterr = false;
    while ((opt = xgetopt_long(argv,
		    posixly_correct ? L"ls:" : L"+ln:s:v",
		    NULL, NULL))) {
	switch (opt) {
	    /* we don't make any differences between -n and -s options */
	    case L'n':  case L's':  parse_signal:
		if (list)
		    goto print_usage;
		if (posixly_correct && wcsncmp(xoptarg, L"SIG", 3) == 0) {
		    xerror(0, Ngt("%ls: signal name must be specified "
				"without `SIG'"), xoptarg);
		    return Exit_ERROR;
		}
		signum = get_signal_number_w(xoptarg);
		if (signum < 0 || (signum == 0 && !iswdigit(xoptarg[0]))) {
		    xerror(0, Ngt("%ls: no such signal"), xoptarg);
		    return Exit_FAILURE;
		}
		goto no_more_options;
	    case L'l':
		list = true;
		break;
	    case L'v':
		list = verbose = true;
		break;
	    default:
		if (ARGV(xoptind)[0] == L'-' && ARGV(xoptind)[1] == xoptopt) {
		    if (list)
			goto no_more_options;
		    xoptarg = &ARGV(xoptind++)[1];
		    goto parse_signal;
		}
		goto print_usage;
	    no_more_options:
		if (xoptind < argc && wcscmp(ARGV(xoptind), L"--") == 0)
		    xoptind++;
		goto main;
	}
    }

main:
    if (list) {
	/* print signal info */
	if (xoptind == argc) {
	    for (const signal_T *s = signals; s->no; s++)
		print_signal(s->no, s->name, verbose);
#if defined SIGRTMIN && defined SIGRTMAX
	    for (int i = SIGRTMIN, max = SIGRTMAX; i <= max; i++)
		print_signal(i, NULL, verbose);
#endif
	} else {
	    do {
		int signum;

		if (xwcstoi(ARGV(xoptind), 10, &signum) && signum >= 0) {
		    if (signum >= TERMSIGOFFSET)
			signum -= TERMSIGOFFSET;
		    else if (signum >= (TERMSIGOFFSET & 0xFF))
			signum -= (TERMSIGOFFSET & 0xFF);
		} else {
		    signum = get_signal_number_w(ARGV(xoptind));
		}
		if (signum <= 0 || !print_signal(signum, NULL, verbose)) {
		    xerror(0, Ngt("%ls: no such signal"), ARGV(xoptind));
		    err = true;
		}
	    } while (++xoptind < argc);
	}
    } else {
	/* send signal */
	if (xoptind == argc)
	    goto print_usage;
	do {
	    wchar_t *proc = ARGV(xoptind);
	    if (proc[0] == L'%') {
		if (!signal_job(signum, proc))
		    err = true;
	    } else {
		long pid;

		if (!xwcstol(proc, 10, &pid)) {
		    xerror(0, Ngt("`%ls' is not a valid integer"), proc);
		    err = true;
		    continue;
		}
		// XXX this cast might not be safe
		if (kill((pid_t) pid, signum) < 0) {
		    xerror(errno, "%ls", proc);
		    err = true;
		    continue;
		}
	    }
	} while (++xoptind < argc);
    }
    return err ? Exit_FAILURE : Exit_SUCCESS;

print_usage:
    if (posixly_correct)
	fprintf(stderr, Ngt(
		    "Usage:  kill [-s signal] process...\n"
		    "        kill -l [number...]\n"));
    else
	fprintf(stderr, Ngt(
		    "Usage:  kill [-s signal | -n signum] process...\n"
		    "        kill -l [-v] [number...]\n"));
    return Exit_ERROR;
}

/* Prints info about the specified signal.
 * If `name' is non-NULL, it must be a valid signal name of `signum'.
 * If `name' is NULL and `signum' is not a valid signal number, it is an error.
 * `get_signal_name' may be called in this function.
 * Returns true iff successful. */
bool print_signal(int signum, const char *name, bool verbose)
{
    if (!name) {
	name = get_signal_name(signum);
	if (strcmp(name, "?") == 0)
	    return false;
    }
    if (!verbose) {
	puts(name);
    } else {
#if HAVE_STRSIGNAL
	const char *sigdesc = strsignal(signum);
	if (sigdesc) {
	    printf("%d\t%-10s %s\n", signum, name, sigdesc);
	} else
#endif
	{
	    printf("%d\t%-10s\n", signum, name);
	}
    }
    return true;
}

/* Sends the specified signal to the specified job.
 * Returns true iff successful. */
bool signal_job(int signum, const wchar_t *jobspec)
{
    pid_t jobpgid = get_job_pgid(jobspec);
    if (jobpgid <= 0)
	return false;

    if (kill(-jobpgid, signum) < 0) {
	xerror(errno, "%ls", jobspec);
	return false;
    }
    return true;
}

#if YASH_ENABLE_HELP
const char kill_help[] = Ngt(
"kill - send a signal to processes\n"
"\tkill [-signal|-s signal|-n number] process...\n"
"\tkill -l [-v] [number...]\n"
"The first form sends a signal to the specified processes. The signal to send\n"
"can be specified by the name or by the number, which defaults to SIGTERM if\n"
"not specified. The processes can be specified by the process ID or in the\n"
"job specification format like \"%1\".\n"
"If the process ID is negative, the signal is sent to the process group.\n"
"The second form prints info about signals. For each <number> given, the name\n"
"of the corresponding signal is printed. The <number> must be a valid signal\n"
"number or the exit status of a process kill by a signal. If no <number>s are\n"
"given, a list of available signals is printed.\n"
"With the -v option, verbose info is printed.\n"
);
#endif


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
