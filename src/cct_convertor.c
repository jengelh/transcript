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
#include <string.h>
#include <pthread.h>

#include "charconv.h"
#include "charconv_errors.h"
#include "cct_convertor.h"
#include "utf.h"

typedef struct {
	charconv_common_t common;
	convertor_t *convertor;
	uint8_t to_state, from_state;
} convertor_state_t;

typedef struct {
	uint8_t to_state, from_state;
} save_state_t;

static int to_unicode_skip(convertor_state_t *handle, char **inbuf, size_t *inbytesleft);
static void close_convertor(convertor_state_t *handle);

static pthread_mutex_t cct_list_mutex = PTHREAD_MUTEX_INITIALIZER;
convertor_t *cct_head = NULL;
static put_unicode_func_t put_utf16;

#define PUT_UNICODE(codepoint) do { int result; \
	if ((result = handle->common.put_unicode(codepoint, outbuf, outbytesleft)) != 0) \
		return result; \
} while (0)

static inline size_t min(size_t a, size_t b) {
	return a < b ? a : b;
}

static int to_unicode_conversion(convertor_state_t *handle, char **inbuf, size_t *inbytesleft,
		char **outbuf, size_t *outbytesleft, int flags)
{
	uint8_t *_inbuf = (uint8_t *) *inbuf;
	size_t _inbytesleft = *inbytesleft;
	uint_fast8_t state = handle->to_state;
	uint_fast32_t idx = handle->convertor->codepage_states[handle->to_state].base;
	uint_fast32_t codepoint;
	entry_t *entry;
	uint_fast8_t conv_flags;

	while (_inbytesleft > 0) {
		entry = &handle->convertor->codepage_states[state].entries[handle->convertor->codepage_states[state].map[*_inbuf]];

		idx += entry->base + (uint_fast32_t)(*_inbuf - entry->low) * entry->mul;
		_inbuf++;
		_inbytesleft--;

		switch (entry->action) {
			case ACTION_FINAL:
			case ACTION_FINAL_PAIR:
				conv_flags = handle->convertor->codepage_flags.get_flags(&handle->convertor->codepage_flags, idx);
				if ((conv_flags & TO_UNICODE_MULTI_START) && !(flags & CHARCONV_NO_MN_CONVERSION)) {
					size_t outbytesleft_tmp, check_len;
					uint_fast32_t i, j;
					char *outbuf_tmp;
					int result;

					for (i = 0; i < handle->convertor->nr_multi_mappings; i++) {
						check_len = min(handle->convertor->multi_mappings[i].bytes_length, *inbytesleft);

						if (memcmp(handle->convertor->multi_mappings[i].bytes, *inbuf, check_len) != 0)
							continue;

						if (check_len != handle->convertor->multi_mappings[i].bytes_length) {
							if (flags & CHARCONV_END_OF_TEXT)
								continue;
							return CHARCONV_INCOMPLETE;
						}

						outbuf_tmp = *outbuf;
						outbytesleft_tmp = *outbytesleft;
						for (j = 0; j < handle->convertor->multi_mappings[i].codepoints_length; j++) {
							codepoint = handle->convertor->multi_mappings[i].codepoints[j];
							if (codepoint >= UINT32_C(0xD800) && codepoint <= UINT32_C(0xD8FF)) {
								j++;
								codepoint -= UINT32_C(0xD800);
								codepoint <<= 10;
								codepoint += handle->convertor->multi_mappings[i].codepoints[j] - UINT32_C(0xDC00);
								codepoint += 0x10000;
							}
							if ((result = handle->common.put_unicode(codepoint, &outbuf_tmp, &outbytesleft_tmp)) != 0)
								return result;
						}
						*outbuf = outbuf_tmp;
						*outbytesleft = outbytesleft_tmp;

						handle->to_state = state = entry->next_state;
						*inbuf = (char *) _inbuf;
						check_len = (*inbytesleft) - check_len;
						*inbytesleft = _inbytesleft;
						while (*inbytesleft > check_len)
							if (to_unicode_skip(handle, inbuf, inbytesleft) != 0)
								return CHARCONV_INTERNAL_ERROR;
						idx = handle->convertor->codepage_states[handle->to_state].base;
						break;
					}
					if (i != handle->convertor->nr_multi_mappings)
						continue;
				}

				if ((conv_flags & TO_UNICODE_PRIVATE_USE) && !(flags & CHARCONV_ALLOW_PRIVATE_USE)) {
					if (!(flags & CHARCONV_SUBSTITUTE))
						return CHARCONV_PRIVATE_USE;
					PUT_UNICODE(UINT32_C(0xFFFD));
					goto sequence_done;
				}
				if ((conv_flags & TO_UNICODE_FALLBACK) && !(flags & CHARCONV_ALLOW_FALLBACK))
					return CHARCONV_FALLBACK;

				codepoint = handle->convertor->codepage_mappings[idx];
				if (codepoint == UINT32_C(0xFFFF)) {
					if (!(flags & CHARCONV_SUBSTITUTE))
						return CHARCONV_UNASSIGNED;
					PUT_UNICODE(UINT32_C(0xFFFD));
				} else {
					if (entry->action == ACTION_FINAL_PAIR && codepoint >= UINT32_C(0xD800) && codepoint <= UINT32_C(0xD8FF)) {
						codepoint -= UINT32_C(0xD800);
						codepoint <<= 10;
						codepoint += handle->convertor->codepage_mappings[idx + 1] - UINT32_C(0xDC00);
						codepoint += 0x10000;
					}
					PUT_UNICODE(codepoint);
				}
				goto sequence_done;
			case ACTION_VALID:
				state = entry->next_state;
				break;
			case ACTION_ILLEGAL:
				if (!(flags & CHARCONV_SUBSTITUTE_ALL))
					return CHARCONV_ILLEGAL;
				PUT_UNICODE(UINT32_C(0xFFFD));
				goto sequence_done;
			case ACTION_UNASSIGNED:
				if (!(flags & CHARCONV_SUBSTITUTE))
					return CHARCONV_UNASSIGNED;
				PUT_UNICODE(UINT32_C(0xFFFD));
				/* FALLTHROUGH */
			case ACTION_SHIFT:
			sequence_done:
				*inbuf = (char *) _inbuf;
				*inbytesleft = _inbytesleft;
				handle->to_state = state = entry->next_state;
				idx = handle->convertor->codepage_states[handle->to_state].base;
				if (flags & CHARCONV_SINGLE_CONVERSION)
					return CHARCONV_SUCCESS;
				break;
			default:
				return CHARCONV_INTERNAL_ERROR;
		}
	}

	if (*inbytesleft != 0) {
		if (flags & CHARCONV_END_OF_TEXT) {
			if (!(flags & CHARCONV_SUBSTITUTE_ALL))
				return CHARCONV_ILLEGAL_END;
			PUT_UNICODE(UINT32_C(0xFFFD));
			*inbuf += *inbytesleft;
			*inbytesleft = 0;
		} else {
			return CHARCONV_INCOMPLETE;
		}
	}
	return CHARCONV_SUCCESS;
}

static int to_unicode_skip(convertor_state_t *handle, char **inbuf, size_t *inbytesleft) {
	uint8_t *_inbuf = (uint8_t *) *inbuf;
	size_t _inbytesleft = *inbytesleft;
	uint_fast8_t state = handle->to_state;
	uint_fast32_t idx = handle->convertor->codepage_states[handle->to_state].base;
	entry_t *entry;

	while (_inbytesleft > 0) {
		entry = &handle->convertor->codepage_states[state].entries[handle->convertor->codepage_states[state].map[*_inbuf]];

		idx += entry->base + (uint_fast32_t)(*_inbuf - entry->low) * entry->mul;
		_inbuf++;
		_inbytesleft--;

		switch (entry->action) {
			case ACTION_SHIFT:
			case ACTION_VALID:
				state = entry->next_state;
				break;
			case ACTION_FINAL:
			case ACTION_FINAL_PAIR:
			case ACTION_ILLEGAL:
			case ACTION_UNASSIGNED:
				*inbuf = (char *) _inbuf;
				*inbytesleft = _inbytesleft;
				handle->to_state = state = entry->next_state;
				return CHARCONV_SUCCESS;
			default:
				return CHARCONV_INTERNAL_ERROR;
		}
	}

	return CHARCONV_INCOMPLETE;
}

static void to_unicode_reset(convertor_state_t *handle) {
	handle->to_state = 0;
}


#define GET_UNICODE() do { \
	codepoint = handle->common.get_unicode((char **) &_inbuf, &_inbytesleft, t3_false); \
} while (0)

#define PUT_BYTES(count, buffer) do { \
	if (put_bytes(handle, outbuf, outbytesleft, count, buffer) == CHARCONV_NO_SPACE) \
		return CHARCONV_NO_SPACE; \
} while (0)

static int put_bytes(convertor_state_t *handle, char **outbuf, size_t *outbytesleft, size_t count, uint8_t *bytes) {
	uint_fast8_t required_state;
	uint_fast8_t i;

	if (handle->convertor->flags & MULTIBYTE_START_STATE_1) {
		required_state = count > 1 ? 1 : 0;
		if (handle->from_state != required_state) {
			for (i = 0; i < handle->convertor->nr_shift_states; i++) {
				if (handle->convertor->shift_states[i].from_state == handle->from_state &&
						handle->convertor->shift_states[i].to_state == required_state)
				{
					if (*outbytesleft < count + handle->convertor->shift_states[i].len)
						return CHARCONV_NO_SPACE;
					memcpy(*outbuf, handle->convertor->shift_states[i].bytes, handle->convertor->shift_states[i].len);
					*outbuf += handle->convertor->shift_states[i].len;
					*outbytesleft -= handle->convertor->shift_states[i].len;
					handle->from_state = required_state;
					goto write_bytes;
				}
			}
		}
	}
	if (*outbytesleft < count)
		return CHARCONV_NO_SPACE;
write_bytes:
	memcpy(*outbuf, bytes, count);
	*outbuf += count;
	*outbytesleft -= count;
	return CHARCONV_SUCCESS;
}

static int from_unicode_check_multi_mappings(convertor_state_t *handle, char **inbuf, size_t *inbytesleft,
		char **outbuf, size_t *outbytesleft, int flags)
{
	uint_fast32_t codepoint;
	uint_fast32_t i;

	uint16_t codepoints[19];
	char *ptr = (char *) codepoints;
	size_t codepoints_left = 19 * 2;

	uint8_t *_inbuf = (uint8_t *) *inbuf;
	size_t _inbytesleft = *inbytesleft;
	size_t check_len;
	size_t mapping_check_len;
	t3_bool can_read_more = t3_true;

	GET_UNICODE();
	if (put_utf16(codepoint, &ptr, &codepoints_left) != 0)
		return CHARCONV_INTERNAL_ERROR;

	for (i = 0; i < handle->convertor->nr_multi_mappings; i++) {
		mapping_check_len = handle->convertor->multi_mappings[i].codepoints_length * 2;
		check_len = min(19 * 2 - codepoints_left, mapping_check_len);

		/* No need to read more of the input if we already know that the start doesn't match. */
		if (memcmp(codepoints, handle->convertor->multi_mappings[i].codepoints, check_len) != 0)
			continue;

		/* If we already read enough codepoints, then the comparison already verified that
		   the sequence matches. */
		if (check_len == mapping_check_len)
			goto check_complete;

		while (can_read_more && check_len < mapping_check_len) {
			GET_UNICODE();

			if (codepoint == CHARCONV_UTF_INCOMPLETE) {
				if (flags & CHARCONV_END_OF_TEXT) {
					can_read_more = t3_false;
					goto check_next_mapping;
				}
				return CHARCONV_INCOMPLETE;
			}

			if (codepoint == CHARCONV_UTF_ILLEGAL) {
				can_read_more = t3_false;
				goto check_next_mapping;
			}

			switch (put_utf16(codepoint, &ptr, &codepoints_left)) {
				case CHARCONV_INCOMPLETE:
					if (flags & CHARCONV_END_OF_TEXT) {
						can_read_more = t3_false;
						goto check_next_mapping;
					}
					return CHARCONV_INCOMPLETE;
				case CHARCONV_SUCCESS:
					break;
				case CHARCONV_NO_SPACE:
					can_read_more = t3_false;
					goto check_next_mapping;
				default:
					return CHARCONV_INTERNAL_ERROR;
			}
			check_len = 19 * 2 - codepoints_left;
		}

		if (check_len < mapping_check_len)
			continue;

		if (memcmp(codepoints, handle->convertor->multi_mappings[i].codepoints, check_len) == 0) {
check_complete:
			if (*outbytesleft < handle->convertor->multi_mappings[i].bytes_length)
				return CHARCONV_NO_SPACE;
			PUT_BYTES(handle->convertor->multi_mappings[i].bytes_length, handle->convertor->multi_mappings[i].bytes);

			if (19 * 2 - codepoints_left != mapping_check_len) {
				/* Re-read codepoints up to the number in the mapping. */
				_inbuf = (uint8_t *) *inbuf;
				_inbytesleft = *inbytesleft;
				check_len = 19 * 2 - check_len;
				codepoints_left = 19 * 2;
				ptr = (char *) codepoints;
				while (codepoints_left > check_len) {
					GET_UNICODE();
					put_utf16(codepoint, &ptr, &codepoints_left);
				}
			}
			*inbuf = (char *) _inbuf;
			*inbytesleft = _inbytesleft;
			return CHARCONV_SUCCESS;
		}
check_next_mapping: ;
	}
	return -1;
}

static int from_unicode_conversion(convertor_state_t *handle, char **inbuf, size_t *inbytesleft,
		char **outbuf, size_t *outbytesleft, int flags)
{
	uint8_t *_inbuf;
	size_t _inbytesleft;
	uint_fast8_t state = 0;
	uint_fast32_t idx = 0;
	uint_fast32_t codepoint;
	entry_t *entry;
	int_fast16_t i;
	uint_fast8_t byte;
	uint_fast8_t conv_flags;

	_inbuf = (uint8_t *) *inbuf;
	_inbytesleft = *inbytesleft;

	while (_inbytesleft > 0) {
		GET_UNICODE();
		if (codepoint == CHARCONV_UTF_INCOMPLETE)
			break;

		if (codepoint == CHARCONV_UTF_ILLEGAL) {
			if (!(flags & CHARCONV_SUBSTITUTE_ALL))
				return CHARCONV_ILLEGAL;
			PUT_BYTES(handle->convertor->subchar_len, handle->convertor->subchar);
			*inbuf = (char *) _inbuf;
			*inbytesleft = _inbytesleft;
			continue;
		}

		for (i = 16; i >= 0 ; i -= 8) {
			byte = (codepoint >> i) & 0xff;
			entry = &handle->convertor->unicode_states[state].entries[handle->convertor->unicode_states[state].map[byte]];

			idx += entry->base + (byte - entry->low) * entry->mul;

			switch (entry->action) {
				case ACTION_FINAL:
				case ACTION_FINAL_PAIR:
					conv_flags = handle->convertor->unicode_flags.get_flags(&handle->convertor->unicode_flags, idx);
					if ((conv_flags & FROM_UNICODE_MULTI_START) && !(flags & CHARCONV_NO_MN_CONVERSION)) {
						switch (from_unicode_check_multi_mappings(handle, inbuf, inbytesleft, outbuf, outbytesleft, flags)) {
							case CHARCONV_SUCCESS:
								_inbuf = (uint8_t *) *inbuf;
								_inbytesleft = *inbytesleft;
								state = 0;
								idx = 0;
								continue;
							case CHARCONV_INCOMPLETE:
								return CHARCONV_INCOMPLETE;
							case CHARCONV_INTERNAL_ERROR:
							default:
								return CHARCONV_INTERNAL_ERROR;
							case CHARCONV_NO_SPACE:
								return CHARCONV_NO_SPACE;
							case -1:
								break;
						}
					}

					if ((conv_flags & FROM_UNICODE_FALLBACK) && !(flags & CHARCONV_ALLOW_FALLBACK))
						return CHARCONV_FALLBACK;

					if (conv_flags & FROM_UNICODE_NOT_AVAIL) {
						if (!(flags & CHARCONV_SUBSTITUTE))
							return CHARCONV_UNASSIGNED;
						if (conv_flags & FROM_UNICODE_SUBCHAR1)
							PUT_BYTES(1, &handle->convertor->subchar1);
						else
							PUT_BYTES(handle->convertor->subchar_len, handle->convertor->subchar);
					} else {
						PUT_BYTES((conv_flags & FROM_UNICODE_LENGTH_MASK) + 1,
							&handle->convertor->unicode_mappings[idx * handle->convertor->single_size]);
					}
					goto sequence_done;
				case ACTION_VALID:
					state = entry->next_state;
					break;
				case ACTION_ILLEGAL:
					if (!(flags & CHARCONV_SUBSTITUTE_ALL))
						return CHARCONV_ILLEGAL;
					PUT_BYTES(handle->convertor->subchar_len, handle->convertor->subchar);
					goto sequence_done;
				case ACTION_UNASSIGNED:
					if (!(flags & CHARCONV_SUBSTITUTE))
						return CHARCONV_UNASSIGNED;
					PUT_BYTES(handle->convertor->subchar_len, handle->convertor->subchar);
					/* FALLTHROUGH */
				sequence_done:
					*inbuf = (char *) _inbuf;
					*inbytesleft = _inbytesleft;
					state = 0; /* Should always be 0! */
					idx = 0;
					if (flags & CHARCONV_SINGLE_CONVERSION)
						return CHARCONV_SUCCESS;
					break;
				case ACTION_SHIFT:
				default:
					return CHARCONV_INTERNAL_ERROR;
			}
		}
	}

	if (*inbytesleft != 0) {
		if (flags & CHARCONV_END_OF_TEXT) {
			if (!(flags & CHARCONV_SUBSTITUTE_ALL))
				return CHARCONV_ILLEGAL_END;
			PUT_BYTES(handle->convertor->subchar_len, handle->convertor->subchar);
			*inbuf += *inbytesleft;
			*inbytesleft = 0;
		} else {
			return CHARCONV_INCOMPLETE;
		}
	} else if (flags & CHARCONV_END_OF_TEXT) {
		if (handle->from_state != 0)
			PUT_BYTES(0, NULL);
	}
	return CHARCONV_SUCCESS;
}

static void from_unicode_reset(convertor_state_t *handle) {
	handle->from_state = 0;
}

static void save_cct_state(convertor_state_t *handle, save_state_t *save) {
	save->to_state = handle->to_state;
	save->from_state = handle->from_state;
}

static void load_cct_state(convertor_state_t *handle, save_state_t *save) {
	handle->to_state = save->to_state;
	handle->from_state = save->from_state;
}

void *open_cct_convertor(const char *name, int flags, int *error) {
	size_t len = strlen(DB_DIRECTORY) + strlen(name) + 6;
	convertor_state_t *retval;
	convertor_t *ptr;
	char *file_name;

	if ((file_name = malloc(len)) == NULL) {
		if (error != NULL)
			*error = T3_ERR_OUT_OF_MEMORY;
		return NULL;
	}

	strcpy(file_name, DB_DIRECTORY);
	strcat(file_name, "/");
	strcat(file_name, name);
	strcat(file_name, ".cct");

	pthread_mutex_lock(&cct_list_mutex);
	if (put_utf16 == NULL)
		put_utf16 = get_put_unicode(UTF16);

	for (ptr = cct_head; ptr != NULL; ptr = ptr->next) {
		if (strcmp(ptr->name, file_name) == 0)
			break;
	}

	if (ptr == NULL) {
		ptr = load_cct_convertor(file_name, error);
		if (ptr == NULL) {
			pthread_mutex_unlock(&cct_list_mutex);
			return NULL;
		}
		ptr->next = cct_head;
		cct_head = ptr;
	}
	free(file_name);

	if ((retval = malloc(sizeof(convertor_state_t))) == NULL) {
		if (ptr->refcount == 0)
			unload_cct_convertor(ptr);
		if (error != NULL)
			*error = T3_ERR_OUT_OF_MEMORY;
		pthread_mutex_unlock(&cct_list_mutex);
		return NULL;
	}
	ptr->refcount++;
	pthread_mutex_unlock(&cct_list_mutex);

	retval->convertor = ptr;
	retval->from_state = 0;
	retval->to_state = 0;

	retval->common.convert_from = (conversion_func_t) from_unicode_conversion;
	retval->common.reset_from = (reset_func_t) from_unicode_reset;
	retval->common.convert_to = (conversion_func_t) to_unicode_conversion;
	retval->common.skip_to = (skip_func_t) to_unicode_skip;
	retval->common.reset_to = (reset_func_t) to_unicode_reset;
	retval->common.flags = flags;
	retval->common.close = (close_func_t) close_convertor;
	retval->common.save = (save_func_t) save_cct_state;
	retval->common.load = (load_func_t) load_cct_state;
	return retval;
}

static void close_convertor(convertor_state_t *handle) {
	pthread_mutex_lock(&cct_list_mutex);
	if (handle->convertor->refcount == 1)
		unload_cct_convertor(handle->convertor);
	else
		handle->convertor->refcount--;
	pthread_mutex_unlock(&cct_list_mutex);
	free(handle);
}

size_t get_cct_saved_state_size(void) {
	return sizeof(save_state_t);
}
