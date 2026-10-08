// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from holdmysocks/mcla-recomp b5765a912a8efc65a3fc1753237831c9c229a4ab.
// Keep the driver platform's own link/symlink/readlink/mkstemp bindings.
/* MCLA's installed-title import checks found libc functions that link but
 * resolve to NULL on firmware 13.42. These remaining replacements are bound
 * locally by build_title.sh; the pinned platform library supplies the rest.
 */

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

int tt_title_isatty(int descriptor) {
  (void)descriptor;
  errno = ENOTTY;
  return 0;
}

long tt_title_pathconf(const char* path, int name) {
  (void)path;
  switch (name) {
    case _PC_NAME_MAX:
      return 255;
    case _PC_PATH_MAX:
      return 1024;
    default:
      errno = EINVAL;
      return -1;
  }
}

/* Three more that the title link has no definition for at all (the payload
 * link gets them from its SDK's static libc): the linker reports them as
 * undefined once the whole runtime and the game are linked in. */

#include <time.h>

int tt_title_getresuid(uid_t* real, uid_t* effective, uid_t* saved) {
  const uid_t id = getuid();
  if (real) *real = id;
  if (effective) *effective = id;
  if (saved) *saved = id;
  return 0;
}

int tt_title_getresgid(gid_t* real, gid_t* effective, gid_t* saved) {
  const gid_t id = getgid();
  if (real) *real = id;
  if (effective) *effective = id;
  if (saved) *saved = id;
  return 0;
}

/* Broken-down UTC time to seconds since 1970, without time zones. The day
 * count is the usual civil-calendar formula with the year starting in March. */
time_t tt_title_timegm(struct tm* value) {
  long year = value->tm_year + 1900L;
  long month = value->tm_mon + 1L;
  if (month <= 2) {
    year -= 1;
    month += 12;
  }
  const long era = (year >= 0 ? year : year - 399) / 400;
  const long year_of_era = year - era * 400;
  const long day_of_year = (153 * (month - 3) + 2) / 5 + value->tm_mday - 1;
  const long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  const long days = era * 146097 + day_of_era - 719468;
  return (time_t)days * 86400 + value->tm_hour * 3600L + value->tm_min * 60L + value->tm_sec;
}

