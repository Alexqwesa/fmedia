/** MPEG Layer3 (.mp3) reader/writer.
Copyright (c) 2017 Simon Zolin */

#include <fmedia.h>

#include <avpack/mp3-read.h>
#include <format/mmtag.h>
#include <util/array.h>

#define errlog1(trk, ...)  fmed_errlog(core, trk, NULL, __VA_ARGS__)
#define warnlog1(trk, ...)  fmed_warnlog(core, trk, NULL, __VA_ARGS__)
#define dbglog1(trk, ...)  fmed_dbglog(core, trk, NULL, __VA_ARGS__)

extern const fmed_core *core;
extern const fmed_queue *qu;

#include <format/mp3-write.h>
#include <format/mp3-copy.h>

typedef struct mp3_in {
	mp3read mpg;
	ffstr in;
	uint sample_rate;
	uint nframe;
	char codec_name[9];
	uint have_id32tag :1
		;
} mp3_in;

void* mp3_open(fmed_filt *d)
{
	if (d->stream_copy && !d->track->cmd(d->trk, FMED_TRACK_META_HAVEUSER)) {

		if (0 != d->track->cmd2(d->trk, FMED_TRACK_ADDFILT, "fmt.mp3-copy"))
			return NULL;
		return FMED_FILT_SKIP;
	}

	mp3_in *m = ffmem_new(mp3_in);
	ffuint64 total_size = 0;
	if ((int64)d->input.size != FMED_NULL) {
		total_size = d->input.size;
	}
	mp3read_open(&m->mpg, total_size);
	m->mpg.id3v2.codepage = core->getval("codepage");
	return m;
}

void mp3_close(void *ctx)
{
	mp3_in *m = ctx;
	mp3read_close(&m->mpg);
	ffmem_free(m);
}

void mp3_meta(mp3_in *m, fmed_filt *d, uint type)
{
	if (type == MP3READ_ID32) {
		if (!m->have_id32tag) {
			m->have_id32tag = 1;
			dbglog1(d->trk, "ID3v2.%u  size:%u"
				, id3v2read_version(&m->mpg.id3v2), id3v2read_size(&m->mpg.id3v2));
		}
	}

	ffstr name, val;
	int tag = mp3read_tag(&m->mpg, &name, &val);
	if (tag != 0)
		ffstr_setz(&name, ffmmtag_str[tag]);

	dbglog1(d->trk, "tag: %S: %S", &name, &val);
	d->track->meta_set(d->trk, &name, &val, FMED_QUE_TMETA);
}

int mp3_process(void *ctx, fmed_filt *d)
{
	mp3_in *m = ctx;
	int r;

	if (d->flags & FMED_FSTOP) {
		d->outlen = 0;
		return FMED_RLASTOUT;
	}

	if (d->datalen != 0) {
		m->in = d->data_in;
		d->datalen = 0;
	}

	ffstr out;
	for (;;) {

		if (d->seek_req && (int64)d->audio.seek != FMED_NULL && m->sample_rate != 0) {
			d->seek_req = 0;
			mp3read_seek(&m->mpg, ffpcm_samples(d->audio.seek, m->sample_rate));
			dbglog1(d->trk, "seek: %Ums", d->audio.seek);
		}

		r = mp3read_process(&m->mpg, &m->in, &out);

		switch (r) {
		case MPEG1READ_DATA:
			goto data;

		case MPEG1READ_MORE:
			if (d->flags & FMED_FLAST) {
				d->outlen = 0;
				return FMED_RDONE;
			}
			return FMED_RMORE;

		case MP3READ_DONE:
			d->outlen = 0;
			return FMED_RLASTOUT;

		case MPEG1READ_HEADER: {
			const struct mpeg1read_info *info = mp3read_info(&m->mpg);
			d->audio.fmt.format = FFPCM_16;
			m->sample_rate = info->sample_rate;
			d->audio.fmt.sample_rate = info->sample_rate;
			d->audio.fmt.channels = info->channels;
			d->audio.bitrate = info->bitrate;
			d->audio.total = info->total_samples;
			ffs_format(m->codec_name, sizeof(m->codec_name), "MPEG1-L%u%Z", info->layer);
			d->audio.decoder = m->codec_name;
			d->datatype = "mpeg";
			d->mpeg1_delay = info->delay;
			d->mpeg1_padding = info->padding;
			fmed_setval("mpeg.vbr_scale", info->vbr_scale);

			if (d->input_info)
				return FMED_RDONE;

			if (!d->stream_copy
				&& 0 != d->track->cmd2(d->trk, FMED_TRACK_ADDFILT, "mpeg.decode"))
				return FMED_RERR;

			break;
		}

		case MP3READ_ID31:
		case MP3READ_ID32:
		case MP3READ_APETAG:
			mp3_meta(m, d, r);
			break;

		case MPEG1READ_SEEK:
			d->input.seek = mp3read_offset(&m->mpg);
			return FMED_RMORE;

		case MP3READ_WARN:
			warnlog1(d->trk, "mp3read_read(): %s. Near sample %U, offset %U"
				, mp3read_error(&m->mpg), mp3read_cursample(&m->mpg), mp3read_offset(&m->mpg));
			break;

		case MPEG1READ_ERROR:
		default:
			errlog1(d->trk, "mp3read_read(): %s. Near sample %U, offset %U"
				, mp3read_error(&m->mpg), mp3read_cursample(&m->mpg), mp3read_offset(&m->mpg));
			return FMED_RERR;
		}
	}

data:
	d->audio.pos = mp3read_cursample(&m->mpg);
	dbglog1(d->trk, "passing frame #%u  samples:%u[%U]  size:%u  br:%u  off:%xU"
		, ++m->nframe, mpeg1_samples(out.ptr), d->audio.pos, (uint)out.len
		, mpeg1_bitrate(out.ptr), (ffint64)mp3read_offset(&m->mpg) - out.len);
	d->data_out = out;
	return FMED_RDATA;
}

const fmed_filter mp3_input = {
	mp3_open, mp3_process, mp3_close
};
