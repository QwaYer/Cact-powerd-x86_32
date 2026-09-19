/*
 * powerd — демон управления питанием CactOS (упрощённый аналог systemd-logind
 * в части power).
 *
 * Слушает AF_UNIX-сокет /run/powerd.sock и исполняет команды:
 *   status    — отвечает "ok running\n"
 *   reboot    — перезагрузка (CACT_SYSCTL_REBOOT + CACT_REBOOT_RESTART)
 *   halt      — остановка (CACT_REBOOT_HALT)
 *   poweroff  — выключение (CACT_REBOOT_POWEROFF)
 *   suspend   — сон в RAM (CACT_REBOOT_SUSPEND; алиас: sleep), отвечает
 *               "ok\n" только после пробуждения
 *
 * Сокет только в ядерном реестре (файл в /run не создаётся). Кнопочные/ACPI
 * события питанием не передаются в юзерспейс — powerd лишь исполняет запросы
 * и ведёт журнал. Запускается супервизором cgoct как /sbin/powerd.
 *
 * /etc/powerd.conf (все ключи необязательны; создаётся при первом запуске):
 *   file=/var/log/powerd.log
 *   console=0
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <socket.h>
#include <ioctl_abi.h>
#include <poll.h>

#define CONFIG_PATH "/etc/powerd.conf"
#define SOCK_PATH   "/run/powerd.sock"
#define LOG_DEFAULT "/var/log/powerd.log"
#define LINE_MAX    128

static char log_path[128] = LOG_DEFAULT;
static int  console_on    = 0;
static int  out_fd        = -1;

/* Конфиг по умолчанию: пишется при первом запуске, если файла ещё нет. */
static const char default_config[] =
    "# powerd config - auto-generated on first start.\n"
    "#\n"
    "# file    - журнал событий\n"
    "# console - дублировать на /dev/console (0|1)\n"
    "\n"
    "file=/var/log/powerd.log\n"
    "console=0\n";

static void ensure_dir(const char *path) {
    (void)mkdir(path, 0755);
}

static void config_write_default(void) {
    int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;
    write(fd, default_config, sizeof(default_config) - 1);
    close(fd);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config_write_default();
        f = fopen(CONFIG_PATH, "r");
        if (!f) return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *val = eq + 1;
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';
        if (strcmp(p, "file") == 0) {
            strncpy(log_path, val, sizeof(log_path) - 1);
            log_path[sizeof(log_path) - 1] = '\0';
        } else if (strcmp(p, "console") == 0) {
            console_on = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
        }
    }
    fclose(f);
}

static void log_event(const char *msg) {
    if (out_fd >= 0) {
        write(out_fd, msg, strlen(msg));
    }
    if (console_on) {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0) {
            write(cfd, msg, strlen(msg));
            close(cfd);
        }
    }
}

/* Выполнить команду управления питанием (root only, /dev/sys).
 * Возвращает результат ioctl: для reboot/halt/poweroff управление сюда обычно
 * не возвращается, suspend возвращается после пробуждения. */
static int power_cmd(uint32_t cmd, const char *name) {
    char line[128];
    snprintf(line, sizeof(line), "powerd: executing %s\n", name);
    log_event(line);
    printf("%s", line);

    int fd = open("/dev/sys", O_RDWR);
    if (fd < 0) {
        snprintf(line, sizeof(line), "powerd: cannot open /dev/sys\n");
        log_event(line);
        printf("%s", line);
        return -1;
    }
    int r = ioctl(fd, CACT_SYSCTL_REBOOT, &cmd);
    close(fd);
    return r;
}

static void handle_client(int cl) {
    char req[LINE_MAX];
    char buf[LINE_MAX + 1];
    int  got = 0;
    int  n;

    /* Читаем строку запроса (до '\n'). */
    while (got < LINE_MAX - 1) {
        n = (int)recv(cl, buf, 1, 0);
        if (n <= 0) break;
        if (buf[0] == '\n' || buf[0] == '\r') break;
        req[got++] = buf[0];
    }
    req[got] = '\0';

    if (got == 0) {
        return; /* клиент закрыл соединение */
    }

    /* Убрать хвостовые пробелы. */
    while (got > 0 && (req[got - 1] == ' ' || req[got - 1] == '\t'))
        req[--got] = '\0';

    if (strcmp(req, "status") == 0) {
        send(cl, "ok running\n", 11, 0);
        log_event("powerd: status requested\n");
    } else if (strcmp(req, "reboot") == 0) {
        if (power_cmd(CACT_REBOOT_RESTART, "reboot") != 0)
            send(cl, "ERR reboot rejected\n", 20, 0);
    } else if (strcmp(req, "halt") == 0) {
        if (power_cmd(CACT_REBOOT_HALT, "halt") != 0)
            send(cl, "ERR halt rejected\n", 18, 0);
    } else if (strcmp(req, "poweroff") == 0) {
        if (power_cmd(CACT_REBOOT_POWEROFF, "poweroff") != 0)
            send(cl, "ERR poweroff rejected\n", 22, 0);
    } else if (strcmp(req, "suspend") == 0 || strcmp(req, "sleep") == 0) {
        /* Блокируется до пробуждения платформы. */
        if (power_cmd(CACT_REBOOT_SUSPEND, "suspend") == 0) {
            send(cl, "ok\n", 3, 0);
            log_event("powerd: resumed\n");
        } else {
            send(cl, "ERR suspend rejected\n", 21, 0);
        }
    } else {
        const char *err = "ERR unknown command\n";
        send(cl, err, (uint32_t)strlen(err), 0);
        char line[128];
        snprintf(line, sizeof(line), "powerd: unknown command '%s'\n", req);
        log_event(line);
    }
}

static int bind_listener(void) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(srv);
        return -1;
    }
    if (listen(srv, 4) < 0) {
        close(srv);
        return -1;
    }
    return srv;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("powerd: starting\n");
    config_load();
    ensure_dir("/var/log");
    ensure_dir("/run");

    out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        printf("powerd: cannot open %s\n", log_path);
    }
    log_event("powerd: starting\n");

    int srv = -1;
    for (;;) {
        if (srv < 0) {
            srv = bind_listener();
            if (srv < 0) {
                /* Не выходим: ждём, когда адрес освободится. */
                sleep(3);
                continue;
            }
            printf("powerd: listening on %s\n", SOCK_PATH);
            log_event("powerd: listening\n");
        }

        struct pollfd pfd;
        pfd.fd = srv;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
            int cl = accept(srv, 0, 0);
            if (cl >= 0) {
                handle_client(cl);
                close(cl);
            }
        }
    }
    return 0;
}
