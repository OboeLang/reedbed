/* SPDX-License-Identifier: GPL-2.0-only
 * © 2026 Sushii64
 * © 2026 robinpie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include "proto.h"

#include "izim.h"

#include <string.h>

const char *rb_status_word(enum rb_status s)
{
	switch (s) {
	case ST_SI:
		return "si";
	case ST_KERESEBYR:
		return "keresebyr";
	case ST_SENTYRE:
		return "sentyre";
	case ST_EZHAZEBYR:
		return "ezhazebyr";
	case ST_VAZOJ:
		return "vazoj";
	case ST_RAMUZHU:
		return "ramuzhu";
	case ST_WUWOJI:
		return "wuwoji";
	case ST_BYR:
		return "byr";
	case ST_KOJA:
		return "koja";
	}
	return "byr";
}

bool rb_agent_valid(const char *s)
{
	if (!s)
		return false;

	size_t n = strlen(s);

	if (n == 0 || n > 64)
		return false;

	const char *slash = strchr(s, '/');

	if (!slash || slash == s || slash[1] == '\0')
		return false;
	if (strchr(slash + 1, '/'))
		return false;

	char name[IZIM_MAX + 1];
	size_t nlen = (size_t)(slash - s);

	if (nlen > IZIM_MAX)
		return false;
	memcpy(name, s, nlen);
	name[nlen] = '\0';
	if (!izim_valid(name))
		return false;

	for (const char *p = slash + 1; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
			return false;
	}
	return true;
}
