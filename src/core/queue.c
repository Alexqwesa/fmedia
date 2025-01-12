/** Track queue.
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>
#include <FFOS/dir.h>
#include <FFOS/random.h>
#include <avpack/m3u.h>


#undef syserrlog
#define infolog(...)  fmed_infolog(core, NULL, "queue", __VA_ARGS__)
#define dbglog0(...)  fmed_dbglog(core, NULL, "queue", __VA_ARGS__)
#define syserrlog(...)  fmed_syserrlog(core, NULL, "queue", __VA_ARGS__)

#define MAX_N_ERRORS  15  // max. number of consecutive errors

/*
Metadata priority:
  . from user (--meta)
    if "--meta=clear" is used, skip transient meta
  . from .cue
  . from file or ICY server (transient)
  . artist/title from .m3u (used as transient due to lower priority)
*/

static const fmed_core *core;

struct entry;
typedef struct entry entry;
typedef struct plist plist;

struct plist {
	fflist_item sib;
	fflist ents; //entry[]
	ffarr indexes; //entry*[]  Get an entry by its number;  find a number by an entry pointer.
	entry *cur, *xcursor;
	struct plist *filtered_plist; //list with the filtered tracks
	uint nerrors; // number of consecutive errors
	uint rm :1;
	uint allow_random :1;
	uint filtered :1;
	uint parallel :1; // every item in this queue will start via FMED_TRACK_XSTART
	uint expand_all :1; // read meta data of all items
};

static void plist_free(plist *pl);
static ssize_t plist_ent_idx(plist *pl, entry *e);
static struct entry* plist_ent(struct plist *pl, size_t idx);

struct que_conf {
	byte next_if_err;
};

typedef struct que {
	fflist plists; //plist[]
	plist *curlist;
	const fmed_track *track;
	fmed_que_onchange_t onchange;
	fflock plist_lock;

	struct que_conf conf;
	uint list_random;
	uint repeat;
	uint quit_if_done :1
		, next_if_err :1
		, fmeta_lowprio :1 //meta from file has lower priority
		, rnd_ready :1
		, random :1
		, mixing :1;
} que;

static que *qu;


//FMEDIA MODULE
static const void* que_iface(const char *name);
static int que_mod_conf(const char *name, fmed_conf_ctx *ctx);
static int que_sig(uint signo);
static void que_destroy(void);
static const fmed_mod fmed_que_mod = {
	.ver = FMED_VER_FULL, .ver_core = FMED_VER_CORE,
	&que_iface, &que_sig, &que_destroy, &que_mod_conf
};

static ssize_t que_cmdv(uint cmd, ...);
static ssize_t que_cmd2(uint cmd, void *param, size_t param2);
static fmed_que_entry* _que_add(fmed_que_entry *ent);
static void que_cmd(uint cmd, void *param);
static void _que_meta_set(fmed_que_entry *ent, const char *name, size_t name_len, const char *val, size_t val_len, uint flags);
void que_meta_set2(fmed_que_entry *ent, ffstr name, ffstr val, uint flags);
static ffstr* que_meta_find(fmed_que_entry *ent, const char *name, size_t name_len);
static ffstr* que_meta(fmed_que_entry *ent, size_t n, ffstr *name, uint flags);
static const fmed_queue fmed_que_mgr = {
	&que_cmdv, &que_cmd, &que_cmd2, &_que_add, &_que_meta_set, &que_meta_find, &que_meta, que_meta_set2
};

static fmed_que_entry* que_add(plist *pl, fmed_que_entry *ent, entry *prev, uint flags);
static void que_play(entry *e);
static void que_play2(entry *ent, uint flags);
static void que_save(entry *first, const fflist_item *sentl, const char *fn);
static void ent_start_prepare(entry *e, void *trk);
static void rnd_init();
static void plist_remove_entry(entry *e, ffbool from_index, ffbool remove);
static entry* que_getnext(entry *from);
static void pl_expand_next(plist *pl, entry *e);

#include <core/queue-entry.h>
#include <core/queue-track.h>

static const fmed_conf_arg que_conf_args[] = {
	{ "next_if_error",	FMC_BOOL8,  FMC_O(struct que_conf, next_if_err) },
	{}
};
static int que_config(fmed_conf_ctx *ctx)
{
	qu->conf.next_if_err = 1;
	fmed_conf_addctx(ctx, &qu->conf, que_conf_args);
	return 0;
}


const fmed_mod* fmed_getmod_queue(const fmed_core *_core)
{
	core = _core;
	return &fmed_que_mod;
}


static const void* que_iface(const char *name)
{
	if (!ffsz_cmp(name, "track"))
		return &fmed_que_trk;
	else if (!ffsz_cmp(name, "queue"))
		return (void*)&fmed_que_mgr;
	return NULL;
}

static int que_mod_conf(const char *name, fmed_conf_ctx *ctx)
{
	if (!ffsz_cmp(name, "track"))
		return que_config(ctx);
	return -1;
}

static int que_sig(uint signo)
{
	switch (signo) {
	case FMED_SIG_INIT:
		if (NULL == (qu = ffmem_tcalloc1(que)))
			return 1;
		fflist_init(&qu->plists);
		fflk_init(&qu->plist_lock);
		break;

	case FMED_OPEN:
		que_cmd2(FMED_QUE_NEW, NULL, 0);
		que_cmd2(FMED_QUE_SEL, (void*)0, 0);
		qu->track = core->getmod("#core.track");
		qu->next_if_err = qu->conf.next_if_err;
		break;
	}
	return 0;
}

/** Prepare the item before starting a track. */
static void ent_start_prepare(entry *e, void *trk)
{
	FFSLICE_FOREACH_T(&e->tmeta, ffstr_free, ffstr);
	ffslice_free(&e->tmeta);
	qu->track->setval(trk, "queue_item", (int64)e);
	ent_ref(e);
}

static void plist_remove_entry(entry *e, ffbool from_index, ffbool remove)
{
	plist *pl = e->plist;

	if (from_index) {
		ssize_t i = plist_ent_idx(pl, e);
		if (i >= 0)
			ffslice_rmT((ffslice*)&pl->indexes, i, 1, entry*);

		if (pl->filtered_plist != NULL) {
			i = plist_ent_idx(pl->filtered_plist, e);
			if (i >= 0)
				ffslice_rmT((ffslice*)&pl->filtered_plist->indexes, i, 1, entry*);
		}
	}

	if (!remove)
		return;

	if (pl->cur == e)
		pl->cur = NULL;
	if (pl->xcursor == e)
		pl->xcursor = NULL;
	fflist_rm(&pl->ents, &e->sib);
	if (pl->ents.len == 0 && pl->rm)
		plist_free(pl);
}

static void plist_free(plist *pl)
{
	if (pl == NULL)
		return;
	FFLIST_ENUMSAFE(&pl->ents, ent_free, entry, sib);
	ffarr_free(&pl->indexes);
	plist_free(pl->filtered_plist);
	ffmem_free(pl);
}

/** Find a number by an entry pointer. */
static ssize_t plist_ent_idx(plist *pl, entry *e)
{
	int update = 0;
	if (!pl->filtered) {
		entry *e2 = plist_ent(pl, e->list_pos);
		if (e == e2)
			return e->list_pos;
		update = 1;
	}

	entry **p;
	FFARR_WALKT(&pl->indexes, p, entry*) {
		if (*p == e) {
			if (update)
				e->list_pos = p - (entry**)pl->indexes.ptr;
			return p - (entry**)pl->indexes.ptr;
		}
	}
	return -1;
}

/** Get an entry pointer by its index. */
static struct entry* plist_ent(struct plist *pl, size_t idx)
{
	if (idx >= pl->indexes.len)
		return NULL;
	struct entry *e = ((entry**)pl->indexes.ptr) [idx];
	return e;
}


static void que_destroy(void)
{
	if (qu == NULL)
		return;
	FFLIST_ENUMSAFE(&qu->plists, plist_free, plist, sib);
	ffmem_free0(qu);
}


/** Save playlist file. */
static void que_save(entry *first, const fflist_item *sentl, const char *fn)
{
	fffd f = FF_BADFD;
	m3uwrite m3 = {};
	int rc = -1;
	entry *e;
	ffvec v = {};
	char *tmp_fname = NULL;

	m3uwrite_create(&m3, 0);

	for (e = first;  &e->sib != sentl;  e = FF_GETPTR(entry, sib, e->sib.next)) {
		int r = 0;
		ffstr *s;
		m3uwrite_entry me = {};

		int dur = (e->e.dur != 0) ? e->e.dur / 1000 : -1;
		me.duration_sec = dur;

		if (NULL != (s = que_meta_find(&e->e, FFSTR("artist"))))
			me.artist = *s;

		if (NULL != (s = que_meta_find(&e->e, FFSTR("title"))))
			me.title = *s;

		me.url = e->e.url;
		r |= m3uwrite_process(&m3, &me);

		if (r != 0)
			goto done;
	}

	ffstr out = m3uwrite_fin(&m3);

	fffile_readwhole(fn, &v, 10*1024*1024);
	if (ffstr_eq2(&out, &v)) {
		rc = 0;
		goto done; // the playlist hasn't changed
	}

	if (NULL == (tmp_fname = ffsz_allocfmt("%s.tmp", fn)))
		goto done;

	if (FF_BADFD == (f = fffile_open(tmp_fname, FFO_CREATE | FFO_TRUNC | FFO_WRONLY))) {
		if (0 != ffdir_make_path(tmp_fname, 0))
			goto done;
		if (FF_BADFD == (f = fffile_open(tmp_fname, FFO_CREATE | FFO_TRUNC | FFO_WRONLY))) {
			syserrlog("%s: %s", fffile_open_S, tmp_fname);
			goto done;
		}
	}

	if (out.len != (size_t)fffile_write(f, out.ptr, out.len))
		goto done;
	fffile_safeclose(f);
	if (0 != fffile_rename(tmp_fname, fn))
		goto done;
	dbglog(core, NULL, "que", "saved playlist to %s (%L KB)", fn, out.len / 1024);
	rc = 0;

done:
	if (rc != 0)
		syserrlog("saving playlist to file: %s", fn);
	ffvec_free(&v);
	fffile_safeclose(f);
	ffmem_free(tmp_fname);
	m3uwrite_close(&m3);
}

/** Get the first playlist item. */
static entry* pl_first(plist *pl)
{
	if (fflist_first(&pl->ents) == fflist_sentl(&pl->ents))
		return NULL;
	return FF_GETPTR(entry, sib, fflist_first(&pl->ents));
}

/** Get the next item.
from: previous item
Return NULL if there's no next item */
static entry* pl_next(entry *from)
{
	ffchain_item *it = from->sib.next;
	if (it == fflist_sentl(&from->plist->ents))
		return NULL;
	return FF_GETPTR(entry, sib, it);
}

/** Get the next (or the first) item; apply "random" and "repeat" settings.
from: previous item
 NULL: get the first item
Return NULL if there's no next item */
static entry* que_getnext(entry *from)
{
	plist *pl = (from == NULL) ? qu->curlist : from->plist;

	if (pl->allow_random && qu->random && pl->indexes.len != 0) {
		rnd_init();
		uint i = ffrnd_get() % pl->indexes.len;
		entry *e = ((entry**)pl->indexes.ptr) [i];
		return e;
	}

	if (from == NULL) {
		from = pl_first(pl);
	} else if (qu->repeat == FMED_QUE_REPEAT_TRACK) {
		dbglog0("repeat: same track");
	} else {
		from = pl_next(from);
		if (from == NULL && qu->repeat == FMED_QUE_REPEAT_ALL) {
			dbglog0("repeat: starting from the beginning");
			from = pl_first(pl);
		}
	}
	if (from == NULL) {
		dbglog0("no next file in playlist");
		qu->track->cmd(NULL, FMED_TRACK_LAST);
	}
	return from;
}

/** Expand the next (or the first) item in list.
e: the last expanded item
 NULL: expand the first item */
static void pl_expand_next(plist *pl, entry *e)
{
	void *trk = NULL;

	if (e == NULL) {
		e = pl_first(pl);
		if (e == NULL)
			return;
		pl->expand_all = 1;
		dbglog0("expanding plist %p", pl);
		trk = (void*)que_cmdv(FMED_QUE_EXPAND, e);
	}

	while (trk == NULL || trk == FMED_TRK_EFMT) {
		e = pl_next(e);
		if (e == NULL) {
			pl->expand_all = 0;
			dbglog0("done expanding plist %p", pl);
			break;
		}

		trk = (void*)que_cmdv(FMED_QUE_EXPAND, e);
	}
}

/** Get playlist by its index. */
static plist* plist_by_idx(size_t idx)
{
	uint i = 0;
	fflist_item *li;
	FFLIST_FOREACH(&qu->plists, li) {
		if (i++ == idx)
			return FF_GETPTR(plist, sib, li);
	}
	return NULL;
}

struct plist_sortdata {
	ffstr meta; // meta key by which to sort
	uint url :1
		, dur :1
		, reverse :1;
};

#define ffint_cmp(a, b) \
	(((a) == (b)) ? 0 : ((a) < (b)) ? -1 : 1)

/** Compare two entries. */
static int plist_entcmp(const void *a, const void *b, void *udata)
{
	struct plist_sortdata *ps = udata;
	struct entry *e1 = *(void**)a, *e2 = *(void**)b;
	ffstr *s1, *s2;
	int r = 0;

	if (ps->url) {
		r = ffstr_cmp2(&e1->e.url, &e2->e.url);

	} else if (ps->dur) {
		r = ffint_cmp(e1->e.dur, e2->e.dur);

	} else if (ps->meta.len != 0) {
		s1 = que_meta_find(&e1->e, ps->meta.ptr, ps->meta.len);
		s2 = que_meta_find(&e2->e, ps->meta.ptr, ps->meta.len);
		if (s1 == NULL && s2 == NULL)
			r = 0;
		else if (s1 == NULL)
			r = 1;
		else if (s2 == NULL)
			r = -1;
		else
			r = ffstr_cmp2(s1, s2);
	}

	if (ps->reverse)
		r = -r;
	return r;
}

/** Initialize random number generator */
static void rnd_init()
{
	if (qu->rnd_ready)
		return;
	qu->rnd_ready = 1;
	fftime t;
	fftime_now(&t);
	ffrnd_seed(t.sec);
}

/** Sort indexes randomly */
static void sort_random(plist *pl)
{
	rnd_init();
	entry **arr = (void*)pl->indexes.ptr;
	for (size_t i = 0;  i != pl->indexes.len;  i++) {
		size_t to = ffrnd_get() % pl->indexes.len;
		*arr[i] = FF_SWAP(arr[i], *arr[to]);
	}
}

/** Sort playlist entries. */
static void plist_sort(struct plist *pl, const char *by, uint flags)
{
	if (ffsz_eq(by, "__random")) {
		sort_random(pl);
	} else {
		struct plist_sortdata ps = {};
		if (ffsz_eq(by, "__url"))
			ps.url = 1;
		else if (ffsz_eq(by, "__dur"))
			ps.dur = 1;
		else
			ffstr_setz(&ps.meta, by);
		ps.reverse = !!(flags & 1);
		ffsort(pl->indexes.ptr, pl->indexes.len, sizeof(void*), &plist_entcmp, &ps);
	}

	fflist_init(&pl->ents);
	entry **arr = (void*)pl->indexes.ptr;
	for (size_t i = 0;  i != pl->indexes.len;  i++) {
		fflist_ins(&pl->ents, &arr[i]->sib);
	}
}

static void que_cmd(uint cmd, void *param)
{
	que_cmd2(cmd, param, 0);
}

// matches enum FMED_QUE
static const char *const scmds[] = {
	"FMED_QUE_PLAY",
	"FMED_QUE_PLAY_EXCL",
	"FMED_QUE_MIX",
	"FMED_QUE_STOP_AFTER",
	"FMED_QUE_NEXT2",
	"FMED_QUE_PREV2",
	"FMED_QUE_SAVE",
	"FMED_QUE_CLEAR",
	"FMED_QUE_ADD",
	"FMED_QUE_RM",
	"FMED_QUE_RMDEAD",
	"FMED_QUE_METASET",
	"FMED_QUE_SETONCHANGE",
	"FMED_QUE_EXPAND",
	"FMED_QUE_HAVEUSERMETA",
	"FMED_QUE_NEW",
	"FMED_QUE_DEL",
	"FMED_QUE_SEL",
	"FMED_QUE_LIST",
	"FMED_QUE_ISCURLIST",
	"FMED_QUE_ID",
	"FMED_QUE_ITEM",
	"FMED_QUE_ITEMLOCKED",
	"FMED_QUE_ITEMUNLOCK",
	"FMED_QUE_NEW_FILTERED",
	"FMED_QUE_ADD_FILTERED",
	"FMED_QUE_DEL_FILTERED",
	"FMED_QUE_LIST_NOFILTER",
	"FMED_QUE_SORT",
	"FMED_QUE_COUNT",
	"FMED_QUE_XPLAY",
	"FMED_QUE_ADD2",
	"FMED_QUE_ADDAFTER",
	"FMED_QUE_SETTRACKPROPS",
	"FMED_QUE_COPYTRACKPROPS",
	"FMED_QUE_SET_RANDOM",
	"FMED_QUE_SET_NEXTIFERROR",
	"FMED_QUE_SET_REPEATALL",
	"FMED_QUE_SET_QUITIFDONE",
	"FMED_QUE_EXPAND2",
	"FMED_QUE_EXPAND_ALL",
	"FMED_QUE_CURID",
	"FMED_QUE_SETCURID",
	"FMED_QUE_N_LISTS",
	"FMED_QUE_FLIP_RANDOM",
};

static ssize_t que_cmdv(uint cmd, ...)
{
	ssize_t r = 0;
	struct plist *pl;
	uint cmdflags = cmd & _FMED_QUE_FMASK;
	cmd &= ~_FMED_QUE_FMASK;
	va_list va;
	va_start(va, cmd);

	switch (cmd) {
	case FMED_QUE_SORT:
	case FMED_QUE_COUNT:
		FF_ASSERT(_FMED_QUE_LAST == FFCNT(scmds));
		dbglog0("received command:%s", scmds[cmd]);
		break;
	}

	switch ((enum FMED_QUE)cmd) {
	case FMED_QUE_XPLAY: {
		fmed_que_entry *first = va_arg(va, fmed_que_entry*);
		entry *e = FF_GETPTR(entry, e, first);
		dbglog0("received command:%s  first-entry:%p", scmds[cmd], e);
		/* Note: we should reset it after the last track is stopped,
		 but this isn't required now - no one will call FMED_QUE_PLAY on this playlist.
		*/
		e->plist->parallel = 1;
		que_xplay(e);
		goto end;
	}

	case FMED_QUE_ADD2: {
		int plist_idx = va_arg(va, int);
		fmed_que_entry *ent = va_arg(va, fmed_que_entry*);

		plist *pl = qu->curlist;
		if (plist_idx >= 0)
			pl = plist_by_idx(plist_idx);

		r = (ssize_t)que_add(pl, ent, NULL, cmdflags);
		goto end;
	}

	case FMED_QUE_ADDAFTER: {
		fmed_que_entry *ent = va_arg(va, void*);
		fmed_que_entry *qprev = va_arg(va, void*);
		entry *prev = FF_GETPTR(entry, e, qprev);
		r = (ssize_t)que_add(prev->plist, ent, prev, cmdflags);
		goto end;
	}

	case FMED_QUE_COUNT: {
		pl = qu->curlist;
		if (pl->filtered_plist != NULL)
			pl = pl->filtered_plist;
		r = pl->indexes.len;
		goto end;
	}

	case FMED_QUE_CURID: {
		int plid = va_arg(va, int);
		pl = (plid != -1) ? plist_by_idx(plid) : qu->curlist;
		if (pl == NULL)
			goto end;
		r = que_cmdv(FMED_QUE_ID, pl->cur);
		if (r == -1)
			r = 0;
		goto end;
	}

	case FMED_QUE_N_LISTS: {
		r = qu->plists.len;
		goto end;
	}

	case FMED_QUE_SETCURID: {
		int plid = va_arg(va, int);
		size_t eid = va_arg(va, size_t);
		pl = (plid != -1) ? plist_by_idx(plid) : qu->curlist;
		if (pl == NULL)
			goto end;
		pl->cur = (void*)que_cmdv(FMED_QUE_ITEM, (size_t)plid, (size_t)eid);
		goto end;
	}

	case FMED_QUE_DEL:
		pl = plist_by_idx(va_arg(va, int));
		fflist_rm(&qu->plists, &pl->sib);
		FFLIST_ENUMSAFE(&pl->ents, ent_rm, entry, sib);
		if (fflist_empty(&pl->ents)) {
			plist_free(pl);
			if (qu->curlist == pl)
				qu->curlist = NULL;
		} else
			pl->rm = 1;
		break;

	case FMED_QUE_SORT: {
		int plist_idx = va_arg(va, int);
		const char *by = va_arg(va, char*);
		int flags = va_arg(va, int);
		pl = qu->curlist;
		if (plist_idx >= 0)
			pl = plist_by_idx(plist_idx);
		plist_sort(pl, by, flags);
		goto end;
	}

	case FMED_QUE_ITEMLOCKED: {
		ssize_t plid = va_arg(va, size_t);
		ssize_t idx = va_arg(va, size_t);
		fflk_lock(&qu->plist_lock);
		r = que_cmdv(FMED_QUE_ITEM, plid, idx);
		if ((void*)r == NULL)
			fflk_unlock(&qu->plist_lock);
		goto end;
	}

	case FMED_QUE_ITEMUNLOCK: {
		// struct entry *e = va_arg(va, void*);
		fflk_unlock(&qu->plist_lock);
		goto end;
	}

	case FMED_QUE_SETTRACKPROPS: {
		fmed_que_entry *qent = va_arg(va, void*);
		fmed_trk *trk = va_arg(va, void*);
		entry *e = FF_GETPTR(entry, e, qent);
		if (e->trk == NULL) {
			e->trk = ffmem_alloc(sizeof(fmed_trk));
			qu->track->copy_info(e->trk, NULL);
		}
		qu->track->copy_info(e->trk, trk);
		goto end;
	}

	case FMED_QUE_COPYTRACKPROPS: {
		fmed_que_entry *qent = va_arg(va, void*);
		fmed_que_entry *qsrc = va_arg(va, void*);
		entry *e = FF_GETPTR(entry, e, qent);
		entry *src = FF_GETPTR(entry, e, qsrc);
		que_copytrackprops(e, src);
		goto end;
	}

	case FMED_QUE_SET_RANDOM: {
		uint val = va_arg(va, uint);
		qu->random = val;
		goto end;
	}

	case FMED_QUE_FLIP_RANDOM:
		qu->random = !qu->random;
		r = qu->random;
		goto end;

	case FMED_QUE_SET_NEXTIFERROR: {
		uint val = va_arg(va, uint);
		qu->next_if_err = val;
		goto end;
	}
	case FMED_QUE_SET_REPEATALL: {
		uint val = va_arg(va, uint);
		qu->repeat = val;
		goto end;
	}
	case FMED_QUE_SET_QUITIFDONE: {
		uint val = va_arg(va, uint);
		qu->quit_if_done = val;
		goto end;
	}

	case FMED_QUE_EXPAND2: {
		void *_e = va_arg(va, void*);
		void *ondone = va_arg(va, void*);
		void *ctx = va_arg(va, void*);
		entry *e = FF_GETPTR(entry, e, _e);
		r = (ffsize)que_expand2(e, ondone, ctx);
		goto end;
	}

	case FMED_QUE_EXPAND_ALL:
		pl_expand_next(qu->curlist, NULL);
		goto end;

	default:
		break;
	}

	void *param = va_arg(va, void*);
	size_t param2 = va_arg(va, size_t);
	r = que_cmd2(cmd, param, param2);

end:
	va_end(va);
	return r;
}

static ssize_t que_cmd2(uint cmd, void *param, size_t param2)
{
	plist *pl;
	fflist *ents = &qu->curlist->ents;
	entry *e;
	uint flags = cmd & _FMED_QUE_FMASK;
	cmd &= ~_FMED_QUE_FMASK;

	FF_ASSERT(_FMED_QUE_LAST == FFCNT(scmds));
	if (cmd != FMED_QUE_ITEM)
		dbglog0("received command:%s, param:%p", scmds[cmd], param);

	switch ((enum FMED_QUE)cmd) {
	case FMED_QUE_PLAY_EXCL:
		qu->track->cmd(NULL, FMED_TRACK_STOPALL);
		// break

	case FMED_QUE_PLAY:
		pl = qu->curlist;
		if (param != NULL) {
			e = param;
			pl = e->plist;
			pl->cur = param;

		} else if (pl->cur == NULL) {
			if (NULL == (pl->cur = que_getnext(NULL)))
				break;
		}
		qu->mixing = 0;
		que_play(pl->cur);
		break;

	case FMED_QUE_MIX:
		que_mix();
		break;

	case FMED_QUE_STOP_AFTER:
		pl = qu->curlist;
		if (pl->cur != NULL && pl->cur->refcount != 0)
			pl->cur->stop_after = 1;
		break;

	case FMED_QUE_NEXT2:
		pl = qu->curlist;
		e = pl->cur;
		if (param != NULL) {
			e = FF_GETPTR(entry, e, param);
			pl = e->plist;
		} else {
			qu->track->cmd(NULL, FMED_TRACK_STOPALL);
		}
		if (NULL != (e = que_getnext(e))) {
			pl->cur = e;
			que_play(pl->cur);
		}
		break;

	case FMED_QUE_PREV2:
		pl = qu->curlist;
		e = pl->cur;
		if (param != NULL) {
			e = FF_GETPTR(entry, e, param);
			pl = e->plist;
		}
		if (pl->cur == NULL || pl->cur->sib.prev == fflist_sentl(&pl->cur->plist->ents)) {
			pl->cur = NULL;
			dbglog(core, NULL, "que", "no previous file in playlist");
			break;
		}
		pl->cur = FF_GETPTR(entry, sib, pl->cur->sib.prev);
		que_play(pl->cur);
		break;

	case FMED_QUE_ADD:
		return (ssize_t)que_add(qu->curlist, param, NULL, flags);

	case FMED_QUE_EXPAND: {
		void *r = param;
		e = FF_GETPTR(entry, e, r);
		fmed_track_obj *trk = qu->track->create(FMED_TRK_TYPE_EXPAND, e->e.url.ptr);
		if (trk == NULL || trk == FMED_TRK_EFMT)
			return (size_t)trk;
		ent_start_prepare(e, trk);
		qu->track->cmd(trk, FMED_TRACK_START);
		return (size_t)trk;
	}

	case FMED_QUE_RM:
		e = param;
		dbglog(core, NULL, "que", "removed item %S", &e->e.url);

		if (!(flags & FMED_QUE_NO_ONCHANGE) && qu->onchange != NULL) {
			e->e.list_index = que_cmdv(FMED_QUE_ID, &e->e);

			// remove item from index, but don't free the object yet
			e->refcount++;
			fflk_lock(&qu->plist_lock);
			ent_rm(e);
			fflk_unlock(&qu->plist_lock);
			e->refcount--;

			dbglog0("calling onchange(FMED_QUE_ONRM): index:%u"
				, e->e.list_index);
			qu->onchange(&e->e, FMED_QUE_ONRM);
		}

		fflk_lock(&qu->plist_lock);
		ent_rm(e);
		fflk_unlock(&qu->plist_lock);
		break;

	case FMED_QUE_RMDEAD: {
		int n = 0;
		fflist_item *it;
		FFLIST_FOR(ents, it) {
			e = FF_GETPTR(entry, sib, it);
			it = it->next;
			if (!fffile_exists(e->e.url.ptr)) {
				que_cmd(FMED_QUE_RM, e);
				n++;
			}
		}
		return n;
	}

	case FMED_QUE_METASET:
		{
		const ffstr *pair = (void*)param2;
		que_meta_set(param, &pair[0], &pair[1], flags >> 16);
		}
		break;

	case FMED_QUE_HAVEUSERMETA:
		e = param;
		return (e->meta.len != 0 || e->no_tmeta);


	case FMED_QUE_SAVE:
		if ((ssize_t)param == -1)
			pl = qu->curlist;
		else
			pl = plist_by_idx((size_t)param);
		if (pl == NULL)
			return -1;
		ents = &pl->ents;
		que_save(FF_GETPTR(entry, sib, fflist_first(ents)), fflist_sentl(ents), (void*)param2);
		break;

	case FMED_QUE_CLEAR:
		fflk_lock(&qu->plist_lock);
		FFLIST_ENUMSAFE(ents, ent_rm, entry, sib);
		fflk_unlock(&qu->plist_lock);
		dbglog(core, NULL, "que", "cleared");
		if (!(flags & FMED_QUE_NO_ONCHANGE) && qu->onchange != NULL)
			qu->onchange(NULL, FMED_QUE_ONCLEAR);
		break;

	case FMED_QUE_SETONCHANGE:
		qu->onchange = param;
		break;


	case FMED_QUE_NEW: {
		uint f = (size_t)param;
		if (NULL == (pl = ffmem_tcalloc1(plist)))
			return -1;
		pl->allow_random = !(f & FMED_QUE_NORND);
		fflist_init(&pl->ents);
		fflist_ins(&qu->plists, &pl->sib);
		break;
	}

	case FMED_QUE_SEL:
		{
		uint i = 0, n = (uint)(size_t)param;
		fflist_item *li;

		if (n >= qu->plists.len)
			return -1;

		FFLIST_FOREACH(&qu->plists, li) {
			if (i++ == n)
				break;
		}
		qu->curlist = FF_GETPTR(plist, sib, li);
		}
		break;

	case FMED_QUE_LIST:
		if (qu->curlist->filtered_plist != NULL) {
			fmed_que_entry **ent = param;
			pl = qu->curlist->filtered_plist;
			uint i = 0;
			if (*ent != NULL)
				i = que_cmdv(FMED_QUE_ID, *ent) + 1;
			if (i == pl->indexes.len)
				return 0;
			*ent = (void*)que_cmdv(FMED_QUE_ITEM, (size_t)i);
			return 1;
		}
		//fallthrough

	case FMED_QUE_LIST_NOFILTER:
		{
		fmed_que_entry **ent = param;
		ffchain_item *it;
		if (*ent == NULL) {
			it = fflist_first(ents);
		} else {
			e = FF_GETPTR(entry, e, *ent);
			it = e->sib.next;
		}
		for (;;) {
			if (it == fflist_sentl(ents))
				return 0;
			e = FF_GETPTR(entry, sib, it);
			if (!e->rm)
				break;
			it = e->sib.next;
		}
		*ent = &e->e;
		}
		return 1;

	case FMED_QUE_ISCURLIST:
		e = param;
		return (e->plist == qu->curlist);

	case FMED_QUE_ID:
		e = param;
		if (e == NULL)
			return -1;
		pl = e->plist;
		if (pl->filtered_plist != NULL)
			pl = pl->filtered_plist;
		return plist_ent_idx(pl, e);

	case FMED_QUE_ITEM: {
		ssize_t plid = (size_t)param;
		pl = (plid != -1) ? plist_by_idx(plid) : qu->curlist;
		if (pl->filtered_plist != NULL)
			pl = pl->filtered_plist;
		return (size_t)plist_ent(pl, param2);
	}

	case FMED_QUE_NEW_FILTERED:
		if (qu->curlist->filtered_plist != NULL)
			que_cmdv(FMED_QUE_DEL_FILTERED);
		if (NULL == (pl = ffmem_new(struct plist)))
			return -1;
		fflist_init(&pl->ents);
		pl->filtered = 1;
		qu->curlist->filtered_plist = pl;
		break;

	case FMED_QUE_ADD_FILTERED:
		e = param;
		pl = qu->curlist->filtered_plist;
		if (NULL == ffvec_growT(&pl->indexes, 16, entry*))
			return -1;
		*ffvec_pushT(&pl->indexes, entry*) = e;
		break;

	case FMED_QUE_DEL_FILTERED:
		plist_free(qu->curlist->filtered_plist);
		qu->curlist->filtered_plist = NULL;
		break;

	case _FMED_QUE_LAST:
		break;

	default:
		break;
	}

	return 0;
}

static fmed_que_entry* _que_add(fmed_que_entry *ent)
{
	return (void*)que_cmd2(FMED_QUE_ADD, ent, 0);
}

/** Shift elements to the right.
A[0]...  ( A[i]... )  A[i+n]... ->
A[0]...  ( ... )  A[i]...
*/
static inline void _ffvec_shiftr(ffvec *v, size_t i, size_t n, size_t elsz)
{
	char *dst = v->ptr + (i + n) * elsz;
	const char *src = v->ptr + i * elsz;
	const char *end = v->ptr + v->len * elsz;
	ffmem_move(dst, src, end - src);
}

static fmed_que_entry* que_add(plist *pl, fmed_que_entry *ent, entry *prev, uint flags)
{
	entry *e = NULL;

	if (flags & FMED_QUE_ADD_DONE) {
		if (ent == NULL) {
			if (!(flags & FMED_QUE_NO_ONCHANGE) && qu->onchange != NULL) {
				dbglog0("calling onchange(FMED_QUE_ONADD_DONE)", 0);
				qu->onchange(NULL, FMED_QUE_ONADD_DONE);
			}
			return NULL;
		}
		e = FF_GETPTR(entry, e, ent);
		goto done;
	}

	e = ent_new(ent);
	if (e == NULL)
		return NULL;
	e->plist = pl;

	fflk_lock(&qu->plist_lock);

	ffchain_append(&e->sib, (prev != NULL) ? &prev->sib : fflist_last(&e->plist->ents));
	e->plist->ents.len++;
	if (NULL == ffvec_growT(&e->plist->indexes, 16, entry*)) {
		ent_rm(e);
		fflk_unlock(&qu->plist_lock);
		return NULL;
	}
	ssize_t i = e->plist->indexes.len;
	if (prev != NULL) {
		ssize_t i2 = plist_ent_idx(e->plist, prev);
		if (i2 != -1) {
			i = i2 + 1;
			_ffvec_shiftr(&e->plist->indexes, i, 1, sizeof(entry*));
		}
	}
	e->list_pos = i;
	((entry**)e->plist->indexes.ptr) [i] = e;
	e->plist->indexes.len++;
	fflk_unlock(&qu->plist_lock);

	dbglog0("added: [%L/%L] '%S' (%d: %d-%d) after:'%s'"
		, e->list_pos+1, e->plist->indexes.len
		, &ent->url
		, ent->dur, ent->from, ent->to
		, (prev != NULL) ? prev->url : NULL);

done:
	if (!(flags & FMED_QUE_NO_ONCHANGE) && qu->onchange != NULL)
		qu->onchange(&e->e, FMED_QUE_ONADD | (flags & FMED_QUE_MORE));
	return &e->e;
}
