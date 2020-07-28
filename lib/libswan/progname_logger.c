/* logging declaratons
 *
 * Copyright (C) 1998-2001,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2004 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2017-2019 Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include "lswlog.h"
#include "jambuf.h"

static size_t jam_progname_prefix(struct jambuf *buf, const void *object UNUSED)
{
	if (progname_logger.object != NULL) {
		return jam(buf, "%s: ", (const char *)progname_logger.object);
	} else {
		return 0;
	}
}

static bool suppress_progname_log(const void *object UNUSED)
{
	return false;
}

const struct logger_object_vec progname_object_vec = {
	.name = "tool",
	.jam_object_prefix = jam_progname_prefix,
	.suppress_object_log = suppress_progname_log,
};

struct logger progname_logger = {
	.object_vec = &progname_object_vec,
	.object = NULL, /* progname */
};
