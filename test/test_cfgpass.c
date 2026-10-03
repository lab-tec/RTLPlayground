/*
 * test_cfgpass.c: keeping the password out of what the web interface shows.
 *
 * cfgpass.c is compiled unmodified. The texts are laid out the way the web
 * server hands them over: a configuration read from flash, the command log,
 * and an uploaded configuration sitting behind its multipart headers in the
 * upload buffer, so cfgpass_keep() has the headers to write over.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl837x_common.h"
#include "cfgpass.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

char passwd[21];

static uint8_t buf[512];

/* Runs cfgpass_hide() on s and returns the result as a string */
static const char *hide(const char *s)
{
	static char out[512];

	memcpy(buf, s, strlen(s));
	cfgpass_buf = buf;
	cfgpass_len = strlen(s);
	cfgpass_hide();
	memcpy(out, cfgpass_buf, cfgpass_len);
	out[cfgpass_len] = 0;
	return out;
}

/* An upload: headers, then the configuration; keep() may use the headers */
static const char *keep(const char *headers, const char *cfg, int *ok)
{
	static char out[512];
	size_t h = strlen(headers);

	memset(buf, '#', sizeof(buf));
	memcpy(buf, headers, h);
	memcpy(buf + h, cfg, strlen(cfg));
	cfgpass_floor = buf;
	cfgpass_buf = buf + h;
	cfgpass_len = strlen(cfg);
	*ok = cfgpass_keep();
	memcpy(out, cfgpass_buf, cfgpass_len);
	out[cfgpass_len] = 0;
	return out;
}

#define HDRS "------WebKitFormBoundaryABC\r\nContent-Disposition: form-data; name=\"file\"; filename=\"config.txt\"\r\nContent-Type: text/plain\r\n\r\n"

int main(void)
{
	int ok;

	puts("--- hide");
	CHECK(!strcmp(hide("ip 172.16.0.6\npasswd s3cret\ngw 172.16.0.254\n"),
		      "ip 172.16.0.6\ngw 172.16.0.254\n"), "a passwd line in the middle goes");
	CHECK(!strcmp(hide("passwd s3cret\nip 1.2.3.4\n"), "ip 1.2.3.4\n"), "first line");
	CHECK(!strcmp(hide("ip 1.2.3.4\npasswd s3cret"), "ip 1.2.3.4\n"), "last line, no newline");
	CHECK(!strcmp(hide("passwd a\npasswd b\nvlan 10 x 2t\npasswd c\n"), "vlan 10 x 2t\n"), "every one of several");
	CHECK(!strcmp(hide("ip 1.2.3.4\r\npasswd s3cret\r\nhostname x\r\n"), "ip 1.2.3.4\r\nhostname x\r\n"), "CRLF lines");
	CHECK(!strcmp(hide("  passwd s3cret\nip 1.2.3.4\n"), "ip 1.2.3.4\n"), "leading blanks");
	CHECK(!strcmp(hide("passwd\nip 1.2.3.4\n"), "ip 1.2.3.4\n"), "passwd without a value");
	CHECK(!strcmp(hide("passwdx 1\nmypasswd 2\n# passwd 3\n"), "passwdx 1\nmypasswd 2\n# passwd 3\n"),
	      "other words are left alone");
	CHECK(!strcmp(hide("\nsflow on\n\npasswd q\nsession 1200\n"), "\nsflow on\n\nsession 1200\n"),
	      "a command log, which starts with a newline");
	CHECK(!strcmp(hide(""), ""), "empty");
	CHECK(!strcmp(hide("passwd s3cret"), ""), "only the password");

	puts("--- keep");
	strcpy(passwd, "R4nd0m-Pw");
	CHECK(!strcmp(keep(HDRS, "ip 172.16.0.6\nsflow on\n", &ok), "passwd R4nd0m-Pw\nip 172.16.0.6\nsflow on\n") && ok,
	      "no passwd line: the running password goes first");
	CHECK(!memcmp(buf, "------WebKitFormBoundaryABC", 27), "only the bytes just ahead of the text are used");
	CHECK(!strcmp(keep(HDRS, "ip 1.2.3.4\npasswd newpw\n", &ok), "ip 1.2.3.4\npasswd newpw\n") && ok,
	      "a passwd line in the upload sets that password, untouched");
	CHECK(!strcmp(keep(HDRS, "  passwd newpw\r\n", &ok), "  passwd newpw\r\n") && ok,
	      "...with blanks and CRLF too");
	CHECK(!strcmp(keep(HDRS, "", &ok), "passwd R4nd0m-Pw\n") && ok, "an empty upload still keeps it");
	CHECK(!strcmp(keep(HDRS, "mypasswd 2\n", &ok), "passwd R4nd0m-Pw\nmypasswd 2\n") && ok,
	      "a lookalike isn't a passwd line");
	strcpy(passwd, "12345678901234567890");
	CHECK(!strcmp(keep(HDRS, "ip 1.2.3.4\n", &ok), "passwd 12345678901234567890\nip 1.2.3.4\n") && ok,
	      "a 20-character password");
	CHECK(keep("short\r\n\r\n", "ip 1.2.3.4\n", &ok) && !ok, "no room ahead of the text: refused");
	CHECK(cfgpass_buf == buf + 9 && cfgpass_len == 11, "and the text is left as it was");

	puts("--- round trip: what the UI shows, saved back, keeps the password");
	strcpy(passwd, "R4nd0m-Pw");
	{
		char shown[512];
		strcpy(shown, hide("passwd R4nd0m-Pw\nip 172.16.0.6\nsflow on\n"));
		CHECK(!strstr(shown, "R4nd0m"), "not shown");
		CHECK(!strcmp(keep(HDRS, shown, &ok), "passwd R4nd0m-Pw\nip 172.16.0.6\nsflow on\n") && ok,
		      "saved back intact");
	}

	printf("test_cfgpass: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
