/*
 * Asterisk Channel Driver for modern 4G/5G modems
 * Project: asterisk-dongle-ng
 *
 * Phase 5: Persistent Naming (v5 - Final Fix)
 *
 * This version fixes the critical "undefined symbol: ast_category_next"
 * error by using the correct API for iterating through config file
 * categories. This should be the final fix for loading the module.
 */

// --- المتطلبات الأساسية للموديول ---
#define AST_MODULE "chan_dongle_ng"
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_STANDARD

// --- Headers ---
#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/logger.h"
#include "asterisk/ast_version.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"

// --- Headers خاصة بالنظام ---
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <dirent.h>
#include <poll.h>
#include <ctype.h>

// --- تعريفات ---
#define MAX_DONGLES 16
#define DEVICE_PREFIX "ttyUSB"
#define RESPONSE_TIMEOUT 3000
#define CONFIG_FILE "/etc/asterisk/dongle_ng.conf"

// --- هياكل البيانات (Structs) ---
enum dongle_state {
	DONGLE_STATE_FREE,
	DONGLE_STATE_INITIALIZING,
	DONGLE_STATE_READY,
	DONGLE_STATE_ERROR,
};

struct dongle_device {
	char name[80];
	char imei[16];
	char at_path[256];
	char audio_path[256];
	int at_fd;
	enum dongle_state state;
	ast_mutex_t lock;
};

// --- متغيرات عامة ---
static struct dongle_device dongles[MAX_DONGLES];
static int num_dongles = 0;
static struct ast_config *cfg;

// --- دوال المساعدة (Helper Functions) ---

static struct dongle_device *find_dongle_by_imei(const char *imei)
{
	int i;
	for (i = 0; i < num_dongles; i++) {
		if (strcmp(dongles[i].imei, imei) == 0) {
			return &dongles[i];
		}
	}
	return NULL;
}

static int set_interface_attribs(int fd, int speed)
{
	struct termios tty;
	if (tcgetattr(fd, &tty) < 0) { return -1; }
	cfsetospeed(&tty, (speed_t)speed);
	cfsetispeed(&tty, (speed_t)speed);
	tty.c_cflag |= (CLOCAL | CREAD); tty.c_cflag &= ~CSIZE; tty.c_cflag |= CS8;
	tty.c_cflag &= ~PARENB; tty.c_cflag &= ~CSTOPB; tty.c_cflag &= ~CRTSCTS;
	tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tty.c_oflag &= ~OPOST;
	tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;
	if (tcsetattr(fd, TCSANOW, &tty) != 0) { return -1; }
	return 0;
}

static void flush_port(const char *path, int fd)
{
	char buffer[256];
	struct pollfd pfd;
	int i;
	for (i = 0; i < 5; i++) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 100) > 0) {
			if (pfd.revents & POLLIN) {
				read(fd, buffer, sizeof(buffer) - 1);
			}
		} else {
			break;
		}
	}
}

static int send_command_and_wait(int fd, const char *path, const char *command, char *response_buf, size_t buf_size)
{
	int n;
	struct pollfd pfd;
	char read_buf[1024];
	int res_len = 0;

	if (response_buf) {
		memset(response_buf, 0, buf_size);
	}

	n = write(fd, command, strlen(command));
	if (n < 0) {
		return -1;
	}

	pfd.fd = fd;
	pfd.events = POLLIN;

	while (1) {
		n = poll(&pfd, 1, RESPONSE_TIMEOUT);
		if (n <= 0) {
			return -1;
		}

		if (pfd.revents & POLLIN) {
			memset(read_buf, 0, sizeof(read_buf));
			n = read(fd, read_buf, sizeof(read_buf) - 1);
			if (n > 0) {
				if (response_buf && (res_len + n < buf_size)) {
					strncat(response_buf, read_buf, n);
					res_len += n;
				}
				if (strstr(read_buf, "OK") || strstr(read_buf, "ERROR")) {
					break;
				}
			} else if (n < 0) {
				return -1;
			}
		}
	}
	return (strstr(response_buf, "OK")) ? 0 : -1;
}

static int probe_port_for_imei(const char *path, char *imei_out, size_t imei_size)
{
	char response[1024];
	int fd;
	char *line, *brkt;

	ast_log(LOG_NOTICE, "Probing port %s...\n", path);

	fd = open(path, O_RDWR | O_NOCTTY | O_SYNC);
	if (fd < 0) {
		return -1;
	}

	set_interface_attribs(fd, B115200);
	
	sleep(1);
	flush_port(path, fd);
	write(fd, "AT\r\n", 4);
	usleep(200000);
	flush_port(path, fd);
	
	if (send_command_and_wait(fd, path, "ATE0\r\n", response, sizeof(response)) != 0) {
		close(fd);
		return -1;
	}

	if (send_command_and_wait(fd, path, "AT\r\n", response, sizeof(response)) != 0) {
		close(fd);
		return -1;
	}

	if (send_command_and_wait(fd, path, "AT+CGSN\r\n", response, sizeof(response)) == 0) {
		for (line = strtok_r(response, "\r\n", &brkt); line; line = strtok_r(NULL, "\r\n", &brkt)) {
			char *p = line;
			while (*p && isspace((unsigned char)*p)) { p++; }
			if (strlen(p) >= 14 && strspn(p, "0123456789") == strlen(p)) {
				strncpy(imei_out, p, imei_size - 1);
				close(fd);
				return 0;
			}
		}
	}

	close(fd);
	return -1;
}

static int scan_for_dongles(void)
{
	DIR *dir;
	struct dirent *entry;

	if (!(dir = opendir("/dev/"))) {
		ast_log(LOG_ERROR, "Failed to open /dev directory\n");
		return -1;
	}

	ast_log(LOG_NOTICE, "Scanning for dongle devices...\n");

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, DEVICE_PREFIX, strlen(DEVICE_PREFIX)) == 0) {
			char current_path[256];
			char probed_imei[16] = {0};
			snprintf(current_path, sizeof(current_path), "/dev/%s", entry->d_name);

			if (probe_port_for_imei(current_path, probed_imei, sizeof(probed_imei)) == 0) {
				if (find_dongle_by_imei(probed_imei) != NULL) {
					ast_log(LOG_NOTICE, "Found additional AT port %s for already registered IMEI %s. Ignoring for now.\n", current_path, probed_imei);
					continue;
				}

				if (num_dongles < MAX_DONGLES) {
					struct dongle_device *d = &dongles[num_dongles];
					const char *user_defined_name = NULL;
					
					// --- FINAL, CORRECTED CONFIG PARSING LOGIC ---
					if (cfg) {
						struct ast_category *category = NULL;
						const char *category_name;
						// Use the correct API to iterate through categories
						while ((category = ast_category_browse(cfg, category))) {
							category_name = ast_category_get_name(category);
							const char *imei_from_conf = ast_variable_retrieve(cfg, category_name, "imei");
							if (imei_from_conf && strcmp(imei_from_conf, probed_imei) == 0) {
								user_defined_name = category_name;
								break;
							}
						}
					}
					// --- END OF CORRECTION ---

					if (user_defined_name) {
						snprintf(d->name, sizeof(d->name), "%s", user_defined_name);
						ast_log(LOG_NOTICE, "Found device with IMEI %s on port %s, mapping to configured name [%s]\n", probed_imei, current_path, d->name);
					} else {
						snprintf(d->name, sizeof(d->name), "dongle%d", num_dongles);
						ast_log(LOG_NOTICE, "Found new device with IMEI %s on port %s, assigning default name [%s]\n", probed_imei, current_path, d->name);
					}
					
					strncpy(d->imei, probed_imei, sizeof(d->imei) - 1);
					strncpy(d->at_path, current_path, sizeof(d->at_path) - 1);
					
					d->at_fd = open(d->at_path, O_RDWR | O_NOCTTY | O_SYNC);
					if (d->at_fd < 0) {
						ast_log(LOG_ERROR, "Failed to re-open AT port %s. Skipping device.\n", d->at_path);
						continue;
					}
					
					set_interface_attribs(d->at_fd, B115200);
					ast_mutex_init(&d->lock);
					d->state = DONGLE_STATE_READY;
					ast_log(LOG_NOTICE, "Dongle-NG (%s): Device is now in READY state.\n", d->name);
					num_dongles++;
				}
			}
		}
	}

	closedir(dir);
	ast_log(LOG_NOTICE, "Scan complete. Found and configured %d unique dongle(s).\n", num_dongles);
	return num_dongles;
}

// --- دوال الموديول الرئيسية ---
static int load_module(void)
{
	struct ast_flags config_flags = { 0 };
	
	ast_log(LOG_NOTICE, "Dongle-NG: Module is loading.\n");
	
	config_flags.flags = CONFIG_FLAG_NOCACHE;
	cfg = ast_config_load(CONFIG_FILE, config_flags);
	if (!cfg) {
		ast_log(LOG_WARNING, "Unable to load config file '%s'. Using default names.\n", CONFIG_FILE);
	} else {
		ast_log(LOG_NOTICE, "Loaded config file '%s'.\n", CONFIG_FILE);
	}

	memset(dongles, 0, sizeof(dongles));
	num_dongles = 0;
	scan_for_dongles();

	if (num_dongles == 0) {
		ast_log(LOG_WARNING, "Dongle-NG: No dongle devices found or initialized.\n");
	}
	ast_log(LOG_NOTICE, "Dongle-NG: Module loaded successfully.\n");
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	int i;
	ast_log(LOG_NOTICE, "Dongle-NG: Unloading module.\n");
	
	if (cfg) {
		ast_config_destroy(cfg);
		cfg = NULL;
	}

	for (i = 0; i < num_dongles; i++) {
		struct dongle_device *d = &dongles[i];
		if (d->state != DONGLE_STATE_FREE) {
			ast_log(LOG_NOTICE, "Dongle-NG (%s): Shutting down device...\n", d->name);
			d->state = DONGLE_STATE_FREE;
			close(d->at_fd);
			ast_mutex_destroy(&d->lock);
		}
	}
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Next-Gen Dongle Channel Driver",
	.load = load_module,
	.unload = unload_module
);
