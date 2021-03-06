/**
 * Offline ( when SELinux is disabled ) restorecon
 * Uses both matchpathcon & setfilecon ( xattr ) to set contexts
 * It's usefull to do a restorecon BEFORE reboot to avoid .autorelabel
 * (C) Ivan Agarkov, 2017
 **/

#define _GNU_SOURCE
#include <stdio.h>
#include <selinux/selinux.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dirent.h>
#include <errno.h>
#include "queue.h"
#define MAX_CONTEXT_SIZE 255
#define DEFAULT_LABEL "system_u:object_r:unlabeled_t"

int get_mode(char *path) {
  struct stat fstat;
  bzero(&fstat, sizeof(stat));
  if(lstat(path, &fstat) == 0) {
    return fstat.st_mode;
  }
  return -1;
}

int restorecon(char *path, int verbose) {
  int mode;
  char **conptr = malloc(sizeof(char *));
  char context[MAX_CONTEXT_SIZE];
  if((mode = get_mode(path)) > 0) {
    if(matchpathcon(path, mode, conptr) != 0) {
      strncpy(context, DEFAULT_LABEL, sizeof(context));
      fprintf(stderr, "Warning: no default context for %s\n", path);
    } else {
      strncpy(context, *conptr, sizeof(context));
      freecon(*conptr);
    }
    if(setfilecon(path, context) == 0) {
      if(verbose) {
        printf("%s: %s\n", path, context);
      }
    } else {
      mode = -1;
    }
  }
  if(mode < 0) {
    fprintf(stderr, "Error: %s for %s\n", strerror(errno), path);
  }
  free(conptr);
  return mode;
}

// DEPRECATED
//int recursecon(char *path, int verbose) {
//  int mode = restorecon(path, verbose);
//  if(mode < 0) {
//      return -1;
//  }
//  if(S_ISLNK(mode)) {
//      return 0;
//  }
//  if(S_ISDIR(mode)) {
//      struct dirent *dir;
//      DIR *dp;
//      if(!(dp = opendir(path))) {
//          return -1;
//      }
//      FQ *root = 0, *q = 0;
//      while((dir = readdir(dp))) {
//          if(strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
//              continue;
//          }
//          char *path_next = (char *)malloc(strlen(path)+strlen(dir->d_name)+2);
//          int fl = (path[strlen(path)] == '/');
//          sprintf(path_next, (fl)?"%s%s":"%s/%s", path, dir->d_name);
//          if(!root) {
//              root = create_queue(path_next);
//              q = root;
//          } else {
//              q = append_queue(q, path_next);
//          }
//          free(path_next);
//      }
//      closedir(dp);
//      q = root;
//      while(q) {
//          int mode = get_mode(q->path);
//          if(!S_ISLNK(mode)) {
//              if(S_ISDIR(mode))  {
//                  recursecon(q->path, verbose);
//              } else {
//                  restorecon(q->path, verbose);
//              }
//          } else {
//              printf("%s is a link, skipping!\n", q->path);
//          }
//          q = q->next;
//      }
//      free_queue(root);
//  }
//  return 0;
//}

char *lsdir(char *path) {
  static char *ret = 0;
  static char *oldpath = 0;
  static DIR *dp = 0;
  if(ret) {
    free(ret);
    ret = 0;
  }
  if(!oldpath || strcmp(path, oldpath) != 0) {
    if(dp) {
      closedir(dp);
      dp = 0;
    }
    if(oldpath) {
      free(oldpath);
      oldpath = 0;
    }
    oldpath = strdup(path);
    dp = opendir(oldpath);
  }
  struct dirent *d;
  while((d = readdir(dp))) {
    if(strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0) {
      continue;
    }
    int sz = strlen(oldpath)+2+strlen(d->d_name);
    ret = malloc(sz);
    int fl = oldpath[strlen(oldpath)-1] == '/';
    snprintf(ret, sz, (fl)?"%s%s":"%s/%s", oldpath, d->d_name);
    return ret;
  }
  // latest - free all
  if(dp) {
    closedir(dp);
    dp = 0;
  }
  if(oldpath) {
    free(oldpath);
    oldpath = 0;
  }
  return 0;
}


int dircon(char *path, int verbose) {
  FQ *root = create_queue(path);
  FQ *next = 0, *q = 0, *q2 = 0;
  int count = 0;
  while(root) {
    q = root;
    while(q) {
      int mode = restorecon(q->path, verbose);
      count++;
      if(mode > 0) {
        if(!S_ISLNK(mode) && S_ISDIR(mode)) {
          char *first = lsdir(q->path);
          if(first) {
            if(!next) {
              q2 = next = create_queue(first);
            }
            while((first = lsdir(q->path))) {
              q2 = append_queue(q2, first);
            }
          }
        }
      }
      q = q->next;
    }
    free_queue(root);
    root = next; // hope next is not 0;
    next = 0;
  }
  return count;
}

inline int restore(char *path, int recurse, int verbose) {
  return (recurse)?dircon(path, verbose):restorecon(path, verbose);
}

int print_help() {
  printf("Usage: %s [-Rvh] <path1>...[pathN]\n", program_invocation_name);
  printf("This program restores SELinux file context ( xattr wrapped fields ) without enabling SELinux on the host\n");
  printf("  -R - recursive restore SELinux labels\n");
  printf("  -v - be verbose\n");
  printf("  -i - set ionice to idle/nice to 20 to prevent cpu load\n");
  printf(" -h/-? - see this help\n");
  return 0;
}

enum {
  IOPRIO_CLASS_NONE,
  IOPRIO_CLASS_RT,
  IOPRIO_CLASS_BE,
  IOPRIO_CLASS_IDLE,
};

enum {
  IOPRIO_WHO_PROCESS = 1,
  IOPRIO_WHO_PGRP,
  IOPRIO_WHO_USER,
};

#define IOPRIO_CLASS_SHIFT  13

void set_idle() {
  if(syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE|IOPRIO_CLASS_BE<<IOPRIO_CLASS_SHIFT) < 0) {
    fprintf(stderr, "ioprio_set(): %s\n", strerror(errno));
    exit(-1);
  }
  // nice
  if(setpriority(PRIO_PROCESS, 0, 19) < 0) {
    fprintf(stderr, "setpriority(): %s\n", strerror(errno));
  }

}

int main(int argc, char *argv[]) {
  int o;
  int verbose = 0;
  int recurse = 0;
  int idle = 0;

  while((o = getopt(argc, argv, "Rvhi")) > 0) {
    if(o == 'h') {
      return print_help();
    } else if(o == 'R') {
      recurse = 1;
    } else if(o == 'v') {
      verbose = 1;
    } else if(o == 'i') {
      idle = 1;
    } else {
      return print_help();
    }
  }
  if ( idle ) {
    set_idle();
    if (verbose) {
      printf("Setting iopriority to IDLE priority\n");
      printf("Setting priority to 20 priority\n");
    }
  }
  if(optind >= argc) {
    return print_help();
  }
  do {
    int d = restore(argv[optind++], recurse, verbose);
    printf("Total: %d\n", d);
  } while(optind < argc);
  return 0;
}

