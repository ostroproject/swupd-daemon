/*
 * Daemon for controlling Clear Linux Software Update Client
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * Contact: Dmitry Rozhkov <dmitry.rozhkov@intel.com>
 *
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>

#include "log.h"
#include "list.h"
#include "global-state.h"

#define SWUPD_CLIENT    "swupd"

static const char * const _method_str_map[] = {
	NULL,
	"checkUpdate",
	"update",
	"verify",
	"bundleAdd",
	"bundleRemove"
};

static const char * const _method_opt_map[] = {
	NULL,
	"check-update",
	"update",
	"verify",
	"bundle-add",
	"bundle-remove"
};

static char **list_to_strv(struct list *strlist)
{
	char **strv;
	char **temp;

	strv = (char **)malloc((list_len(strlist) + 1) * sizeof(char *));
	memset(strv, 0x00, (list_len(strlist) + 1) * sizeof(char *));

	temp = strv;
	while (strlist)
	{
		*temp = strlist->data;
		temp++;
		strlist = strlist->next;
	}
	return strv;
}

static int is_in_array(const char *key, char const * const arr[])
{
	if (arr == NULL) {
		return 0;
	}

	char const * const *temp = arr;
	while (*temp) {
		if (strcmp(key, *temp) == 0) {
			return 1;
		}
		temp++;
	}
	return 0;
}

static int bus_message_read_option_string(struct list **strlist,
					  sd_bus_message *m,
					  const char *optname)
{
	int r = 0;
	char *option = NULL;
	const char *value;

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
	if (r < 0) {
		ERR("Failed to enter array container: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_read(m, "s", &value);
	if (r < 0) {
		ERR("Can't read option value: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit variant container: %s", strerror(-r));
		return r;
	}

	r = asprintf(&option, "--%s", optname);
	if (r < 0) {
		return -ENOMEM;
	}

	*strlist = list_append_data(*strlist, option);
	*strlist = list_append_data(*strlist, strdup(value));

	return 0;
}

static int bus_message_read_option_bool(struct list **strlist,
					  sd_bus_message *m,
					  const char *optname)
{
	int value;
	int r;

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b");
	if (r < 0) {
		ERR("Failed to enter array container: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_read(m, "b", &value);
	if (r < 0) {
		ERR("Can't read option value: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit variant container: %s", strerror(-r));
		return r;
	}

	if (value) {
		char *option = NULL;

		r = asprintf(&option, "--%s", optname);
		if (r < 0) {
			return -ENOMEM;
		}
		*strlist = list_append_data(*strlist, option);
	}
	return 0;
}

static int bus_message_read_options(struct list **strlist,
	                            sd_bus_message *m,
	                            char const * const opts_str[],
	                            char const * const opts_bool[])
{
	int r;

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
	if (r < 0) {
		ERR("Failed to enter array container: %s", strerror(-r));
		return r;
	}

	while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const char *argname;

                r = sd_bus_message_read(m, "s", &argname);
                if (r < 0) {
			ERR("Can't read argument name: %s", strerror(-r));
                        return r;
		}
		if (is_in_array(argname, opts_str)) {
			r = bus_message_read_option_string(strlist, m, argname);
			if (r < 0) {
				ERR("Can't read option '%s': %s", argname, strerror(-r));
				return r;
			}
		} else if (is_in_array(argname, opts_bool)) {
			r = bus_message_read_option_bool(strlist, m, argname);
			if (r < 0) {
				ERR("Can't read option '%s': %s", argname, strerror(-r));
				return r;
			}
		} else {
			r = sd_bus_message_skip(m, "v");
			if (r < 0) {
				ERR("Can't skip unwanted option value: %s", strerror(-r));
				return r;
			}
		}

                r = sd_bus_message_exit_container(m);
                if (r < 0) {
			ERR("Can't exit dict entry container: %s", strerror(-r));
                        return r;
		}
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit array container: %s", strerror(-r));
		return r;
	}

	return 0;
}

static void on_child_exit(int signum)
{
	int child_exit_status;
	int status = 0;
	char **lines = NULL;
	char **temp = NULL;
	sd_bus_message *m = NULL;
	int r;

	pid_t child_pid = global_state_get_child_pid();
	assert(child_pid);
	method_t child_method = global_state_get_child_method();
	assert(child_method);
	int channel_fd = global_state_get_child_channel_fd();
	assert(channel_fd >= 0);

	{
		FILE *stream = fdopen(channel_fd, "r");
		char *line = NULL;
		ssize_t count;
		size_t len = 0;
		size_t line_count = 0;

		while ((count = getline(&line, &len, stream)) != -1) {
			printf(line); /* echo child's output */
			line[strcspn(line, "\n")] = 0; /* trim newline */
			line_count++;
			lines = realloc(lines, sizeof(char*) * line_count);
			lines[line_count-1] = strdup(line);
		}
		free(line);
		/* realloc one extra element for the last NULL */
		lines = realloc(lines, sizeof(char*) * (line_count + 1));
		lines[line_count] = NULL;

		fclose(stream);
	}

	pid_t ws = waitpid(child_pid, &child_exit_status, 0);
	if (ws == -1) {
		ERR("Failed to wait for child process");
		assert(0);
	}

	if (WIFEXITED(child_exit_status)) {
		status = WEXITSTATUS(child_exit_status);
		if (status != 0) {
			DEBUG("Failed to run command");
		}
	} else {
		DEBUG("Child process was killed");
		status = 130; /* killed by SIGINT */
	}

	global_state_set_child_data(0, METHOD_NOTSET, -1);

	r = sd_bus_message_new_signal(global_state_get_bus(), &m,
		                      "/org/O1/swupdd/Client",
		                      "org.O1.swupdd.Client",
		                      "requestCompleted");
	if (r < 0) {
		ERR("Can't create D-Bus signal: %s", strerror(-r));
		goto finish;
	}
	r = sd_bus_message_append(m, "si", _method_str_map[child_method], status);
	if (r < 0) {
		ERR("Can't append method and status to D-Bus message: %s", strerror(-r));
		goto finish;
	}
	r = sd_bus_message_append_strv(m, lines);
	if (r < 0) {
		ERR("Can't append output to D-Bus message: %s", strerror(-r));
		goto finish;
	}
	r = sd_bus_send(global_state_get_bus(), m, NULL);
	if (r < 0) {
		ERR("Can't send message: %s", strerror(-r));
		goto finish;
	}

finish:
	temp = lines;
	while (*temp) {
		free(*temp);
		temp++;
	}
	free(lines);
	sd_bus_message_unref(m);
}

static int run_swupd(method_t method, struct list *args)
{
	pid_t pid;
	int fds[2];
	int r;

	r = pipe(fds);
	if (r < 0) {
		ERR("Can't create pipe: %s", strerror(errno));
		return -errno;
	}

	pid = fork();

	if (pid == 0) { /* child */
		dup2(STDOUT_FILENO, STDERR_FILENO);
		while ((dup2(fds[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {}
		close(fds[1]);
		close(fds[0]);
		char **argv = list_to_strv(list_head(args));
		execvp(*argv, argv);
		ERR("This line must not be reached");
		_exit(1);
	} else if (pid < 0) {
		ERR("Failed to fork: %s", strerror(errno));
		return -errno;
	}

	close(fds[1]);
	global_state_set_child_data(pid, method, fds[0]);

	return 0;
}

static int method_update(sd_bus_message *m,
	                 void *userdata,
	                 sd_bus_error *ret_error)
{
	int r = 0;
	struct list *args = NULL;

	if (global_state_get_child_pid()) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_UPDATE]));

	char const * const accepted_str_opts[] = {"url", "contenturl", "versionurl", "log", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, NULL);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	r  = run_swupd(METHOD_UPDATE, args);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_verify(sd_bus_message *m,
	                 void *userdata,
	                 sd_bus_error *ret_error)
{
	int r = 0;
	struct list *args = NULL;

	if (global_state_get_child_pid()) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_VERIFY]));

	char const * const accepted_str_opts[] = {"url", "contenturl", "versionurl", "log", NULL};
	char const * const accepted_bool_opts[] = {"fix", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, accepted_bool_opts);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	r  = run_swupd(METHOD_VERIFY, args);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_check_update(sd_bus_message *m,
			       void *userdata,
			       sd_bus_error *ret_error)
{
	int r = 0;
	struct list *args = NULL;

	if (global_state_get_child_pid()) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_CHECK_UPDATE]));

	char const * const accepted_str_opts[] = {"url", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, NULL);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	const char *bundle;
	r = sd_bus_message_read(m, "s", &bundle);
	if (r < 0) {
		ERR("Can't read bundle: %s", strerror(-r));
		goto finish;
	}
	args = list_append_data(args, strdup(bundle));

	r  = run_swupd(METHOD_CHECK_UPDATE, args);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_bundle_add(sd_bus_message *m,
			     void *userdata,
			     sd_bus_error *ret_error)
{
	int r = 0;
	struct list *args = NULL;
	const char* bundle = NULL;

	if (global_state_get_child_pid()) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_BUNDLE_ADD]));

	char const * const accepted_str_opts[] = {"url", NULL};
	char const * const accepted_bool_opts[] = {"list", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, accepted_bool_opts);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s");
	if (r < 0) {
		ERR("Can't enter container: %s", strerror(-r));
		goto finish;
	}
	while ((r = sd_bus_message_read(m, "s", &bundle)) > 0) {
		args = list_append_data(args, strdup(bundle));
	}
	if (r < 0) {
		ERR("Can't read bundle name from message: %s", strerror(-r));
		goto finish;
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit array container: %s", strerror(-r));
		goto finish;
	}

	r  = run_swupd(METHOD_BUNDLE_ADD, args);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_bundle_remove(sd_bus_message *m,
				void *userdata,
				sd_bus_error *ret_error)
{
	int r = 0;
	struct list *args = NULL;

	if (global_state_get_child_pid()) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_BUNDLE_REMOVE]));

	char const * const accepted_str_opts[] = {"url", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, NULL);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	const char *bundle;
	r = sd_bus_message_read(m, "s", &bundle);
	if (r < 0) {
		ERR("Can't read bundle: %s", strerror(-r));
		goto finish;
	}
	args = list_append_data(args, strdup(bundle));

	r  = run_swupd(METHOD_BUNDLE_REMOVE, args);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_cancel(sd_bus_message *m,
			 void *userdata,
			 sd_bus_error *ret_error)
{
	int r = 0;
	pid_t child;
	int force;

	child = global_state_get_child_pid();
	if (!child) {
		r = -ECHILD;
		ERR("No child process to cancel");
		goto finish;
	}

	r = sd_bus_message_read(m, "b", &force);
	if (r < 0) {
		ERR("Can't read 'force' option: %s", strerror(-r));
		goto finish;
	}

	if (force) {
		kill(child, SIGKILL);
	} else {
		kill(child, SIGINT);
	}

finish:
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static const sd_bus_vtable swupdd_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("checkUpdate", "a{sv}s", "b", method_check_update, 0),
	SD_BUS_METHOD("update", "a{sv}", "b", method_update, 0),
	SD_BUS_METHOD("verify", "a{sv}", "b", method_verify, 0),
	SD_BUS_METHOD("bundleAdd", "a{sv}as", "b", method_bundle_add, 0),
	SD_BUS_METHOD("bundleRemove", "a{sv}s", "b", method_bundle_remove, 0),
	SD_BUS_METHOD("cancel", "b", "b", method_cancel, 0),
	SD_BUS_SIGNAL("requestCompleted", "sias", 0),
	SD_BUS_VTABLE_END
};

int main(int argc, char *argv[]) {
	sd_bus_slot *slot = NULL; /* FIXME: not needed? */
	sd_event *event = NULL;
	sd_bus *bus = NULL;
	int r;

	global_state_reset();

	signal(SIGCHLD, on_child_exit);

        r = sd_event_default(&event);
        if (r < 0) {
                ERR("Failed to allocate event loop: %s", strerror(-r));
                goto finish;
        }

        sd_event_set_watchdog(event, 1);

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		ERR("Failed to connect to system bus: %s", strerror(-r));
		goto finish;
	}
	global_state_set_bus(bus);

	r = sd_bus_add_object_vtable(bus,
				     &slot,
				     "/org/O1/swupdd/Client",
				     "org.O1.swupdd.Client",
				     swupdd_vtable,
				     NULL);
	if (r < 0) {
		ERR("Failed to issue method call: %s", strerror(-r));
		goto finish;
	}

	r = sd_bus_request_name(bus, "org.O1.swupdd.Client", 0);
	if (r < 0) {
		ERR("Failed to acquire service name: %s", strerror(-r));
		goto finish;
	}

        r = sd_bus_attach_event(bus, event, 0);
        if (r < 0) {
                ERR("Failed to attach bus to event loop: %s", strerror(-r));
		goto finish;
	}

	for (;;) {
		r = sd_event_get_state(event);
		if (r < 0) {
			ERR("Failed to get event loop's state: %s", strerror(-r));
			goto finish;
		}
		if (r == SD_EVENT_FINISHED) {
			break;
		}

		r = sd_event_run(event, (uint64_t) -1 /*timeout*/);
		if (r < 0) {
			ERR("Failed to run event loop: %s", strerror(-r));
			goto finish;
		}

		if (r == 0) {
			r = sd_bus_try_close(bus);
			if (r == -EBUSY) {
				continue;
			}
			if (r < 0) {
				ERR("Failed to close bus: %s", strerror(-r));
				goto finish;
			}
			sd_event_exit(event, 0);
			break;
		}
	}

finish:
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);
	sd_event_unref(event);

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
