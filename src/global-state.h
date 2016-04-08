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

#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

/* For the sake of simplicity we use simple POSIX signal handler to emit
 * D-Bus signals upon completion of the swupd command. As signals'
 * nature is global we'd better have means to update the global state
 * in a controlable manner. Hence this module.
 */

typedef enum {
	METHOD_NOTSET = 0,
	METHOD_CHECK_UPDATE,
	METHOD_UPDATE,
	METHOD_VERIFY,
	METHOD_BUNDLE_ADD,
	METHOD_BUNDLE_REMOVE
} method_t;

void global_state_reset();

/* Setter and getters for a child's data.
 * Every second call to the setter must either set a newly launched child's
 * data or unset the data upon the child's exit.
 */
void global_state_set_child_data(pid_t child, method_t method, int channel_fd);
pid_t global_state_get_child_pid();
method_t global_state_get_child_method();
int global_state_get_child_channel_fd();

/* Setter and getter for a bus.
 * The bus is supposed to be set only once as a part of initialization
 * procedure. Any update is considered to be an error.
 */
void global_state_set_bus(sd_bus *bus);
sd_bus* global_state_get_bus();

#endif /* GLOBAL_STATE_H */
