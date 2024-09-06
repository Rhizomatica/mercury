/** ffbase: Formatted output into string
2020, Simon Zolin */

#include <ffbase/string.h>

ffssize ffs_formatv(char *dst, ffsize cap, const char *fmt, va_list va)
{
	ffsize len = 0;

	for (;  *fmt != '\0';  fmt++) {
		if (*fmt != '%') {
			if (len < cap)
				dst[len] = *fmt;
			len++;
			continue;
		}
		fmt++;

		ffuint have_width = 0, width = 0, prec = 6, iflags = 0;
		ffssize r = 0;
		ffuint64 n;

		if (*fmt == '0') {
			iflags |= FFS_INTZERO;
			fmt++;
		}

		for (;  (*fmt >= '0' && *fmt <= '9');  fmt++) {
			width = width * 10 + *fmt - '0';
			have_width = 1;
		}

		for (;;) {
			switch (*fmt) {
			case 'x':
				FF_ASSERT(!(iflags & FFS_INTHEX));
				fmt++;
				iflags |= FFS_INTHEX;
				break;

			case 'X':
				FF_ASSERT(!(iflags & FFS_INTHEX));
				fmt++;
				iflags |= FFS_INTHEX | FFS_INTHEXUP;
				break;

			case ',':
				FF_ASSERT(!(iflags & FFS_INTSEP1000));
				fmt++;
				iflags |= FFS_INTSEP1000;
				break;

			case '.':
				fmt++;
				prec = 0;
				for (;  (*fmt >= '0' && *fmt <= '9');  fmt++) {
					prec = prec * 10 + *fmt - '0';
				}
				break;

			case '*':
				FF_ASSERT(!have_width);
				if (have_width)
					return 0; // "%5*s"
				fmt++;
				width = va_arg(va, ffsize);
				have_width = 1;
				continue;
			}

			break;
		}

		switch (*fmt) {

		case 'U':
			n = va_arg(va, ffuint64);
			break;
		case 'D':
			n = va_arg(va, ffint64);
			iflags |= FFS_INTSIGN;
			break;

		case 'u':
			n = va_arg(va, ffuint);
			break;
		case 'd':
			n = va_arg(va, int);
			iflags |= FFS_INTSIGN;
			break;

		case 'L':
			n = va_arg(va, ffsize);
			break;

		case 'p':
			n = va_arg(va, ffsize);
			iflags |= FFS_INTHEX | FFS_INTZERO;
			width = sizeof(void*) * 2;
			break;

		case 'F':
		case 'f': {
			double d = va_arg(va, double);
			if (len < cap) {
				ffuint fzero = (iflags & FFS_INTZERO) ? FFS_FLTZERO : 0;
				r = ffs_fromfloat(d, dst + len, cap - len, FFS_FLTWIDTH(width) | fzero | prec);
			}
			len += (r != 0) ? r : FFS_FLTCAP;
			continue;
		}


#if !defined FF_WIN && defined _FFBASE_UNICODE_H
		case 'q':
#endif
		case 's': {
			const char *sz = va_arg(va, char*);

			if (have_width) {
				if (len + width < cap)
					ffmem_copy(dst + len, sz, width);
				r = width;

			} else {
				if (sz == NULL)
					sz = "(null)";

				if (len < cap) {
					r = _ffs_copyz(dst + len, cap - len, sz);
					if (len + r >= cap)
						r = ffsz_len(sz);

				} else {
					r = ffsz_len(sz);
				}
			}

			len += r;
			continue;
		}

		case 'S': {
			const ffstr *s = va_arg(va, ffstr*);

			if (len + s->len < cap)
				ffmem_copy(dst + len, s->ptr, s->len);
			len += s->len;

			r = width - s->len;
			if (r > 0) {
				if (len + r < cap)
					ffmem_fill(dst + len, ' ', r);
				len += r;
			}
			continue;
		}

#if defined FF_WIN && defined _FFBASE_UNICODE_H
		case 'q': {
			const wchar_t *sz = va_arg(va, wchar_t*);

			r = -1;
			if (have_width) {
				if (len < cap)
					r = ffs_wtou(dst + len, cap - len, sz, width);
				if (r < 0)
					r = ffs_wtou(NULL, 0, sz, width);

			} else {
				if (sz == NULL)
					sz = L"(null)";

				if (len < cap)
					r = ffs_wtouz(dst + len, cap - len, sz);
				if (r < 0)
					r = ffs_wtouz(NULL, 0, sz);
			}

			if (r > 0)
				len += r;
			continue;
		}
#endif


		case 'b': {
			FF_ASSERT(have_width);
			FF_ASSERT(iflags & FFS_INTHEX);
			if (!have_width)
				return 0; // width must be specified
			if (!(iflags & FFS_INTHEX))
				return 0; // 'x|X' must be specified

			const void *d = va_arg(va, void*);
			if (len + width * 2 < cap)
				ffs_fromhex(dst + len, cap - len, d, width, iflags & FFS_INTHEXUP);
			len += width * 2;
			continue;
		}


		case 'c': {
			int ch = va_arg(va, int);
			if (have_width) {
				if (len + width < cap)
					ffmem_fill(dst + len, ch, width);
				len += width;
			} else {
				if (len < cap)
					dst[len] = ch;
				len++;
			}
			continue;
		}

		case '%':
			if (len < cap)
				dst[len] = '%';
			len++;
			continue;

		case 'Z':
			if (len < cap)
				dst[len] = '\0';
			len++;
			continue;

#ifdef FFBASE_HAVE_FFERR_STR
		case 'E': {
			// "(%u) %s", errno, strerror(errno)
			int e = va_arg(va, int);
			if (len + 3 < cap) {
				r = ffs_fromint(e, dst + len + 1, cap - (len + 1), 0);
				if (r != 0) {
					dst[len++] = '(';
					len += r;
					dst[len++] = ')';
					dst[len++] = ' ';

					if (0 == fferr_str(e, dst + len, cap - len))
						r = ffsz_len(dst + len);
					else
						r = 255;
				} else {
					r = 3 + FFS_INTCAP + 255;
				}
			} else {
				r = 3 + FFS_INTCAP + 255;
			}
			len += r;
			continue;
		}
#endif

		default:
			FF_ASSERT(0);
			return 0; // bad format string
		}

		if (len < cap)
			r = ffs_fromint(n, dst + len, cap - len, iflags | FFS_INTWIDTH(width));
		len += (r != 0) ? r : FFS_INTCAP;
	}

	if (len >= cap)
		return -(ffssize)(len + 1); // not enough space
	return len;
}

ffssize ffstr_matchfmtv(const ffstr *s, const char *fmt, va_list args)
{
	ffsize is = 0;
	ffuint width;
	ffuint intflags;
	for (ffsize i = 0;  fmt[i] != '\0';) {
		int ch = fmt[i++];
		if (ch != '%') {
			if (s->len == 0 || ch != s->ptr[is])
				return -1; // mismatch
			is++;
			continue;
		}

		width = 0;
		for (;;) {
			ch = fmt[i++];
			if (!(ch >= '0' && ch <= '9'))
				break;
			width = width*10 + (ch-'0');
		}
		if (is + width > s->len)
			return -1; // too small input

		intflags = 0;
		if (ch == 'x') {
			ch = fmt[i++];
			intflags = FFS_INTHEX;
		}

		ffstr chunk;
		switch (ch) {
		case 'S':
		case 'd':
		case 'D':
		case 'u':
		case 'U': {
			if (width != 0) {
				chunk.ptr = &s->ptr[is];  chunk.len = width;
				is += width;
				break;
			}

			if (fmt[i] == '\0') {
				width = s->len - is;
				chunk.ptr = &s->ptr[is];  chunk.len = width;
				is += width;
				break;

			} else if (fmt[i] == '%') {
				if (fmt[i+1] == '%') { // "%S%%"
					i++;
				} else {
					return -1; // "%S%?" - bad format string
				}
			}
			int stop_char = fmt[i++];

			chunk.ptr = &s->ptr[is];  chunk.len = 0;
			for (;;) {
				if (is == s->len) {
					return -1; // mismatch
				}
				if (s->ptr[is] == stop_char) {
					// match text until next %-var or EOS
					ffsize is2 = is, i2 = i;
					is++;
					for (;; i++, is++) {

						if (fmt[i] == '\0') {
							goto delim_ok;
						} else if (fmt[i] == '%') {
							if (fmt[i+1] != '%')
								goto delim_ok;
							// "%%"
							i++;
						}

						if (s->ptr[is] != fmt[i])
							break; // continue search
					}

					is = is2;
					i = i2;
				}
				is++;
				chunk.len++;
			}
delim_ok:
			break;
		}
		}

		switch (ch) {
		case 'S': {
			ffstr *pstr = va_arg(args, ffstr*);
			*pstr = chunk;
			break;
		}

		case 'd':
		case 'D':
		case 'u':
		case 'U': {
			void *pint;
			if (ch == 'd' || ch == 'u') {
				intflags |= FFS_INT32;
				pint = va_arg(args, ffuint*);
			} else {
				intflags |= FFS_INT64;
				pint = va_arg(args, ffuint64*);
			}

			if (ch == 'd' || ch == 'D')
				intflags |= FFS_INTSIGN;

			if (chunk.len == 0
				|| chunk.len != ffs_toint(chunk.ptr, chunk.len, pint, intflags))
				return -1; // bad integer
			break;
		}

		case '%':
			if (s->ptr[is] != '%')
				return -1; // mismatch
			is++;
			continue;

		default:
			return -1; // bad format string
		}
	}

	if (is == s->len)
		return 0;

	return is + 1;
}
