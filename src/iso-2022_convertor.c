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


/* What do we need to know in the convertor:
	- which sets correspond to GL and GR, and C0 and C1 (if these are actually switchable for any of the supported sets)
	- which sequence selects which set for G0 through G3
per set:
	- #bytes per char
	- CCT convertor
*/
#include <string.h>
#include <search.h>

#include "charconv_internal.h"
#include "convertors.h"
#include "utf.h"

enum {
	ISO2022_JP,
	ISO2022_JP1,
	ISO2022_JP2,
	ISO2022_JP3,
	ISO2022_JP2004,
	ISO2022_KR,
	ISO2022_CN,
	ISO2022_CNEXT,
	ISO2022_TEST
};

typedef struct {
	const char *name;
	int iso2022_type;
} name_to_iso2022type;

typedef struct cct_handle_t cct_handle_t;

struct cct_handle_t {
	charconv_t *cct; /* Handle for the table based convertor. */
	uint_fast8_t bytes_per_char; /* Bytes per character code. */
	uint_fast8_t seq_len; /* Length of the escape sequence used to shift. */
	char escape_seq[7]; /* The escape sequence itselft. */
	bool high_bit, write; /* Whether the cct has the high bit set for characters. */
	cct_handle_t *prev, *next; /* Doubly-linked list ptrs. */
};

struct _charconv_iso2022_state_t {
	cct_handle_t *g_to[4]; /* Shifted-in sets. */
	cct_handle_t *g_from[4]; /* Shifted-in sets. */
	uint_fast8_t to, from;
};
typedef struct _charconv_iso2022_state_t state_t;

typedef struct {
	charconv_common_t common;
	cct_handle_t *g_initial[4];
	cct_handle_t *g_sets[4]; /* Linked lists of possible tables. */
	state_t state;
	int iso2022_type;
} convertor_state_t;

typedef struct {
	const char *name;
	uint_fast8_t bytes_per_char;
	uint_fast8_t g;
	const char *escape_seq;
	bool high_bit;
} cct_descriptor_t ;

/* We use the lower part of the ISO8859-1 convertor for ASCII. */
cct_descriptor_t ascii = { NULL, 1, 0, "\x1b\x28\x42", false };
cct_descriptor_t iso8859_1 = { NULL, 1, 2, "\x1b\x2e\x41", true };
//FIXME: use the correct codepage names and check the high_bit flag
cct_descriptor_t jis_x_0201_1976_kana = { "ibm-897_P100-1995", 1, 0, "\x1b\x28\x49", true };
cct_descriptor_t jis_x_0201_1976_roman = { "ibm-897_P100-1995", 1, 0, "\x1b\x28\x4a", false };
cct_descriptor_t jis_x_0208_1978 = { "JIS-X-0208-1978", 2, 0, "\x1b\x24\x40", true };
cct_descriptor_t jis_x_0208_1983 = { "JIS-X-0208-1983", 2, 0, "\x1b\x24\x42", true };
cct_descriptor_t jis_x_0212_1990 = { "JIS-X-0212-1990", 2, 0, "\x1b\x24\x28\x44", true };
cct_descriptor_t jis_x_0213_2000_1 = { "JIS-X-0213-2000-1", 2, 0, "\x1b\x24\x28\x4f", true };
cct_descriptor_t jis_x_0213_2000_2 = { "JIS-X-0213-2000-2", 2, 0, "\x1b\x24\x28\x50", true };
cct_descriptor_t jis_x_0213_2004_1 = { "JIS-X-0213-2004-1", 2, 0, "\x1b\x24\x28\x51", true };
cct_descriptor_t iso8859_7 = { "ISO-8859-7", 1, 2, "\x1b\x2e\x4f", true };
cct_descriptor_t ksc5601_1987_g0 = { "KSC5601-1987", 2, 0, "\x1b\x24\x28\x43", true };
cct_descriptor_t ksc5601_1987_g1 = { "KSC5601-1987", 2, 1, "\x1b\x24\x29\x43", true };
cct_descriptor_t gb2312_1980_g0 = { "GB2312-1980", 2, 0, "\x1b\x24\x41", true };
cct_descriptor_t gb2312_1980_g1 = { "GB2312-1980", 2, 1, "\x1b\x24\x29\x41", true };

cct_descriptor_t cns_11643_1992_1 = { "CNS-11643-1992-1", 2, 1, "\x1b\x24\x29\x47", true };
cct_descriptor_t cns_11643_1992_2 = { "CNS-11643-1992-2", 2, 2, "\x1b\x24\x2A\x48", true };
cct_descriptor_t cns_11643_1992_3 = { "CNS-11643-1992-3", 2, 3, "\x1b\x24\x2B\x49", true };
cct_descriptor_t cns_11643_1992_4 = { "CNS-11643-1992-4", 2, 3, "\x1b\x24\x2B\x4a", true };
cct_descriptor_t cns_11643_1992_5 = { "CNS-11643-1992-5", 2, 3, "\x1b\x24\x2B\x4b", true };
cct_descriptor_t cns_11643_1992_6 = { "CNS-11643-1992-6", 2, 3, "\x1b\x24\x2B\x4c", true };
cct_descriptor_t cns_11643_1992_7 = { "CNS-11643-1992-7", 2, 3, "\x1b\x24\x2B\x4d", true };
cct_descriptor_t iso_ir_165 = { "ISO-IR-165", 2, 1, "\x1b\x24\x29\x45", true };

static void close_convertor(convertor_state_t *handle);

static int check_escapes(convertor_state_t *handle, uint8_t *_inbuf, size_t _inbytesleft) {
	cct_handle_t *ptr;
	size_t i;

	//FIXME: we should probably limit the number of bytes to check
	for (i = 1; i < _inbytesleft; i++) {
		if (_inbuf[i] >= 0x20 && _inbuf[i] <= 0x2f)
			continue;
		if (_inbuf[i] >= 0x40 && _inbuf[i] <= 0x7f)
			break;
		return CHARCONV_ILLEGAL;
	}
	if (i == _inbytesleft)
		return CHARCONV_INCOMPLETE;

	for (i = 0; i < 3; i++) {
		for (ptr = handle->g_sets[i]; ptr != NULL; ptr = ptr->next) {
			if (_inbytesleft < ptr->seq_len)
				continue;

			if (memcmp(_inbuf, ptr->escape_seq, ptr->seq_len) != 0)
				continue;

			handle->state.g_to[i] = ptr;
			return -ptr->seq_len;
		}
	}
	return CHARCONV_ILLEGAL;
}

#define PUT_UNICODE(codepoint) do { int result; \
	if ((result = handle->common.put_unicode(codepoint, outbuf, outbytesleft)) != 0) \
		return result; \
} while (0)

static int to_unicode_conversion(convertor_state_t *handle, char **inbuf, size_t *inbytesleft,
		char **outbuf, size_t *outbytesleft, int flags)
{
	uint8_t *_inbuf = (uint8_t *) *inbuf;
	size_t _inbytesleft = *inbytesleft;
	uint_fast8_t state;
	int result;

	while (_inbytesleft > 0) {
		//FIXME: should we accept shift sequences in non-locking shift states?
		if (*_inbuf < 32) {
			/* Control characters. */
			if (*_inbuf == 0x1b) {
				/* Escape sequence. */
				if (_inbytesleft == 1)
					goto incomplete_char;

				/* _inbytesleft at least 2 at this point. */
				switch (_inbuf[1]) {
					case 0x6e:
						state = 2;
						goto state_shift_done;
					case 0x6f:
						state = 3;
						goto state_shift_done;
					case 0x4e:
						state = (handle->state.to & 3) | (2 << 2);
						goto state_shift_done;
					case 0x4f:
						state = (handle->state.to & 3) | (3 << 2);
						goto state_shift_done;

				state_shift_done:
						if (handle->state.g_to[state > 3 ? state >> 2 : state] == NULL)
							return CHARCONV_ILLEGAL;
						handle->state.to = state;

						_inbuf = (uint8_t *) ((*inbuf) += 2);
						_inbytesleft = ((*inbytesleft) -= 2);
						continue;

					default:
						break;
				}

				switch (result = check_escapes(handle, _inbuf, _inbytesleft)) {
					case CHARCONV_INCOMPLETE:
						goto incomplete_char;
					case CHARCONV_ILLEGAL:
						return result;
					default:
						_inbuf = (uint8_t *) ((*inbuf) -= result);
						_inbytesleft = (*inbytesleft) += result;
						continue;
				}
			} else if (*_inbuf == 0xe) {
				/* Shift out. */
				if (handle->state.g_to[1] == NULL)
					return CHARCONV_ILLEGAL;
				handle->state.to = 1;
				_inbuf = (uint8_t *) ++(*inbuf);
				_inbytesleft = --(*inbytesleft);
				continue;
			} else if (*_inbuf == 0xf) {
				/* Shift in. */
				handle->state.to = 0;
				_inbuf = (uint8_t *) ++(*inbuf);
				_inbytesleft = --(*inbytesleft);
				continue;
			} else if (*_inbuf == 0xd) {
				if (_inbytesleft > 1) {
					if (_inbuf[1] == 0xa) {
						//FIXME: reset state
					}
				} else if (!(flags & CHARCONV_END_OF_TEXT)) {
					return CHARCONV_INCOMPLETE;
				}
			}
			/* Other control. */
			PUT_UNICODE(*_inbuf);
			_inbuf++;
			_inbytesleft--;
		} else if (*_inbuf & 0x80) {
			/* All ISO-2022 convertors implemented here are 7 bit only. */
			return CHARCONV_ILLEGAL;
		} else {
			char buffer[3];
			char *buffer_ptr = buffer;
			uint32_t codepoint;
			char *codepoint_ptr = (char *) &codepoint;
			size_t codepoint_size = 4;
			int i;

			state = handle->state.to;
			if (state > 3)
				state >>= 2;

			if (_inbytesleft < handle->state.g_to[state]->bytes_per_char)
				goto incomplete_char;

			for (i = 0; i < handle->state.g_to[state]->bytes_per_char; i++)
				buffer[i] = _inbuf[i] | (handle->state.g_to[state]->high_bit << 7);
			size_t buffer_size = handle->state.g_to[state]->bytes_per_char;

			if ((result = handle->state.g_to[state]->cct->convert_to(handle->state.g_to[state]->cct, &buffer_ptr, &buffer_size,
					&codepoint_ptr, &codepoint_size, 0)) != CHARCONV_SUCCESS)
				return result;
			PUT_UNICODE(codepoint);
			_inbuf += handle->state.g_to[state]->bytes_per_char;
			_inbytesleft -= handle->state.g_to[state]->bytes_per_char;
		}

		if (handle->state.to > 3)
			handle->state.to &= 3;

		*inbuf = (char *) _inbuf;
		*inbytesleft = _inbytesleft;
		if (flags & CHARCONV_SINGLE_CONVERSION)
			return CHARCONV_SUCCESS;
	}
	if (flags & CHARCONV_END_OF_TEXT) {
		//FIXME: reset
	}

	return CHARCONV_SUCCESS;

incomplete_char:
	if (flags & CHARCONV_END_OF_TEXT) {
		if (flags & CHARCONV_SUBST_ILLEGAL) {
			//FIXME: reset
			PUT_UNICODE(0xfffd);
			(*inbuf) += _inbytesleft;
			*inbytesleft = 0;
			return CHARCONV_SUCCESS;
		}
		return CHARCONV_ILLEGAL_END;
	}
	return CHARCONV_INCOMPLETE;
}

static int to_unicode_skip(charconv_common_t *handle, char **inbuf, size_t *inbytesleft) {
	(void) handle;

	if (*inbytesleft == 0)
		return CHARCONV_INCOMPLETE;
	(*inbuf)++;
	(*inbytesleft)--;
	return CHARCONV_SUCCESS;
}

static void to_unicode_reset(convertor_state_t *handle) {
	memcpy(handle->state.g_to, handle->g_initial, sizeof(handle->g_initial));
	handle->state.to = 0;
}

static bool load_table(convertor_state_t *handle, cct_descriptor_t *desc, charconv_error_t *error, bool write)
{
	cct_handle_t *cct_handle;
	charconv_t *ext_handle;

	if (desc->name == NULL)
		ext_handle = _charconv_fill_utf(_charconv_open_iso8859_1_convertor(desc->name, 0, error), UTF32);
	else
		ext_handle = _charconv_fill_utf(_charconv_open_cct_convertor_internal(desc->name, 0, error, true), UTF32);

	if (ext_handle == NULL)
		return false;

	if ((cct_handle = malloc(sizeof(cct_handle_t))) == NULL) {
		charconv_close_convertor(ext_handle);
		if (error != NULL)
			*error = CHARCONV_OUT_OF_MEMORY;
		return false;
	}

	cct_handle->cct = ext_handle;
	cct_handle->bytes_per_char = desc->bytes_per_char;
	cct_handle->seq_len = strlen(desc->escape_seq);
	strcpy(cct_handle->escape_seq, desc->escape_seq);
	cct_handle->high_bit = desc->high_bit;
	cct_handle->write = write;
	cct_handle->next = handle->g_sets[desc->g];
	handle->g_sets[desc->g] = cct_handle;
	cct_handle->prev = NULL;
	return true;
}

#define CHECK_LOAD(x) do { if (!(x)) { close_convertor(retval); return NULL; }} while (0)

void *_charconv_open_iso2022_convertor(const char *name, int flags, charconv_error_t *error) {
	static const name_to_iso2022type map[] = {
		{ "ISO-2022-JP", ISO2022_JP },
		{ "ISO-2022-JP-1", ISO2022_JP1 },
		{ "ISO-2022-JP-2", ISO2022_JP2 },
		{ "ISO-2022-JP-3", ISO2022_JP3 },
		{ "ISO-2022-JP-2004", ISO2022_JP2004 },
		{ "ISO-2022-KR", ISO2022_KR },
		{ "ISO-2022-CN", ISO2022_CN },
		{ "ISO-2022-CN-EXT", ISO2022_CNEXT }
#ifdef DEBUG
#warning using ISO-2022-TEST
		, { "ISO-2022-TEST", ISO2022_TEST }
#endif
	};

	convertor_state_t *retval;
	name_to_iso2022type *ptr;
	size_t array_size = ARRAY_SIZE(map);

	if ((ptr = lfind(name, map, &array_size, sizeof(map[0]), _charconv_element_strcmp)) == NULL) {
		if (error != NULL)
			*error = CHARCONV_INTERNAL_ERROR;
		return NULL;
	}

	if ((retval = malloc(sizeof(convertor_state_t))) == NULL) {
		if (error != NULL)
			*error = CHARCONV_OUT_OF_MEMORY;
		return NULL;
	}
	retval->g_sets[0] = NULL;
	retval->g_sets[1] = NULL;
	retval->g_sets[2] = NULL;
	retval->g_sets[3] = NULL;
	retval->g_initial[0] = NULL;
	retval->g_initial[1] = NULL;
	retval->g_initial[2] = NULL;
	retval->g_initial[3] = NULL;

	retval->iso2022_type = ptr->iso2022_type;

	switch (retval->iso2022_type) {
		/* Current understanding of the ISO-2022-JP-* situation:
		   JIS X 0213 has two planes: the first plane which is a superset of
		   JIS X 0208, and plane 2 which contains only new chars. However, in
		   making JIS X 0213, they found that they needed to amend the standard
		   for plane 1 in 2004.

		   ISO-2022-JP-2004 is the completely new and revised version, which
		   _should_ only contain ASCII and JIS X 0213 (2004). Note that plane 2
		   of JIS-X-0213 was never revised.

		   ISO-2022-JP-3 is the same as ISO-2022-JP-2004, but based on the
		   original JIS X 0213. For plane 1 of JIS X 0213 a different escape
		   sequence is used than in ISO-2022-JP-2004, so there are no nasty
		   problems there.

		   ISO-2022-JP-2 extends ISO-2022-JP-1, which in turn extends ISO-2022-JP
		   standard by adding more character sets.

		   The best approach in this case would be to make a distinction between
		   character sets which are understood for reading, and those which are
		   used for writing (according to the be conservative in what you send;
		   be liberal in what you accept philosophy.

		   Also note that, to make things slightly worse, in the attempts to
		   register the ISO-2022-JP-2004 character set with IANA, the following
		   aliases are named:

		   ISO-2022-JP-3-2003
		   ISO-2022-JP-2003

		   It is unclear what part JIS X 0201 has to play in this. It does encode
		   characters that do not appear to be in JIS X 0213...
		*/
		case ISO2022_JP2004:
			/* Load the JP and JP-3 sets, but only for reading. */
			CHECK_LOAD(load_table(retval, &jis_x_0201_1976_roman, error, false));
			CHECK_LOAD(load_table(retval, &jis_x_0208_1983, error, false));
			CHECK_LOAD(load_table(retval, &jis_x_0208_1978, error, false));
			CHECK_LOAD(load_table(retval, &jis_x_0213_2000_1, error, false));

			/* I'm not very sure about this one. Different sources seem to say different things */
			CHECK_LOAD(load_table(retval, &jis_x_0201_1976_kana, error, true));
			CHECK_LOAD(load_table(retval, &jis_x_0213_2000_2, error, true));
			CHECK_LOAD(load_table(retval, &jis_x_0213_2004_1, error, true));
			/* Load ASCII last, as that is what should be the initial state. */
			CHECK_LOAD(load_table(retval, &ascii, error, true));
			break;
		case ISO2022_JP3:
			/* Load the JP sets, but only for reading. */
			CHECK_LOAD(load_table(retval, &jis_x_0201_1976_roman, error, false));
			CHECK_LOAD(load_table(retval, &jis_x_0208_1983, error, false));
			CHECK_LOAD(load_table(retval, &jis_x_0208_1978, error, false));

			/* I'm not very sure about this one. Different sources seem to say different things */
			CHECK_LOAD(load_table(retval, &jis_x_0201_1976_kana, error, true));
			CHECK_LOAD(load_table(retval, &jis_x_0213_2000_1, error, true));
			CHECK_LOAD(load_table(retval, &jis_x_0213_2000_2, error, true));
			/* Load ASCII last, as that is what should be the initial state. */
			CHECK_LOAD(load_table(retval, &ascii, error, true));
			break;
		case ISO2022_JP2:
			CHECK_LOAD(load_table(retval, &iso8859_1, error, true));
			CHECK_LOAD(load_table(retval, &iso8859_7, error, true));
			CHECK_LOAD(load_table(retval, &ksc5601_1987_g0, error, true));
			CHECK_LOAD(load_table(retval, &gb2312_1980_g0, error, true));
			/* FALLTHROUGH */
		case ISO2022_JP1:
			CHECK_LOAD(load_table(retval, &jis_x_0212_1990, error, true));
			/* FALLTHROUGH */
		case ISO2022_JP:
			CHECK_LOAD(load_table(retval, &jis_x_0201_1976_roman, error, true));
			CHECK_LOAD(load_table(retval, &jis_x_0208_1983, error, true));
			CHECK_LOAD(load_table(retval, &jis_x_0208_1978, error, true));
			/* Load ASCII last, as that is what should be the initial state. */
			CHECK_LOAD(load_table(retval, &ascii, error, true));
			break;
		case ISO2022_KR:
			CHECK_LOAD(load_table(retval, &ksc5601_1987_g1, error, true));
			CHECK_LOAD(load_table(retval, &ascii, error, true));
			break;
		case ISO2022_CNEXT:
			CHECK_LOAD(load_table(retval, &iso_ir_165, error, true));
			CHECK_LOAD(load_table(retval, &cns_11643_1992_3, error, true));
			CHECK_LOAD(load_table(retval, &cns_11643_1992_4, error, true));
			CHECK_LOAD(load_table(retval, &cns_11643_1992_5, error, true));
			CHECK_LOAD(load_table(retval, &cns_11643_1992_6, error, true));
			CHECK_LOAD(load_table(retval, &cns_11643_1992_7, error, true));
			/* FALLTHROUGH */
		case ISO2022_CN:
			CHECK_LOAD(load_table(retval, &gb2312_1980_g1, error, true));
			CHECK_LOAD(load_table(retval, &cns_11643_1992_1, error, true));
			CHECK_LOAD(load_table(retval, &cns_11643_1992_2, error, true));
			CHECK_LOAD(load_table(retval, &ascii, error, true));
			break;
		case ISO2022_TEST:
			CHECK_LOAD(load_table(retval, &jis_x_0201_1976_roman, error, true));
			CHECK_LOAD(load_table(retval, &jis_x_0201_1976_kana, error, true));
			CHECK_LOAD(load_table(retval, &iso8859_1, error, true));
			CHECK_LOAD(load_table(retval, &ascii, error, true));
			retval->g_initial[0] = retval->g_sets[0];
			break;
		default:
			close_convertor(retval);
			if (error != NULL)
				*error = CHARCONV_INTERNAL_ERROR;
			return NULL;
	}

/*	retval->common.convert_from = (conversion_func_t) from_unicode_conversion;
	retval->common.reset_from = (reset_func_t) from_unicode_reset;*/
	retval->common.convert_to = (conversion_func_t) to_unicode_conversion;
	retval->common.skip_to = (skip_func_t) to_unicode_skip;
	retval->common.reset_to = (reset_func_t) to_unicode_reset;
	retval->common.flags = flags;
/*	retval->common.close = (close_func_t) close_convertor;
	retval->common.save = (save_func_t) save_cct_state;
	retval->common.load = (load_func_t) load_cct_state;*/

	to_unicode_reset(retval);
/* 	from_unicode_reset(retval); */
	return retval;
}

static void close_convertor(convertor_state_t *handle) {
	cct_handle_t *ptr;
	size_t i;

	for (i = 0; i < 4; i++) {
		for (ptr = handle->g_sets[i]; ptr != NULL; ptr = ptr->next) {
			charconv_close_convertor(ptr->cct);
			free(ptr);
		}
	}
	free(handle);
}
