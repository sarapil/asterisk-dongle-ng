/*
 * Asterisk Channel Driver for modern 4G/5G modems
 * Project: asterisk-dongle-ng
 *
 * Phase 7: Call State Management & Reader Thread
 *
 * This version introduces a dedicated reader thread for each dongle to
 * listen for unsolicited AT responses (URCs). This allows us to detect
 * call state changes like CONNECT, BUSY, and NO CARRIER, and update
 * the Asterisk channel state accordingly.
 */

// --- المتطلبات الأساسية للموديول ---
#define AST_MODULE "chan_dongle_ng"
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_STANDARD

// --- Headers ---
#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/cli.h"
#include "asterisk/logger.h"
#include "asterisk/ast_version.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/causes.h"
#include "asterisk/frame.h"

// --- Headers خاصة بالنظام ---
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <dirent.h>
#include <poll.h>
#include <ctype.h>
#include <stdlib.h>

// --- تعريفات ---
#define MAX_DONGLES 16
#define DEVICE_PREFIX "ttyUSB"
#define RESPONSE_TIMEOUT 3000
#define CONFIG_FILE "/etc/asterisk/dongle_ng.conf"
#define RESET_SCRIPT_PATH "/usr/local/sbin/reset-usb.sh"

// --- هياكل البيانات (Structs) ---
enum dongle_state {
	DONGLE_STATE_FREE,
	DONGLE_STATE_INITIALIZING,
	DONGLE_STATE_READY,
	DONGLE_STATE_ACTIVE,
	DONGLE_STATE_ERROR,
};

struct dongle_pvt {
	struct dongle_device *dev;
	struct ast_channel *owner;
};

struct dongle_device {
	char name[80];
	char imei[16];
	char at_path[256];
	char audio_path[256];
	int at_fd;
	enum dongle_state state;
	ast_mutex_t lock;
	struct dongle_pvt *pvt;
	pthread_t reader_thread; // خيط الاستماع المخصص لهذا الجهاز
	int reader_thread_running; // علم للتحكم في الخيط
};

// --- متغيرات عامة ---
static struct dongle_device dongles[MAX_DONGLES];
static int num_dongles = 0;
static struct ast_config *cfg;

// --- تعريف دوال القناة (للإعلان المسبق) ---
static int dongle_call(struct ast_channel *ast, const char *dest, int timeout);
static int dongle_hangup(struct ast_channel *ast);
static struct ast_channel *dongle_requester(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *dest, int *cause);
static struct ast_frame *dongle_read(struct ast_channel *ast);
static int dongle_write(struct ast_channel *ast, struct ast_frame *frame);
static int dongle_indicate(struct ast_channel *ast, int condition, const void *data, size_t datalen);
static char *handle_cli_reset(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
static void *dongle_reader_main(void *data);


// --- هيكل تعريف تكنولوجيا القناة ---
static struct ast_channel_tech dongle_tech = {
	.type = "Dongle",
	.description = "GSM/4G Dongle Channel Driver",
	.requester = dongle_requester,
	.call = dongle_call,
	.hangup = dongle_hangup,
	.indicate = dongle_indicate,
	.read = dongle_read,
	.write = dongle_write,
};

// --- تعريف أوامر CLI ---
static struct ast_cli_entry cli_dongle[] = {
	AST_CLI_DEFINE(handle_cli_reset, "Reset a dongle device (by name or path)"),
};

// --- دوال المساعدة (Helper Functions) ---

static struct dongle_device *find_dongle_by_name(const char *name)
{
	int i;
	for (i = 0; i < num_dongles; i++) {
		if (strcmp(dongles[i].name, name) == 0) {
			return &dongles[i];
		}
	}
	return NULL;
}

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

	fd = open(path, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		ast_log(LOG_WARNING, "Probe failed for %s: Cannot open port: %s\n", path, strerror(errno));
		return -1;
	}
	ast_log(LOG_DEBUG, "Port %s opened successfully (fd=%d).\n", path, fd);

	if (set_interface_attribs(fd, B115200) != 0) {
		ast_log(LOG_WARNING, "Probe failed for %s: Cannot set interface attributes.\n", path);
		close(fd);
		return -1;
	}
	ast_log(LOG_DEBUG, "Port %s attributes set successfully.\n", path);
	
	ast_log(LOG_DEBUG, "Port %s: Waiting 1 second for device to settle...\n", path);
	sleep(1);
	
	ast_log(LOG_DEBUG, "Port %s: Sending multiple blind AT commands to wake up modem...\n", path);
	write(fd, "AT\r\n", 4);
	usleep(200000);
	write(fd, "AT\r\n", 4);
	usleep(200000);

	ast_log(LOG_DEBUG, "Port %s: Flushing any initial boot messages after wakeup...\n", path);
	flush_port(path, fd);
	
	if (send_command_and_wait(fd, path, "ATE0\r\n", response, sizeof(response)) != 0) {
		ast_log(LOG_NOTICE, "Probe failed for %s: No OK to ATE0.\n", path);
		close(fd);
		return -1;
	}

	if (send_command_and_wait(fd, path, "AT\r\n", response, sizeof(response)) != 0) {
		ast_log(LOG_NOTICE, "Probe failed for %s: No OK to AT.\n", path);
		close(fd);
		return -1;
	}

	if (send_command_and_wait(fd, path, "AT+CGSN\r\n", response, sizeof(response)) == 0) {
		for (line = strtok_r(response, "\r\n", &brkt); line; line = strtok_r(NULL, "\r\n", &brkt)) {
			char *p = line;
			while (*p && isspace((unsigned char)*p)) { p++; }
			if (strlen(p) >= 14 && strspn(p, "0123456789") == strlen(p)) {
				strncpy(imei_out, p, imei_size - 1);
				ast_log(LOG_NOTICE, "Probe SUCCESS for %s: Found IMEI %s\n", path, imei_out);
				close(fd);
				return 0;
			}
		}
	}

	ast_log(LOG_NOTICE, "Probe failed for %s: Could not extract IMEI from CGSN response.\n", path);
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
					
					if (cfg) {
						const char *category_name = NULL;
						while ((category_name = ast_category_browse(cfg, category_name))) {
							const char *imei_from_conf = ast_variable_retrieve(cfg, category_name, "imei");
							if (imei_from_conf && strcmp(imei_from_conf, probed_imei) == 0) {
								user_defined_name = category_name;
								break;
							}
						}
					}

					if (user_defined_name) {
						snprintf(d->name, sizeof(d->name), "%s", user_defined_name);
						ast_log(LOG_NOTICE, "Found device with IMEI %s on port %s, mapping to configured name [%s]\n", probed_imei, current_path, d->name);
					} else {
						snprintf(d->name, sizeof(d->name), "dongle%d", num_dongles);
						ast_log(LOG_NOTICE, "Found new device with IMEI %s on port %s, assigning default name [%s]\n", probed_imei, current_path, d->name);
					}
					
					strncpy(d->imei, probed_imei, sizeof(d->imei) - 1);
					strncpy(d->at_path, current_path, sizeof(d->at_path) - 1);
					
					d->at_fd = open(d->at_path, O_RDWR | O_NOCTTY);
					if (d->at_fd < 0) {
						ast_log(LOG_ERROR, "Failed to re-open AT port %s. Skipping device.\n", d->at_path);
						continue;
					}
					
					set_interface_attribs(d->at_fd, B115200);
					ast_mutex_init(&d->lock);
					d->state = DONGLE_STATE_READY;
					d->reader_thread_running = 1;
					// إنشاء وتشغيل خيط الاستماع
					if (ast_pthread_create_background(&d->reader_thread, NULL, dongle_reader_main, d)) {
						ast_log(LOG_ERROR, "Failed to create reader thread for %s\n", d->name);
						close(d->at_fd);
						continue;
					}

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


// --- دوال تنفيذ القناة ---

static int dongle_call(struct ast_channel *ast, const char *dest, int timeout)
{
	struct dongle_pvt *pvt = ast_channel_tech_pvt(ast);
	struct dongle_device *dev = pvt->dev;
	char cmdbuf[256];

	if (ast_channel_state(ast) != AST_STATE_DOWN) {
		return 0;
	}

	ast_log(LOG_NOTICE, "Dongle-NG (%s): Dialing %s\n", dev->name, dest);
	
	ast_channel_state_set(ast, AST_STATE_DIALING);
	
	snprintf(cmdbuf, sizeof(cmdbuf), "ATD%s;\r\n", dest);
	
	ast_mutex_lock(&dev->lock);
	write(dev->at_fd, cmdbuf, strlen(cmdbuf));
	ast_mutex_unlock(&dev->lock);

	// لا نغير الحالة هنا، سننتظر رد CONNECT من خيط الاستماع
	return 0;
}

static int dongle_hangup(struct ast_channel *ast)
{
	struct dongle_pvt *pvt = ast_channel_tech_pvt(ast);
	struct dongle_device *dev;

	if (!pvt || !pvt->dev) {
		return 0;
	}
	dev = pvt->dev;

	ast_log(LOG_NOTICE, "Dongle-NG (%s): Hanging up channel\n", dev->name);

	ast_mutex_lock(&dev->lock);
	write(dev->at_fd, "ATH\r\n", 5);
	ast_mutex_unlock(&dev->lock);

	dev->state = DONGLE_STATE_READY;
	dev->pvt = NULL;
	
	ast_free(pvt);
	ast_channel_tech_pvt_set(ast, NULL);

	return 0;
}

static int dongle_indicate(struct ast_channel *ast, int condition, const void *data, size_t datalen)
{
	return 0;
}

static int dongle_write(struct ast_channel *ast, struct ast_frame *frame)
{
	return 0;
}

static struct ast_frame *dongle_read(struct ast_channel *ast)
{
	ast_log(LOG_NOTICE, "Dongle-NG: Audio read requested, but not implemented. Hanging up.\n");
	ast_queue_hangup(ast);
	return NULL;
}

static struct ast_channel *dongle_requester(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *dest, int *cause)
{
	struct ast_channel *ast;
	struct dongle_pvt *pvt;
	struct dongle_device *dev;
	char *dev_name, *number;
	char *parse_str, *to_free;

	ast_log(LOG_NOTICE, "Dongle-NG: Request for new channel to %s\n", dest);

	if (ast_strlen_zero(dest)) {
		return NULL;
	}
	
	to_free = parse_str = ast_strdup(dest);
	if (!parse_str) {
		return NULL;
	}
	dev_name = strsep(&parse_str, "/");
	number = parse_str;

	if (ast_strlen_zero(dev_name) || ast_strlen_zero(number)) {
		ast_log(LOG_WARNING, "Invalid destination format. Use 'Dongle/device_name/number'.\n");
		ast_free(to_free);
		return NULL;
	}

	dev = find_dongle_by_name(dev_name);
	if (!dev) {
		ast_log(LOG_WARNING, "Device [%s] not found.\n", dev_name);
		ast_free(to_free);
		return NULL;
	}

	ast_mutex_lock(&dev->lock);
	if (dev->state != DONGLE_STATE_READY) {
		ast_log(LOG_WARNING, "Device [%s] is busy.\n", dev_name);
		*cause = AST_CAUSE_BUSY;
		ast_mutex_unlock(&dev->lock);
		ast_free(to_free);
		return NULL;
	}
	dev->state = DONGLE_STATE_ACTIVE;
	ast_mutex_unlock(&dev->lock);

	pvt = ast_calloc(1, sizeof(*pvt));
	if (!pvt) {
		ast_free(to_free);
		return NULL;
	}

	pvt->dev = dev;
	
	ast = ast_channel_alloc(1, AST_STATE_DOWN, dev->name, dev->name, "", number, "default", assignedids, requestor, 0, "%s/%s", dongle_tech.type, dev->name);
	if (!ast) {
		ast_free(pvt);
		ast_free(to_free);
		return NULL;
	}
	
	ast_channel_tech_pvt_set(ast, pvt);
	pvt->owner = ast;
	dev->pvt = pvt;
	
	ast_channel_tech_set(ast, &dongle_tech);
	ast_channel_nativeformats_set(ast, cap);

	ast_free(to_free);
	
	if (ast_pbx_start(ast)) {
		ast_log(LOG_WARNING, "Unable to start PBX on channel\n");
		ast_hangup(ast);
		return NULL;
	}

	return ast;
}


// --- دوال CLI ---
static char *handle_cli_reset(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char cmdbuf[512];
    const char *target_path = NULL;
    char temp_path[256];

	if (a->argc < 3) {
		return CLI_SHOWUSAGE;
	}

    if (a->argc == 3) {
        struct dongle_device *d = find_dongle_by_name(a->argv[2]);
        if (!d) {
            ast_cli(a->fd, "Device '%s' not found. Try 'dongle reset path /dev/ttyUSBx' instead.\n", a->argv[2]);
            return CLI_SUCCESS;
        }
        if (ast_strlen_zero(d->at_path)) {
            ast_cli(a->fd, "Device '%s' does not have a valid device path.\n", d->name);
            return CLI_SUCCESS;
        }
        target_path = d->at_path;
    } 
    else if (a->argc == 4 && strcasecmp(a->argv[2], "path") == 0) {
        if (strncmp(a->argv[3], "/dev/ttyUSB", 11) != 0) {
            ast_cli(a->fd, "Invalid path: '%s'. Path must start with /dev/ttyUSB.\n", a->argv[3]);
            return CLI_SUCCESS;
        }
        strncpy(temp_path, a->argv[3], sizeof(temp_path) - 1);
        temp_path[sizeof(temp_path) - 1] = '\0';
        target_path = temp_path;
    } 
    else {
        return CLI_SHOWUSAGE;
    }

	ast_cli(a->fd, "Attempting to reset device at path %s...\n", target_path);
	
	snprintf(cmdbuf, sizeof(cmdbuf), "sudo %s %s", RESET_SCRIPT_PATH, target_path);
	system(cmdbuf);

	ast_cli(a->fd, "Reset command sent. Please wait a few seconds, then 'module reload' to re-scan devices.\n");

	return CLI_SUCCESS;
}

// --- خيط الاستماع الرئيسي ---
static void *dongle_reader_main(void *data)
{
	struct dongle_device *d = data;
	char buffer[1024];
	struct pollfd pfd;

	ast_log(LOG_NOTICE, "Dongle-NG (%s): Reader thread started.\n", d->name);

	pfd.fd = d->at_fd;
	pfd.events = POLLIN;

	while(d->reader_thread_running) {
		int n = poll(&pfd, 1, 1000); // انتظر ثانية واحدة
		if (n < 0) {
			ast_log(LOG_ERROR, "Dongle-NG (%s): Poll error in reader thread. Exiting.\n", d->name);
			break;
		}
		if (n == 0) { // لا توجد بيانات
			continue;
		}

		if (pfd.revents & POLLIN) {
			memset(buffer, 0, sizeof(buffer));
			n = read(d->at_fd, buffer, sizeof(buffer) - 1);
			if (n <= 0) {
				ast_log(LOG_ERROR, "Dongle-NG (%s): Read error in reader thread. Exiting.\n", d->name);
				break;
			}
			
			ast_log(LOG_NOTICE, "Dongle-NG (%s): Received URC: %s\n", d->name, buffer);

			// تحليل الرسائل الواردة
			if (d->pvt && d->pvt->owner) {
				if (strstr(buffer, "CONNECT")) {
					ast_log(LOG_NOTICE, "Dongle-NG (%s): Call connected! Answering channel.\n", d->name);
					ast_channel_answer(d->pvt->owner);
				} else if (strstr(buffer, "BUSY")) {
					ast_log(LOG_NOTICE, "Dongle-NG (%s): Call is busy. Hanging up.\n", d->name);
					ast_queue_hangup_with_cause(d->pvt->owner, AST_CAUSE_BUSY);
				} else if (strstr(buffer, "NO CARRIER")) {
					ast_log(LOG_NOTICE, "Dongle-NG (%s): No carrier. Hanging up.\n", d->name);
					ast_queue_hangup_with_cause(d->pvt->owner, AST_CAUSE_NO_ANSWER);
				}
			}
		}
	}

	ast_log(LOG_NOTICE, "Dongle-NG (%s): Reader thread finished.\n", d->name);
	return NULL;
}


// --- دوال الموديول الرئيسية ---
static int load_module(void)
{
	struct ast_flags config_flags = { 0 };
	
	ast_log(LOG_NOTICE, "Dongle-NG: Module is loading.\n");
	
	if (ast_channel_register(&dongle_tech)) {
		ast_log(LOG_ERROR, "CRITICAL: FAILED to register channel type 'Dongle'.\n");
		return AST_MODULE_LOAD_FAILURE;
	}
	ast_log(LOG_NOTICE, "SUCCESS: Channel type 'Dongle' is now registered.\n");

	ast_cli_register_multiple(cli_dongle, ARRAY_LEN(cli_dongle));

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
	
	ast_log(LOG_NOTICE, "Unregistering channel type 'Dongle'...\n");
	ast_channel_unregister(&dongle_tech);
	ast_cli_unregister_multiple(cli_dongle, ARRAY_LEN(cli_dongle));
	
	if (cfg) {
		ast_config_destroy(cfg);
		cfg = NULL;
	}

	for (i = 0; i < num_dongles; i++) {
		struct dongle_device *d = &dongles[i];
		if (d->state != DONGLE_STATE_FREE) {
			ast_log(LOG_NOTICE, "Dongle-NG (%s): Shutting down device...\n", d->name);
			d->reader_thread_running = 0;
			pthread_join(d->reader_thread, NULL);
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
