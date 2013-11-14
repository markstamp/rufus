/*
 * Rufus: The Reliable USB Formatting Utility
 * SMART HDD vs Flash detection (using ATA over USB, S.M.A.R.T., etc.)
 * Copyright © 2013 Pete Batard <pete@akeo.ie>
 *
 * Based in part on scsiata.cpp from Smartmontools: http://smartmontools.sourceforge.net
 * Copyright © 2006-12 Douglas Gilbert <dgilbert@interlog.com>
 * Copyright © 2009-13 Christian Franke <smartmontools-support@lists.sourceforge.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>

#include "msapi_utf8.h"
#include "rufus.h"
#include "smart.h"


/* Helper functions */
static uint8_t GetAtaDirection(uint8_t AtaCmd, uint8_t Features) {
	// Far from complete -- only the commands we *may* use.

	// Most SMART commands require DATA_IN but there are a couple exceptions
	BOOL smart_out = (AtaCmd == ATA_SMART_CMD) && 
		((Features == ATA_SMART_STATUS) || (Features == ATA_SMART_WRITE_LOG_SECTOR));

	switch (AtaCmd) {
	case ATA_IDENTIFY_DEVICE:
	case ATA_READ_LOG_EXT:
		return ATA_PASSTHROUGH_DATA_IN;
	case ATA_SMART_CMD:
		if (!smart_out)
			return ATA_PASSTHROUGH_DATA_IN;
		// fall through
	case ATA_DATA_SET_MANAGEMENT:
		return ATA_PASSTHROUGH_DATA_OUT;
	default:
		return ATA_PASSTHROUGH_DATA_NONE;
	}
}

const char* SptStrerr(int errcode)
{
	static char scsi_err[64];

	if ((errcode > 0) && (errcode <= 0xff)) {
		safe_sprintf(scsi_err, sizeof(scsi_err), "SCSI status: 0x%02X", (uint8_t)errcode);
		return (const char*)scsi_err;
	}

	switch(errcode) {
	case SPT_SUCCESS:
		return "Success";
	case SPT_ERROR_CDB_LENGTH:
		return "Invalid CDB length";
	case SPT_ERROR_BUFFER:
		return "Buffer must be aligned to a page boundary and less than 64KB in size";
	case SPT_ERROR_DIRECTION:
		return "Invalid Direction";
	case SPT_ERROR_EXTENDED_CDB:
		return "Extended and variable length CDB commands are not supported";
	case SPT_ERROR_CDB_OPCODE:
		return "Opcodes above 0xC0 are not supported";
	case SPT_ERROR_TIMEOUT:
		return "Timeout";
	case SPT_ERROR_INVALID_PARAMETER:
		return "Invalid DeviceIoControl parameter";
	case SPT_ERROR_CHECK_STATUS:
		return "SCSI error (check Status)";
	default:
		return "Unknown error";
	}
}

/*
 * SCSI Passthrough (using IOCTL_SCSI_PASS_THROUGH_DIRECT)
 * Should be provided a handle to the physical device (R/W) as well as a Cdb and a buffer that is page aligned
 * Direction should be one of SCSI_IOCTL_DATA_###
 *
 * Returns 0 (SPT_SUCCESS) on success, a positive SCSI Status in case of an SCSI error or negative otherwise.
 */

BOOL ScsiPassthroughDirect(HANDLE hPhysical, uint8_t* Cdb, size_t CdbLen, uint8_t Direction,
						   void* DataBuffer, size_t BufLen, uint32_t Timeout)
{
	SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER sptdwb = {{0}, 0, {0}};
	DWORD err, size = sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER);
	BOOL r;

	// Sanity checks
	if ((CdbLen == 0) || (CdbLen > sizeof(sptdwb.sptd.Cdb)))
		return SPT_ERROR_CDB_LENGTH;

	if (((uintptr_t)DataBuffer % 0x10 != 0) || (BufLen > 0xFFFF))
		return SPT_ERROR_BUFFER;

	if (Direction > SCSI_IOCTL_DATA_UNSPECIFIED)
		return SPT_ERROR_DIRECTION;

	// http://en.wikipedia.org/wiki/SCSI_command
	if ((Cdb[0] == 0x7e) || (Cdb[0] == 0x7f))
		return SPT_ERROR_EXTENDED_CDB;

	// Opcodes above 0xC0 are unsupported (apart for the special JMicron/Sunplus modes)
	if ( (Cdb[0] >= 0xc0) && (Cdb[0] != USB_JMICRON_ATA_PASSTHROUGH)
	  && (Cdb[0] != USB_SUNPLUS_ATA_PASSTHROUGH) )
		return SPT_ERROR_CDB_OPCODE;

	sptdwb.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
	sptdwb.sptd.PathId = 0;
	sptdwb.sptd.TargetId = 0;
	sptdwb.sptd.Lun = 0;
	sptdwb.sptd.CdbLength = (uint8_t)CdbLen;
	sptdwb.sptd.DataIn = Direction;		// One of SCSI_IOCTL_DATA_###
	sptdwb.sptd.SenseInfoLength = SPT_SENSE_LENGTH;
	sptdwb.sptd.DataTransferLength = (uint16_t)BufLen;
	sptdwb.sptd.TimeOutValue = Timeout;
	sptdwb.sptd.DataBuffer = DataBuffer;
	sptdwb.sptd.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER, SenseBuf);

	memcpy(sptdwb.sptd.Cdb, Cdb, CdbLen);

	r = DeviceIoControl(hPhysical, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptdwb, size, &sptdwb, size, &size, FALSE);
	if ((r) && (sptdwb.sptd.ScsiStatus == 0)) {
		return SPT_SUCCESS;
	}

	if (sptdwb.sptd.ScsiStatus != 0) {
		// uprintf("ScsiPassthroughDirect: CDB command 0x%02X failed (SCSI status 0x%02X)\n", Cdb[0], sptdwb.sptd.ScsiStatus);
		return (int)sptdwb.sptd.ScsiStatus;
	} else {
		err = GetLastError();
		// uprintf("ScsiPassthroughDirect: CDB command 0x%02X failed %s\n", Cdb[0], WindowsErrorString()); SetLastError(err);
		switch(err) {
		case ERROR_SEM_TIMEOUT:
			return SPT_ERROR_TIMEOUT;
		case ERROR_INVALID_PARAMETER:
			return SPT_ERROR_INVALID_PARAMETER;
		default:
			return SPT_ERROR_UNKNOWN_ERROR;
		}
	}
	return FALSE;
}


/* See ftp://ftp.t10.org/t10/document.04/04-262r8.pdf, http://www.scsitoolbox.com/pdfs/UsingSAT.pdf,
 * as well as http://nevar.pl/pliki/ATA8-ACS-3.pdf‎ */
static int SatAtaPassthrough(HANDLE hPhysical, ATA_PASSTHROUGH_CMD* Command, void* DataBuffer, size_t BufLen, uint32_t Timeout)
{
	uint8_t Cdb[12] = {0};
	int extend = 0;     /* For 48-bit ATA command (unused here) */
	int ck_cond = 0;    /* Set to 1 to read register(s) back */
	int protocol = 3;   /* Non-data */
	int t_dir = 1;      /* 0 -> to device, 1 -> from device */
	int byte_block = 1; /* 0 -> bytes, 1 -> 512 byte blocks */
	int t_length = 0;   /* 0 -> no data transferred */
	uint8_t Direction;

	if (BufLen % 512 != 0) {
		uprintf("SatAtaPassthrough: BufLen must be a multiple of <block size>\n");
		return SPT_ERROR_BUFFER;
	}

	// Set data direction
	Direction = GetAtaDirection(Command->AtaCmd, Command->Features);
	if (BufLen != 0) {
		switch (Direction) {
		case ATA_PASSTHROUGH_DATA_NONE:
			break;
		case ATA_PASSTHROUGH_DATA_IN:
			protocol = 4;  // PIO data-in
			t_length = 2;  // The transfer length is specified in the sector_count field
			break;
		case ATA_PASSTHROUGH_DATA_OUT:
			protocol = 5;  // PIO data-out
			t_length = 2;  // The transfer length is specified in the sector_count field
			t_dir = 0;     // to device
			break;
		}
	}

	Cdb[0] = SAT_ATA_PASSTHROUGH_12;
	Cdb[1] = (protocol << 1) | extend;
	Cdb[2] = (ck_cond << 5) | (t_dir << 3) | (byte_block << 2) | t_length;
	Cdb[3] = Command->Features;
	Cdb[4] = (uint8_t)(BufLen >> SECTOR_SIZE_SHIFT_BIT);
	Cdb[5] = Command->Lba_low;
	Cdb[6] = Command->Lba_mid;
	Cdb[7] = Command->Lba_high;
	Cdb[8] = Command->Device;			// (m_port == 0 ? 0xa0 : 0xb0);  // Must be 0 for identify
	Cdb[9] = Command->AtaCmd;

	return ScsiPassthroughDirect(hPhysical, Cdb, sizeof(Cdb), Direction, DataBuffer, BufLen, Timeout);
}

/* The only differences between JMicron and Prolific are the extra 2 bytes for the CDB */
static int _UsbJMPLAtaPassthrough(HANDLE hPhysical, ATA_PASSTHROUGH_CMD* Command,
		void* DataBuffer, size_t BufLen, uint32_t Timeout, BOOL prolific)
{
	uint8_t Cdb[14] = {0};
	uint8_t Direction;

	Direction = GetAtaDirection(Command->AtaCmd, Command->Features);

	Cdb[0] = USB_JMICRON_ATA_PASSTHROUGH;
	Cdb[1] = ((BufLen != 0) && (Direction == ATA_PASSTHROUGH_DATA_OUT))?0x00:0x10;
	Cdb[3] = (uint8_t)(BufLen >> 8);
	Cdb[4] = (uint8_t)(BufLen);
	Cdb[5] = Command->Features;
	Cdb[6] = (uint8_t)(BufLen >> SECTOR_SIZE_SHIFT_BIT);
	Cdb[7] = Command->Lba_low;
	Cdb[8] = Command->Lba_mid;
	Cdb[9] = Command->Lba_high;
	Cdb[10] = Command->Device;			// (m_port == 0 ? 0xa0 : 0xb0);  // Must be 0 for identify
	Cdb[11] = Command->AtaCmd;
	// Prolific PL3507
	Cdb[12] = 0x06;
	Cdb[13] = 0x7b;

	return ScsiPassthroughDirect(hPhysical, Cdb, sizeof(Cdb)-(prolific?2:0), Direction, DataBuffer, BufLen, Timeout);
}

static int UsbJmicronAtaPassthrough(HANDLE hPhysical, ATA_PASSTHROUGH_CMD* Command, void* DataBuffer, size_t BufLen, uint32_t Timeout)
{
	return _UsbJMPLAtaPassthrough(hPhysical, Command, DataBuffer, BufLen, Timeout, FALSE);
}

/* UNTESTED!!! */
static int UsbProlificAtaPassthrough(HANDLE hPhysical, ATA_PASSTHROUGH_CMD* Command, void* DataBuffer, size_t BufLen, uint32_t Timeout)
{
	return _UsbJMPLAtaPassthrough(hPhysical, Command, DataBuffer, BufLen, Timeout, TRUE);
}

/* UNTESTED!!! */
static int UsbSunPlusAtaPassthrough(HANDLE hPhysical, ATA_PASSTHROUGH_CMD* Command, void* DataBuffer, size_t BufLen, uint32_t Timeout)
{
	uint8_t Cdb[12] = {0};
	uint8_t Direction;

	Direction = GetAtaDirection(Command->AtaCmd, Command->Features);

	Cdb[0] = USB_SUNPLUS_ATA_PASSTHROUGH;
	Cdb[2] = 0x22;
	if (BufLen != 0) {
		if (Direction == ATA_PASSTHROUGH_DATA_IN)
			Cdb[3] = 0x10;
		else if (Direction == ATA_PASSTHROUGH_DATA_OUT)
			Cdb[3] = 0x11;
	}
	Cdb[4] = (uint8_t)(BufLen >> SECTOR_SIZE_SHIFT_BIT);
	Cdb[5] = Command->Features;
	Cdb[6] = (uint8_t)(BufLen >> SECTOR_SIZE_SHIFT_BIT);
	Cdb[7] = Command->Lba_low;
	Cdb[8] = Command->Lba_mid;
	Cdb[9] = Command->Lba_high;
	Cdb[10] = Command->Device | 0xa0;
	Cdb[11] = Command->AtaCmd;

	return ScsiPassthroughDirect(hPhysical, Cdb, sizeof(Cdb), Direction, DataBuffer, BufLen, Timeout);
}

/* UNTESTED!!! */
/* See: http://kernel.opensuse.org/cgit/kernel/tree/drivers/usb/storage/cypress_atacb.c */
static int UsbCypressAtaPassthrough(HANDLE hPhysical, ATA_PASSTHROUGH_CMD* Command, void* DataBuffer, size_t BufLen, uint32_t Timeout)
{
	uint8_t Cdb[16] = {0};
	uint8_t Direction;

	Direction = GetAtaDirection(Command->AtaCmd, Command->Features);

	Cdb[0] = USB_CYPRESS_ATA_PASSTHROUGH;
	Cdb[1] = USB_CYPRESS_ATA_PASSTHROUGH;
	if (Command->AtaCmd == ATA_IDENTIFY_DEVICE || Command->AtaCmd == ATA_IDENTIFY_PACKET_DEVICE)
		Cdb[2] = (1<<7);				// Set IdentifyPacketDevice
	Cdb[3] = 0xff - (1<<0) - (1<<6);	// Features, sector count, lba low, lba med, lba high
	Cdb[4] = 1;							// Units in blocks rather than bytes

	Cdb[6] = Command->Features;
	Cdb[7] = (uint8_t)(BufLen >> SECTOR_SIZE_SHIFT_BIT);
	Cdb[8] = Command->Lba_low;
	Cdb[9] = Command->Lba_mid;
	Cdb[10] = Command->Lba_high;
	Cdb[11] = Command->Device;
	Cdb[12] = Command->AtaCmd;

	return ScsiPassthroughDirect(hPhysical, Cdb, sizeof(Cdb), Direction, DataBuffer, BufLen, Timeout);
}

/* The various bridges we will try, in order */
AtaPassThroughType pt[] = {
	{ SatAtaPassthrough, "SAT" },
	{ UsbJmicronAtaPassthrough, "JMicron" },
	{ UsbProlificAtaPassthrough, "Prolific" },
	{ UsbSunPlusAtaPassthrough, "SunPlus" },
	{ UsbCypressAtaPassthrough, "Cypress" },
};

BOOL Identify(HANDLE hPhysical)
{
	ATA_PASSTHROUGH_CMD Command = {0};
	IDENTIFY_DEVICE_DATA* idd;
	int i, r;

	Command.AtaCmd = ATA_IDENTIFY_DEVICE;

	// You'll get an error here if your compiler does not properly pack the IDENTIFY struct
	COMPILE_TIME_ASSERT(sizeof(IDENTIFY_DEVICE_DATA) == 512);

	idd = (IDENTIFY_DEVICE_DATA*)_aligned_malloc(sizeof(IDENTIFY_DEVICE_DATA), 0x10);
	if (idd == NULL)
		return FALSE;

	for (i=0; i<ARRAYSIZE(pt); i++) {
		r = pt[i].fn(hPhysical, &Command, idd, sizeof(IDENTIFY_DEVICE_DATA), SPT_TIMEOUT_VALUE);
		if (r == SPT_SUCCESS) {
			uprintf("Success using %s\n", pt[i].type);
			if (idd->CommandSetSupport.SmartCommands) {
				DumpBufferHex(idd, sizeof(IDENTIFY_DEVICE_DATA));
				uprintf("SMART support detected!\n");
			} else {
				uprintf("No SMART support\n");
			}
			break;
		}
		uprintf("No joy with: %s (%s)\n", pt[i].type, SptStrerr(r));
	}
	if (i >= ARRAYSIZE(pt))
		uprintf("NO ATA FOR YOU!\n");

	_aligned_free(idd);
	return TRUE;
}

/* Generic SMART access. Kept for reference, as it doesn't work for USB to ATA/SATA bridges */
#if 0
#pragma pack(1)
typedef struct  {
	UCHAR  bVersion;
	UCHAR  bRevision;
	UCHAR  bReserved;
	UCHAR  bIDEDeviceMap;
	ULONG  fCapabilities;
	ULONG  dwReserved[4];
} MY_GETVERSIONINPARAMS;
#pragma pack()

#ifndef SMART_GET_VERSION
#define SMART_GET_VERSION \
  CTL_CODE(IOCTL_DISK_BASE, 0x0020, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

BOOL SmartGetVersion(HANDLE hdevice)
{
	MY_GETVERSIONINPARAMS vers;
	DWORD size = sizeof(MY_GETVERSIONINPARAMS);
	BOOL r;

	memset(&vers, 0, sizeof(vers));

	r = DeviceIoControl(hdevice, SMART_GET_VERSION, NULL, 0, &vers, sizeof(vers), &size, NULL);
	if ( (!r) || (size != sizeof(MY_GETVERSIONINPARAMS)) ) {
		uprintf("SmartGetVersion failed: %s\n", r?"unexpected size":WindowsErrorString());
		return FALSE;
	}
	uprintf("Smart Version: %d.%d, Caps = 0x%x, DeviceMap = 0x%02x\n",
		vers.bVersion, vers.bRevision, (unsigned)vers.fCapabilities, vers.bIDEDeviceMap);

	return vers.bIDEDeviceMap;
}
#endif

/* 
 * TODO: SMART HDD vs UFD detection:
 * - if the USB ID starts with 
 * "WDC", "IBM", "ST" + number, "STM", "HTS", "HITACHI", "SEAGATE", "MAXTOR", "SAMSUNG", "HP ", "FUJITSU", "TOSHIBA", "QUANTUM"
 * - if IDENTIFY reports SMART capabilities
 * - if it has extra non hidden partitions that aren't Windows
 * - if the VID:PID (or VID) is of known USB to IDE/SATA bridge or known UFD maker
 * - removable flag (how do you actually find that one?)
 */

typedef struct {
	const char* name;
	const int score;
} str_score;

typedef struct {
	const uint16_t vid;
	const int score;
} vid_score;

// If a disk ID starts with these, we consider it likely to be an HDD
// The info from http://knowledge.seagate.com/articles/en_US/FAQ/204763en is a start, but not
// entirely accurate for our usage as some models will be prefixed with the manufacturer name
// '#' below means any number in [0-9]
static str_score manufacturer_str[] = {
	{ "HP ", 10 },
	{ "ST#", 10 },
	{ "MX#", 10 },
	{ "WDC", 10 },
	{ "IBM", 10 },
	{ "STM#", 10 },
	{ "HTS#", 10 },
	{ "MAXTOR", 10 },
	{ "HITACHI", 10 },
	{ "SEAGATE", 10 },
	{ "SAMSUNG", 10 },
	{ "FUJITSU", 10 },
	{ "TOSHIBA", 10 },
	{ "QUANTUM", 10 },
};

// http://www.linux-usb.org/usb.ids
static vid_score manufacturer_vid[] = {
	{ 0x04b4, 10 },	// Cypress
	{ 0x067b, 10 },	// Prolific
	{ 0x0bc2, 10 },	// Seagate
	{ 0x152d, 10 }, // JMicron
};

/*
 * This attempts to detect whether a drive is an USB HDD or an USB Flash Drive (UFD).
 * If someone already has an USB HDD plugged in (say as a backup drive) and plugs an
 * UFD we *try* to do what we can to avoid them formatting that drive by mistake.
 * But because there is no foolproof (let alone easy), way to differentiate UFDs from
 * HDDs, thanks to every manufacturer, Microsoft, and their mothers making it 
 * exceedingly troublesome to find out what type of hardware we are actually accessing
 * please pay heed to the following warning:
 *
 * WARNING: NO PROMISE IS MADE  ABOUT THIS ALGORITHM BEING ABLE TO CORRECTLY
 * DIFFERENTIATE AN USB HDD FROM A FLASH DRIVE. ALSO, REMEMBER THAT THE LICENSE OF THIS
 * APPLICATION MAKES ABSOLUETLY NO PROMISE ABOUT DATA PRESERVATION (PROVIDED "AS IS").
 * THUS, IF DATA LOSS IS INCURRED DUE TO THE ALGORITHM BELOW, OR ANY OTHER PART OF THIS
 * APPLICATION, THE RESPONSIBILITY IS ENTIRELY ON YOU!
 *
 * But let me just elaborate further on why differentiating UFDs from HDDs is not as
 * 'simple' as it seems:
 * - many USB flash drives manufacturer will present UFDs as non-removable, which used
 *   to be reserved for HDDs => we can't use that as differentiator.
 * - some UFDs (SanDisk Extreme) have added S.M.A.R.T. support, which also used to be
 *   reserved for HDDs => can't use that either
 * - even if S.M.A.R.T. was enough, not all USB->IDE or USB->SATA bridges support ATA
 *   passthrough, which is required S.M.A.R.T. data, and each manufacturer of an 
 *   USB<->(S)ATA bridge seem to have their own method of implementing passthrough.
 * - SSDs have also changed the deal completely, as you can get something that looks
 *   like Flash but that is really an HDD.
 * - Some manufacturers (eg. ALI) provide both USB Flash controllers and USB IDE/SATA
 *   controllers, so we can't exactly use the VID to say for sure what we're looking at.
 * - Finally, Microsoft is abdsolutely no help either (which is kind of understandable
 *   from the above) => there is no magic API we can query that will tell us what we're
 *   really looking at.
 *
 * What you have below, then, is our *current best guess* at differentiating an UFD from
 * an HDD. Short of a crystal ball however, this remains just a guess, which may be way
 * off mark. Still, Rufus does produce PROMINENT warnings before you format a drive, and
 * also provides extensive info about the drive (from the toolips and the log) => PAY
 * ATTENTION TO THESE OR PAY THE PRICE!
 */
int IsHDD(UINT drive_type, uint16_t vid, uint16_t pid, const char* strid)
{
	int score = 0;
	size_t i, mlen, ilen;
	BOOL wc;

	if (drive_type == DRIVE_FIXED)
		score += 3;

	ilen = safe_strlen(strid);
	for (i=0; i<ARRAYSIZE(manufacturer_str); i++) {
		mlen = strlen(manufacturer_str[i].name);
		if (mlen > ilen)
			break;
		wc = (manufacturer_str[i].name[mlen-1] == '#');
		if ( (_strnicmp(strid, manufacturer_str[i].name, mlen-((wc)?1:0)) == 0)
		  && ((!wc) || ((strid[mlen] >= '0') && (strid[mlen] <= '9'))) ) {
			score += manufacturer_str[i].score;
			break;
		}
	}

	for (i=0; i<ARRAYSIZE(manufacturer_vid); i++) {
		if (vid == manufacturer_vid[i].vid) {
			score += manufacturer_vid[i].score;
			break;
		}
	}

	// TODO: try to perform inquiry if uncertain
	// TODO: lower the score for well known UFD manufacturers (ADATA, SanDisk, etc.)
	return score;
}

