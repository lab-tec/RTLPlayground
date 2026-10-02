/*
 * test_rollback.c: rollback.c against a simulated 2 MB SPI NOR flash.
 *
 * rollback.c is compiled unmodified. The flash model behaves like the chip in
 * the ways the code relies on: an erase sets a 4 kB sector to 0xff, and a
 * write can only clear bits, so a write to a byte that was not erased first
 * is caught rather than silently stored. crc16_bank1() switches the code bank
 * as the real one does, so a caller left in the wrong bank shows up too.
 *
 * check_and_flash_update_image() lives in rtlplayground.c with the rest of the
 * boot code; apply_staged_update() below repeats its steps (marker, CRC over
 * the upload area, rollback_backup(), copy below CONFIG_START, erase) so a
 * whole update -> rollback -> update cycle can be run.
 */
#include <stdio.h>
#include <stdlib.h>
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "rollback.h"

#define FLASH_2MB	0x200000UL
#define IMAGE_LEN	0x80000UL

static uint8_t flash[FLASH_2MB];
static int fails, checks;

#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

/* ---- the firmware's globals and leaf calls rollback.c links against ---- */
struct flash_region_t flash_region;
uint8_t flash_buf[FLASH_BUF_SIZE];
uint32_t flash_size = FLASH_2MB;
uint16_t crc_value;
volatile uint8_t PSBANK;
static int resets, flash_inits, bad_writes;
static char out[4096];
static size_t out_len;

static uint16_t crc16_update(uint16_t crc, uint8_t a)
{
	crc ^= a;
	for (int i = 0; i < 8; i++)
		crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
	return crc;
}

void crc16_bank1(uint8_t *v) { crc_value = crc16_update(crc_value, *v); PSBANK = 1; }
void flash_init(uint8_t enable_dio) { (void)enable_dio; flash_inits++; }
void flash_read_bulk(uint8_t *dst)
{
	memcpy(dst, flash + flash_region.addr, flash_region.len);
	flash_region.addr += flash_region.len;
}
void flash_write_bytes(uint8_t *ptr)
{
	for (uint16_t i = 0; i < flash_region.len; i++) {
		uint8_t *f = flash + flash_region.addr + i;
		if ((*f & ptr[i]) != ptr[i])
			bad_writes++;	/* would need a 0 -> 1 bit: no erase before */
		*f &= ptr[i];
	}
}
void flash_sector_erase(void)
{
	memset(flash + (flash_region.addr & ~(uint32_t)(FLASH_SECTOR_SIZE - 1)), 0xff, FLASH_SECTOR_SIZE);
}
void reset_chip(void) { resets++; }
void delay(uint16_t t) { (void)t; }
void write_char(char c) { if (out_len < sizeof(out) - 1) { out[out_len++] = c; out[out_len] = 0; } }
void print_string(const char *p) { while (*p) write_char(*p++); }
void print_string_x(char *p) { while (*p) write_char(*p++); }

/* ---- helpers ---- */
static uint16_t crc_of(uint32_t addr, uint32_t len)
{
	uint16_t crc = 0;
	for (uint32_t i = 0; i < len; i++)
		crc = crc16_update(crc, flash[addr + i]);
	return crc;
}

/* A firmware image as the build makes it: marker, payload, CRC trailer */
static void make_image(uint8_t *img, uint8_t seed)
{
	srand(seed);
	for (uint32_t i = 0; i < IMAGE_LEN; i++)
		img[i] = rand();
	img[0] = 0x00;
	img[1] = 0x40;
	memset(img + CONFIG_START, 0xff, IMAGE_LEN - CONFIG_START);
	uint16_t crc = 0;
	for (uint32_t i = 0; i < IMAGE_LEN - 2; i++)
		crc = crc16_update(crc, img[i]);
	crc ^= 0xffff;
	img[IMAGE_LEN - 2] = crc;
	img[IMAGE_LEN - 1] = crc >> 8;
}

/* The steps of check_and_flash_update_image() in rtlplayground.c */
static int apply_staged_update(void)
{
	if (flash[FIRMWARE_UPLOAD_START] != 0x00 || flash[FIRMWARE_UPLOAD_START + 1] != 0x40)
		return 0;
	if (crc_of(FIRMWARE_UPLOAD_START, IMAGE_LEN) != 0xb001) {
		memset(flash + FIRMWARE_UPLOAD_START, 0xff, IMAGE_LEN);
		return -1;
	}
	rollback_backup();
	for (uint32_t a = 0; a < CONFIG_START; a += FLASH_SECTOR_SIZE)
		memset(flash + a, 0xff, FLASH_SECTOR_SIZE);
	for (uint32_t a = 0; a < CONFIG_START; a++)
		flash[a] &= flash[FIRMWARE_UPLOAD_START + a];
	memset(flash + FIRMWARE_UPLOAD_START, 0xff, IMAGE_LEN);
	return 1;
}

static void stage(const uint8_t *img)
{
	memcpy(flash + FIRMWARE_UPLOAD_START, img, IMAGE_LEN);
}

static int running_is(const uint8_t *img)
{
	return !memcmp(flash, img, CONFIG_START);
}

int main(void)
{
	static uint8_t v1[IMAGE_LEN], v2[IMAGE_LEN], v3[IMAGE_LEN];
	make_image(v1, 1);
	make_image(v2, 2);
	make_image(v3, 3);

	memset(flash, 0xff, sizeof(flash));
	memcpy(flash, v1, CONFIG_START);
	memcpy(flash + CONFIG_START, "ip 172.16.0.6", 13);	/* startup config */

	/* No backup yet */
	PSBANK = 2;
	out_len = 0;
	rollback_status();
	CHECK(strstr(out, "none") != NULL, "status reports no backup before any update");
	CHECK(PSBANK == 2, "status leaves the caller's bank selected");

	/* Update v1 -> v2 keeps v1 */
	stage(v2);
	CHECK(apply_staged_update() == 1, "update v1 -> v2 applied");
	CHECK(running_is(v2), "v2 is running");
	CHECK(!memcmp(flash + ROLLBACK_START, v1, CONFIG_START), "backup holds v1");
	CHECK(!memcmp(flash + ROLLBACK_HDR, "KCRB", 4), "header magic written");
	CHECK(flash[ROLLBACK_HDR + 6] == 0xff, "header state idle");
	/* rollback.c includes the version.h next to it (a firmware build's, when
	 * one exists), not test/stub's, so check the field's shape, not its text */
	CHECK(flash[ROLLBACK_HDR + 8] != 0 && flash[ROLLBACK_HDR + 8] != 0xff
	      && memchr(flash + ROLLBACK_HDR + 8, 0, 32) != NULL, "header names the backed-up version");
	CHECK(!memcmp(flash + CONFIG_START, "ip 172.16.0.6", 13), "startup config untouched");
	CHECK(bad_writes == 0, "no write without an erase during the backup");

	PSBANK = 2;
	out_len = 0;
	rollback_status();
	CHECK(strstr(out, "CRC OK") != NULL, "status sees a good backup");
	CHECK(strstr(out, (char *)flash + ROLLBACK_HDR + 8) != NULL, "status names the backed-up version");
	CHECK(PSBANK == 2, "status restores the bank after CRC work");

	/* Roll back: stages v1 as an update and resets */
	PSBANK = 3;
	out_len = 0;
	rollback_restore();
	CHECK(resets == 1, "restore resets the chip");
	CHECK(PSBANK == 3, "restore leaves the caller's bank selected");
	CHECK(flash[ROLLBACK_HDR + 6] == 0x00, "header marked restore pending");
	CHECK(crc_of(FIRMWARE_UPLOAD_START, IMAGE_LEN) == 0xb001, "staged backup carries a valid CRC trailer");
	CHECK(bad_writes == 0, "no write without an erase while staging");
	CHECK(flash_inits >= 1, "flash put in single-IO mode before writing");

	/* Next boot installs it, and keeps the backup */
	CHECK(apply_staged_update() == 1, "staged backup accepted by the update path");
	CHECK(running_is(v1), "v1 is running again");
	CHECK(!memcmp(flash + ROLLBACK_START, v1, CONFIG_START), "backup still holds v1");
	CHECK(flash[ROLLBACK_HDR + 6] == 0xff, "restore-pending mark cleared");
	CHECK(!memcmp(flash + CONFIG_START, "ip 172.16.0.6", 13), "startup config survives the rollback");

	/* A later update backs up whatever runs at the time */
	stage(v3);
	CHECK(apply_staged_update() == 1, "update v1 -> v3 applied");
	CHECK(running_is(v3), "v3 is running");
	CHECK(!memcmp(flash + ROLLBACK_START, v1, CONFIG_START), "backup holds v1, the replaced image");

	/* A damaged backup is refused */
	flash[ROLLBACK_START + 1234] ^= 0x01;
	resets = 0;
	out_len = 0;
	memset(flash + FIRMWARE_UPLOAD_START, 0xff, IMAGE_LEN);
	rollback_restore();
	CHECK(resets == 0, "no reset with a damaged backup");
	CHECK(strstr(out, "nothing restored") != NULL, "damaged backup reported");
	CHECK(flash[FIRMWARE_UPLOAD_START] == 0xff, "nothing staged from a damaged backup");

	/* Too little flash: no backup, update still works */
	flash_size = 0x100000;
	memset(flash + ROLLBACK_START, 0xff, FLASH_2MB - ROLLBACK_START);
	stage(v2);
	out_len = 0;
	CHECK(apply_staged_update() == 1, "update on 1 MB flash applied");
	CHECK(strstr(out, "too small") != NULL, "1 MB flash: backup skipped, said so");
	CHECK(flash[ROLLBACK_HDR] == 0xff, "1 MB flash: nothing written past the upload area");

	printf("test_rollback: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
