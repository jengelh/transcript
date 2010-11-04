/* Copyright (C) 2010 G.P. Halkes
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3, as
   published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <cstring>

#include "ucm2cct.h"

Variant::Variant(Ucm *_base, const char *_id) : base(_base) {
	size_t len;

	while (strpbrk(_id, DIRSEPS) != NULL)
		_id = strpbrk(_id, DIRSEPS) + 1;

	if ((id = strdup(_id)) == NULL)
		OOM();

	len = strlen(id);
	if (len < 4 || strcmp(id + len - 4, ".ucm") == 0)
		id[len - 4] = 0;
}

int Variant::check_codepage_bytes(vector<uint8_t> &bytes) {
	return base->check_codepage_bytes(bytes);
}

const char *Variant::get_tag_value(tag_t tag) {
	return base->get_tag_value(tag);
}
