/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT file system module  R0.12c                             /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2017, ChaN, all right reserved.
/----------------------------------------------------------------------------*/

#ifndef _FATFS
#define _FATFS	68300	/* Revision ID */

#ifdef __cplusplus
extern "C" {
#endif

#include "integer.h"	/* Basic integer types */
#include "ffconf.h"		/* FatFs configuration options */

#if _FATFS != _FFCONF
#error Wrong configuration file (ffconf.h).
#endif

#if _MULTI_PARTITION
typedef struct {
	BYTE pd;	/* Physical drive number */
	BYTE pt;	/* Partition: 0:Auto detect, 1-4:Forced partition) */
} PARTITION;
extern PARTITION VolToPart[];
#endif

#if _LFN_UNICODE
#if _USE_LFN == 0
#error _LFN_UNICODE must be 0 at non-LFN cfg.
#endif
#ifndef _INC_TCHAR
typedef WCHAR TCHAR;
#define _T(x) L ## x
#define _TEXT(x) L ## x
#endif
#else
#ifndef _INC_TCHAR
typedef char TCHAR;
#define _T(x) x
#define _TEXT(x) x
#endif
#endif

#if _FS_EXFAT
#if _USE_LFN == 0
#error LFN must be enabled when enable exFAT
#endif
typedef QWORD FSIZE_t;
#else
typedef DWORD FSIZE_t;
#endif

typedef struct {
	BYTE	fs_type;		/* File system type (0:N/A) */
	BYTE	drv;			/* Physical drive number */
	BYTE	n_fats;			/* Number of FATs (1 or 2) */
	BYTE	wflag;			/* win[] flag (b0:dirty) */
	BYTE	fsi_flag;		/* FSINFO flags (b7:disabled, b0:dirty) */
	WORD	id;				/* File system mount ID */
	WORD	n_rootdir;		/* Number of root directory entries (FAT12/16) */
	WORD	csize;			/* Cluster size [sectors] */
#if _MAX_SS != _MIN_SS
	WORD	ssize;			/* Sector size (512, 1024, 2048 or 4096) */
#endif
#if _USE_LFN != 0
	WCHAR*	lfnbuf;			/* LFN working buffer */
#endif
#if _FS_EXFAT
	BYTE*	dirbuf;			/* Directory entry block scratchpad buffer */
#endif
#if _FS_REENTRANT
	_SYNC_t	sobj;			/* Identifier of sync object */
#endif
#if !_FS_READONLY
	DWORD	last_clst;		/* Last allocated cluster */
	DWORD	free_clst;		/* Number of free clusters */
#endif
#if _FS_RPATH != 0
	DWORD	cdir;			/* Current directory start cluster (0:root) */
#if _FS_EXFAT
	DWORD	cdc_scl;		/* Containing directory start cluster (invalid when cdir is 0) */
	DWORD	cdc_size;		/* b31-b8:Size of containing directory, b7-b0: Chain status */
	DWORD	cdc_ofs;		/* Offset in the containing directory (invalid when cdir is 0) */
#endif
#endif
	DWORD	n_fatent;		/* Number of FAT entries (number of clusters + 2) */
	DWORD	fsize;			/* Size of an FAT [sectors] */
	DWORD	volbase;		/* Volume base sector */
	DWORD	fatbase;		/* FAT base sector */
	DWORD	dirbase;		/* Root directory base sector/cluster */
	DWORD	database;		/* Data base sector */
	DWORD	winsect;		/* Current sector appearing in the win[] */
	BYTE	win[_MAX_SS];	/* Disk access window for Directory, FAT (and file data at tiny cfg) */
} FATFS;

typedef struct {
	FATFS*	fs;
	WORD	id;
	BYTE	attr;
	BYTE	stat;
	DWORD	sclust;
	FSIZE_t	objsize;
#if _FS_EXFAT
	DWORD	n_cont;
	DWORD	n_frag;
	DWORD	c_scl;
	DWORD	c_size;
	DWORD	c_ofs;
#endif
#if _FS_LOCK != 0
	UINT	lockid;
#endif
} _FDID;

typedef struct {
	_FDID	obj;
	BYTE	flag;
	BYTE	err;
	FSIZE_t	fptr;
	DWORD	clust;
	DWORD	sect;
#if !_FS_READONLY
	DWORD	dir_sect;
	BYTE*	dir_ptr;
#endif
#if _USE_FASTSEEK
	DWORD*	cltbl;
#endif
#if !_FS_TINY
	BYTE	buf[_MAX_SS];
#endif
} FIL;

typedef struct {
	_FDID	obj;
	DWORD	dptr;
	DWORD	clust;
	DWORD	sect;
	BYTE*	dir;
	BYTE	fn[12];
#if _USE_LFN != 0
	DWORD	blk_ofs;
#endif
#if _USE_FIND
	const TCHAR* pat;
#endif
} DIR;

typedef struct {
	FSIZE_t	fsize;
	WORD	fdate;
	WORD	ftime;
	BYTE	fattrib;
#if _USE_LFN != 0
	TCHAR	altname[13];
	TCHAR	fname[_MAX_LFN + 1];
#else
	TCHAR	fname[13];
#endif
} FILINFO;

typedef enum {
	FR_OK = 0,				/* (0) Succeeded */
	FR_DISK_ERR,			/* (1) A hard error occurred in the low level disk I/O layer */
	FR_INT_ERR,				/* (2) Assertion failed */
	FR_NOT_READY,			/* (3) The physical drive cannot work */
	FR_NO_FILE,				/* (4) Could not find the file */
	FR_NO_PATH,				/* (5) Could not find the path */
	FR_INVALID_NAME,		/* (6) The path name format is invalid */
	FR_DENIED,				/* (7) Access denied due to prohibited access or directory full */
	FR_EXIST,				/* (8) Access denied due to prohibited access */
	FR_INVALID_OBJECT,		/* (9) The file/directory object is invalid */
	FR_WRITE_PROTECTED,		/* (10) The physical drive is write protected */
	FR_INVALID_DRIVE,		/* (11) The logical drive number is invalid */
	FR_NOT_ENABLED,			/* (12) The volume has no work area */
	FR_NO_FILESYSTEM,		/* (13) There is no valid FAT volume */
	FR_MKFS_ABORTED,		/* (14) The f_mkfs() aborted due to any problem */
	FR_TIMEOUT,				/* (15) Could not get a grant to access the volume within defined period */
	FR_LOCKED,				/* (16) The operation is rejected according to the file sharing policy */
	FR_NOT_ENOUGH_CORE,		/* (17) LFN working buffer could not be allocated */
	FR_TOO_MANY_OPEN_FILES,	/* (18) Number of open files > _FS_LOCK */
	FR_INVALID_PARAMETER	/* (19) Given parameter is invalid */
} FRESULT;

FRESULT f_open (FIL* fp, const TCHAR* path, BYTE mode);
FRESULT f_close (FIL* fp);
FRESULT f_read (FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write (FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek (FIL* fp, FSIZE_t ofs);
FRESULT f_truncate (FIL* fp);
FRESULT f_sync (FIL* fp);
FRESULT f_opendir (DIR* dp, const TCHAR* path);
FRESULT f_closedir (DIR* dp);
FRESULT f_readdir (DIR* dp, FILINFO* fno);
FRESULT f_findfirst (DIR* dp, FILINFO* fno, const TCHAR* path, const TCHAR* pattern);
FRESULT f_findnext (DIR* dp, FILINFO* fno);
FRESULT f_mkdir (const TCHAR* path);
FRESULT f_unlink (const TCHAR* path);
FRESULT f_rename (const TCHAR* path_old, const TCHAR* path_new);
FRESULT f_stat (const TCHAR* path, FILINFO* fno);
FRESULT f_chmod (const TCHAR* path, BYTE attr, BYTE mask);
FRESULT f_utime (const TCHAR* path, const FILINFO* fno);
FRESULT f_chdir (const TCHAR* path);
FRESULT f_chdrive (const TCHAR* path);
FRESULT f_getcwd (TCHAR* buff, UINT len);
FRESULT f_getfree (const TCHAR* path, DWORD* nclst, FATFS** fatfs);
FRESULT f_getlabel (const TCHAR* path, TCHAR* label, DWORD* vsn);
FRESULT f_setlabel (const TCHAR* label);
FRESULT f_forward (FIL* fp, UINT(*func)(const BYTE*,UINT), UINT btf, UINT* bf);
FRESULT f_expand (FIL* fp, FSIZE_t szf, BYTE opt);
FRESULT f_mount (FATFS* fs, const TCHAR* path, BYTE opt);
FRESULT f_mkfs (const TCHAR* path, BYTE opt, DWORD au, void* work, UINT len);
FRESULT f_fdisk (BYTE pdrv, const DWORD* szt, void* work);
int f_putc (TCHAR c, FIL* fp);
int f_puts (const TCHAR* str, FIL* cp);
int f_printf (FIL* fp, const TCHAR* str, ...);
TCHAR* f_gets (TCHAR* buff, int len, FIL* fp);

#define f_eof(fp) ((int)((fp)->fptr == (fp)->obj.objsize))
#define f_error(fp) ((fp)->err)
#define f_tell(fp) ((fp)->fptr)
#define f_size(fp) ((fp)->obj.objsize)
#define f_rewind(fp) f_lseek((fp), 0)
#define f_rewinddir(dp) f_readdir((dp), 0)
#define f_rmdir(path) f_unlink(path)

#ifndef EOF
#define EOF (-1)
#endif

#if !_FS_READONLY && !_FS_NORTC
DWORD get_fattime (void);
#endif

#if _USE_LFN != 0
WCHAR ff_convert (WCHAR chr, UINT dir);
WCHAR ff_wtoupper (WCHAR chr);
#if _USE_LFN == 3
void* ff_memalloc (UINT msize);
void ff_memfree (void* mblock);
#endif
#endif

#if _FS_REENTRANT
int ff_cre_syncobj (BYTE vol, _SYNC_t* sobj);
int ff_req_grant (_SYNC_t sobj);
void ff_rel_grant (_SYNC_t sobj);
int ff_del_syncobj (_SYNC_t sobj);
#endif

#define	FA_READ				0x01
#define	FA_WRITE			0x02
#define	FA_OPEN_EXISTING	0x00
#define	FA_CREATE_NEW		0x04
#define	FA_CREATE_ALWAYS	0x08
#define	FA_OPEN_ALWAYS		0x10
#define	FA_OPEN_APPEND		0x30

#define CREATE_LINKMAP	((FSIZE_t)0 - 1)

#define FM_FAT		0x01
#define FM_FAT32	0x02
#define FM_EXFAT	0x04
#define FM_ANY		0x07
#define FM_SFD		0x08

#define FS_FAT12	1
#define FS_FAT16	2
#define FS_FAT32	3
#define FS_EXFAT	4

#define	AM_RDO	0x01
#define	AM_HID	0x02
#define	AM_SYS	0x04
#define AM_DIR	0x10
#define AM_ARC	0x20

#ifdef __cplusplus
}
#endif

#endif /* _FATFS */
