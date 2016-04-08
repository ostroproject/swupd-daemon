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

#include <string.h>      /* for memset() */
#include <sys/types.h>
#include <assert.h>
#include <systemd/sd-bus.h>
#include "global-state.h"

typedef struct _global_state
{
	pid_t child;
	sd_bus* bus;
	method_t method;
	int channel_fd;
} global_state_t;

static global_state_t gs;

void global_state_reset()
{
	memset(&gs, 0x00, sizeof(gs));
	gs.channel_fd = -1;
}

void global_state_set_child_data(pid_t child, method_t method, int channel_fd)
{
	/* Check that every second call either sets or unsets the global data. */
	assert((!gs.child && child) || (gs.child && !child));
	assert((!gs.method && method) || (gs.method && !method));
	assert((gs.channel_fd < 0 && channel_fd >= 0) || (gs.channel_fd >= 0 && channel_fd < 0));

	gs.child = child;
	gs.method = method;
	gs.channel_fd = channel_fd;
}

pid_t global_state_get_child_pid()
{
	return gs.child;
}

method_t global_state_get_child_method()
{
	return gs.method;
}

int global_state_get_child_channel_fd()
{
	return gs.channel_fd;
}

void global_state_set_bus(sd_bus *bus)
{
	/* Check that we set only once. */
	assert(!gs.bus);

	gs.bus = bus;
}

sd_bus* global_state_get_bus()
{
	/* By the time the bus is used it must be already initialized. */
	assert(gs.bus);

	return gs.bus;
}
