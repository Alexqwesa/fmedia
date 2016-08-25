/**
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>

#include <FF/audio/pcm.h>
#include <FF/array.h>
#include <FF/rbtree.h>
#include <FF/list.h>
#include <FF/taskqueue.h>
#include <FFOS/asyncio.h>
#include <FFOS/file.h>


typedef struct inmap_item {
	const fmed_modinfo *mod;
	char ext[0];
} inmap_item;

typedef struct fmedia {
	fftaskmgr taskmgr;
	ffkevent evposted;

	uint srcid;
	fflist srcs; //fm_src[]

	fffd kq;
	const ffkqu_time *pkqutime;
	ffkqu_time kqutime;

	unsigned recording :1
		, stopped :1
		;

	fflist mods; //core_mod[]

	//conf:
	const fmed_log *log;
	ffstr root;
	struct { FFARR(char*) } in_files;
	ffstr outfn
		, outdir;
	uint playdev_name
		, captdev_name;
	uint seek_time
		, until_time;
	uint64 fseek;
	const fmed_modinfo *output;

	const fmed_modinfo *input;
	ffpcm inp_pcm;

	struct {
	uint out_format;
	uint out_rate;
	byte out_channels;
	};

	char *usrconf_modname;

	ffbool repeat_all
		, overwrite
		, stream_copy
		, out_copy
		, rec
		, debug
		, mix
		, tags
		, silent
		, gui
		, info;
	byte volume;
	byte codepage;
	byte instance_mode;
	char *trackno;
	ffstr3 inmap; //inmap_item[]
	ffstr3 outmap; //inmap_item[]
	const fmed_modinfo *inmap_curmod;

	ffstr meta;
	float ogg_qual;
	float gain;
	uint aac_qual;
	byte flac_complevel;
	byte cue_gaps;
	byte pcm_crc;
	byte pcm_peaks;
	byte preserve_date;
	ushort mpeg_qual;
} fmedia;
