/*
** Garbage collector.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_vm.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

static GCRef empty;

#include <sys/time.h>
long long tick()
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec*1000 + time.tv_usec/1000;
}

#ifdef _GC_DEBUG2
static long long last_time;
#endif
void enter(long long *time)
{
#ifdef _GC_DEBUG2
  if (time == NULL)
    last_time = tick();
  else
    *time = tick();
#endif
}

void leave(const char *log, long long *time)
{
#ifdef _GC_DEBUG2
  if (time == NULL) {
    gc_debug2("%s: %lld\n", log, tick()-last_time);
  }
  else {
    gc_debug2("%s: %lld\n", log, tick()-*time);
  }
#endif
}

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  //lua_assert(iswhite(o) && !isdead(g, o));
  gc_debug("gc_mark: %p, %d\n", o, gct);
  gc_debug4("gc_mark: %p, %d, %d\n", o, gct, getage(o));
  gc_debug5("gc_mark: %p, %d, %d\n", o, gct, getage(o));
  gc_debug6("gc_mark: %p, %d, %d\n", o, gct, getage(o));
  white2gray(o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    gc_markobj(g, tabref(gco2ud(o)->env));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lua_assert(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  gc_markobj(g, mainthread(g));
  gc_markobj(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

/* Mark userdata in mmudata list. */
static void gc_mark_mmudata(global_State *g)
{
  GCobj *root = gcref(g->gc.mmudata);
  GCobj *u = root;
  if (u) {
    do {
      u = gcnext(u);
      makewhite(g, u);  /* Could be from previous GC. */
      gc_mark(g, u);
    } while (u != root);
  }
}

/* Separate userdata objects to be finalized to mmudata list. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  GCRef *p = &mainthread(g)->nextgc;
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    if (!(iswhite(o) || all) || isfinalized(gco2ud(o))) {
      p = &o->gch.nextgc;  /* Nothing to do. */
    } else if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
      markfinalized(o);  /* Done, as there's no __gc metamethod. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise move userdata to be finalized to mmudata list. */
      gc_debug3("lj_gc_separateudata: %p\n", o);
      m += sizeudata(gco2ud(o));
      markfinalized(o);
      *p = o->gch.nextgc;
      if (LJ_UNLIKELY(o == gcref(g->gc.udatasur)))
        setgcrefr(g->gc.udatasur, o->gch.nextgc);
      if (LJ_UNLIKELY(o == gcref(g->gc.udataold)))
        setgcrefr(g->gc.udataold, o->gch.nextgc);
      if (gcref(g->gc.mmudata)) {  /* Link to end of mmudata list. */
	GCobj *root = gcref(g->gc.mmudata);
	setgcrefr(o->gch.nextgc, root->gch.nextgc);
	setgcref(root->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
      } else {  /* Create circular list. */
	setgcref(o->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
      }
    }
  }
  return m;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  gc_debug("gc_traverse_tab: %p\n", t);
  int weak = 0;
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_markobj(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      CTState *cts = ctype_ctsG(g);
      if (cts && cts->finalizer == t) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
      }
    }
  }
  gc_debug("gc_traverse_tab: %p, %d, %d, %d\n", t, weak, t->asize, t->hmask);
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  gc_debug("gc_traverse_tab: mark array part: %p\n", t);
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++) {
      gc_debug("gc_traverse_tab: %p, %d, %p\n", t, ~itype(arrayslot(t, i)), arrayslot(t, i));
      gc_marktv(g, arrayslot(t, i));
    }
  }
  gc_debug("gc_traverse_tab: mark hash part: %p\n", t);
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lua_assert(!tvisnil(&n->key));
	if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n->key);
	if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  if (!weak && g->gc.kind == KGC_GEN) {
    gc_debug("gc_traverse_tab: add to grayagain: %p\n", t);
    setgcrefr(t->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, obj2gco(t));
    black2gray(obj2gco(t));
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  gc_debug4("gc_traverse_func: %p, %d\n", fn, getage(obj2gco(fn)));
  gc_debug6("gc_traverse_func: %p, %d, %d\n", fn, getage(obj2gco(fn)), isluafunc(fn));
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lua_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  gc_debug6("gc_marktrace: %p, %d, %d\n", o, traceno, o->gch.marked);
  lua_assert(traceno != G2J(g)->cur.traceno);
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  gc_debug6("gc_traverse_trace: %p\n", T);
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  gc_debug5("gc_traverse_thread: %p, %p, %p\n", th, tvref(th->stack)+1+LJ_FR2, th->top);
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++) {
    gc_debug5("gc_traverse_thread: %p, %d, %p\n", o, ~itype(o), gcval(o));
    gc_marktv(g, o);
  }
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  //lua_assert(isgray(o));
  gc_debug("propagatemark: %p, %d, %d\n", o, gct, getage(o));
  gc_debug4("propagatemark: %p, %d, %d\n", o, gct, getage(o));
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lua_assert(0);
    return 0;
#endif
  }
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lua_assert(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED));
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lua_assert(isdead(g, o) || ow == LJ_GC_SFIXED);
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return p;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
static void gc_clearweak(GCobj *o)
{
  while (o) {
    GCtab *t = gco2tab(o);
    lua_assert((t->marked & LJ_GC_WEAK));
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1))
	  setnilV(tv);
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1)))
	  setnilV(&n->val);
      }
    }
    o = gcref(t->gclist);
  }
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  GCSize oldt = g->gc.threshold;
  int errcode;
  TValue *top;
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  top = L->top;
  copyTV(L, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(L, top, o, ~o->gch.gct);
  L->top = top+1;
  errcode = lj_vm_pcall(L, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

/* Finalize one userdata or cdata object from the mmudata list. */
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo;
  lua_assert(tvref(g->jit_base) == NULL);  /* Must not be called on trace. */
  /* Unchain from list of userdata to be finalized. */
  if (o == gcref(g->gc.mmudata))
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(gcref(g->gc.mmudata)->gch.nextgc, o->gch.nextgc);
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    setgcrefr(o->gch.nextgc, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      g->gc.nocdatafin = 0;
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  /* Add userdata back to the main userdata list and make it white. */
  setgcrefr(o->gch.nextgc, mainthread(g)->nextgc);
  setgcref(mainthread(g)->nextgc, o);
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L);
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *node = noderef(t->node);
    ptrdiff_t i;
    setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
    for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
      if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
	GCobj *o = gcV(&node[i].key);
	TValue tmp;
	makewhite(g, o);
	o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
	copyTV(L, &tmp, &node[i].val);
	setnilV(&node[i].val);
	gc_call_finalizer(g, L, &tmp, o);
      }
  }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i, strmask;
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.root);
  strmask = g->strmask;
  for (i = 0; i <= strmask; i++)  /* Free all string hash chains. */
    gc_fullsweep(g, &g->strhash[i]);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  // 保存当前的grayagain链表;
  GCRef grayagain;
  setgcrefr(grayagain, g->gc.grayagain);
  setgcrefnull(g->gc.grayagain);

  g->gc.state = GCSatomic;

  gc_debug("atomic: print grayagain\n");

  gc_debug("atomic: propagate uv\n");
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  gc_debug("atomic: propagate weak, mainthread, gcroot\n");
  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lua_assert(!iswhite(obj2gco(mainthread(g))));
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  gc_debug("atomic: propagate weak, grayagain\n");
  setgcrefr(g->gc.gray, grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  gc_debug("atomic: propagate udata\n");
  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
  gc_clearweak(gcref(g->gc.weak));

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    gc_fullsweep(g, &g->strhash[g->gc.sweepstr++]);  /* Sweep one chain. */
    if (g->gc.sweepstr > g->strmask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = g->gc.total;
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
	g->gc.state = GCSfinalize;
#if LJ_HASFFI
	g->gc.nocdatafin = 1;
#endif
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	g->gc.debt = 0;
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one userdata object. */
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
#if LJ_HASFFI
    if (!g->gc.nocdatafin) lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
#endif
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    return 0;
  default:
    lua_assert(0);
    return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
static int incstep(lua_State *L)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    lim -= (GCSize)gc_onestep(L);
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    g->gc.threshold = g->gc.total + GCSTEPSIZE;
    g->vmstate = ostate;
    return -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
    g->gc.threshold = g->gc.total;
    g->vmstate = ostate;
    return 0;
  }
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->cur_L));
  L->base = tvref(G(L)->jit_base);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/*
** {======================================================
** Generational Collector
** =======================================================
*/

/*
** Sweep a list of objects, deleting dead ones and turning
** the non dead to old (without changing their colors).
*/
static void sweep2old(lua_State *L, GCRef *p) {
  GCobj *o;
  global_State *g = G(L);
  while ((o = gcref(*p)) != NULL) {
    gc_debug("sweep2old: %p, %d\n", o, o->gch.gct);

    // 线程upvalue是否需要特殊处理
    if (o->gch.gct == ~LJ_TTHREAD)
      sweep2old(L, &gco2th(o)->openupval);

    if (iswhite(o) && !isfixed(o)) {
      lua_assert(isdead(g, o));
      setgcrefr(*p, o->gch.nextgc);
      gc_debug("sweep2old: free: %p\n", o);
      gc_debug5("sweep2old: free: %p\n", o);
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
    else {
      gc_debug("sweep2old: age to old: %p\n", o);
      setage(o, G_OLD);
      p = &o->gch.nextgc;
    }
  }
}

static void sweepstringsold(lua_State *L) {
  global_State *g = G(L);
  int i = 0;
  for (; i <= g->strmask; ++i) {
    sweep2old(L, &g->strhash[i]);
  }
}

/*
** Sweep for generational mode. Delete dead objects. (Because the
** collection is not incremental, there are no "new white" objects
** during the sweep. So, any white object must be dead.) For
** non-dead objects, advance their ages and clear the color of
** new objects. (Old objects keep their colors.)
*/
static GCRef *sweepgen(lua_State *L, global_State *g, GCRef *p, GCRef limit, GCRef *root) {

  static uint8_t nextage[] = {
    G_SURVIVAL,  /* from G_NEW */
    G_OLD1,      /* from G_SURVIVAL */
    G_OLD1,      /* from G_OLD0 */
    G_OLD,       /* from G_OLD1 */
    G_OLD,       /* from G_OLD (do not change) */
    G_TOUCHED1,  /* from G_TOUCHED1 (do not change) */
    G_TOUCHED2   /* from G_TOUCHED2 (do not change) */
  };

  GCobj *o;
  GCobj *objlimit = gcref(limit);
  while ((o = gcref(*p)) != objlimit) {
    gc_debug("sweepgen: %p, %d, %d\n", o, o->gch.gct, getage(o));

    // 线程upvalue是否需要特殊处理
    if (o->gch.gct == ~LJ_TTHREAD)
      sweepgen(L, g, &gco2th(o)->openupval, empty, NULL);

    if (iswhite(o) && !isfixed(o)) {
      lua_assert(!isold(o) && isdead(g, o));
      setgcrefr(*p, o->gch.nextgc);
      if (root && o == gcref(*root))
        setgcrefr(*root, o->gch.nextgc);
      gc_debug("sweepgen: free: %p\n", o);
      gc_debug5("sweepgen: free: %p\n", o);
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
    else {
      gc_debug("sweepgen: change age: %p, %d\n", o, nextage[getage(o)]);
      if (getage(o) == G_NEW)
        makewhite(g, o);
      setage(o, nextage[getage(o)]);
      p = &o->gch.nextgc;
    }
  }
  return p;
}

static void sweepstringsgen(lua_State *L) {
  global_State *g = G(L);
  int i = 0;
  for (; i <= g->strmask; ++i) {
    sweepgen(L, g, &g->strhash[i], empty, NULL);
  }
}

/*
** Traverse a list making all its elements white and clearing their
** age.
*/
static void whitelist(global_State *g, GCRef p) {
  GCobj *o;
  while ((o = gcref(p)) != NULL) {
    makewhite(g, o);
    setage(o, G_NEW);

    if (o->gch.gct == ~LJ_TTHREAD)
      whitelist(g, gco2th(o)->openupval);

    setgcrefr(p, o->gch.nextgc);
  }
}

static void whiltestrings(global_State *g) {
  int i = 0;
  for (; i < g->strmask; ++i) {
    whitelist(g, g->strhash[i]);
  }
}

/*
** advances the garbage collector until it reaches a state allowed
** by 'statemask'
** 一直执行分步操作，直到到达想要的状态;
*/
void lj_gc_runtilstate (lua_State *L, int state) {
  global_State *g = G(L);
  while (g->gc.state != state)
    gc_onestep(L);
}

/*
** Correct a list of gray objects. Because this correction is
** done after sweeping, young objects can be white and still
** be in the list. They are only removed.
** For tables and userdata, advance 'touched1' to 'touched2'; 'touched2'
** objects become regular old and are removed from the list.
** For threads, just remove white ones from the list.
*/
static GCRef *correctgraylist(GCRef *p, GCRef *root) {
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    gc_debug("correctgraylist: %p, %d, %d\n", o, o->gch.gct, getage(o));
    switch (~o->gch.gct) {
      case LJ_TTAB: case LJ_TUDATA: {
        if (getage(o) == G_TOUCHED1) {
          lua_assert(isgray(o));
          gray2black(o);
          changeage(o, G_TOUCHED1, G_TOUCHED2);
          p = &o->gch.gclist;
        }
        else {
          if (!iswhite(o)) {
            lua_assert(isold(o));
            if (getage(o) == G_TOUCHED2)
              changeage(o, G_TOUCHED2, G_OLD);
            gray2black(o);
          }
          gc_debug("correctgraylist: remove from list: %p\n", o);
          *p = o->gch.gclist;
        }
        gc_debug("correctgraylist after: %p, %d\n", o, getage(o));
        break;
      }
      case LJ_TTHREAD: {
        lua_assert(!isblack(o));
        if (iswhite(o))
          *p = o->gch.gclist;
        else
          p = &o->gch.gclist;
        break;
      }
      default: lua_assert(0);
    }
  }
  return p;
}

/*
** Correct all gray lists, coalescing them into 'grayagain'.
*/
static void correctgraylists(global_State *g) {
  gc_debug("correctgraylists: correct grayagain list\n");
  GCRef *list = correctgraylist(&g->gc.grayagain, &g->gc.grayagain);
  *list = g->gc.weak;
  setgcrefnull(g->gc.weak);
  gc_debug("correctgraylists: correct weak list\n");
  correctgraylist(list, list);
}

/*
** Mark 'old1' objects when starting a new young collection.
** Gray objects are already in some gray list, and so will be visited
** in the atomic step.
*/
static void markold(global_State *g, GCRef from, GCRef to) {
  GCobj *o;
  GCobj *toobj = gcref(to);
  while ((o = gcref(from)) != toobj) {
    gc_debug5("markold: %p, %d\n", o, getage(o));
    if (getage(o) == G_OLD1) {
      gc_debug("markold: %p\n", o);
      lua_assert(!iswhite(o) || isfixed(o));
      if (isblack(o)) {
        black2gray(o);
        gc_mark(g, o);
      }
    }
    from = o->gch.nextgc;
  }
}


static void markstringold(global_State *g) {
  int i = 0;
  for (; i < g->strmask; ++i) {
    markold(g, g->strhash[i], empty);
  }
}

/*
** call all pending finalizers
*/
static void callallpendingfinalizers (lua_State *L) {
  global_State *g = G(L);
  while (gcref(g->gc.mmudata) != NULL)
    gc_finalize(L);
}

/*
** Finish a young-generation collection.
*/
static void finishgencycle(lua_State *L, global_State *g) {
  correctgraylists(g);
  g->gc.state = GCSpropagate;
  callallpendingfinalizers(L);
}

/*
** Does a young collection. First, mark 'old1' objects.  (Only survival
** and "recent old" lists can contain 'old1' objects. New lists cannot
** contain 'old1' objects, at most 'old0' objects that were already
** visited when marked old.) Then does the atomic step. Then,
** sweep all lists and advance pointers. Finally, finish the collection.
*/
static void youngcollection(lua_State *L, global_State *g) {
  gc_debug2("youngcollection: \n");
  lua_assert(g->gc.state == GCSpropagate);
  enter(NULL);
  gc_debug3("mark gcobj: %p, %p\n", gcref(g->gc.surival), gcref(g->gc.reallyold));
  markold(g, g->gc.surival, g->gc.reallyold);
  leave("mark old1", NULL);

  enter(NULL);
  gc_debug3("mark udata: %p, %p\n", gcref(g->gc.udatasur), gcref(g->gc.udatarold));
  markold(g, g->gc.udatasur, g->gc.udatarold);
  leave("mark old2", NULL);

  // 特殊处理字符串;
  gc_debug3("mark string:\n");
  markstringold(g);

  enter(NULL);
  atomic(g, L);
  leave("atomic", NULL);

  enter(NULL);
  GCRef *psurvival = sweepgen(L, g, &g->gc.root, g->gc.surival, NULL);
  sweepgen(L, g, psurvival, g->gc.reallyold, &g->gc.old);
  leave("sweepgen1", NULL);
  g->gc.reallyold = g->gc.old;
  g->gc.old = *psurvival;
  g->gc.surival = g->gc.root;

  sweepstringsgen(L);

  enter(NULL);
  psurvival = sweepgen(L, g, &mainthread(g)->nextgc, g->gc.udatasur, NULL);
  sweepgen(L, g, psurvival, g->gc.udatarold, &g->gc.udataold);
  leave("sweepgen2", NULL);
  gc_debug3("udata after gen: %p %p %p, %p\n", gcref(mainthread(g)->nextgc), gcref(g->gc.udatasur), gcref(g->gc.udataold), gcref(g->gc.udatarold));
  g->gc.udatarold = g->gc.udataold;
  g->gc.udataold = *psurvival;
  g->gc.udatasur = mainthread(g)->nextgc;
  gc_debug3("udata after: %p %p, %p\n", gcref(g->gc.udatasur), gcref(g->gc.udataold), gcref(g->gc.udatarold));

  enter(NULL);
  finishgencycle(L, g);
  leave("finishgencycle", NULL);
}

// 进入到分代模式，进行一次完整的标记操作，之后把所有存活的打上old标记
static void entergen(lua_State *L, global_State *g) {
  lj_gc_runtilstate(L, GCSpause);
  lj_gc_runtilstate(L, GCSpropagate);
  atomic(g, L);

  // 标记所有对象为old;
  sweep2old(L, &g->gc.root);
  sweepstringsold(L);

  g->gc.reallyold = g->gc.old = g->gc.surival = g->gc.root;

  g->gc.udatarold = g->gc.udataold = g->gc.udatasur = mainthread(g)->nextgc;

  g->gc.kind = KGC_GEN;
  g->gc.estimate = g->gc.total;
  g->gc.threshold = (g->gc.total / 100) * (100 + g->gc.genminormul);
  finishgencycle(L, g);
}

static void enterinc(global_State *g) {
  whitelist(g, g->gc.root);
  setgcrefnull(g->gc.reallyold);
  setgcrefnull(g->gc.old);
  setgcrefnull(g->gc.surival);

  // 遍历所有字符串;
  whiltestrings(g);

  g->gc.state = GCSpause;
  g->gc.kind = KGC_INC;
}

/*
** Does a full collection in generational mode.
*/
static void fullgen(lua_State *L, global_State *g) {
  gc_debug2("fullgen: \n");
  enterinc(g);
  entergen(L, g);
}

/*
** Does a generational "step". If memory grows 'genmajormul'% larger
** than last major collection (kept in 'g->GCestimate'), does a major
** collection. Otherwise, does a minor collection and set debt to make
** another collection when memory grows 'genminormul'% larger.
** 一次分代gc step，会stop the world直到gc完成;
** 可以通过genmajormul和genminormul来控制分代full gc和分代young gc的时机;
*/
static void genstep(lua_State *L, global_State *g) {
  MSize majorbase = g->gc.estimate;
  int majormul = getgcparam(g->gc.genmajormul);
  gc_debug2("genstep: %d, %d, %ld, %ld\n", majorbase, majormul, g->gc.total, g->gc.threshold);
  if (g->gc.total > g->gc.threshold && g->gc.total > (majorbase / 100) * (100 + majormul))
    fullgen(L, g);
  else {
    youngcollection(L, g);
    MSize mem = g->gc.total;
    g->gc.threshold = (mem / 100) * (100 + g->gc.genminormul);
    g->gc.estimate = majorbase;
  }
  gc_debug2("genstep end: %ld, %ld, %ld\n", g->gc.estimate, g->gc.total, g->gc.threshold);
}

int LJ_FASTCALL lj_gc_step(lua_State *L) {
  gc_debug2("lj_gc_step: \n");
  global_State *g = G(L);
  long long begin;
  enter(&begin);
  if (g->gc.kind == KGC_INC)
    return incstep(L);
  else
    genstep(L, g);
  leave("lj_gc_step: ", &begin);
  return 1;
}

// 更改gc模式;
void lj_gc_changemode(lua_State *L, int newmode) {
  global_State *g = G(L);
  if (newmode != g->gc.kind) {
    if (newmode == KGC_GEN)
      entergen(L, g);
    else
      enterinc(g);
  }
}

static void fullinc(lua_State *L, global_State *g) {
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lua_assert(g->gc.state == GCSfinalize || g->gc.state == GCSpause);
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
}

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  if (g->gc.kind == KGC_INC)
    fullinc(L, g);
  else
    fullgen(L, g);
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  gc_debug("lj_gc_barrierf: %p, %p, %d\n", o, v, getage(o));
  gc_debug6("lj_gc_barrierf: %p, %p, %d\n", o, v, getage(o));
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    gc_mark(g, v);  /* Move frontier forward. */
    if (isold(o)) {
      lua_assert(!isold(v));
      setage(v, G_OLD0);
    }
  }
  else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
  gc_debug("lj_gc_barrieruv: %p\n", gcV(tv));
  gc_debug6("lj_gc_barrieruv: %p\n", gcV(tv));
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, gcV(tv));
  else
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(&uv->tv))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  gc_debug6("lj_gc_barriertrace: %d\n", traceno);
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    gc_marktrace(g, traceno);
    GCobj *o = gcref(traceref(G2J(g), traceno)->startpt);
    if (isold(o)) {
      GCobj *v = obj2gco(traceref(G2J(g), traceno));
      lua_assert(!isold(v));
      setage(v, G_OLD0);
    }
  }
}
#endif

/* -- Allocator ----------------------------------------------------------- */

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lua_assert((osz == 0) == (p == NULL));
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lua_assert((nsz == 0) == (p == NULL));
  lua_assert(checkptrGC(p));
  g->gc.total = (g->gc.total - osz) + nsz;
  gc_debug5("lj_mem_realloc: %p\n", p);
  gc_debug6("lj_mem_realloc: %p\n", p);
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lua_assert(checkptrGC(o));
  g->gc.total += size;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  setage(o, G_NEW);
  gc_debug5("lj_mem_newgco: %p\n", o);
  gc_debug6("lj_mem_newgco: %p\n", o);
  return o;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

