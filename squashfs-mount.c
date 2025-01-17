#define _GNU_SOURCE

#include <err.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/loop.h>

#include <libmount/libmount.h>

#define exit_with_error(...)                                                   \
  do {                                                                         \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

static void help(char const *argv0) {
  fputs("Usage: ", stderr);
  fputs(argv0, stderr);
  fputs(" <squashfs file> <mountpoint> [--offset=4096] <command> [args...]\n\n "
        " The --offset=4096 option "
        "translates to an offset=4096 mount option.\n",
        stderr);
  exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
  struct libmnt_context *cxt;
  uid_t uid = getuid();
  char *program = argv[0];
  int has_offset = 0;
  struct stat mnt_stat;
  char err_buf[BUFSIZ] = {0};

  argv++;
  argc--;

  // Only parse flags up to position 3, we don't want to do arg parsing of the
  // command that is going to be executed.
  for (int i = 0; i < argc && i < 3; ++i) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      help(program);
    } else if (strcmp(argv[i], "-v") == 0 ||
               strcmp(argv[i], "--version") == 0) {
      puts(VERSION);
      exit(EXIT_SUCCESS);
    }
  }

  // We need at least 3 args
  if (argc < 3)
    help(program);

  char *squashfs_file = *argv++;
  argc--;

  char *mountpoint = *argv++;
  argc--;

  // The optional offset toggle.
  if (strcmp(*argv, "--offset=4096") == 0) {
    has_offset = 1;
    argv++;
    argc--;
  }

  if (argc == 0)
    help(program);

  // Check that the mount point exists.
  int mnt_status = stat(mountpoint, &mnt_stat);

  if (mnt_status)
    exit_with_error("Invalid mount point \"%s\"", mountpoint);

  if (!S_ISDIR(mnt_stat.st_mode))
    exit_with_error("Invalid mount point \"%s\" is not a directory",
                    mountpoint);

  // Check that the input squashfs file exists.
  int squashfs_fd = open(squashfs_file, O_RDONLY);

  if (squashfs_fd < 0)
    exit_with_error("Could not open squashfs file \"%s\"\n", squashfs_file);

  int sqsh_status = fstat(squashfs_fd, &mnt_stat);

  if (sqsh_status)
    exit_with_error("Invalid squashfs image file \"%s\"\n", squashfs_file);

  if (!S_ISREG(mnt_stat.st_mode))
    exit_with_error("Requested squashfs image \"%s\" is not a file\n",
                    squashfs_file);

  if (unshare(CLONE_NEWNS) != 0)
    exit_with_error("Failed to unshare the mount namespace\n");

  if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) != 0)
    exit_with_error("Failed to remount \"/\" with MS_SLAVE\n");

  // Set real user to root before creating the mount context, otherwise it
  // fails.
  if (setreuid(0, 0) != 0)
    exit_with_error("Failed to setreuid\n");

  cxt = mnt_new_context();

  if (mnt_context_disable_mtab(cxt, 1) != 0)
    exit_with_error("Failed to disable mtab\n");

  if (mnt_context_set_fstype(cxt, "squashfs") != 0)
    exit_with_error("Failed to set fstype to squashfs\n");

  char const *mount_options =
      has_offset ? "loop,nosuid,nodev,ro,offset=4096" : "loop,nosuid,nodev,ro";

  if (mnt_context_append_options(cxt, mount_options) != 0)
    exit_with_error("Failed to set mount options\n");

  if (mnt_context_set_source(cxt, squashfs_file) != 0)
    exit_with_error("Failed to set source\n");

  if (mnt_context_set_target(cxt, mountpoint) != 0)
    exit_with_error("Failed to set target\n");

  if (flock(squashfs_fd, LOCK_EX) != 0)
    exit_with_error("Could not acquire a lock on the squashfs file\n");

  int mount_exit_code = mnt_context_mount(cxt);
  if (mount_exit_code != 0) {
    mnt_context_get_excode(cxt, mount_exit_code, err_buf, sizeof(err_buf));
    const char *tgt = mnt_context_get_target(cxt);
    if (*err_buf != '\0' && tgt != NULL)
      exit_with_error("%s: %s\n", tgt, err_buf);
    exit_with_error("Failed to mount\n");
  }

  if (flock(squashfs_fd, LOCK_UN) != 0)
    exit_with_error("Could not release the lock on the squashfs file\n");

  if (close(squashfs_fd) != 0)
    exit_with_error("Could not close squashfs file\n");

  if (setresuid(uid, uid, uid) != 0)
    exit_with_error("setresuid failed\n");

  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    exit_with_error("PR_SET_NO_NEW_PRIVS failed\n");

  return execvp(argv[0], argv);
}
