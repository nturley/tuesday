//
//  Copyright (C) 2011-2018  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//


/* THESE ARE NOT PLAYING NICE WITH CMAKE
#if defined(__MINGW32__)
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <DbgHelp.h>
#include <fileapi.h>
#include <psapi.h>
#endif
*/
#include "util.h"
#include "ident.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <time.h>

#include <sys/types.h>
#include <sys/time.h>
#ifdef HAVE_SYS_PTRACE_H
#include <sys/ptrace.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif
#ifndef __MINGW32__
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef __CYGWIN__
#include <process.h>
#endif

#if defined(HAVE_UCONTEXT_H)
#include <ucontext.h>
#elif defined(HAVE_SYS_UCONTEXT_H)
#include <sys/ucontext.h>
#endif

#ifdef HAVE_LIBDW
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <dwarf.h>
#include <unwind.h>
#endif

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#define N_TRACE_DEPTH   16
#define ERROR_SZ        1024
#define PAGINATE_RIGHT  72
#define TRACE_MAX_LINE  256

#define ANSI_RESET      0
#define ANSI_BOLD       1
#define ANSI_FG_BLACK   30
#define ANSI_FG_RED     31
#define ANSI_FG_GREEN   32
#define ANSI_FG_YELLOW  33
#define ANSI_FG_BLUE    34
#define ANSI_FG_MAGENTA 35
#define ANSI_FG_CYAN    36
#define ANSI_FG_WHITE   37

#define MAX_FMT_BUFS    32
#define MAX_PRINTF_BUFS 8



typedef void (*print_fn_t)(const char *fmt, ...);

static void def_error_fn(const char *msg, const loc_t *loc);
static void show_hint(void);

typedef struct guard guard_t;
typedef struct option option_t;
typedef struct hint hint_t;

typedef enum {
   OPTION_INT,
   OPTION_STRING
} option_kind_t;

typedef union {
   int   i;
   char *s;
} optval_t;

struct option {
   option_t      *next;
   option_kind_t  kind;
   ident_t        key;
   optval_t       value;
};

struct hint {
   hint_fn_t func;
   char     *str;
   void     *context;
   loc_t     loc;
   hint_t   *next;
};

struct color_escape {
   const char *name;
   int         value;
};

struct guard {
   const char *tag;
   uintptr_t   base;
   uintptr_t   limit;
   guard_t    *next;
};

struct text_buf {
   char  *buf;
   size_t alloc;
   size_t len;
};

static error_fn_t      error_fn = def_error_fn;
static fatal_fn_t      fatal_fn = NULL;
static bool            want_color = false;
static bool            error_force_plain = false;
static struct option  *options = NULL;
static guard_t        *guards;
static message_style_t message_style = MESSAGE_FULL;
static hint_t         *hints = NULL;

static const struct color_escape escapes[] = {
   { "",        ANSI_RESET },
   { "bold",    ANSI_BOLD },
   { "black",   ANSI_FG_BLACK },
   { "red",     ANSI_FG_RED },
   { "green",   ANSI_FG_GREEN },
   { "yellow",  ANSI_FG_YELLOW },
   { "blue",    ANSI_FG_BLUE },
   { "magenta", ANSI_FG_MAGENTA },
   { "cyan",    ANSI_FG_CYAN },
   { "white",   ANSI_FG_WHITE },
};

static char *filter_color(const char *str, bool force_plain)
{
   // Replace color strings like "$red$foo$$bar" with ANSI escaped
   // strings like "\033[31mfoo\033[0mbar"

   const size_t maxlen = strlen(str) * 2;
   char *copy = xmalloc(maxlen);
   char *p = copy;
   char *eptr = copy + maxlen;

   const char *escape_start = NULL;

   while (*str != '\0') {
      if (*str == '$') {
         if (escape_start == NULL)
            escape_start = str;
         else {
            const char *e = escape_start + 1;
            const size_t len = str - e;

            if (want_color && !force_plain) {
               bool found = false;
               for (int i = 0; i < ARRAY_LEN(escapes); i++) {
                  if (strncmp(e, escapes[i].name, len) == 0) {
                     p += snprintf(p, eptr - p, "\033[%dm", escapes[i].value);
                     found = true;
                     break;
                  }
               }

               if (!found) {
                  strncpy(p, escape_start, len + 1);
                  p += len + 1;
                  escape_start = str;
               }
               else
                  escape_start = NULL;
            }
            else
               escape_start = NULL;
         }
      }
      else if (escape_start == NULL)
         *p++ = *str;

      ++str;
   }

   if (escape_start != NULL) {
      const size_t len = str - escape_start;
      strncpy(p, escape_start, len + 1);
      p += len + 1;
   }

   *p = '\0';

   return copy;
}

static void paginate_msg(const char *fmt, va_list ap,
                         int start, int left, int right)
{
   char *strp = xvasprintf(fmt, ap);

   char *filtered = filter_color(strp, false);

   const char *p = filtered;
   int col = start;
   bool escape = false;
   while (*p != '\0') {
      if ((*p == '\n') || (*p == '\r') || (isspace((int)*p) && col >= right)) {
         // Can break line here
         fputc('\n', stderr);
         if (*p == '\r')
            col = 0;
         else {
            for (col = 0; col < left; col++)
               fputc(' ', stderr);
         }
      }
      else {
         fputc(*p, stderr);
         if (*p == '\033')
            escape = true;
         else if (escape) {
            if (*p == 'm')
               escape = false;
         }
         else
            ++col;
      }
      ++p;
   }
   fputc('\n', stderr);

#ifdef __MINGW32__
   fflush(stderr);
#endif

   free(filtered);
   free(strp);
}

static void set_attr(int attr)
{
   if (want_color)
      fprintf(stderr, "\033[%dm", attr);
}

void *xmalloc(size_t size)
{
   void *p = malloc(size);
   if (p == NULL)
      fatal("memory exhausted (malloc %lu)", (long unsigned)size);
   return p;
}

void *xcalloc(size_t size)
{
   void *p = calloc(1, size);
   if (p == NULL)
      fatal("memory exhausted (calloc %lu)", (long unsigned)size);
   return p;
}

void *xrealloc(void *ptr, size_t size)
{
   ptr = realloc(ptr, size);
   if (ptr == NULL)
      fatal("memory exhausted (realloc %lu)", (long unsigned)size);
   return ptr;
}

char *xstrdup(const char *str)
{
   char *copy = strdup(str);
   if (copy == NULL)
      fatal("memory exhausted (strdup %p)", str);
   return copy;
}

char *xvasprintf(const char *fmt, va_list ap)
{
   char *strp = NULL;
   if (vasprintf(&strp, fmt, ap) < 0)
      abort();
   return strp;
}

char *xasprintf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char *strp = xvasprintf(fmt, ap);
   va_end(ap);
   return strp;
}

static void fmt_color(int color, const char *prefix,
                      const char *fmt, va_list ap)
{
   set_attr(color);
   if (message_style == MESSAGE_COMPACT)
      fprintf(stderr, "%c%s: ", tolower((int)prefix[0]), prefix + 1);
   else
      fprintf(stderr, "** %s: ", prefix);
   set_attr(ANSI_RESET);
   paginate_msg(fmt, ap, strlen(prefix) + 5, 10,
                (message_style == MESSAGE_COMPACT) ? INT_MAX : PAGINATE_RIGHT);
}

void errorf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Error", fmt, ap);
   va_end(ap);
}

void warnf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_YELLOW, "Warning", fmt, ap);
   va_end(ap);
}

void notef(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_RESET, "Note", fmt, ap);
   va_end(ap);
}

static void fatalf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   va_end(ap);
}

static void def_error_fn(const char *msg, const loc_t *loc)
{
   if (message_style == MESSAGE_COMPACT)
      fmt_loc(stderr, loc);
   errorf("%s", msg);
   if (message_style == MESSAGE_FULL)
      fmt_loc(stderr, loc);
}

static char *prepare_msg(const char *fmt, va_list ap, bool force_plain)
{
   char *strp LOCAL = xvasprintf(fmt, ap);
   return filter_color(strp, force_plain);
}

static void msg_at(print_fn_t fn, const loc_t *loc, const char *fmt, va_list ap)
{
   char *strp = prepare_msg(fmt, ap, false);
   if (message_style == MESSAGE_COMPACT)
      fmt_loc(stderr, loc);
   (*fn)("%s", strp);
   if (message_style == MESSAGE_FULL)
      fmt_loc(stderr, loc);
   free(strp);
}

static int color_vfprintf(FILE *f, const char *fmt, va_list ap)
{
   char *strp LOCAL = prepare_msg(fmt, ap, false);

   bool escape = false;
   int len = 0;
   for (const char *p = strp; *p != '\0'; p++) {
      if (*p == '\033')
         escape = true;
      if (escape)
         escape = (*p != 'm');
      else
         len += 1;
   }

   fputs(strp, f);
   return len;
}

static int color_fprintf(FILE *f, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   const int len = color_vfprintf(f, fmt, ap);
   va_end(ap);
   return len;
}

int color_printf(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   int rc = color_vprintf(fmt, ap);
   va_end(ap);
   return rc;
}

int color_vprintf(const char *fmt, va_list ap)
{
   char *strp LOCAL = prepare_msg(fmt, ap, false);

   bool escape = false;
   int len = 0;
   for (const char *p = strp; *p != '\0'; p++) {
      if (*p == '\033')
         escape = true;
      if (escape)
         escape = (*p != 'm');
      else
         len += 1;
   }

   printf("%s", strp);
   return len;
}

void error_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   char *strp LOCAL = prepare_msg(fmt, ap, error_force_plain);
   error_fn(strp, loc != NULL ? loc : &LOC_INVALID);
   show_hint();

   va_end(ap);
}

static void catch_in_unit_test(print_fn_t fn, const loc_t *loc,
                               const char *fmt, va_list ap)
{
   if (opt_get_int("unit-test")) {
      char *strp LOCAL = prepare_msg(fmt, ap, error_force_plain);
      error_fn(strp, loc != NULL ? loc : &LOC_INVALID);
   }
   else
      msg_at(fn, loc, fmt, ap);
}

static void default_hint_fn(void *arg)
{
   hint_t *h = arg;
   note_at(&(h->loc), "%s", h->str);
}

static void pop_hint(void)
{
   hint_t *tmp = hints->next;
   free(hints->str);
   free(hints);
   hints = tmp;
}

static void show_hint(void)
{
   static bool inside = false;

   if (inside)
      return;

   inside = true;

   while (hints != NULL) {
      (*hints->func)(hints->context);
      pop_hint();
   }

   inside = false;
}

void set_hint_fn(hint_fn_t fn, void *context)
{
   hint_t *h = xmalloc(sizeof(hint_t));
   h->func = fn;
   h->str = NULL;
   h->context = context;
   h->next = hints;
   h->loc = LOC_INVALID;

   hints = h;
}

void clear_hint(void)
{
   while (hints != NULL)
      pop_hint();
}

void hint_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   hint_t *h = xmalloc(sizeof(hint_t));
   h->func = default_hint_fn;
   h->str = prepare_msg(fmt, ap, error_force_plain);
   h->context = h;
   h->loc = *loc;
   h->next = hints;

   va_end(ap);

   hints = h;
}

void warn_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   catch_in_unit_test(warnf, loc, fmt, ap);
   show_hint();
   va_end(ap);
}

void note_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   catch_in_unit_test(notef, loc, fmt, ap);
   show_hint();
   va_end(ap);
}

void fatal_at(const loc_t *loc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   catch_in_unit_test(fatalf, loc, fmt, ap);
   show_hint();
   va_end(ap);

   if (fatal_fn != NULL)
      (*fatal_fn)();

   exit(EXIT_FAILURE);
}

error_fn_t set_error_fn(error_fn_t fn, bool want_color)
{
   error_fn_t old = error_fn;
   error_fn = fn ?: def_error_fn;
   error_force_plain = !want_color;
   return old;
}

void set_fatal_fn(fatal_fn_t fn)
{
   fatal_fn = fn;
}

void fatal(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   show_hint();
   va_end(ap);

   if (fatal_fn != NULL)
      (*fatal_fn)();

   exit(EXIT_FAILURE);
}


void fatal_trace(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fmt_color(ANSI_FG_RED, "Fatal", fmt, ap);
   va_end(ap);

#ifndef NO_STACK_TRACE
   show_stacktrace();
#endif  // !NO_STACK_TRACE

   exit(EXIT_FAILURE);
}

void fatal_errno(const char *fmt, ...)
{
  exit(EXIT_FAILURE);
}

void fmt_loc(FILE *f, const struct loc *loc)
{
   if (loc == NULL || loc->first_line == LINE_INVALID || loc->file == NULL)
      return;

   if (message_style == MESSAGE_COMPACT) {
      fprintf(f, "%s:%d:%d: ", istr(loc->file), loc->first_line,
              loc->first_column + 1);
      return;
   }

   fprintf(f, "\tFile %s, Line %u\n", istr(loc->file), loc->first_line);

   if (loc->linebuf == NULL)
      return;

   const char *lb = loc->linebuf;
   char buf[80];
   size_t i = 0;
   while (i < sizeof(buf) - 1 && *lb != '\0' && *lb != '\n') {
      if (*lb == '\t')
         buf[i++] = ' ';
      else
         buf[i++] = *lb;
      ++lb;
   }
   buf[i] = '\0';

   // Print ... if error location spans multiple lines
   bool many_lines = (loc->first_line != loc->last_line)
      || (i == sizeof(buf) - 1 && i <= loc->last_column);
   int last_col = many_lines ? strlen(buf) + 3 : loc->last_column;

   set_attr(ANSI_FG_CYAN);
   fprintf(f, "    %s%s\n", buf, many_lines ? " ..." : "");
   if (last_col >= loc->first_column) {
      for (unsigned j = 0; j < loc->first_column + 4; j++)
         fprintf(f, " ");
      set_attr(ANSI_FG_GREEN);
      for (unsigned j = 0; j < last_col - loc->first_column + 1; j++)
         fprintf(f, "^");
      set_attr(ANSI_RESET);
      fprintf(f, "\n");
   }

   fflush(f);
}

#ifndef NO_STACK_TRACE

static bool check_guard_page(uintptr_t addr)
{
   for (guard_t *it = guards; it != NULL; it = it->next) {
      if ((addr >= it->base) && (addr < it->limit)) {
         fatal_trace("accessed %d bytes beyond $cyan$%s$$ region",
                     (int)(addr - it->base), it->tag);
      }
   }

   return false;
}

#if defined HAVE_LIBDW
static bool die_has_pc(Dwarf_Die* die, Dwarf_Addr pc)
{
   Dwarf_Addr low, high;

   if (dwarf_hasattr(die, DW_AT_low_pc) && dwarf_hasattr(die, DW_AT_high_pc)) {
      if (dwarf_lowpc(die, &low) != 0)
         return false;
      if (dwarf_highpc(die, &high) != 0) {
         Dwarf_Attribute attr_mem;
         Dwarf_Attribute* attr = dwarf_attr(die, DW_AT_high_pc, &attr_mem);
         Dwarf_Word value;
         if (dwarf_formudata(attr, &value) != 0)
            return false;
         high = low + value;
      }
      return pc >= low && pc < high;
   }

   Dwarf_Addr base;
   ptrdiff_t offset = 0;
   while ((offset = dwarf_ranges(die, offset, &base, &low, &high)) > 0) {
      if (pc >= low && pc < high)
         return true;
   }

   return false;
}

static _Unwind_Reason_Code libdw_trace_iter(struct _Unwind_Context* ctx,
                                            void *param)
{
   static Dwfl *handle = NULL;
   static Dwfl_Module *home = NULL;

   if (handle == NULL) {
      static Dwfl_Callbacks callbacks = {
         .find_elf = dwfl_linux_proc_find_elf,
         .find_debuginfo = dwfl_standard_find_debuginfo,
         .debuginfo_path = NULL
      };

      if ((handle = dwfl_begin(&callbacks)) == NULL) {
         warnf("failed to initialise dwfl");
         return _URC_NORMAL_STOP;
      }

      dwfl_report_begin(handle);
      if (dwfl_linux_proc_report(handle, getpid()) < 0) {
         warnf("dwfl_linux_proc_report failed");
         return _URC_NORMAL_STOP;
      }
      dwfl_report_end(handle, NULL, NULL);

      home = dwfl_addrmodule(handle, (uintptr_t)libdw_trace_iter);
   }

   int *skip = param;
   if (skip != NULL && *skip > 0) {
      (*skip)--;
      return _URC_NO_REASON;
   }

   int ip_before_instruction = 0;
   uintptr_t ip = _Unwind_GetIPInfo(ctx, &ip_before_instruction);

   if (ip == 0)
      return _URC_NO_REASON;
   else if (!ip_before_instruction)
      ip -= 1;

   Dwfl_Module *mod = dwfl_addrmodule(handle, ip);

   const char *module_name = dwfl_module_info(mod, 0, 0, 0, 0, 0, 0, 0);
   const char *sym_name = dwfl_module_addrname(mod, ip);

   Dwarf_Addr mod_bias = 0;
   Dwarf_Die *die = dwfl_module_addrdie(mod, ip, &mod_bias);

   if (die == NULL) {
      // Hack to support Clang taken from backward-cpp
      while ((die = dwfl_module_nextcu(mod, die, &mod_bias))) {
         Dwarf_Die child;
         if (dwarf_child(die, &child) != 0)
            continue;

         Dwarf_Die* iter = &child;
         do {
            switch (dwarf_tag(iter)) {
            case DW_TAG_subprogram:
            case DW_TAG_inlined_subroutine:
               if (die_has_pc(iter, ip))
                  goto found_die_with_ip;
            }
         } while (dwarf_siblingof(iter, iter) == 0);
      }
   found_die_with_ip:
      ;
   }

   Dwarf_Line* srcloc = dwarf_getsrc_die(die, ip - mod_bias);
   const char* srcfile = dwarf_linesrc(srcloc, 0, 0);

   int line = 0, col = 0;
   dwarf_lineno(srcloc, &line);
   dwarf_linecol(srcloc, &col);

   color_printf("[$green$%p$$] ", (void *)ip);
   if (mod != home)
      color_printf("($red$%s$$) ", module_name);
   if (srcfile != NULL)
      color_printf("%s:%d ", srcfile, line);
   if (sym_name != NULL)
      color_printf("$yellow$%s$$", sym_name);
   printf("\n");

   FILE *f = fopen(srcfile, "r");
   if (f != NULL) {
      char buf[TRACE_MAX_LINE];
      for (int i = 0; i < line + 1 && fgets(buf, sizeof(buf), f); i++) {
         if (i < line - 2)
            continue;

         const size_t len = strlen(buf);
         if (len <= 1)
            continue;
         else if (buf[len - 1] == '\n')
            buf[len - 1] = '\0';

         if (i == line - 1)
            color_printf("$cyan$$bold$-->$$ $cyan$%s$$\n", buf);
         else
            color_printf("    $cyan$%s$$\n", buf);
      }
      fclose(f);
   }

   if (sym_name != NULL && strcmp(sym_name, "main") == 0)
      return _URC_NORMAL_STOP;
   else
      return _URC_NO_REASON;
}
#elif defined HAVE_EXECINFO_H

static void print_trace(char **messages, int trace_size)
{
   fputs("\n-------- STACK TRACE --------\n", stderr);

   for (int i = 0; i < trace_size; i++)
      fprintf(stderr, "%s\n", messages[i]);

   fputs("-----------------------------\n", stderr);

#ifdef __linux
   color_fprintf(stderr, "\n$cyan$Hint: you can get better stack traces by "
                 "installing the libdw-dev package and reconfiguring$$\n");
#endif  // __linux
}
#endif  // HAVE_EXECINFO_H

#ifdef __MINGW32__

#ifdef __WIN64

#endif  // __WIN64



#endif  // __MINGW32__

void show_stacktrace(void)
{

}

#ifndef __MINGW32__
static const char *signame(int sig)
{
   switch (sig) {
   case SIGSEGV: return "SIGSEGV";
   case SIGABRT: return "SIGABRT";
   case SIGILL: return "SIGILL";
   case SIGFPE: return "SIGFPE";
   case SIGUSR1: return "SIGUSR1";
   case SIGBUS: return "SIGBUS";
   default: return "???";
   }
}

static void bt_sighandler(int sig, siginfo_t *info, void *secret)
{

}
#endif  // !__MINGW32__

#endif  // NO_STACK_TRACE

#if defined __linux__
static bool scan_file_for_token(const char *file, const char *token)
{
   bool found = false;
   FILE *f = fopen(file, "r");
   if (f != NULL) {
      char buf[1024];
      while (!found && fgets(buf, sizeof(buf), f)) {
         if (strstr(buf, token))
            found = true;
      }
      fclose(f);
   }

   return found;
}
#endif

bool is_debugger_running(void)
{
   return false;
}

#ifdef __linux
static void gdb_sighandler(int sig, siginfo_t *info)
{
   char exe[256];
   if (readlink("/proc/self/exe", exe, sizeof(exe)) < 0)
      fatal_errno("readlink");

   pid_t pp = getpid();

   pid_t p = fork();
   if (p == 0) {
      char *pid = xasprintf("%d", pp);
      execl("/usr/bin/gdb", "gdb", "-ex", "cont", exe, pid, NULL);
      free(pid);
      fatal_errno("execl");
   }
   else if (p < 0)
      fatal_errno("fork");
   else {
      // Allow a little time for GDB to start before dropping
      // into the default signal handler
      sleep(1);
      signal(sig, SIG_DFL);
   }
}
#endif  // __linux

void register_trace_signal_handlers(void)
{
#if defined __MINGW32__



#elif !defined NO_STACK_TRACE
   if (is_debugger_running())
      return;

   struct sigaction sa;
   sa.sa_sigaction = (void*)bt_sighandler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART | SA_SIGINFO;

   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGUSR1, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
#endif  // NO_STACK_TRACE
}

void register_gdb_signal_handlers(void)
{
#ifdef __linux
   if (is_debugger_running())
      return;

   struct sigaction sa;
   sa.sa_sigaction = (void*)gdb_sighandler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART | SA_SIGINFO;

   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGUSR1, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
#else  // __linux
   register_trace_signal_handlers();
#endif  // __linux
}

void term_init(void)
{
   const char *nvc_no_color = getenv("NVC_NO_COLOR");
   const char *term = getenv("TERM");

   static const char *term_blacklist[] = {
      "dumb"
   };

   bool is_tty = isatty(STDERR_FILENO) && isatty(STDOUT_FILENO);

#ifdef __MINGW32__

#endif

   want_color = is_tty && (nvc_no_color == NULL);

   if (want_color && (term != NULL)) {
      for (size_t i = 0; i < ARRAY_LEN(term_blacklist); i++) {
         if (strcmp(term, term_blacklist[i]) == 0) {
            want_color = false;
            break;
         }
      }
   }

}

static void opt_set_generic(const char *name, option_kind_t kind,
                            optval_t value)
{
   ident_t name_i = ident_new(name);
   struct option *it;
   for (it = options; (it != NULL) && (it->key != name_i); it = it->next)
      ;

   if (it != NULL) {
      if (it->kind == OPTION_STRING)
         free(it->value.s);
      it->value = value;
   }
   else {
      it = xmalloc(sizeof(struct option));
      it->key   = ident_new(name);
      it->value = value;
      it->next  = options;
      it->kind  = kind;

      options = it;
   }
}

static optval_t opt_get_generic(const char *name, option_kind_t kind)
{
   ident_t name_i = ident_new(name);
   struct option *it;
   for (it = options; (it != NULL) && (it->key != name_i); it = it->next)
      ;

   if (it != NULL) {
      if (it->kind == kind)
         return it->value;
      else
         fatal_trace("wrong option kind for %s", name);
   }
   else
      fatal_trace("invalid option %s", name);
}

void opt_set_int(const char *name, int val)
{
   opt_set_generic(name, OPTION_INT, (optval_t)val);
}

int opt_get_int(const char *name)
{
   return opt_get_generic(name, OPTION_INT).i;
}

void opt_set_str(const char *name, const char *val)
{
   opt_set_generic(name, OPTION_STRING, (optval_t)(val ? strdup(val) : NULL));
}

const char *opt_get_str(const char *name)
{
   return opt_get_generic(name, OPTION_STRING).s;
}

char *get_fmt_buf(size_t len)
{
   // This is a bit of a kludge but keeping a sufficient number
   // of static buffers allows us to use format functions multiple
   // times in printf
   static char   *buf_set[MAX_FMT_BUFS];
   static size_t  buflen[MAX_FMT_BUFS];
   static int     next_buf = 0;

   char **bufp = &buf_set[next_buf];
   size_t *blenp = &buflen[next_buf];
   next_buf = (next_buf + 1) % MAX_FMT_BUFS;

   if (*bufp == NULL) {
      *bufp = xmalloc(len);
      *blenp = len;
   }

   while (len > *blenp) {
      *blenp *= 2;
      *bufp = xrealloc(*bufp, *blenp);
   }

   return *bufp;
}

int next_power_of_2(int n)
{
   n--;
   n |= n >> 1;
   n |= n >> 2;
   n |= n >> 4;
   n |= n >> 8;
   n |= n >> 16;
   n++;
   return n;
}

int ilog2(int64_t n)
{
   if (n <= 1)
      return 1;
   else {
      int r = 0;
      int64_t c = 1;
      while (c < n) {
         r += 1;
         c *= 2;
      }
      return r;
   }
}

int64_t ipow(int64_t x, int64_t y)
{
   int64_t r = 1;
   while (y) {
      if (y & 1)
         r *= x;
      y >>= 1;
      x *= x;
   }
   return r;
}

void *mmap_guarded(size_t sz, const char *tag)
{
   return NULL;
}

int checked_sprintf(char *buf, int len, const char *fmt, ...)
{
   assert(len > 0);

   va_list ap;
   va_start(ap, fmt);

   const int nbytes = vsnprintf(buf, len, fmt, ap);
   if (nbytes >= len)
      fatal_trace("checked_sprintf requires %d bytes but have %d", nbytes, len);

   va_end(ap);

   return nbytes;
}

text_buf_t *tb_new(void)
{
   text_buf_t *tb = xmalloc(sizeof(text_buf_t));
   tb->alloc = 256;
   tb->len   = 0;
   tb->buf   = xmalloc(tb->alloc);

   tb->buf[0] = '\0';

   return tb;
}

void tb_free(text_buf_t *tb)
{
   free(tb->buf);
   free(tb);
}

void _tb_cleanup(text_buf_t **tb)
{
    tb_free(*tb);
}

void tb_printf(text_buf_t *tb, const char *fmt, ...)
{
   int nchars, avail;
   for (;;) {
      va_list ap;
      va_start(ap, fmt);

      avail  = tb->alloc - tb->len;
      nchars = vsnprintf(tb->buf + tb->len, avail, fmt, ap);

      va_end(ap);

      if (nchars < avail)
         break;

      tb->alloc *= 2;
      tb->buf = xrealloc(tb->buf, tb->alloc);
   }

   tb->len += nchars;
}

void tb_append(text_buf_t *tb, char ch)
{
   if (tb->len + 1 >= tb->alloc) {
      tb->alloc *= 2;
      tb->buf = xrealloc(tb->buf, tb->alloc);
   }

   tb->buf[(tb->len)++] = ch;
   tb->buf[tb->len] = '\0';
}

char *tb_claim(text_buf_t *tb)
{
   char *buf = tb->buf;
   tb->buf = NULL;
   return buf;
}

const char *tb_get(text_buf_t *tb)
{
   return tb->buf;
}

void tb_rewind(text_buf_t *tb)
{
   tb->len = 0;
   tb->buf[0] = '\0';
}

void tb_backup(text_buf_t *tb, unsigned n)
{
   tb->len = n > tb->len ? 0 : tb->len - n;
   tb->buf[tb->len] = '\0';
}

void _local_free(void *ptr)
{
   free(*(void **)ptr);
}

void set_message_style(message_style_t style)
{
   message_style = style;

   if (style == MESSAGE_COMPACT)
      want_color = false;
}

#ifndef __MINGW32__
static unsigned tv2ms(struct timeval *tv)
{
   return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}
#endif

void nvc_rusage(nvc_rusage_t *ru)
{
#ifndef __MINGW32__
   static struct rusage last;

   struct rusage sys;
   if (getrusage(RUSAGE_SELF, &sys) < 0)
      fatal_errno("getrusage");

   const unsigned utime = tv2ms(&(sys.ru_utime)) - tv2ms(&(last.ru_utime));
   const unsigned stime = tv2ms(&(sys.ru_stime)) - tv2ms(&(last.ru_stime));

   ru->ms = utime + stime;

#ifdef __APPLE__
   const int rss_units = 1024;
#else
   const int rss_units = 1;
#endif

   ru->rss = sys.ru_maxrss / rss_units;

   last = sys;
#else

#endif
}

void run_program(const char *const *args, size_t n_args)
{
   const bool quiet = (getenv("NVC_LINK_QUIET") != NULL);

   if (!quiet) {
      for (size_t i = 0; i < n_args; i++)
         printf("%s%c", args[i], (i + 1 == n_args ? '\n' : ' '));
      fflush(stdout);
   }

#if defined __CYGWIN__ || defined __MINGW32__
   int status = spawnv(_P_WAIT, args[0], (char *const *)args);
   if (status != 0)
      fatal("%s failed with status %d", args[0], status);
#else  // __CYGWIN__
   pid_t pid = fork();
   if (pid == 0) {
      execv(args[0], (char *const *)args);
      fatal_errno("execv");
   }
   else if (pid > 0) {
      int status;
      if (waitpid(pid, &status, 0) != pid)
         fatal_errno("waitpid");

      if (WEXITSTATUS(status) != 0)
         fatal("%s failed with status %d", args[0], WEXITSTATUS(status));
   }
   else
      fatal_errno("fork");
#endif  // __CYGWIN__
}

void file_read_lock(int fd)
{
#ifdef __MINGW32__

#else
   if (flock(fd, LOCK_SH) < 0)
      fatal_errno("flock");
#endif
}

void file_write_lock(int fd)
{
#ifdef __MINGW32__

#else
   if (flock(fd, LOCK_EX) < 0)
      fatal_errno("flock");
#endif
}

void file_unlock(int fd)
{
#ifdef __MINGW32__

#else
   if (flock(fd, LOCK_UN) < 0)
      fatal_errno("flock");
#endif
}

void *map_file(int fd, size_t size)
{
   return NULL;
}

void unmap_file(void *ptr, size_t size)
{
#ifdef __MINGW32__

#else
   munmap(ptr, size);
#endif
}

void make_dir(const char *path)
{
#ifdef __MINGW32__

#else
   if (mkdir(path, 0777) != 0 && errno != EEXIST)
      fatal_errno("mkdir: %s", path);
#endif
}

uint64_t get_timestamp_us()
{
#if defined __MACH__ && !defined CLOCK_MONOTONIC
   clock_serv_t cclock;
   mach_timespec_t mts;
   host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cclock);
   clock_get_time(cclock, &mts);
   mach_port_deallocate(mach_task_self(), cclock);
   return (mts.tv_nsec / 1000) + (mts.tv_sec * 1000 * 1000);
#elif defined _WIN32
   return 0;  // TODO
#else
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      fatal_errno("clock_gettime");
   return (ts.tv_nsec / 1000) + (ts.tv_sec * 1000 * 1000);
#endif
}

const char *safe_symbol(const char *text)
{
   // Return a string that is safe to use as a symbol name on this platform
#if defined _WIN32 || defined __CYGWIN__
   if (strpbrk(text, "()\"[]*+=") == NULL)
      return text;

   text_buf_t *tb = tb_new();

   for (const char *p = text; *p != '\0' && p - text < 240; p++) {
      switch (*p) {
      case '(': tb_printf(tb, "_lp_"); break;
      case ')': tb_printf(tb, "_rp_"); break;
      case '"': tb_printf(tb, "_q_"); break;
      case '[': tb_printf(tb, "_ls_"); break;
      case ']': tb_printf(tb, "_rs_"); break;
      case '*': tb_printf(tb, "_mult_"); break;
      case '+': tb_printf(tb, "_plus_"); break;
      case '=': tb_printf(tb, "_eq_"); break;
      default:
         tb_append(tb, *p);
      }
   }

   return tb_get(tb);
#else
   return text;
#endif
}

