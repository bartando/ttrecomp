// SPDX-License-Identifier: GPL-3.0-or-later
// elfldr payload that launches or closes the installed title, so test runs
// don't need someone at the console. Build with -DTITLE_CTL_LAUNCH or
// -DTITLE_CTL_KILL; payloads receive no arguments.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef TITLE_ID
#define TITLE_ID "PPSA99782"
#endif

// Layout used by ps5-payload-dev/websrv for sceLncUtilLaunchApp.
typedef struct {
  uint32_t size;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} launch_param_t;

int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(uint32_t*);
int sceLncUtilLaunchApp(const char* title_id, const char* argv[], launch_param_t* param);
int sceLncUtilGetAppId(const char* title_id);
int sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char* title_id);
int sceLncUtilKillApp(uint32_t app_id);

#if defined(TITLE_CTL_LAUNCH)
// ShadowMountPlus mounts /data/homebrew/<id> onto /system_ex/app/<id> from a
// ShellCore launch hook and releases it when the game exits. That hook can die
// (the launch then fails with 0x80940033 / CE-105773-3), so ask its loopback
// API for the mount explicitly. Prints the HTTP status line and JSON body.
static void request_mount(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("title_ctl: mount socket");
    return;
  }
  struct timeval timeout = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_port = htons(10101);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
    perror("title_ctl: ShadowMount API connect");
    close(fd);
    return;
  }
  const char body[] = "{\"title_id\":\"" TITLE_ID "\"}";
  char request[512];
  int length = snprintf(request, sizeof(request),
                        "POST /api/v1/games/mount HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        strlen(body), body);
  if (send(fd, request, (size_t)length, 0) != length) {
    perror("title_ctl: ShadowMount API send");
    close(fd);
    return;
  }
  char reply[2048];
  size_t used = 0;
  ssize_t got;
  while (used < sizeof(reply) - 1 && (got = recv(fd, reply + used, sizeof(reply) - 1 - used, 0)) > 0) {
    used += (size_t)got;
  }
  reply[used] = 0;
  close(fd);
  const char* line_end = strstr(reply, "\r\n");
  const char* json = strstr(reply, "\r\n\r\n");
  printf("title_ctl: mount %.*s %s\n", line_end ? (int)(line_end - reply) : 0, reply,
         json ? json + 4 : "");
}
#endif

int main(void) {
  sceUserServiceInitialize(0);
#if defined(TITLE_CTL_LAUNCH)
  request_mount();
  launch_param_t param = {sizeof(param), 0, 0, 0, 0};
  int status = sceUserServiceGetForegroundUser(&param.user_id);
  if (status) {
    printf("title_ctl: foreground user failed 0x%08x\n", (unsigned)status);
    return 1;
  }
  const char* argv[] = {0};
  status = sceLncUtilLaunchApp(TITLE_ID, argv, &param);
  // Positive results are the new app id.
  printf("title_ctl: launch %s -> 0x%08x\n", TITLE_ID, (unsigned)status);
  return status < 0;
#elif defined(TITLE_CTL_KILL)
  // The title can come back under a new app id right after launch, and
  // sceLncUtilGetAppId may keep returning the old one. Kill the running big
  // app too, but only after confirming it is this title.
  int killed = 0;
  int app_ids[2] = {sceLncUtilGetAppIdOfRunningBigApp(), sceLncUtilGetAppId(TITLE_ID)};
  for (int i = 0; i < 2; ++i) {
    if (app_ids[i] <= 0 || (i == 1 && app_ids[1] == app_ids[0])) continue;
    char title[32] = {0};
    if (i == 0 && (sceLncUtilGetAppTitleId((uint32_t)app_ids[0], title) != 0 ||
                   strncmp(title, TITLE_ID, sizeof(TITLE_ID) - 1) != 0)) {
      continue;
    }
    int status = sceLncUtilKillApp((uint32_t)app_ids[i]);
    printf("title_ctl: kill %s app 0x%08x -> 0x%08x\n", TITLE_ID, (unsigned)app_ids[i],
           (unsigned)status);
    killed += status == 0;
  }
  if (!killed) printf("title_ctl: %s not running\n", TITLE_ID);
  return 0;
#else
#error define TITLE_CTL_LAUNCH or TITLE_CTL_KILL
#endif
}
