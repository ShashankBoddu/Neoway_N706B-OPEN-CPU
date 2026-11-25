/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT file system module  R0.12c                             /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2017, ChaN, all right reserved.
/ Portions Copyright (C) STMicroelectronics, all right reserved.
/
/ FatFs module is an open source software. Redistribution and use of FatFs in
/ source and binary forms, with or without modification, are permitted provided
/ that the following condition is met:

/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holder and contributors "AS IS"
/ and any warranties related to this software are DISCLAIMED.
/ The copyright owner or contributors be NOT LIABLE for any damages caused
/ by use of this software.
/----------------------------------------------------------------------------*/

#define _FFCONF 68300	/* Revision ID */

#define _FS_READONLY	0
#define _FS_MINIMIZE	0
#define	_USE_STRFUNC	0
#define _USE_FIND		0
#define _USE_MKFS		0
#define	_USE_FASTSEEK	0
#define	_USE_EXPAND		0
#define _USE_CHMOD		0
#define _USE_LABEL		0
#define	_USE_FORWARD	0

#define _CODE_PAGE	437
#define	_USE_LFN	2
#define	_MAX_LFN	255
#define	_LFN_UNICODE	0
#define _STRF_ENCODE	3
#define _FS_RPATH	0

#define _VOLUMES	1
#define _STR_VOLUME_ID	0
#define _VOLUME_STRS	"SD"
#define	_MULTI_PARTITION	0
#define	_MIN_SS		512
#define	_MAX_SS		512
#define	_USE_TRIM	0
#define _FS_NOFSINFO	0

#define	_FS_TINY	0
#define _FS_EXFAT	0
#define _FS_NORTC	0
#define _NORTC_MON	11
#define _NORTC_MDAY	17
#define _NORTC_YEAR	2026
#define	_FS_LOCK	0
#define _FS_REENTRANT	0
#define _USE_MUTEX	0
