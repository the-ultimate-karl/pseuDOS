#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "rtc.h"

/* Basic freestanding string utilities */
static size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static int strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static __attribute__((unused)) int strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2 || n == 0) return 0;
    while (n-- && *s1 && *s2) {
        if (*s1 != *s2) return *(const unsigned char *)s1 - *(const unsigned char *)s2;
        s1++;
        s2++;
    }
    return 0;
}

static char *strcpy(char *dest, const char *src) {
    char *d = dest;
    if (!dest || !src) return dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static __attribute__((unused)) char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    if (!dest || !src || n == 0) return dest;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = '\0';
        n--;
    }
    return dest;
}

static __attribute__((unused)) void *memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

static __attribute__((unused)) void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static char *strchr(const char *s, int c) {
    if (!s) return NULL;
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}

static char *strrchr(const char *s, int c) {
    if (!s) return NULL;
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)last;
}

static char *trim(char *str) {
    if (!str) return NULL;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end-- = '\0';
    }
    return str;
}

static int atoi(const char *str) {
    int res = 0;
    int sign = 1;
    if (!str) return 0;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res * sign;
}

/* I/O Redirection State */
static int g_redirect_active = 0;
static char g_redirect_path[256];
static int g_redirect_append = 0;
static int g_redirect_first_flush = 1;
static char g_redirect_buf[4096];
static size_t g_redirect_len = 0;

static void puts(const char *str);

static void redirect_flush(void) {
    if (!g_redirect_active || g_redirect_path[0] == '\0') return;
    int append_flag = g_redirect_first_flush ? g_redirect_append : 1;
    syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)g_redirect_path,
            (uint64_t)(uintptr_t)g_redirect_buf, (uint64_t)g_redirect_len,
            (uint64_t)append_flag, 0);
    g_redirect_first_flush = 0;
    g_redirect_len = 0;
}

static void redirect_finish(void) {
    if (!g_redirect_active) return;
    if (g_redirect_first_flush || g_redirect_len > 0) {
        int append_flag = g_redirect_first_flush ? g_redirect_append : 1;
        int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)g_redirect_path,
                             (uint64_t)(uintptr_t)g_redirect_buf, (uint64_t)g_redirect_len,
                             (uint64_t)append_flag, 0);
        if (wr == -EPERM) {
            g_redirect_active = 0;
            puts("redirect: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
            return;
        } else if (wr < 0) {
            g_redirect_active = 0;
            puts("redirect: failed to write to destination\n");
            return;
        }
    }
    g_redirect_active = 0;
    g_redirect_len = 0;
    g_redirect_first_flush = 1;
    g_redirect_path[0] = '\0';
}

/* Console I/O helpers */
static void puts(const char *str) {
    if (!str) return;
    if (g_redirect_active) {
        size_t len = strlen(str);
        for (size_t i = 0; i < len; i++) {
            if (g_redirect_len >= sizeof(g_redirect_buf) - 1) {
                redirect_flush();
            }
            g_redirect_buf[g_redirect_len++] = str[i];
        }
        return;
    }
    syscall(SYS_WRITE, 1, (uint64_t)(uintptr_t)str, strlen(str), 0, 0);
}

static void putc(char c) {
    if (g_redirect_active) {
        if (g_redirect_len >= sizeof(g_redirect_buf) - 1) {
            redirect_flush();
        }
        g_redirect_buf[g_redirect_len++] = c;
        return;
    }
    char buf[1] = { c };
    syscall(SYS_WRITE, 1, (uint64_t)(uintptr_t)buf, 1, 0, 0);
}

static void print_num(uint64_t num) {
    char buf[32];
    int idx = 0;
    if (num == 0) {
        putc('0');
        return;
    }
    while (num > 0) {
        buf[idx++] = '0' + (num % 10);
        num /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) {
        putc(buf[i]);
    }
}

static char getchar(void) {
    char c = 0;
    while (1) {
        int64_t n = syscall(SYS_READ, 0, (uint64_t)(uintptr_t)&c, 1, 0, 0);
        if (n > 0 && c != 0) return c;
        syscall(SYS_SLEEP, 10, 0, 0, 0, 0);
    }
}

/* Command History */
#define HISTORY_MAX 16
static char g_history[HISTORY_MAX][256];
static int g_history_count = 0;
static int g_history_browse = -1;
static char g_scratch_line[256];

static void history_add(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return;
    if (g_history_count > 0 && strcmp(g_history[(g_history_count - 1) % HISTORY_MAX], cmd) == 0) {
        return;
    }
    size_t idx = (size_t)(g_history_count % HISTORY_MAX);
    strncpy(g_history[idx], cmd, sizeof(g_history[idx]) - 1);
    g_history[idx][sizeof(g_history[idx]) - 1] = '\0';
    g_history_count++;
}

static void cmd_history(void) {
    int start = (g_history_count > HISTORY_MAX) ? (g_history_count - HISTORY_MAX) : 0;
    for (int i = start; i < g_history_count; i++) {
        print_num((uint64_t)(i + 1));
        puts("  ");
        puts(g_history[i % HISTORY_MAX]);
        puts("\n");
    }
}

static void readline(char *buf, size_t max_len, const char *prompt) {
    size_t idx = 0;
    g_history_browse = -1;
    g_scratch_line[0] = '\0';
    puts(prompt);

    while (idx + 1 < max_len) {
        char c = getchar();
        if (c == '\r' || c == '\n') {
            puts("\n");
            break;
        } else if (c == '\b' || c == 127) {
            if (idx > 0) {
                idx--;
                putc('\b');
            }
        } else if (c == 27) { /* ESC / ANSI arrow sequence */
            char c2 = getchar();
            if (c2 == '[') {
                char c3 = getchar();
                if (c3 == 'A') { /* UP ARROW */
                    if (g_history_count > 0) {
                        if (g_history_browse == -1) {
                            buf[idx] = '\0';
                            strcpy(g_scratch_line, buf);
                            g_history_browse = g_history_count - 1;
                        } else if (g_history_browse > 0 && g_history_browse > g_history_count - HISTORY_MAX) {
                            g_history_browse--;
                        }
                        while (idx > 0) {
                            putc('\b');
                            idx--;
                        }
                        const char *h_entry = g_history[g_history_browse % HISTORY_MAX];
                        size_t h_len = strlen(h_entry);
                        if (h_len >= max_len) h_len = max_len - 1;
                        strncpy(buf, h_entry, h_len);
                        buf[h_len] = '\0';
                        idx = h_len;
                        puts(buf);
                    }
                } else if (c3 == 'B') { /* DOWN ARROW */
                    if (g_history_browse != -1) {
                        g_history_browse++;
                        while (idx > 0) {
                            putc('\b');
                            idx--;
                        }
                        if (g_history_browse >= g_history_count) {
                            g_history_browse = -1;
                            size_t s_len = strlen(g_scratch_line);
                            if (s_len >= max_len) s_len = max_len - 1;
                            strncpy(buf, g_scratch_line, s_len);
                            buf[s_len] = '\0';
                            idx = s_len;
                            puts(buf);
                        } else {
                            const char *h_entry = g_history[g_history_browse % HISTORY_MAX];
                            size_t h_len = strlen(h_entry);
                            if (h_len >= max_len) h_len = max_len - 1;
                            strncpy(buf, h_entry, h_len);
                            buf[h_len] = '\0';
                            idx = h_len;
                            puts(buf);
                        }
                    }
                }
            }
        } else if (c >= 32 && c < 127) {
            buf[idx++] = c;
            putc(c);
        }
    }
    buf[idx] = '\0';
}

static void resolve_path(const char *arg, char *out_buf, size_t max_len) {
    if (!out_buf || max_len == 0) return;
    if (!arg || arg[0] == '\0' || strcmp(arg, ".") == 0) {
        syscall(SYS_GETCWD, (uint64_t)(uintptr_t)out_buf, max_len, 0, 0, 0);
        return;
    }
    char res[256];
    size_t j = 0;
    if (arg[0] == '/' || arg[0] == '\\') {
        for (size_t i = 0; arg[i] && j < sizeof(res) - 1; i++) {
            res[j++] = (arg[i] == '\\') ? '/' : arg[i];
        }
        res[j] = '\0';
    } else {
        char cwd[256];
        syscall(SYS_GETCWD, (uint64_t)(uintptr_t)cwd, sizeof(cwd), 0, 0, 0);
        size_t cwd_len = strlen(cwd);
        for (size_t i = 0; i < cwd_len && j < sizeof(res) - 1; i++) {
            res[j++] = cwd[i];
        }
        if (j > 0 && res[j - 1] != '/' && j < sizeof(res) - 1) {
            res[j++] = '/';
        }
        for (size_t i = 0; arg[i] && j < sizeof(res) - 1; i++) {
            res[j++] = (arg[i] == '\\') ? '/' : arg[i];
        }
        res[j] = '\0';
    }
    strncpy(out_buf, res, max_len - 1);
    out_buf[max_len - 1] = '\0';
}

/* Environment Variables */
#define ENV_MAX 32
typedef struct {
    char name[32];
    char value[128];
} env_var_t;

static env_var_t g_env[ENV_MAX];
static int g_env_count = 0;

static const char *env_get(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_env_count; i++) {
        if (strcmp(g_env[i].name, name) == 0) {
            return g_env[i].value;
        }
    }
    return NULL;
}

static int env_set(const char *name, const char *value) {
    if (!name || name[0] == '\0') return -1;
    for (int i = 0; i < g_env_count; i++) {
        if (strcmp(g_env[i].name, name) == 0) {
            strncpy(g_env[i].value, value ? value : "", sizeof(g_env[i].value) - 1);
            g_env[i].value[sizeof(g_env[i].value) - 1] = '\0';
            return 0;
        }
    }
    if (g_env_count < ENV_MAX) {
        strncpy(g_env[g_env_count].name, name, sizeof(g_env[g_env_count].name) - 1);
        g_env[g_env_count].name[sizeof(g_env[g_env_count].name) - 1] = '\0';
        strncpy(g_env[g_env_count].value, value ? value : "", sizeof(g_env[g_env_count].value) - 1);
        g_env[g_env_count].value[sizeof(g_env[g_env_count].value) - 1] = '\0';
        g_env_count++;
        return 0;
    }
    return -1;
}

static void env_init(void) {
    g_env_count = 0;
    env_set("USER", "shell");
    env_set("HOSTNAME", "pseuDOS");
    env_set("PWD", "/");
    env_set("HOME", "/home/user");
    env_set("SHELL", "/protected/crit/xshss.bin");
}

static void cmd_env(void) {
    for (int i = 0; i < g_env_count; i++) {
        puts(g_env[i].name);
        putc('=');
        puts(g_env[i].value);
        puts("\n");
    }
}

static void cmd_set(const char *arg) {
    if (!arg || arg[0] == '\0') {
        cmd_env();
        return;
    }
    char *eq = strchr(arg, '=');
    if (!eq) {
        const char *val = env_get(arg);
        if (val) {
            puts(val);
            puts("\n");
        }
        return;
    }
    char name[32];
    size_t name_len = (size_t)(eq - arg);
    if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
    strncpy(name, arg, name_len);
    name[name_len] = '\0';
    const char *val = eq + 1;
    env_set(trim(name), val);
}

static void expand_vars(const char *input, char *output, size_t max_len) {
    if (!input || !output || max_len == 0) return;
    size_t i = 0, o = 0;
    while (input[i] && o + 1 < max_len) {
        if (input[i] == '$' && input[i+1] != '\0' && input[i+1] != ' ') {
            i++;
            char var_name[32];
            size_t v = 0;
            while (input[i] && (
                   (input[i] >= 'a' && input[i] <= 'z') ||
                   (input[i] >= 'A' && input[i] <= 'Z') ||
                   (input[i] >= '0' && input[i] <= '9') ||
                   input[i] == '_') && v < sizeof(var_name) - 1) {
                var_name[v++] = input[i++];
            }
            var_name[v] = '\0';
            const char *val = env_get(var_name);
            if (val) {
                for (size_t k = 0; val[k] && o + 1 < max_len; k++) {
                    output[o++] = val[k];
                }
            }
        } else {
            output[o++] = input[i++];
        }
    }
    output[o] = '\0';
}

/* Wildcard Matching & Expansion */
static int glob_match(const char *pattern, const char *str) {
    if (!pattern || !str) return 0;
    if (*pattern == '\0') return (*str == '\0');
    if (*pattern == '*') {
        while (*str) {
            if (glob_match(pattern + 1, str)) return 1;
            str++;
        }
        return glob_match(pattern + 1, str);
    }
    if (*pattern == *str) {
        return glob_match(pattern + 1, str + 1);
    }
    return 0;
}

static void expand_wildcards(const char *input, char *output, size_t max_len) {
    if (!input || !output || max_len == 0) return;
    if (!strchr(input, '*')) {
        strncpy(output, input, max_len - 1);
        output[max_len - 1] = '\0';
        return;
    }

    output[0] = '\0';
    size_t out_len = 0;
    char temp[512];
    strncpy(temp, input, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *token = temp;
    int first_tok = 1;
    while (*token) {
        while (*token == ' ') token++;
        if (*token == '\0') break;
        char *end = strchr(token, ' ');
        if (end) *end = '\0';

        if (strchr(token, '*')) {
            char dir_prefix[256] = "";
            const char *pattern = token;
            char *last_slash = strrchr(token, '/');
            if (last_slash) {
                size_t dlen = (size_t)(last_slash - token + 1);
                if (dlen < sizeof(dir_prefix)) {
                    strncpy(dir_prefix, token, dlen);
                    dir_prefix[dlen] = '\0';
                }
                pattern = last_slash + 1;
            }

            char names_buf[2048];
            char query_dir[256];
            if (dir_prefix[0] != '\0') {
                resolve_path(dir_prefix, query_dir, sizeof(query_dir));
            } else {
                resolve_path(".", query_dir, sizeof(query_dir));
            }

            int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)query_dir,
                                     (uint64_t)(uintptr_t)names_buf, sizeof(names_buf), 0, 0);

            int matched_any = 0;
            if (nbytes > 0) {
                size_t pos = 0;
                while (pos < (size_t)nbytes) {
                    const char *child_name = &names_buf[pos];
                    size_t c_len = strlen(child_name);
                    if (c_len == 0) break;
                    if (glob_match(pattern, child_name)) {
                        if (!first_tok && out_len + 1 < max_len) output[out_len++] = ' ';
                        first_tok = 0;
                        for (size_t d = 0; dir_prefix[d] && out_len + 1 < max_len; d++) {
                            output[out_len++] = dir_prefix[d];
                        }
                        for (size_t c = 0; child_name[c] && out_len + 1 < max_len; c++) {
                            output[out_len++] = child_name[c];
                        }
                        matched_any = 1;
                    }
                    pos += c_len + 1;
                }
            }

            if (!matched_any) {
                if (!first_tok && out_len + 1 < max_len) output[out_len++] = ' ';
                first_tok = 0;
                for (size_t c = 0; token[c] && out_len + 1 < max_len; c++) {
                    output[out_len++] = token[c];
                }
            }
        } else {
            if (!first_tok && out_len + 1 < max_len) output[out_len++] = ' ';
            first_tok = 0;
            for (size_t c = 0; token[c] && out_len + 1 < max_len; c++) {
                output[out_len++] = token[c];
            }
        }

        if (!end) break;
        token = end + 1;
    }
    output[out_len] = '\0';
}

/* Core Utilities */
static void cmd_echo(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("\n");
        return;
    }
    puts(arg);
    puts("\n");
}

static void print_two_digits(uint32_t val) {
    putc('0' + (val / 10) % 10);
    putc('0' + (val % 10));
}

static void cmd_date(void) {
    rtc_datetime_t dt;
    if (syscall(SYS_TIME, (uint64_t)(uintptr_t)&dt, 0, 0, 0, 0) == 0) {
        print_num(dt.year);
        putc('-');
        print_two_digits(dt.month);
        putc('-');
        print_two_digits(dt.day);
        putc(' ');
        print_two_digits(dt.hours);
        putc(':');
        print_two_digits(dt.minutes);
        putc(':');
        print_two_digits(dt.seconds);
        puts("\n");
    } else {
        puts("date: error reading hardware rtc\n");
    }
}

static void cmd_uptime(void) {
    int64_t ticks = syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
    if (ticks < 0) ticks = 0;
    uint64_t total_sec = (uint64_t)ticks / 100;
    uint64_t days = total_sec / 86400;
    uint64_t rem = total_sec % 86400;
    uint64_t hours = rem / 3600;
    rem %= 3600;
    uint64_t mins = rem / 60;
    uint64_t secs = rem % 60;

    puts("uptime: ");
    if (days > 0) {
        print_num(days);
        puts((days == 1) ? " day, " : " days, ");
    }
    if (hours > 0 || days > 0) {
        print_num(hours);
        puts((hours == 1) ? " hour, " : " hours, ");
    }
    print_num(mins);
    puts((mins == 1) ? " minute, " : " minutes, ");
    print_num(secs);
    puts((secs == 1) ? " second (" : " seconds (");
    print_num((uint64_t)ticks);
    puts(" ticks)\n");
}

static void cmd_uname(const char *arg) {
    if (arg && (strcmp(arg, "-a") == 0 || strcmp(arg, "--all") == 0)) {
        puts("pseuDOS 0.6.0-scheduling x86_64 UEFI\n");
    } else if (arg && strcmp(arg, "-r") == 0) {
        puts("0.6.0-scheduling\n");
    } else if (arg && strcmp(arg, "-m") == 0) {
        puts("x86_64\n");
    } else if (arg && strcmp(arg, "-s") == 0) {
        puts("pseuDOS\n");
    } else {
        puts("pseuDOS\n");
    }
}

/* Shell Commands */
static void cmd_help(void) {
    puts("=================================================================\n");
    puts(" pseuDOS Experimental Shell Subsystem (xshss)\n");
    puts("=================================================================\n");
    puts(" help                        : display available commands\n");
    puts(" echo <text>                 : print text to console\n");
    puts(" ls / dir [-l] [-a] [path]   : list directory entries\n");
    puts(" cd <path>                   : change working directory\n");
    puts(" pwd                         : print working directory\n");
    puts(" cat / type <file>           : view plaintext file\n");
    puts(" more / less <file>          : paginated text viewer\n");
    puts(" data <file>                 : display creation/access timestamps & metadata\n");
    puts(" cp <src> <dst>              : copy file\n");
    puts(" mv <src> <dst>              : move or rename file\n");
    puts(" mkdir <path>                : create directory\n");
    puts(" touch <file>                : create empty file\n");
    puts(" write [-a] <file> <txt>     : write (or -a append) text to file\n");
    puts(" del / rm <path>             : delete file or directory\n");
    puts(" fs [drive_no | --drives]    : query filesystem stats and disk partitions\n");
    puts(" ps                          : list processes and CPU time\n");
    puts(" kill <pid>                  : terminate process\n");
    puts(" proctest                    : test preemptive multitasking with concurrent tasks\n");
    puts(" syscalltest                 : test syscall interface\n");
    puts(" date / time                 : display current date and time\n");
    puts(" uptime                      : display system uptime and ticks\n");
    puts(" uname [-a|-r|-m|-s]         : display system identification\n");
    puts(" env                         : display shell environment variables\n");
    puts(" set / export [name=val]     : set or display shell environment variables\n");
    puts(" history                     : display command history\n");
    puts(" screenres [w h]             : adjust or display screen resolution\n");
    puts(" switch-target [--int|--ext] : switch storage target\n");
    puts(" attached-drives [--all]     : list attached storage drives\n");
    puts(" cpu                         : display CPU model, vendor, and feature flags\n");
    puts(" mem                         : display physical memory map and statistics\n");
    puts(" pci                         : scan and list connected PCI / PCIe bus devices\n");
    puts(" devpath [mode]              : display and toggle boot device hardware path\n");
    puts(" kernel / su                 : escalate privilege to KERNEL mode\n");
    puts(" exit / drop                 : drop privileges or exit shell\n");
    puts(" sudo <command>              : run single command with KERNEL privileges\n");
    puts(" dmesg                       : display kernel message buffer\n");
    puts(" grub                        : display GRUB 2 chainloader config & setup\n");
    puts(" flash                       : install pseuDOS to persistent disk (requires sudo)\n");
    puts(" panic [reason]              : trigger a kernel panic\n");
    puts(" clear / cls                 : clear screen\n");
    puts(" reboot                      : restart computer\n");
    puts(" shutdown [now|-c]           : schedule shutdown in 1m, or 'now' to power off immediately\n");
    puts(" halt                        : halt CPU execution (requires sudo)\n");
    puts("=================================================================\n");
}

static void cmd_pwd(void) {
    char cwd[256];
    if (syscall(SYS_GETCWD, (uint64_t)(uintptr_t)cwd, sizeof(cwd), 0, 0, 0) == 0) {
        puts(cwd);
        puts("\n");
    } else {
        puts("/\n");
    }
}

static void cmd_cd(const char *arg) {
    char path[256];
    if (!arg || arg[0] == '\0') {
        strcpy(path, "/");
    } else {
        resolve_path(arg, path, sizeof(path));
    }
    if (syscall(SYS_CHDIR, (uint64_t)(uintptr_t)path, 0, 0, 0, 0) != 0) {
        puts("cd: no such file or directory: ");
        puts(arg ? arg : "");
        puts("\n");
    } else {
        char new_cwd[256];
        if (syscall(SYS_GETCWD, (uint64_t)(uintptr_t)new_cwd, sizeof(new_cwd), 0, 0, 0) == 0) {
            env_set("PWD", new_cwd);
        }
    }
}

static void cmd_ls(const char *arg) {
    int long_mode = 0;
    char target_path[256];
    target_path[0] = '\0';

    if (arg && arg[0] != '\0') {
        char temp[256];
        strncpy(temp, arg, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
        char *p = trim(temp);

        while (p && *p == '-') {
            char *next = strchr(p, ' ');
            if (next) *next = '\0';
            for (size_t i = 1; p[i]; i++) {
                if (p[i] == 'l') long_mode = 1;
            }
            if (!next) {
                p = NULL;
                break;
            }
            p = trim(next + 1);
        }
        if (p && p[0] != '\0') {
            strncpy(target_path, p, sizeof(target_path) - 1);
            target_path[sizeof(target_path) - 1] = '\0';
        }
    }

    if (!long_mode) {
        if (target_path[0] != '\0') {
            char path[256];
            resolve_path(target_path, path, sizeof(path));
            syscall(SYS_READDIR, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
        } else {
            syscall(SYS_READDIR, 0, 0, 0, 0, 0);
        }
        return;
    }

    /* Long listing mode (-l) */
    char path[256];
    resolve_path(target_path[0] != '\0' ? target_path : NULL, path, sizeof(path));

    char names[2048];
    int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)names, sizeof(names), 0, 0);
    if (nbytes < 0) {
        puts("ls: cannot access '");
        puts(target_path[0] != '\0' ? target_path : path);
        puts("': no such file or directory\n");
        return;
    }

    puts("directory of ");
    puts(path);
    puts(":\n");

    /* Print . and .. */
    puts("  drwx  protected   2026-09-14 00:00:00          0 B   .\n");
    puts("  drwx  protected   2026-09-14 00:00:00          0 B   ..\n");

    int count = 0;
    size_t pos = 0;
    while (pos < (size_t)nbytes) {
        const char *name = &names[pos];
        size_t nlen = strlen(name);
        if (nlen == 0) break;

        char child_path[384];
        size_t plen = strlen(path);
        strcpy(child_path, path);
        if (plen > 0 && child_path[plen - 1] != '/') {
            child_path[plen++] = '/';
            child_path[plen] = '\0';
        }
        strncpy(child_path + plen, name, sizeof(child_path) - plen - 1);
        child_path[sizeof(child_path) - 1] = '\0';

        vfs_stat_t st;
        if (syscall(SYS_STAT, (uint64_t)(uintptr_t)child_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) == 0) {
            if (st.type == VFS_TYPE_DIR) {
                puts("  drwx");
            } else {
                puts("  -rw-");
            }
            if (st.is_protected) {
                puts("  protected ");
            } else {
                puts("  standard  ");
            }
            const char *date = st.date_modified[0] ? st.date_modified : (st.date_created[0] ? st.date_created : "0000-00-00 00:00:00");
            puts(date);
            puts("  ");

            uint32_t sz = st.size;
            int num_digits = 0;
            uint32_t t = sz;
            if (t == 0) num_digits = 1;
            while (t > 0) { num_digits++; t /= 10; }
            for (int k = num_digits; k < 9; k++) putc(' ');
            print_num(sz);
            puts(" B   ");
            puts(name);
            puts("\n");
            count++;
        }
        pos += nlen + 1;
    }
    puts("  total: ");
    print_num((uint64_t)count);
    puts(" item(s)\n");
}

static void cmd_cat(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("cat: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));

    char buf[2048];
    size_t offset = 0;
    int first_chunk = 1;
    char last_char = '\0';
    while (1) {
        int64_t bytes = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, sizeof(buf) - 1, offset, 0);
        if (bytes < 0) {
            if (first_chunk) {
                puts("cat: cannot open file '");
                puts(arg);
                puts("'\n");
            }
            return;
        }
        if (bytes == 0) break;
        buf[bytes] = '\0';
        puts(buf);
        last_char = buf[bytes - 1];
        offset += bytes;
        first_chunk = 0;
    }
    if (!first_chunk && last_char != '\n') puts("\n");
}

static void cmd_more_less(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("more: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));

    char buf[2048];
    size_t offset = 0;
    int line_count = 0;
    int first_chunk = 1;
    int should_quit = 0;

    while (!should_quit) {
        int64_t bytes = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, sizeof(buf), offset, 0);
        if (bytes < 0) {
            if (first_chunk) {
                puts("more: cannot open file '");
                puts(arg);
                puts("'\n");
            }
            return;
        }
        if (bytes == 0) break;
        first_chunk = 0;
        for (int64_t i = 0; i < bytes; i++) {
            putc(buf[i]);
            if (buf[i] == '\n') {
                line_count++;
                if (line_count >= 22) {
                    puts("-- more (press space/enter to continue, 'q' to quit) --");
                    char c = getchar();
                    puts("\r                                                         \r");
                    if (c == 'q' || c == 'Q') {
                        should_quit = 1;
                        break;
                    }
                    line_count = 0;
                }
            }
        }
        offset += bytes;
    }
}

static void cmd_data(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("data: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));

    vfs_stat_t st;
    int64_t res = syscall(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0, 0, 0);
    if (res != 0) {
        puts("data: cannot stat '");
        puts(arg);
        puts("': no such file or directory\n");
        return;
    }

    puts("file metadata for: ");
    puts(path);
    puts("\n");
    puts("    node type:          ");
    puts((st.type == VFS_TYPE_DIR) ? "directory\n" : "regular file\n");
    puts("    file size:          ");
    print_num(st.size);
    puts(" bytes\n");
    puts("    date created:       ");
    puts(st.date_created);
    puts("\n");
    puts("    date last accessed: ");
    puts(st.date_accessed);
    puts("\n");
    puts("    date modified:      ");
    puts(st.date_modified);
    puts("\n");
    puts("    protection status:  ");
    puts(st.is_protected ? "protected system node\n" : "standard node\n");
}

static void cmd_touch(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("touch: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));
    int64_t res = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)"", 0, 1, 0);
    if (res == -EPERM) {
        puts("touch: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
    } else if (res < 0) {
        puts("touch: cannot touch '");
        puts(arg);
        puts("'\n");
    }
}

static void cmd_mkdir(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("mkdir: missing operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));
    int64_t res = syscall(SYS_MKDIR, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
    if (res == -EPERM) {
        puts("mkdir: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
    } else if (res != 0) {
        puts("mkdir: cannot create directory '");
        puts(arg);
        puts("'\n");
    }
}

static void cmd_del(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("del: missing operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));
    int64_t res = syscall(SYS_UNLINK, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
    if (res == -EPERM) {
        puts("del: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
    } else if (res != 0) {
        puts("del: cannot remove '");
        puts(arg);
        puts("': no such file or directory\n");
    }
}

static int do_cp(const char *src, const char *dst, int verbose) {
    if (!src || src[0] == '\0' || !dst || dst[0] == '\0') {
        if (verbose) puts("usage: cp <src> <dst>\n");
        return -1;
    }

    char src_path[256];
    char dst_path[256];
    resolve_path(src, src_path, sizeof(src_path));
    resolve_path(dst, dst_path, sizeof(dst_path));

    /* If destination is a directory, append src basename */
    vfs_stat_t st_dst;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)&st_dst, 0, 0, 0) == 0) {
        if (st_dst.type == VFS_TYPE_DIR) { /* Directory */
            size_t len = strlen(dst_path);
            if (len > 0 && dst_path[len - 1] != '/' && len + 1 < sizeof(dst_path)) {
                dst_path[len] = '/';
                dst_path[len + 1] = '\0';
                len++;
            }
            const char *src_base = strrchr(src_path, '/');
            if (!src_base) src_base = src_path;
            else src_base++;
            strncpy(dst_path + len, src_base, sizeof(dst_path) - len - 1);
            dst_path[sizeof(dst_path) - 1] = '\0';
        }
    }

    char buf[2048];
    size_t offset = 0;
    int first_chunk = 1;

    while (1) {
        int64_t read_bytes = syscall(SYS_READFILE, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)buf, sizeof(buf), offset, 0);
        if (read_bytes < 0) {
            if (verbose) {
                puts("cp: cannot read '");
                puts(src);
                puts("'\n");
            }
            return -1;
        }
        if (read_bytes == 0) {
            /* If empty file, ensure 0-byte file is created */
            if (first_chunk) {
                int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)"", 0, 0, 0);
                if (wr == -EPERM) {
                    if (verbose) puts("cp: permission denied: destination is protected path, requires 'sudo' or KERNEL mode\n");
                    return -EPERM;
                } else if (wr < 0) {
                    if (verbose) {
                        puts("cp: cannot write to '");
                        puts(dst);
                        puts("'\n");
                    }
                    return -1;
                }
            }
            break;
        }

        int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)buf, (uint64_t)read_bytes, first_chunk ? 0 : 1, 0);
        if (wr == -EPERM) {
            if (verbose) puts("cp: permission denied: destination is protected path, requires 'sudo' or KERNEL mode\n");
            return -EPERM;
        } else if (wr < 0) {
            if (verbose) {
                puts("cp: cannot write to '");
                puts(dst);
                puts("'\n");
            }
            return -1;
        }

        offset += read_bytes;
        first_chunk = 0;
    }

    if (verbose) {
        puts("cp: copied '");
        puts(src);
        puts("' -> '");
        puts(dst);
        puts("'\n");
    }
    return 0;
}

static void cmd_cp(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: cp <src> <dst>\n");
        return;
    }
    char src[128] = "";
    char dst[128] = "";
    size_t i = 0, j = 0;
    while (arg[i] == ' ') i++;
    while (arg[i] && arg[i] != ' ' && j < sizeof(src) - 1) src[j++] = arg[i++];
    src[j] = '\0';
    while (arg[i] == ' ') i++;
    j = 0;
    while (arg[i] && arg[i] != ' ' && j < sizeof(dst) - 1) dst[j++] = arg[i++];
    dst[j] = '\0';

    do_cp(src, dst, 1);
}

static void cmd_mv(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: mv <src> <dst>\n");
        return;
    }
    char src[128] = "";
    char dst[128] = "";
    size_t i = 0, j = 0;
    while (arg[i] == ' ') i++;
    while (arg[i] && arg[i] != ' ' && j < sizeof(src) - 1) src[j++] = arg[i++];
    src[j] = '\0';
    while (arg[i] == ' ') i++;
    j = 0;
    while (arg[i] && arg[i] != ' ' && j < sizeof(dst) - 1) dst[j++] = arg[i++];
    dst[j] = '\0';

    if (src[0] == '\0' || dst[0] == '\0') {
        puts("usage: mv <src> <dst>\n");
        return;
    }

    int cp_res = do_cp(src, dst, 0);
    if (cp_res != 0) {
        if (cp_res == -EPERM) {
            puts("mv: permission denied: destination is protected path, requires 'sudo' or KERNEL mode\n");
        } else {
            puts("mv: failed to move '");
            puts(src);
            puts("': destination write failed\n");
        }
        return; /* ABORT! Do not delete source file! */
    }

    char src_path[256];
    resolve_path(src, src_path, sizeof(src_path));
    int64_t un_res = syscall(SYS_UNLINK, (uint64_t)(uintptr_t)src_path, 0, 0, 0, 0);
    if (un_res == -EPERM) {
        puts("mv: warning: copied to destination, but source removal denied (requires 'sudo' or KERNEL mode)\n");
    } else if (un_res != 0) {
        puts("mv: warning: copied to destination, but failed to unlink source\n");
    } else {
        puts("mv: moved '");
        puts(src);
        puts("' -> '");
        puts(dst);
        puts("'\n");
    }
}

static void cmd_write(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: write [-a] <file> <text>\n");
        return;
    }
    size_t i = 0;
    while (arg[i] == ' ') i++;

    int append = 0;
    if (arg[i] == '-' && arg[i+1] == 'a' && (arg[i+2] == ' ' || arg[i+2] == '\0')) {
        append = 1;
        i += 2;
        while (arg[i] == ' ') i++;
    }

    char file[128] = "";
    size_t j = 0;
    while (arg[i] && arg[i] != ' ' && j < sizeof(file) - 1) file[j++] = arg[i++];
    file[j] = '\0';
    while (arg[i] == ' ') i++;

    if (file[0] == '\0') {
        puts("usage: write [-a] <file> <text>\n");
        return;
    }

    char path[256];
    resolve_path(file, path, sizeof(path));
    const char *text = &arg[i];
    int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)text, strlen(text), append, 0);
    if (wr == -EPERM) {
        puts("write: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
    } else if (wr < 0) {
        puts("write: cannot write to '");
        puts(file);
        puts("'\n");
    } else {
        if (append) {
            puts("appended text to '");
        } else {
            puts("wrote text to '");
        }
        puts(file);
        puts("'\n");
    }
}

static void cmd_ps(void) {
    syscall(SYS_PS, 0, 0, 0, 0, 0);
}

static void cmd_kill(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: kill <pid>\n");
        return;
    }
    uint32_t pid = (uint32_t)atoi(arg);
    int64_t res = syscall(SYS_KILL, pid, 0, 0, 0, 0);
    if (res == -EPERM) {
        puts("kill: permission denied: killing other processes requires 'sudo' or KERNEL mode\n");
    } else if (res != 0) {
        puts("kill: failed to kill process\n");
    }
}

static void cmd_syscalltest(void) {
    puts("--- testing syscall interface from userspace ---\n");
    int64_t pid = syscall(SYS_GETPID, 0, 0, 0, 0, 0);
    puts("1. sys_getpid() = PID ");
    print_num((uint64_t)pid);
    puts("\n");

    int64_t priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
    puts("2. sys_get_privilege() = ");
    puts((priv == 1) ? "KERNEL\n" : "USER\n");

    puts("3. testing sys_write()... [ok]\n");
    puts("--- all syscall tests completed! ---\n");
}

static void cmd_grub(void) {
    puts("=================================================================\n");
    puts(" pseuDOS GRUB 2 Integration & Chainloader Configuration\n");
    puts("=================================================================\n");
    puts("EFI Bootloader Targets:\n");
    puts("  /EFI/pseuDOS/BOOTX64.EFI\n");
    puts("  /EFI/pseuDOS/pseudos.efi\n\n");
    puts("GRUB 2 Menuentry Snippet (/etc/grub.d/40_custom or /boot/grub/grub.cfg):\n");
    puts("  menuentry \"pseuDOS x86_64\" {\n");
    puts("      insmod fat\n");
    puts("      insmod chain\n");
    puts("      search --no-floppy --set=root --file /EFI/pseuDOS/BOOTX64.EFI\n");
    puts("      chainloader /EFI/pseuDOS/BOOTX64.EFI\n");
    puts("  }\n\n");
    puts("Linux Installation Helper:\n");
    puts("  1. Append the menuentry snippet above to /etc/grub.d/40_custom\n");
    puts("  2. Run 'sudo update-grub' (or 'grub2-mkconfig -o /boot/grub/grub.cfg')\n");
    puts("  3. On reboot, select 'pseuDOS x86_64' from the GRUB boot menu\n");
    puts("  4. Live snippet also available at /EFI/pseuDOS/grub.cfg\n");
    puts("=================================================================\n");
}

static void cmd_fs(const char *arg) {
    syscall(SYS_FS, (uint64_t)(uintptr_t)(arg ? arg : ""), 0, 0, 0, 0);
}

static void cmd_cpu(void) {
    syscall(SYS_CPU, 0, 0, 0, 0, 0);
}

static void cmd_mem(void) {
    syscall(SYS_MEM, 0, 0, 0, 0, 0);
}

static void cmd_pci(void) {
    syscall(SYS_PCI, 0, 0, 0, 0, 0);
}

static void cmd_devpath(const char *arg) {
    syscall(SYS_DEVPATH, (uint64_t)(uintptr_t)(arg ? arg : ""), 0, 0, 0, 0);
}

static void cmd_attached_drives(const char *arg) {
    syscall(SYS_ATTACHED_DRIVES, (uint64_t)(uintptr_t)(arg ? arg : ""), 0, 0, 0, 0);
}

static void cmd_switch_target(const char *arg) {
    syscall(SYS_SWITCH_TARGET, (uint64_t)(uintptr_t)(arg ? arg : ""), 0, 0, 0, 0);
}

static void cmd_screenres(const char *arg) {
    syscall(SYS_SCREENRES, (uint64_t)(uintptr_t)(arg ? arg : ""), 0, 0, 0, 0);
}

static void cmd_proctest(void) {
    syscall(SYS_PROCTEST, 0, 0, 0, 0, 0);
}

static void cmd_halt(void) {
    int64_t res = syscall(SYS_HALT, 0, 0, 0, 0, 0);
    if (res == -EPERM) {
        puts("halt: permission denied: system halt requires 'sudo' or KERNEL mode\n");
    }
}

/* Forward declaration */
static void execute_command_internal(char *cmd_line);

static void execute_command(char *cmd_line) {
    if (!cmd_line) return;

    /* 1. Shell environment variable expansion */
    char exp_line[512];
    expand_vars(cmd_line, exp_line, sizeof(exp_line));

    /* 2. Wildcard globbing expansion */
    char final_line[512];
    expand_wildcards(exp_line, final_line, sizeof(final_line));

    char *cmd_str = trim(final_line);
    if (!cmd_str || cmd_str[0] == '\0') return;

    /* 3. Check for I/O redirection (> or >>) */
    char *redir_append_ptr = strchr(cmd_str, '>');
    char *redir_target = NULL;
    int redir_append = 0;
    if (redir_append_ptr) {
        if (*(redir_append_ptr + 1) == '>') {
            redir_append = 1;
            *redir_append_ptr = '\0';
            redir_target = trim(redir_append_ptr + 2);
        } else {
            redir_append = 0;
            *redir_append_ptr = '\0';
            redir_target = trim(redir_append_ptr + 1);
        }
    }

    if (redir_target && redir_target[0] != '\0') {
        resolve_path(redir_target, g_redirect_path, sizeof(g_redirect_path));
        g_redirect_active = 1;
        g_redirect_append = redir_append;
        g_redirect_first_flush = 1;
        g_redirect_len = 0;
    }

    execute_command_internal(cmd_str);

    if (g_redirect_active) {
        redirect_finish();
    }
}

static void execute_command_internal(char *cmd_line) {
    char *cmd = trim(cmd_line);
    if (!cmd || cmd[0] == '\0') return;

    char *arg = strchr(cmd, ' ');
    if (arg) {
        *arg = '\0';
        arg = trim(arg + 1);
    }

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(arg);
    } else if (strcmp(cmd, "date") == 0 || strcmp(cmd, "time") == 0) {
        cmd_date();
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname(arg);
    } else if (strcmp(cmd, "env") == 0) {
        cmd_env();
    } else if (strcmp(cmd, "set") == 0 || strcmp(cmd, "export") == 0) {
        cmd_set(arg);
    } else if (strcmp(cmd, "history") == 0) {
        cmd_history();
    } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
        cmd_ls(arg);
    } else if (strcmp(cmd, "cd") == 0) {
        cmd_cd(arg);
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
        cmd_cat(arg);
    } else if (strcmp(cmd, "more") == 0 || strcmp(cmd, "less") == 0) {
        cmd_more_less(arg);
    } else if (strcmp(cmd, "data") == 0) {
        cmd_data(arg);
    } else if (strcmp(cmd, "touch") == 0) {
        cmd_touch(arg);
    } else if (strcmp(cmd, "mkdir") == 0) {
        cmd_mkdir(arg);
    } else if (strcmp(cmd, "del") == 0 || strcmp(cmd, "rm") == 0) {
        cmd_del(arg);
    } else if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "copy") == 0) {
        cmd_cp(arg);
    } else if (strcmp(cmd, "mv") == 0 || strcmp(cmd, "move") == 0) {
        cmd_mv(arg);
    } else if (strcmp(cmd, "write") == 0) {
        cmd_write(arg);
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(cmd, "kill") == 0) {
        cmd_kill(arg);
    } else if (strcmp(cmd, "dmesg") == 0) {
        syscall(SYS_DMESG, 0, 0, 0, 0, 0);
    } else if (strcmp(cmd, "fs") == 0 || strcmp(cmd, "mount") == 0 || strcmp(cmd, "df") == 0) {
        cmd_fs(arg);
    } else if (strcmp(cmd, "cpu") == 0) {
        cmd_cpu();
    } else if (strcmp(cmd, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd, "pci") == 0) {
        cmd_pci();
    } else if (strcmp(cmd, "devpath") == 0) {
        cmd_devpath(arg);
    } else if (strcmp(cmd, "attached-drives") == 0) {
        cmd_attached_drives(arg);
    } else if (strcmp(cmd, "switch-target") == 0) {
        cmd_switch_target(arg);
    } else if (strcmp(cmd, "screenres") == 0) {
        cmd_screenres(arg);
    } else if (strcmp(cmd, "proctest") == 0) {
        cmd_proctest();
    } else if (strcmp(cmd, "halt") == 0) {
        cmd_halt();
    } else if (strcmp(cmd, "syscalltest") == 0) {
        cmd_syscalltest();
    } else if (strcmp(cmd, "grub") == 0) {
        cmd_grub();
    } else if (strcmp(cmd, "flash") == 0) {
        int64_t res = syscall(SYS_FLASH, 0, 0, 0, 0, 0);
        if (res == -EPERM) {
            puts("flash: permission denied: installation requires 'sudo' or KERNEL mode\n");
        }
    } else if (strcmp(cmd, "panic") == 0) {
        syscall(SYS_PANIC, (uint64_t)(uintptr_t)(arg && arg[0] ? arg : "manual panic triggered from shell"), 0, 0, 0, 0);
    } else if (strcmp(cmd, "kernel") == 0 || strcmp(cmd, "su") == 0) {
        syscall(SYS_ELEVATE, 0, 0, 0, 0, 0);
        env_set("USER", "kernel");
        puts("privilege elevated to KERNEL (root). type 'exit' or 'drop' to revert to user mode.\n");
    } else if (strcmp(cmd, "drop") == 0) {
        syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
        env_set("USER", "shell");
        puts("privilege reverted to USER mode.\n");
    } else if (strcmp(cmd, "sudo") == 0) {
        if (!arg || arg[0] == '\0') {
            puts("usage: sudo <command>\n");
        } else {
            int64_t prev_priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
            if (prev_priv != 1) {
                syscall(SYS_ELEVATE, 0, 0, 0, 0, 0);
                env_set("USER", "kernel");
            }
            execute_command_internal(arg);
            if (g_redirect_active) {
                redirect_finish();
            }
            if (prev_priv != 1) {
                syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
                env_set("USER", "shell");
            }
        }
    } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        syscall(SYS_CLEAR, 0, 0, 0, 0, 0);
    } else if (strcmp(cmd, "reboot") == 0) {
        puts("rebooting system...\n");
        syscall(SYS_REBOOT, 0, 0, 0, 0, 0);
    } else if (strcmp(cmd, "shutdown") == 0) {
        if (arg && strcmp(arg, "now") == 0) {
            puts("powering off system immediately...\n");
            syscall(SYS_SHUTDOWN, 0, 0, 0, 0, 0);
        } else if (arg && strcmp(arg, "-c") == 0) {
            syscall(SYS_SHUTDOWN, (uint64_t)-1, 0, 0, 0, 0);
        } else {
            syscall(SYS_SHUTDOWN, 60, 0, 0, 0, 0);
        }
    } else if (strcmp(cmd, "exit") == 0) {
        int64_t priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
        if (priv == 1) {
            syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
            env_set("USER", "shell");
            puts("dropped privileges to USER mode.\n");
        } else {
            puts("exiting shell...\n");
            syscall(SYS_EXIT, 0, 0, 0, 0, 0);
        }
    } else {
        puts("unknown command '");
        puts(cmd);
        puts("'. type 'help' for available commands.\n");
    }
}

void xshss_main(void) {
    env_init();

    puts("[  xhss  ] started\n");
    puts("shell started\n");
    puts("type 'help' for a list of commands.\n\n");

    char line_buf[256];
    char cwd_buf[384];
    char prompt_buf[512];

    while (1) {
        if (syscall(SYS_GET_PROMPT_PATH, (uint64_t)(uintptr_t)cwd_buf, sizeof(cwd_buf), 0, 0, 0) != 0) {
            if (syscall(SYS_GETCWD, (uint64_t)(uintptr_t)cwd_buf, sizeof(cwd_buf), 0, 0, 0) != 0) {
                strcpy(cwd_buf, "/");
            }
        }
        int64_t priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
        const char *user = (priv == 1) ? "kernel" : "shell";

        /* Format prompt: user@pseuDOS [cwd] > */
        size_t pidx = 0;
        const char *prefix = "@pseuDOS [";
        while (*user) prompt_buf[pidx++] = *user++;
        while (*prefix) prompt_buf[pidx++] = *prefix++;
        for (size_t c = 0; cwd_buf[c]; c++) prompt_buf[pidx++] = cwd_buf[c];
        prompt_buf[pidx++] = ']';
        prompt_buf[pidx++] = ' ';
        prompt_buf[pidx++] = '>';
        prompt_buf[pidx++] = ' ';
        prompt_buf[pidx] = '\0';

        readline(line_buf, sizeof(line_buf), prompt_buf);
        if (line_buf[0] != '\0') {
            history_add(line_buf);
        }
        execute_command(line_buf);
    }
}
