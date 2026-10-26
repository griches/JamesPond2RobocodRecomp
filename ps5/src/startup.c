/*
 * James Pond 2 for the PlayStation 5 - what runs before the game's main.
 *
 * The game's main is the recomp's own, so the console's start-up is a
 * constructor: standard error into klog and the shell's splash dismissed,
 * where the game keeps its settings and saves, and test options from env.txt
 * and args.txt. The player's disk image goes in the title's folder
 * (/data/homebrew/<TITLE_ID>, the title's /app0): the game's first-run page
 * finds it there, and asks for it until it is.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL_stdinc.h>
#include <ps5platform/memory.h>

#include "platform.h"

#define TITLE_DIR "/app0"
#define USER_DIR TITLE_DIR "/userdata"
#define CACHE_DIR TITLE_DIR "/cache"

/* /app0/env.txt: KEY=VALUE lines for a test run, since the console starts a
 * title with no arguments or environment. Set for the C library's getenv and in
 * SDL's copy of the environment. */
static void
set_variable(const char *name, const char *value)
{
   setenv(name, value, 1);
   SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value, true);
}

static void
load_env_file(void)
{
   char line[1024];
   FILE *file = fopen(TITLE_DIR "/env.txt", "r");

   if (file == NULL)
      return;
   while (fgets(line, sizeof(line), file) != NULL) {
      char *equals, *end = line + strcspn(line, "\r\n");

      *end = '\0';
      if (line[0] == '#' || (equals = strchr(line, '=')) == NULL || equals == line)
         continue;
      *equals = '\0';
      set_variable(line, equals + 1);
      fprintf(stderr, "env.txt: %s=%s\n", line, equals + 1);
   }
   fclose(file);
}

/* The CRT's call to main (--wrap=main, ps5/tools/link-title.sh): one argument a
 * line from /app0/args.txt for a test run (--load, --headless, ...), then any
 * argument the console passed */
int __real_main(int argc, char **argv, char **envp);

int
__wrap_main(int argc, char **argv, char **envp)
{
   static char lines[48][256];
   char *args[64];
   int count = 0;

   args[count++] = argc > 0 ? argv[0] : "eboot.bin";
   FILE *file = fopen(TITLE_DIR "/args.txt", "r");
   for (int i = 0; file != NULL && i < 48 && fgets(lines[i], sizeof(lines[i]), file) != NULL; i++) {
      lines[i][strcspn(lines[i], "\r\n")] = '\0';
      if (lines[i][0] == '\0' || lines[i][0] == '#')
         continue;
      fprintf(stderr, "args.txt: %s\n", lines[i]);
      args[count++] = lines[i];
   }
   if (file != NULL)
      fclose(file);
   for (int i = 1; i < argc && count < 63; i++)
      args[count++] = argv[i];
   args[count] = NULL;
   const int status = __real_main(count, args, envp);
   say("ends (main returned %d)", status);
   return status;
}

/* A watchdog: the title's memory every two seconds into klog when it moves by
 * 64 MiB, and an end to the title before the console itself runs short
 * (JP2_PS5_MEMORY_FLOOR_MB of direct memory left, default 1024; 96 MiB of
 * flexible memory). */
static void *
memory_guard(void *unused)
{
   const char *floor_text = getenv("JP2_PS5_MEMORY_FLOOR_MB");
   const uint64_t floor = (floor_text ? strtoull(floor_text, NULL, 10) : 1024u) << 20;
   uint64_t last = 0;

   (void)unused;
   for (;;) {
      struct ps5_memory_stats memory;
      if (ps5_memory_query(&memory)) {
         const uint64_t used = memory.total_bytes - memory.free_bytes;
         if (last == 0 || used > last + (64u << 20) || used + (64u << 20) < last) {
            say("[memory] direct free %llu MiB of %llu, flexible %llu MiB",
                (unsigned long long)(memory.free_bytes >> 20),
                (unsigned long long)(memory.total_bytes >> 20),
                (unsigned long long)(memory.flexible_bytes >> 20));
            last = used;
         }
         if (memory.flexible_bytes != 0 && memory.flexible_bytes < (96u << 20)) {
            say("[memory] guard: only %llu MiB of flexible memory left, ending the title",
                (unsigned long long)(memory.flexible_bytes >> 20));
            _exit(71);
         }
         if (memory.free_bytes < floor) {
            say("[memory] guard: only %llu MiB of direct memory left, ending the title",
                (unsigned long long)(memory.free_bytes >> 20));
            _exit(70);
         }
      }
      sleep(2);
   }
   return NULL;
}

__attribute__((constructor)) static void
ps5_startup(void)
{
   platform_init(PS5_TITLE_NAME);
   mkdir(USER_DIR, 0777);
   mkdir(CACHE_DIR, 0777);
   /* The game keeps its settings and saves in SDL_GetPrefPath, which is
    * $XDG_DATA_HOME/<org>/<app> here, and the driver its shader cache in
    * $XDG_CACHE_HOME. A title sees its own folder (/app0, which is
    * /data/homebrew/<TITLE_ID>) and nothing else of /data, so that is where they
    * go. Set for the C library's getenv and in SDL's own copy of the
    * environment: the console's libc keeps what setenv sets apart from the
    * `environ` SDL copies. */
   set_variable("HOME", USER_DIR);
   set_variable("XDG_CONFIG_HOME", USER_DIR);
   set_variable("XDG_DATA_HOME", USER_DIR);
   set_variable("XDG_CACHE_HOME", CACHE_DIR);
   load_env_file();

   pthread_t guard;
   if (pthread_create(&guard, NULL, memory_guard, NULL) == 0)
      pthread_detach(guard);
}
