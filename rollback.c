/*
 * Rollback-safe upgrades.
 *
 * An upload that passes its checksum but does not boot properly can only be
 * undone with a flash programmer. To avoid that, the update path keeps a copy
 * of the firmware it is about to replace, and the reset button (or the
 * "rollback restore" command) puts that copy back.
 *
 * - rollback_backup(), called by check_and_flash_update_image() once the
 *   uploaded image has passed its checksum and before it is moved into place,
 *   copies the running firmware (everything below CONFIG_START) to
 *   ROLLBACK_START, then writes a header sector at ROLLBACK_HDR: a magic, the
 *   CRC16 of the copy, a state byte and the version string of the copied
 *   firmware.
 * - rollback_restore() checks the copy against its CRC, writes it to the
 *   upload area as an ordinary update image (padded with erased bytes and
 *   closed with the same CRC trailer tools/crc_calculator writes), marks the
 *   header "restore pending" and resets. The existing update path installs it
 *   on the next boot; seeing the mark, rollback_backup() keeps the copy rather
 *   than replacing it with the firmware being rolled back.
 *
 * This file stays in the common area: crc16_bank1() switches the code bank and
 * does not switch back, so the bank is saved and restored around it for
 * callers in banked code.
 */

#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_sfr.h"
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "rollback.h"
#include "kadam.h"
#include "version.h"

#define RB_MAGIC_0		'K'
#define RB_MAGIC_1		'C'
#define RB_MAGIC_2		'R'
#define RB_MAGIC_3		'B'
#define RB_CRC_LO		4
#define RB_CRC_HI		5
#define RB_STATE		6
#define RB_STATE_IDLE		0xff	/* as erased */
#define RB_STATE_RESTORING	0x00	/* written over 0xff without an erase */
#define RB_VERSION		8
#define RB_VERSION_LEN		32
#define RB_HDR_LEN		(RB_VERSION + RB_VERSION_LEN)

/* The update image is 512 kB and ends with a 2 byte CRC trailer */
#define RB_IMAGE_LEN		(FIRMWARE_UPLOAD_START)

extern __xdata struct flash_region_t flash_region;
extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];
extern __xdata uint32_t flash_size;
extern __xdata uint16_t crc_value;
void crc16_bank1(__xdata uint8_t *v) __naked;

/* Arguments of rb_copy(), kept in xdata: internal RAM is full */
static __xdata uint32_t rb_src;
static __xdata uint32_t rb_dst;
static __xdata uint32_t rb_len;
static __xdata uint8_t rb_write;
static __xdata uint16_t rb_i;
static __xdata uint8_t * __xdata rb_p;
static __xdata uint8_t rb_bank;

/*
 * Runs rb_len bytes from rb_src through the CRC, and also writes them to
 * rb_dst when rb_write is set, erasing each destination sector as it is
 * reached. rb_len and rb_dst must be multiples of FLASH_BUF_SIZE and the
 * destination must start on a sector.
 */
static void rb_copy(void)
{
	while (rb_len) {
		flash_region.addr = rb_src;
		flash_region.len = FLASH_BUF_SIZE;
		flash_read_bulk(flash_buf);
		rb_p = flash_buf;
		for (rb_i = 0; rb_i < FLASH_BUF_SIZE; rb_i++)
			crc16_bank1(rb_p++);
		if (rb_write) {
			if (!(rb_dst & (FLASH_SECTOR_SIZE - 1))) {
				flash_region.addr = rb_dst;
				flash_sector_erase();
			}
			flash_region.addr = rb_dst;
			flash_region.len = FLASH_BUF_SIZE;
			flash_write_bytes(flash_buf);
			rb_dst += FLASH_BUF_SIZE;
		}
		rb_src += FLASH_BUF_SIZE;
		rb_len -= FLASH_BUF_SIZE;
		if (!(rb_src & 0x7fff))
			write_char('.');
	}
}

/*
 * Reads the header into flash_buf; nonzero if it carries the magic.
 * Byte results here, not bool: bit temporaries would need internal RAM.
 */
static uint8_t rb_header_read(void)
{
	flash_region.addr = ROLLBACK_HDR;
	flash_region.len = RB_HDR_LEN;
	flash_read_bulk(flash_buf);
	if (flash_buf[0] != RB_MAGIC_0 || flash_buf[1] != RB_MAGIC_1
	    || flash_buf[2] != RB_MAGIC_2 || flash_buf[3] != RB_MAGIC_3)
		return 0;
	return 1;
}

/* Erases the header sector and writes the header held in flash_buf */
static void rb_header_write(void)
{
	flash_region.addr = ROLLBACK_HDR;
	flash_sector_erase();
	flash_region.addr = ROLLBACK_HDR;
	flash_region.len = RB_HDR_LEN;
	flash_write_bytes(flash_buf);
}

static uint8_t rb_room(void)
{
	if (flash_size < ROLLBACK_HDR + FLASH_SECTOR_SIZE)
		return 0;
	return 1;
}

void rollback_backup(void)
{
	if (!rb_room()) {
		print_string("\nFlash too small to keep a firmware backup.\n");
		return;
	}

	if (rb_header_read() && flash_buf[RB_STATE] == RB_STATE_RESTORING) {
		/* Installing the backup itself: keep it, and clear the mark */
		print_string("\nInstalling the firmware backup; keeping the backup.\n");
		reboot_note(REBOOT_ROLLBACK);
		flash_buf[RB_STATE] = RB_STATE_IDLE;
		rb_header_write();
		return;
	}

	print_string("\nBacking up the running firmware");
	/* Header first: a copy cut short by a power loss must not look valid */
	flash_region.addr = ROLLBACK_HDR;
	flash_sector_erase();

	crc_value = 0;
	rb_src = 0;
	rb_dst = ROLLBACK_START;
	rb_len = ROLLBACK_LEN;
	rb_write = 1;
	rb_copy();

	for (rb_i = 0; rb_i < RB_HDR_LEN; rb_i++)
		flash_buf[rb_i] = 0xff;
	flash_buf[0] = RB_MAGIC_0;
	flash_buf[1] = RB_MAGIC_1;
	flash_buf[2] = RB_MAGIC_2;
	flash_buf[3] = RB_MAGIC_3;
	flash_buf[RB_CRC_LO] = crc_value;
	flash_buf[RB_CRC_HI] = crc_value >> 8;
	for (rb_i = 0; rb_i < RB_VERSION_LEN - 1 && VERSION_SW[rb_i]; rb_i++)
		flash_buf[RB_VERSION + rb_i] = VERSION_SW[rb_i];
	flash_buf[RB_VERSION + rb_i] = 0;
	rb_header_write();
	print_string("Done.\n");
}

/*
 * True if a backup is present and matches its CRC. Leaves the header in
 * flash_buf when it returns true.
 */
bool rollback_check(void)
{
	__xdata uint16_t crc_stored;

	if (!rb_room() || !rb_header_read())
		return false;
	crc_stored = flash_buf[RB_CRC_LO] | (flash_buf[RB_CRC_HI] << 8);

	rb_bank = PSBANK;
	crc_value = 0;
	rb_src = ROLLBACK_START;
	rb_len = ROLLBACK_LEN;
	rb_write = 0;
	rb_copy();
	PSBANK = rb_bank;

	if (crc_value != crc_stored)
		return false;
	return rb_header_read();
}

void rollback_status(void)
{
	print_string("Firmware backup: ");
	if (!rollback_check()) {
		print_string(" none, or damaged\n");
		return;
	}
	print_string(" ");
	print_string_x((__xdata char *)(flash_buf + RB_VERSION));
	print_string(", CRC OK\n");
}

void rollback_restore(void)
{
	print_string("Checking the firmware backup");
	if (!rollback_check()) {
		print_string(" none, or damaged; nothing restored.\n");
		return;
	}
	print_string(" OK: ");
	print_string_x((__xdata char *)(flash_buf + RB_VERSION));
	print_string("\nStaging it as an update");

	rb_bank = PSBANK;
	flash_init(0);	/* writes need single-IO mode, as in the update path */

	/* The copy, then erased bytes up to the CRC trailer */
	crc_value = 0;
	rb_src = ROLLBACK_START;
	rb_dst = FIRMWARE_UPLOAD_START;
	rb_len = ROLLBACK_LEN;
	rb_write = 1;
	rb_copy();
	for (; rb_dst < FIRMWARE_UPLOAD_START + RB_IMAGE_LEN; rb_dst += FLASH_SECTOR_SIZE) {
		flash_region.addr = rb_dst;
		flash_sector_erase();
	}
	flash_buf[0] = 0xff;
	for (rb_src = ROLLBACK_LEN; rb_src < RB_IMAGE_LEN - 2; rb_src++)
		crc16_bank1(flash_buf);
	crc_value ^= 0xffff;
	flash_buf[0] = crc_value;
	flash_buf[1] = crc_value >> 8;
	flash_region.addr = FIRMWARE_UPLOAD_START + RB_IMAGE_LEN - 2;
	flash_region.len = 2;
	flash_write_bytes(flash_buf);

	/* Tell the next rollback_backup() to keep the copy */
	flash_buf[0] = RB_STATE_RESTORING;
	flash_region.addr = ROLLBACK_HDR + RB_STATE;
	flash_region.len = 1;
	flash_write_bytes(flash_buf);
	PSBANK = rb_bank;

	print_string(" Done.\nResetting to install it.\n");
	delay(200);
	reboot_note(REBOOT_ROLLBACK);
	reset_chip();
}
