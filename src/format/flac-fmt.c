/** fmedia: .flac reader
Copyright (c) 2018 Simon Zolin */

#include <fmedia.h>

#include <avpack/flac-read.h>


#undef dbglog
#undef warnlog
#undef errlog
#undef syserrlog
#define dbglog(trk, ...)  fmed_dbglog(core, trk, NULL, __VA_ARGS__)
#define warnlog(trk, ...)  fmed_warnlog(core, trk, NULL, __VA_ARGS__)
#define errlog(trk, ...)  fmed_errlog(core, trk, NULL, __VA_ARGS__)
#define syserrlog(trk, ...)  fmed_syserrlog(core, trk, NULL, __VA_ARGS__)


extern const fmed_core *core;
extern const fmed_queue *qu;

#include <format/flac-write.h>

struct flac {
	flacread fl;
	ffstr in;
	void *trk;
	uint sample_rate;
};

void flac_in_log(void *udata, const char *fmt, va_list va)
{
	struct flac *f = udata;
	fmed_dbglogv(core, f->trk, NULL, fmt, va);
}

static void* flac_in_create(fmed_filt *d)
{
	struct flac *f = ffmem_new(struct flac);
	if (f == NULL)
		return NULL;
	f->trk = d->trk;

	ffuint64 size = 0;
	if ((int64)d->input.size != FMED_NULL)
		size = d->input.size;

	flacread_open(&f->fl, size);
	f->fl.log = flac_in_log;
	f->fl.udata = f;
	return f;
}

static void flac_in_free(void *ctx)
{
	struct flac *f = ctx;
	flacread_close(&f->fl);
	ffmem_free(f);
}

static void flac_meta(struct flac *f, fmed_filt *d)
{
	ffstr name, val;
	int tag = flacread_tag(&f->fl, &name, &val);
	dbglog(d->trk, "%S: %S", &name, &val);

	if (tag == MMTAG_PICTURE)
		return;
	d->track->meta_set(d->trk, &name, &val, FMED_QUE_TMETA);
}

static int flac_in_read(void *ctx, fmed_filt *d)
{
	struct flac *f = ctx;
	int r;
	ffstr out = {};

	if (d->flags & FMED_FSTOP) {
		d->outlen = 0;
		return FMED_RLASTOUT;
	}

	if (d->flags & FMED_FFWD) {
		f->in = d->data_in;
		d->data_in.len = 0;
		if (d->flags & FMED_FLAST)
			flacread_finish(&f->fl);
	}

	for (;;) {

		if (d->seek_req && f->sample_rate != 0) {
			d->seek_req = 0;
			flacread_seek(&f->fl, ffpcm_samples(d->audio.seek, f->sample_rate));
			dbglog(d->trk, "seek: %Ums", d->audio.seek);
		}

		r = flacread_process(&f->fl, &f->in, &out);
		switch (r) {
		case FLACREAD_MORE:
			if (d->flags & FMED_FLAST) {
				warnlog(d->trk, "file is incomplete");
				d->outlen = 0;
				return FMED_RDONE;
			}
			return FMED_RMORE;

		case FLACREAD_HEADER: {
			const struct flac_info *i = flacread_info(&f->fl);
			d->audio.decoder = "FLAC";
			d->audio.fmt.format = i->bits;
			d->audio.fmt.channels = i->channels;
			d->audio.fmt.sample_rate = i->sample_rate;
			d->audio.fmt.ileaved = 0;
			d->datatype = "flac";
			d->audio.total = i->total_samples;
			f->sample_rate = i->sample_rate;
			break;
		}

		case FLACREAD_TAG:
			flac_meta(f, d);
			break;

		case FLACREAD_HEADER_FIN: {
			const struct flac_info *i = flacread_info(&f->fl);
			dbglog(d->trk, "blocksize:%u..%u  framesize:%u..%u  MD5:%16xb  seek-table:%u  meta-length:%u  total-samples:%,U"
				, (int)i->minblock, (int)i->maxblock, (int)i->minframe, (int)i->maxframe
				, i->md5, (int)f->fl.sktab.len, (int)f->fl.frame1_off, i->total_samples);
			d->audio.bitrate = i->bitrate;

			if (d->input_info)
				return FMED_RDONE;

			d->flac_minblock = i->minblock;
			d->flac_maxblock = i->maxblock;

			if (0 != d->track->cmd2(d->trk, FMED_TRACK_ADDFILT, "flac.decode"))
				return FMED_RERR;
			break;
		}

		case FLACREAD_DATA:
			goto data;

		case FLACREAD_SEEK:
			d->input.seek = flacread_offset(&f->fl);
			return FMED_RMORE;

		case FLACREAD_DONE:
			d->outlen = 0;
			return FMED_RDONE;

		case FLACREAD_ERROR:
			errlog(d->trk, "flacread_decode(): at offset 0x%xU: %s"
				, flacread_offset(&f->fl), flacread_error(&f->fl));
			return FMED_RERR;
		}
	}

data:
	dbglog(d->trk, "frame samples:%u pos:%U"
		, flacread_samples(&f->fl), flacread_cursample(&f->fl));
	d->audio.pos = flacread_cursample(&f->fl);

	d->flac_samples = flacread_samples(&f->fl);
	d->data_out = out;
	return FMED_RDATA;
}

const fmed_filter fmed_flac_input = {
	flac_in_create, flac_in_read, flac_in_free
};
