#include "os.h"

int
osGetProcname(char *pname, int len)
{
    strncpy(pname, program_invocation_short_name, len);
    return 0;
}

int
osGetProcMemory(pid_t pid)
{
    int fd;
    long result;
    char *start, *entry, *last;
    const char delim[] = ":";
    char buf[2048];

    if (!g_fn.open || !g_fn.read || !g_fn.close) {
        return -1;
    }

    snprintf(buf, sizeof(buf), "/proc/%d/status", pid);
    if ((fd = g_fn.open(buf, O_RDONLY)) == -1) {
        DBG(NULL);
        return -1;
    }

    if (g_fn.read(fd, buf, sizeof(buf)) == -1) {
        DBG(NULL);
        g_fn.close(fd);
        return -1;
    }

    if ((start = strstr(buf, "VmSize")) == NULL) {
        DBG(NULL);
        g_fn.close(fd);
        return -1;        
    }
    
    entry = strtok_r(start, delim, &last);
    entry = strtok_r(NULL, delim, &last);
    if (entry == NULL) {
        DBG(NULL);
        g_fn.close(fd);
        return -1;        
    }
    
    if ((result = strtol(entry, NULL, 0)) == (long)0) {
        DBG(NULL);
        g_fn.close(fd);
        return -1;
    }
    
    g_fn.close(fd);
    return (int)result;
}

int
osGetNumThreads(pid_t pid)
{
    int fd, i;
    long result;
    char *entry, *last;
    const char delim[] = " ";
    char buf[1024];

    if (!g_fn.open || !g_fn.read || !g_fn.close) {
        return -1;
    }

    // Get the size of the file with stat, malloc buf then free
    snprintf(buf, sizeof(buf), "/proc/%d/stat", pid);
    if ((fd = g_fn.open(buf, O_RDONLY)) == -1) {
        DBG(NULL);
        return -1;
    }

    if (g_fn.read(fd, buf, sizeof(buf)) == -1) {
        DBG(NULL);
        g_fn.close(fd);
        return -1;
    }

    entry = strtok_r(buf, delim, &last);
    for (i = 1; i < 20; i++) {
        entry = strtok_r(NULL, delim, &last);
    }
    g_fn.close(fd);

    if ((result = strtol(entry, NULL, 0)) == (long)0) {
        DBG(NULL);
        return -1;
    }
    return (int)result;
}

int
osGetNumFds(pid_t pid)
{
    int nfile = 0;
    DIR * dirp;
    struct dirent * entry;
    char buf[1024];

    snprintf(buf, sizeof(buf), "/proc/%d/fd", pid);
    if ((dirp = opendir(buf)) == NULL) {
        DBG(NULL);
        return -1;
    }
    
    while ((entry = readdir(dirp)) != NULL) {
        if (entry->d_type != DT_DIR) {
            nfile++;
        }
    }

    closedir(dirp);
    return nfile - 1; // we opened one fd to read /fd :)
}

int
osGetNumChildProcs(pid_t pid)
{
    int nchild = 0;
    DIR * dirp;
    struct dirent * entry;
    char buf[1024];

    snprintf(buf, sizeof(buf), "/proc/%d/task", pid);
    if ((dirp = opendir(buf)) == NULL) {
        DBG(NULL);
        return -1;
    }
    
    while ((entry = readdir(dirp)) != NULL) {
            nchild++;
    }

    closedir(dirp);
    return nchild - 3; // Not including the parent proc itself and ., ..
}

int
osInitTSC(struct rtconfig_t *cfg)
{
    int fd;
    char *entry, *last;
    const char delim[] = ":";
    const char path[] = "/proc/cpuinfo";
    const char freqStr[] = "cpu MHz";
    char *buf;

    if (!g_fn.open || !g_fn.read || !g_fn.close) {
        return -1;
    }

    if ((fd = g_fn.open(path, O_RDONLY)) == -1) {
        DBG(NULL);
        return -1;
    }
    
    /*
     * Anecdotal evidence that there is a max size to proc entrires.
     * In any case this should be big enough.
     */    
    if ((buf = malloc(MAX_PROC)) == NULL) {
        DBG(NULL);
        g_fn.close(fd);
        return -1;
    }
    
    if (g_fn.read(fd, buf, MAX_PROC) == -1) {
        DBG(NULL);
        g_fn.close(fd);
        free(buf);
        return -1;
    }

    if (strstr(buf, "rdtscp") == NULL) {
        cfg->tsc_rdtscp = FALSE;
    } else {
        cfg->tsc_rdtscp = TRUE;
    }
    
    if (strstr(buf, "tsc_reliable") == NULL) {
        cfg->tsc_invariant = FALSE;
    } else {
        cfg->tsc_invariant = TRUE;
    }
    
    entry = strtok_r(buf, delim, &last);
    while (1) {
        if ((entry = strtok_r(NULL, delim, &last)) == NULL) {
            cfg->freq = (uint64_t)-1;
            break;
        }

        if (strcasestr((const char *)entry, freqStr) != NULL) {
            // The next token should be what we want
            if ((entry = strtok_r(NULL, delim, &last)) != NULL) {
                if ((cfg->freq = (uint64_t)strtoll(entry, NULL, 0)) == (long long)0) {
                    cfg->freq = (uint64_t)-1;
                }
                break;
            }
        }
    }
    
    g_fn.close(fd);
    free(buf);
    if (cfg->freq == (uint64_t)-1) {
        DBG(NULL);
        return -1;
    }
    return 0;
}

int
osIsFilePresent(pid_t pid, const char *path)
{
    struct stat sb = {};
    if (!g_fn.__xstat) {
        return -1;
    }

    if (g_fn.__xstat(_STAT_VER, path, &sb) != 0) {
        return -1;
    } else {
        return sb.st_size;
    }
}

int
osGetCmdline(pid_t pid, char **cmd)
{
    int fd = -1;
    int bytesRead = 0;
    char path[64];

    if (!cmd) return 0;
    char* buf = *cmd;
    buf = NULL;

    if (!g_fn.open || !g_fn.read || !g_fn.close) {
        goto out;
    }

    if ((buf = calloc(1, NCARGS)) == NULL) {
        goto out;
    }

    if (snprintf(path, sizeof(path), "/proc/%d/cmdline", pid) < 0) {
        goto out;
    }

    if ((fd = g_fn.open(path, O_RDONLY)) == -1) {
        DBG(NULL);
        goto out;
    }

    if ((bytesRead = g_fn.read(fd, buf, NCARGS)) <= 0) {
        DBG(NULL);
        goto out;
    }

    // Replace all but the last null with spaces
    int i;
    for (i=0; i < (bytesRead - 1); i++) {
        if (buf[i] == '\0') buf[i] = ' ';
    }

out:
    if (!buf || !buf[0]) {
        if (buf) free(buf);
        buf = strdup("none");
    } else {
        // buf is big; try to strdup what we've used and free the rest
        char* tmp = strdup(buf);
        if (tmp) {
            free(buf);
            buf = tmp;
        }
    }
    if (fd != -1) g_fn.close(fd);
    *cmd = buf;
    return (*cmd != NULL);
}

/*
 * Looking for the presence of a Chromium enabled app.
 * It's not as simple as checking the process name.
 *
 * First, Chrome itself changes the name of some of
 * it's processes. Despite what is shown with a 'ps'
 * command the process name we get back from inside
 * the process is not always 'chrome'.
 *
 * Second, several applications use Chromium.
 * So, the process name is unrelated.
 *
 * Therfore, we look for a number of indicators
 * in the maps file to see what libs, config
 * files and data files are being used. This
 * is the result of simple experimentation
 * with a couple of Chromium enable apps.
 * There is likely to be lots of holes.
 */
bool
osThreadNow()
{
    int fd = -1, rc = TRUE;
    char *maps = NULL;

    if (((maps = calloc(1, MAX_MAPS)) != NULL) &&
        (g_fn.open) && ((fd = g_fn.open("/proc/self/maps", O_RDONLY)) != -1) &&
        (g_fn.read) && (g_fn.read(fd, maps, MAX_MAPS) != -1)) {
        if ((strstr(maps, "chrome") != NULL) ||
            (strstr(maps, "Chrome") != NULL) ||
            (strstr(maps, "chromium") != NULL) ||
            (strstr(maps, "Chromium") != NULL) ||
            (strstr(maps, "croco") != NULL)) {
            rc = FALSE;
        }
    }

    if ((fd != -1) && (g_fn.close)) {
            g_fn.close(fd);
    }

    if (maps) free (maps);

    return rc;
}
