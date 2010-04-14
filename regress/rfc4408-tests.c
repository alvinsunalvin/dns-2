/* ==========================================================================
 * rfc4408-tests.c - OpenSPF test suite check.
 * --------------------------------------------------------------------------
 * Copyright (c) 2010  William Ahern
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */
#include <stdio.h>

#include <string.h>

#include <assert.h>

#include <errno.h>

#include <arpa/inet.h>

#include <sys/queue.h>

#include <yaml.h>

#include "cache.h"
#include "dns.h"


#define lengthof(a) (sizeof (a) / sizeof (a)[0])

#define STRINGIFY_(s) #s
#define STRINGIFY(s) STRINGIFY_(s)

#define streq(a, b) (!strcmp((a), (b)))


#define panic_(fn, ln, fmt, ...) \
	do { fprintf(stderr, fmt "%.1s", (fn), (ln), __VA_ARGS__); _Exit(EXIT_FAILURE); } while (0)

#define panic(...) panic_(__func__, __LINE__, __FILE__ ": (%s:%d) " __VA_ARGS__, "\n")

#define SAY_(fmt, ...) \
	do { fprintf(stderr, fmt "%.1s", __func__, __LINE__, __VA_ARGS__); } while (0)
#define SAY(...) SAY_(">>>> (%s:%d) " __VA_ARGS__, "\n")
#define HAI SAY("HAI")



static const char *yaml_strevent(int type) {
	static const char event[][24] = {
		[YAML_NO_EVENT]             = "NO_EVENT",
		[YAML_STREAM_START_EVENT]   = "STREAM_START_EVENT",
		[YAML_STREAM_END_EVENT]     = "STREAM_END_EVENT",
		[YAML_DOCUMENT_START_EVENT] = "DOCUMENT_START_EVENT",
		[YAML_DOCUMENT_END_EVENT]   = "DOCUMENT_END_EVENT",
		[YAML_ALIAS_EVENT]          = "ALIAS_EVENT",
		[YAML_SCALAR_EVENT]         = "SCALAR_EVENT",
		[YAML_SEQUENCE_START_EVENT] = "SEQUENCE_START_EVENT",
		[YAML_SEQUENCE_END_EVENT]   = "SEQUENCE_END_EVENT",
		[YAML_MAPPING_START_EVENT]  = "MAPPING_START_EVENT",
		[YAML_MAPPING_END_EVENT]    = "MAPPING_END_EVENT",
	};

	return event[type];
} /* yaml_strevent() */


static const char *yaml_strevents(int set) {
	static char str[128];
	char *end;
	int type;

	str[0] = '\0';

	for (type = 1; type < 16; type++) {
		if ((1 << type) & set) {
			strncat(str, yaml_strevent(type), sizeof str - 1);
			strncat(str, "|", sizeof str - 1);
		}
	}

	if ((end = strrchr(str, '|')))
		*end = '\0';

	return str;
} /* yaml_strevents() */


static struct {
	int foo;
} MAIN;


#define SET4(a, b, c, d, ...) \
	((1 << (a)) | (1 << (b)) | (1 << (c)) | (1 << (d)))
#define SET(...) SET4(__VA_ARGS__, 0, 0, 0)
#define INSET(set, x) ((set) & (1 << (x)))



struct test {
	char *name;
	char *descr;
	char *comment;
	char *spec;
	char *helo;
	char *host;
	char *mailfrom;
	char *result[2];
	unsigned rcount;
	char *exp;

	CIRCLEQ_ENTRY(test) cqe;
}; /* struct test */


struct section {
	char *descr;
	char *comment;
	struct cache *zonedata;

	CIRCLEQ_HEAD(, test) tests;
}; /* struct section */


struct parser {
	yaml_parser_t yaml;

	struct section section;
}; /* struct parser */


static int expect(yaml_parser_t *parser, yaml_event_t *event, int set) {
	assert(yaml_parser_parse(parser, event));

	if (!INSET(set, event->type))
		panic("got %s, expected %s", yaml_strevent(event->type), yaml_strevents(set));

	return event->type;
} /* expect() */


static yaml_event_t *delete(yaml_event_t *event) {
	yaml_event_delete(event);
	return memset(event, 0, sizeof *event);
} /* delete() */


static void discard(yaml_parser_t *parser, int set) {
	yaml_event_t event;
	expect(parser, &event, set);
	delete(&event);
} /* discard() */


#define SCALAR_STR(event) ((char *)(event)->data.scalar.value)
#define SCALAR_LEN(event) ((event)->data.scalar.length)


static char *nextscalar(char **dst, yaml_parser_t *parser) {
	yaml_event_t event;

	if (YAML_SCALAR_EVENT == expect(parser, &event, SET(YAML_SCALAR_EVENT, YAML_SEQUENCE_START_EVENT))) {
		assert(*dst = strdup((char *)event.data.scalar.value));
	} else {
		size_t size = 0;
		*dst = 0;

		while (YAML_SCALAR_EVENT == expect(parser, delete(&event), SET(YAML_SCALAR_EVENT, YAML_SEQUENCE_END_EVENT))) {
			assert(*dst = realloc(*dst, size + SCALAR_LEN(&event) + 1));
			memcpy(&(*dst)[size], SCALAR_STR(&event), SCALAR_LEN(&event));
			size += SCALAR_LEN(&event);
			(*dst)[size] = '\0';
		}
	}

	delete(&event);

	return *dst;
} /* nextscalar() */


static struct test *nexttest(yaml_parser_t *parser) {
	struct test *test;
	yaml_event_t event;
	int type;

	type = expect(parser, &event, SET(YAML_SCALAR_EVENT, YAML_MAPPING_END_EVENT));

	if (type == YAML_MAPPING_END_EVENT)
		{ delete(&event); return NULL; }

	assert(test = malloc(sizeof *test));
	memset(test, 0, sizeof *test);

	assert(test->name = strdup((char *)event.data.scalar.value));
	delete(&event);

	discard(parser, SET(YAML_MAPPING_START_EVENT));

	while (YAML_SCALAR_EVENT == expect(parser, &event, SET(YAML_SCALAR_EVENT, YAML_MAPPING_END_EVENT))) {
		char *txt = (char *)event.data.scalar.value;

		if (streq(txt, "description")) {
			nextscalar(&test->descr, parser);
		}  else if (streq(txt, "comment")) {
			nextscalar(&test->comment, parser);
		}  else if (streq(txt, "spec")) {
			nextscalar(&test->spec, parser);
		}  else if (streq(txt, "helo")) {
			nextscalar(&test->helo, parser);
		}  else if (streq(txt, "host")) {
			nextscalar(&test->host, parser);
		}  else if (streq(txt, "mailfrom")) {
			nextscalar(&test->mailfrom, parser);
		}  else if (streq(txt, "result")) {
			if (YAML_SCALAR_EVENT == expect(parser, delete(&event), SET(YAML_SCALAR_EVENT, YAML_SEQUENCE_START_EVENT))) {
				assert(test->result[0] = strdup((char *)event.data.scalar.value));
				test->rcount = 1;
			} else {
				while (YAML_SCALAR_EVENT == expect(parser, delete(&event), SET(YAML_SCALAR_EVENT, YAML_SEQUENCE_END_EVENT))) {
					assert(test->rcount < lengthof(test->result));
					assert(test->result[test->rcount++] = strdup((char *)event.data.scalar.value));
				}
			}
		}  else if (streq(txt, "explanation")) {
			nextscalar(&test->exp, parser);
		} else {
			SAY("%s: unknown field", txt);
			discard(parser, SET(YAML_SCALAR_EVENT));
		}

		delete(&event);
	} /* while() */

	delete(&event);

	return test;
} /* nexttest() */


static void nextspf(struct dns_txt *txt, yaml_parser_t *parser) {
	yaml_event_t event;

	memset(txt->data, ' ', sizeof txt->data);

	if (YAML_SCALAR_EVENT == expect(parser, &event, SET(YAML_SCALAR_EVENT, YAML_SEQUENCE_START_EVENT))) {
		assert(SCALAR_LEN(&event) < txt->size);
		memcpy(txt->data, SCALAR_STR(&event), SCALAR_LEN(&event));
		txt->len = SCALAR_LEN(&event);
	} else {
		while (YAML_SCALAR_EVENT == expect(parser, delete(&event), SET(YAML_SCALAR_EVENT, YAML_SEQUENCE_END_EVENT))) {
			assert(txt->size - txt->len >= 255);
			assert(SCALAR_LEN(&event) <= 255);
			memcpy(&txt->data[txt->len], SCALAR_STR(&event), SCALAR_LEN(&event));
			txt->len += 255;
		}
	}

	delete(&event);
} /* nextspf() */


static struct cache *nextzonedata(yaml_parser_t *parser) {
	struct cache *zonedata;
	yaml_event_t event;
	char rrname[256];
	union dns_any anyrr;
	int error, rrtype;

	assert(zonedata = cache_open(&error));

	discard(parser, SET(YAML_MAPPING_START_EVENT));

	while (YAML_SCALAR_EVENT == expect(parser, &event, SET(YAML_SCALAR_EVENT, YAML_MAPPING_END_EVENT))) {
		dns_d_init(rrname, sizeof rrname, (char *)event.data.scalar.value, strlen((char *)event.data.scalar.value), DNS_D_ANCHOR);
		discard(parser, SET(YAML_SEQUENCE_START_EVENT));

		while (INSET(SET(YAML_MAPPING_START_EVENT, YAML_SCALAR_EVENT), expect(parser, delete(&event), SET(YAML_MAPPING_START_EVENT, YAML_SCALAR_EVENT, YAML_SEQUENCE_END_EVENT)))) {
			if (event.type == YAML_SCALAR_EVENT) {
				SAY("%s: unknown zonedata value", (char *)event.data.scalar.value);

				continue;
			}

			dns_any_init(&anyrr, sizeof anyrr);

			expect(parser, delete(&event), SET(YAML_SCALAR_EVENT));

			if (!(rrtype = dns_itype((char *)event.data.scalar.value)))
				panic("%s: unknown RR type", event.data.scalar.value);

			switch (rrtype) {
			case DNS_T_A:
				expect(parser, delete(&event), SET(YAML_SCALAR_EVENT));
				assert(1 == inet_pton(AF_INET, SCALAR_STR(&event), &anyrr.a));

				break;
			case DNS_T_AAAA:
				expect(parser, delete(&event), SET(YAML_SCALAR_EVENT));
				assert(1 == inet_pton(AF_INET6, SCALAR_STR(&event), &anyrr.aaaa));

				break;
			case DNS_T_PTR:
				expect(parser, delete(&event), SET(YAML_SCALAR_EVENT));
				if (!dns_d_init(anyrr.ptr.host, sizeof anyrr.ptr.host, SCALAR_STR(&event), SCALAR_LEN(&event), DNS_D_ANCHOR))
					goto next;

				break;
			case DNS_T_MX:
				discard(parser, SET(YAML_SEQUENCE_START_EVENT));

				expect(parser, delete(&event), SET(YAML_SCALAR_EVENT));
				anyrr.mx.preference = atoi(SCALAR_STR(&event));

				expect(parser, delete(&event), SET(YAML_SCALAR_EVENT));
				dns_d_init(anyrr.mx.host, sizeof anyrr.mx.host, SCALAR_STR(&event), SCALAR_LEN(&event), DNS_D_ANCHOR);

				discard(parser, SET(YAML_SEQUENCE_END_EVENT));

				if (!SCALAR_LEN(&event))
					goto next;

				break;
			case DNS_T_SPF:
				/* FALL THROUGH */
			case DNS_T_TXT:
				nextspf(&anyrr.txt, parser);

				break;
			default:
				discard(parser, SET(YAML_SCALAR_EVENT));

				SAY("%s: unknown RR type", dns_strtype(rrtype));

				goto next;
			} /* switch() */

			assert(!(error = cache_insert(zonedata, rrname, rrtype, 3600, &anyrr)));
next:
			discard(parser, SET(YAML_MAPPING_END_EVENT));
		} /* while() */

		delete(&event);
	} /* while() */

	delete(&event);

//	cache_dumpfile(zonedata, stderr);

	return zonedata;
} /* nextzonedata() */



static struct section *nextsection(yaml_parser_t *parser) {
	struct section *section;
	yaml_event_t event;
	int type;
	char *txt;

	if (YAML_STREAM_END_EVENT == expect(parser, &event, SET(YAML_DOCUMENT_START_EVENT, YAML_STREAM_END_EVENT))) {
		delete(&event);
		return NULL;
	}

	discard(parser, SET(YAML_MAPPING_START_EVENT));

	assert(section = malloc(sizeof *section));
	memset(section, 0, sizeof *section);
	CIRCLEQ_INIT(&section->tests);

	while (expect(parser, delete(&event), SET(YAML_SCALAR_EVENT, YAML_MAPPING_END_EVENT))) {
		if (event.type == YAML_MAPPING_END_EVENT)
			break;

		txt = (char *)event.data.scalar.value;

		if (streq(txt, "description")) {
			nextscalar(&section->descr, parser);
		} else if (streq(txt, "comment")) {
			nextscalar(&section->comment, parser);
		} else if (streq(txt, "tests")) {
			struct test *test;

			discard(parser, SET(YAML_MAPPING_START_EVENT));

			while ((test = nexttest(parser))) {
				CIRCLEQ_INSERT_TAIL(&section->tests, test, cqe);
			}
		} else if (streq(txt, "zonedata")) {
			section->zonedata = nextzonedata(parser);
		} else {
			panic("%s: unknown top-level field", txt);
		}
	}

	delete(&event);

	discard(parser, SET(YAML_DOCUMENT_END_EVENT));

	return section;
} /* nextsection() */


int main(int argc, char **argv) {
	yaml_parser_t parser;
	yaml_event_t event;
	int done = 0, state = 0;
	struct section *section;

	yaml_parser_initialize(&parser);
	yaml_parser_set_input_file(&parser, stdin);

	discard(&parser, SET(YAML_STREAM_START_EVENT));

#if 1
	while ((section = nextsection(&parser))) {
		;;
	} /* while() */
#else
	while (!done) {
		assert(yaml_parser_parse(&parser, &event));

		puts(yaml_strevent(event.type));

		switch (event.type) {
		case YAML_SCALAR_EVENT:
			printf("anchor:%s tag:%s value:%s\n", event.data.scalar.anchor, event.data.scalar.tag, event.data.scalar.value);

			break;
		case YAML_STREAM_END_EVENT:
			done = 1;

			break;
		} /* switch(event.type) */


		yaml_event_delete(&event);
	}
#endif

	return 0;
} /* main() */
