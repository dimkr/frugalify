#include <unistd.h>
#include <linux/loop.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mount.h>

#ifndef LOOP_CTL_GET_FREE
#   define LOOP_CTL_GET_FREE 0x4C82
#endif

#define MAXSFS 8

#ifdef HAVE_AUFS
#   define FS "aufs"
#   define FSOPTS_HEAD "br=/save"
#else
#   define FS "overlay"
#   define FSOPTS_HEAD "upperdir=/save,workdir=/.work,lowerdir="
#endif

static inline void do_autoclose(void *fdp)
{
	if (*(int *)fdp != -1)
		close(*(int *)fdp);
}

#define autoclose __attribute__((cleanup(do_autoclose)))

// use of sprintf() can double the executable size
static char *itoa(char *s, int i)
{
    if (i >= 10)
        s = itoa(s, i / 10);

    *s = '0' + i % 10;
    ++s;
    *s = '\0';
    return s;
}

static char *get_lo_path(const int i)
{
    static char loop[sizeof("/save/dev/loop0")] = "/save/dev/loop";

    itoa(loop + sizeof("/save/dev/loop") - 1, i);
    return loop;
}

static const char *losetup(const char *sfs, const int i)
{
    struct stat stbuf;
    struct loop_info64 info = {.lo_flags = LO_FLAGS_READ_ONLY};
    const char *loop;
    autoclose int loopfd = -1, sfsfd;
    
    sfsfd = open(sfs, O_RDWR);
    if (sfsfd < 0)
        return NULL;

    if (fstat(sfsfd, &stbuf) < 0)
        return NULL;

    loop = get_lo_path(i);

    loopfd = open(loop, O_RDWR);
    if (loopfd < 0)
        return NULL;

    if (ioctl(loopfd, LOOP_SET_FD, sfsfd) < 0)
        return NULL;

#ifdef HAVE_STRLCPY
    strlcpy(info.lo_file_name, sfs, sizeof(info.lo_file_name));
#else
    strncpy((char *)info.lo_file_name, sfs, sizeof(info.lo_file_name));
    info.lo_file_name[sizeof(info.lo_file_name) - 1] = '\0';
#endif
    info.lo_sizelimit = (uint64_t)stbuf.st_size;
    if (ioctl(loopfd, LOOP_SET_STATUS64, &info) < 0)
        return NULL;

    return loop;
}

static void losetup_d(const int i)
{
    autoclose int loopfd = -1;
    
    loopfd = open(get_lo_path(i), O_RDWR);
    if (loopfd >= 0)
        ioctl(loopfd, LOOP_CLR_FD, 0);
}

int main(int argc, char *argv[])
{
    static const char *dirs[] = {
        "/save",
#ifndef HAVE_AUFS
        "/.work",
#endif
        "/save/.pup_new",
        "/save/dev",
#ifdef HAVE_AUFS
        "/save/initrd",
#endif
    };
    static char sfspath[MAXSFS][128], br[1024] = FSOPTS_HEAD;
    struct dirent ent[MAXSFS];
    char *sfs[MAXSFS] = {NULL}, sfsmnt[sizeof("/save/.sfs0")] = "/save/.sfs";
    const char *loop;
    size_t len, brlen = sizeof(FSOPTS_HEAD) - 1;
    ssize_t out;
    DIR *root;
    struct dirent *pent;
    unsigned int nsfs = 0, i;
    int ro = 0;

    // protect against accidental click
    if (getpid() != 1)
        return EXIT_FAILURE;

    mount(NULL, "/", NULL, MS_REMOUNT | MS_NOATIME, NULL);

    root = opendir("/");
    if (!root)
        return EXIT_FAILURE;
    
    // create a list of all SFS files under /
    while ((readdir_r(root, &ent[nsfs], &pent) == 0) && (pent != NULL)) {
        len = strlen(pent->d_name);
        if ((len <= sizeof(".sfs") - 1) || 
            (memcmp(&pent->d_name[len - 4], ".sfs", sizeof(".sfs") - 1) != 0))
            continue;

        sfs[nsfs] = pent->d_name;
        switch (pent->d_type) {
        case DT_REG:
            break;

        case DT_LNK:
            out = readlink(pent->d_name,
                           sfspath[nsfs],
                           sizeof(sfspath[nsfs]) - 1);
            if (out <= 0)
                continue;
            sfspath[nsfs][out] = '\0';
            sfs[nsfs] = sfspath[nsfs];
            break;

        default:
            continue;
        }

        ++nsfs;
        if (nsfs == MAXSFS)
            break;
    }
    closedir(root);

    if (!sfs[0])
        return EXIT_FAILURE;

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        if ((mkdir(dirs[i], 0755) == 0) || (errno == EEXIST))
            continue;

        if ((errno != EROFS) || ro)
            return EXIT_FAILURE;

        // we need some writable file system as the upper layer, so we mount
        // a tmpfs; we assume that /save and /.work already exist if we're
        // booting from optical media or from a corrupt file system mounted
        // read-only, and we assume that only the first mkdir() can return EROFS
        if (mount("save", "/save", "tmpfs", 0, NULL) < 0)
            return EXIT_FAILURE;

        ro = 1;
    }

    // mount a devtmpfs so we have the loop%d device nodes
    if (mount("dev", "/save/dev", "devtmpfs", 0, NULL) < 0)
        return EXIT_FAILURE;
    
    for (i = 0; (i < nsfs) && (brlen < sizeof(br)); ++i) {
        itoa(sfsmnt + sizeof("/save/.sfs") - 1, i);

        if ((mkdir(sfsmnt, 0755) < 0) && (errno != EEXIST))
            continue;

        // bind the SFS to a loop device
        loop = losetup(sfs[i], i);
        if (!loop) {
            rmdir(sfsmnt);
            continue;
        }

        // mount the loop device
        if (mount(loop, sfsmnt, "squashfs", 0, NULL) < 0) {
            losetup_d(i);
            rmdir(sfsmnt);
            continue;
        }

#ifndef HAVE_AUFS
        if (i == 0)
            goto cpy;
#endif

        br[brlen] = ':';
        ++brlen;

cpy:
        memcpy(&br[brlen], sfsmnt, sizeof("/save/.sfs0") - 1);
        brlen += sizeof("/save/.sfs0") - 1;
    }
    br[brlen] = '\0';
    
    // we no longer need /dev
    umount2("/save/dev", MNT_DETACH);
    rmdir("/save/dev");

#ifndef HAVE_AUFS
    if (ro && mount("work", "/.work", "tmpfs", 0, NULL) < 0)
        return EXIT_FAILURE;
#endif

    // mount a union file system with the SFS mount points and /save on top
    if (mount(FS, "/save/.pup_new", FS, MS_NOATIME, br) < 0)
        return EXIT_FAILURE;
    
#ifdef HAVE_AUFS
    // give processes running with the union file system as / a directory
    // outside of the union file system that can be used to add aufs branches
    if (mount("/", "/save/.pup_new/initrd", NULL, MS_BIND, NULL) < 0)
        return EXIT_FAILURE;
#endif

    if (chdir("/save/.pup_new") < 0)
        return EXIT_FAILURE;

    // make the union file system the the file system root, or the file system
    // root for the real init and its children
#ifndef HAVE_AUFS
    if (mount(".", "/", FS, MS_MOVE, NULL) < 0)
        return EXIT_FAILURE;
#endif

    if (chroot(".") < 0)
        return EXIT_FAILURE;

    if (chdir("/") < 0)
        return EXIT_FAILURE;

    // run the real init under the new root file system
    execl("/sbin/init", "init", "initrd_full_install", NULL);

    return EXIT_FAILURE;
}