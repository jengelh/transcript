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
#include <algorithm>

#include "ucm2cct.h"

Variant::Variant(Ucm *_base, const char *_id) : base(_base) {
	size_t len;

	while (strpbrk(_id, DIRSEPS) != NULL)
		_id = strpbrk(_id, DIRSEPS) + 1;

	if ((id = strdup(_id)) == NULL)
		OOM();

	len = strlen(id);
	if (len < 4 || strcmp(id + len - 4, ".ucm") == 0) {
		len -= 4;
		id[len] = 0;
	}

	if (len > 255)
		fatal("%s: Variant name %s too long\n", file_name, id);
}

int Variant::check_codepage_bytes(vector<uint8_t> &bytes) {
	return base->check_codepage_bytes(bytes);
}

const char *Variant::get_tag_value(tag_t tag) {
	return base->get_tag_value(tag);
}

uint32_t Variant::size(void) {
	uint32_t result;

	result = 2 + 2; // Simple mappings size + Multi mappings size
	result += 12 * simple_mappings.size();

	for (vector<Mapping *>::iterator iter = multi_mappings.begin(); iter != multi_mappings.end(); iter++) {
		result += 2; // Byte count + Codepoint count
		result += (*iter)->codepage_bytes.size();
		for (vector<uint32_t>::iterator codepoint_iter = (*iter)->codepoints.begin();
				codepoint_iter != (*iter)->codepoints.end(); codepoint_iter++)
			result += (*codepoint_iter) > UINT32_C(0xffff) ? 4 : 2;
	}
	return result;
}

void Variant::sort_simple_mappings(void) {
	sort(simple_mappings.begin(), simple_mappings.end(), compareCodepageBytes);
	for (size_t idx = 0; idx < simple_mappings.size(); idx++)
		simple_mappings[idx]->idx = idx;

	sort(simple_mappings.begin(), simple_mappings.end(), compareCodepoints);
}

void Variant::dump(void) {
	printf("VARIANT %s\n", id);

	for (vector<Mapping *>::iterator iter = simple_mappings.begin(); iter != simple_mappings.end(); iter++)
		printf("%s %s |%d\n", sprint_codepoints((*iter)->codepoints), sprint_sequence((*iter)->codepage_bytes), (*iter)->precision);

	for (vector<Mapping *>::iterator iter = multi_mappings.begin(); iter != multi_mappings.end(); iter++)
		printf("%s %s |%d\n", sprint_codepoints((*iter)->codepoints), sprint_sequence((*iter)->codepage_bytes), (*iter)->precision);

	printf("END VARIANT\n");
}
