#include "all.h"

enum {
	Bot = -2, /* lattice bottom */
	Top = -1, /* lattice top */
};

typedef struct Edge Edge;

struct Edge {
	int dest;
	int dead;
	Edge *work;
};

static int *val;
static Edge *flowrk, (*edge)[2];
static Use **usewrk;
static uint nuse;

static int
czero(Con *c, int w)
{
	if (c->type != CBits)
		return 0;
	if (w)
		return c->bits.i == 0;
	else
		return (uint32_t)c->bits.i == 0;
}

static int
latval(Ref r)
{
	switch (rtype(r)) {
	case RTmp:
		return val[r.val];
	case RCon:
		return r.val;
	default:
		die("unreachable");
	}
}

static int
latmerge(int v, int m)
{
	return m == Top ? v : (v == Top || v == m) ? m : Bot;
}

static void
update(int t, int m, Fn *fn)
{
	Tmp *tmp;
	uint u;

	m = latmerge(val[t], m);
	if (m != val[t]) {
		tmp = &fn->tmp[t];
		for (u=0; u<tmp->nuse; u++) {
			vgrow(&usewrk, ++nuse);
			usewrk[nuse-1] = &tmp->use[u];
		}
		val[t] = m;
	}
}

static int
deadedge(int s, int d)
{
	Edge *e;

	e = edge[s];
	if (e[0].dest == d && !e[0].dead)
		return 0;
	if (e[1].dest == d && !e[1].dead)
		return 0;
	return 1;
}

static void
visitphi(Phi *p, int n, Fn *fn)
{
	int v;
	uint a;

	v = Top;
	for (a=0; a<p->narg; a++)
		if (!deadedge(p->blk[a]->id, n))
			v = latmerge(v, latval(p->arg[a]));
	update(p->to.val, v, fn);
}

static int opfold(int, int, Con *, Con *, Fn *);

static void
visitins(Ins *i, Fn *fn)
{
	int v, l, r;

	if (rtype(i->to) != RTmp)
		return;
	if (opdesc[i->op].cfold) {
		l = latval(i->arg[0]);
		if (!req(i->arg[1], R))
			r = latval(i->arg[1]);
		else
			r = CON_Z.val;
		if (l == Bot || r == Bot)
			v = Bot;
		else if (l == Top || r == Top)
			v = Top;
		else
			v = opfold(i->op, i->cls, &fn->con[l], &fn->con[r], fn);
	} else
		v = Bot;
	/* fprintf(stderr, "\nvisiting %s (%p)", opdesc[i->op].name, (void *)i); */
	update(i->to.val, v, fn);
}

static void
visitjmp(Blk *b, int n, Fn *fn)
{
	int l;

	switch (b->jmp.type) {
	case JJnz:
		l = latval(b->jmp.arg);
		assert(l != Top);
		if (l == Bot) {
			edge[n][1].work = flowrk;
			edge[n][0].work = &edge[n][1];
			flowrk = &edge[n][0];
		}
		else if (czero(&fn->con[l], 0)) {
			assert(edge[n][0].dead);
			edge[n][1].work = flowrk;
			flowrk = &edge[n][1];
		}
		else {
			assert(edge[n][1].dead);
			edge[n][0].work = flowrk;
			flowrk = &edge[n][0];
		}
		break;
	case JJmp:
		edge[n][0].work = flowrk;
		flowrk = &edge[n][0];
		break;
	default:
		if (isret(b->jmp.type))
			break;
		die("unreachable");
	}
}

static void
initedge(Edge *e, Blk *s)
{
	if (s)
		e->dest = s->id;
	else
		e->dest = -1;
	e->dead = 1;
	e->work = 0;
}

static int
renref(Ref *r)
{
	int l;

	if (rtype(*r) == RTmp)
		if ((l=val[r->val]) != Bot) {
			assert(l != Top);
			*r = CON(l);
			return 1;
		}
	return 0;
}

/* require rpo, use, pred */
void
fold(Fn *fn)
{
	Edge *e, start;
	Use *u;
	Blk *b, **pb;
	Phi *p, **pp;
	Ins *i;
	int n, d;
	uint a;

	val = emalloc(fn->ntmp * sizeof val[0]);
	edge = emalloc(fn->nblk * sizeof edge[0]);
	usewrk = vnew(0, sizeof usewrk[0]);

	for (n=0; n<fn->ntmp; n++)
		val[n] = Top;
	for (n=0; n<fn->nblk; n++) {
		b = fn->rpo[n];
		b->visit = 0;
		initedge(&edge[n][0], b->s1);
		initedge(&edge[n][1], b->s2);
	}
	initedge(&start, fn->start);
	flowrk = &start;
	nuse = 0;

	/* 1. find out constants and dead cfg edges */
	for (;;) {
		e = flowrk;
		if (e) {
			flowrk = e->work;
			e->work = 0;
			if (e->dest == -1 || !e->dead)
				continue;
			e->dead = 0;
			n = e->dest;
			b = fn->rpo[n];
			for (p=b->phi; p; p=p->link)
				visitphi(p, n, fn);
			if (b->visit == 0) {
				for (i=b->ins; i-b->ins < b->nins; i++)
					visitins(i, fn);
				visitjmp(b, n, fn);
			}
			b->visit++;
			assert(b->jmp.type != JJmp
				|| !edge[n][0].dead
				|| flowrk == &edge[n][0]);
		}
		else if (nuse) {
			u = usewrk[--nuse];
			n = u->bid;
			b = fn->rpo[n];
			if (b->visit == 0)
				continue;
			switch (u->type) {
			case UPhi:
				visitphi(u->u.phi, u->bid, fn);
				break;
			case UIns:
				visitins(u->u.ins, fn);
				break;
			case UJmp:
				visitjmp(b, n, fn);
				break;
			default:
				die("unreachable");
			}
		}
		else
			break;
	}

	if (debug['F']) {
		fprintf(stderr, "\n> SCCP findings:");
		for (n=Tmp0; n<fn->ntmp; n++) {
			if (val[n] == Bot)
				continue;
			fprintf(stderr, "\n%10s: ", fn->tmp[n].name);
			if (val[n] == Top)
				fprintf(stderr, "Top");
			else
				printref(CON(val[n]), fn, stderr);
		}
		fprintf(stderr, "\n dead code: ");
	}

	/* 2. trim dead code, replace constants */
	d = 0;
	for (pb=&fn->start; (b=*pb);) {
		if (b->visit == 0) {
			d = 1;
			if (debug['F'])
				fprintf(stderr, "%s ", b->name);
			blkdel(b);
			*pb = b->link;
			continue;
		}
		for (pp=&b->phi; (p=*pp);)
			if (val[p->to.val] != Bot)
				*pp = p->link;
			else {
				for (a=0; a<p->narg; a++)
					if (!deadedge(p->blk[a]->id, b->id))
						renref(&p->arg[a]);
				pp = &p->link;
			}
		for (i=b->ins; i-b->ins < b->nins; i++)
			if (renref(&i->to))
				*i = (Ins){.op = ONop};
			else
				for (n=0; n<2; n++)
					renref(&i->arg[n]);
		renref(&b->jmp.arg);
		if (b->jmp.type == JJnz && rtype(b->jmp.arg) == RCon) {
				b->jmp.type = JJmp;
				if (czero(&fn->con[b->jmp.arg.val], 0))
					b->s1 = b->s2;
				b->jmp.arg = R;
				b->s2 = 0;
		}
		pb = &b->link;
	}

	if (debug['F']) {
		if (!d)
			fprintf(stderr, "(none)");
		fprintf(stderr, "\n\n> After constant folding:\n");
		printfn(fn, stderr);
	}

	free(val);
	free(edge);
}

/* boring folding code */

static void
foldint(Con *res, int op, int w, Con *cl, Con *cr)
{
	union {
		int64_t s;
		uint64_t u;
		float fs;
		double fd;
	} l, r;
	uint64_t x;
	char *lab;

	lab = 0;
	l.s = cl->bits.i;
	r.s = cr->bits.i;
	if (op == OAdd) {
		if (cl->type == CAddr) {
			if (cr->type == CAddr)
				err("undefined addition (addr + addr)");
			lab = cl->label;
		}
		else if (cr->type == CAddr)
			lab = cr->label;
	}
	else if (op == OSub) {
		if (cl->type == CAddr) {
			if (cr->type != CAddr)
				lab = cl->label;
			else if (strcmp(cl->label, cr->label) != 0)
				err("undefined substraction (addr1 - addr2)");
		}
		else if (cr->type == CAddr)
			err("undefined substraction (num - addr)");
	}
	else if (cl->type == CAddr || cr->type == CAddr)
		err("invalid address operand for '%s'", opdesc[op].name);
	switch (op) {
	case OAdd:  x = l.u + r.u; break;
	case OSub:  x = l.u - r.u; break;
	case ODiv:  x = l.s / r.s; break;
	case ORem:  x = l.s % r.s; break;
	case OUDiv: x = l.u / r.u; break;
	case OURem: x = l.u % r.u; break;
	case OMul:  x = l.u * r.u; break;
	case OAnd:  x = l.u & r.u; break;
	case OOr:   x = l.u | r.u; break;
	case OXor:  x = l.u ^ r.u; break;
	case OSar:  x = l.s >> (r.u & 63); break;
	case OShr:  x = l.u >> (r.u & 63); break;
	case OShl:  x = l.u << (r.u & 63); break;
	case OExtsb: x = (int8_t)l.u;   break;
	case OExtub: x = (uint8_t)l.u;  break;
	case OExtsh: x = (int16_t)l.u;  break;
	case OExtuh: x = (uint16_t)l.u; break;
	case OExtsw: x = (int32_t)l.u;  break;
	case OExtuw: x = (uint32_t)l.u; break;
	case OFtosi:
		if (w)
			x = (int64_t)cl->bits.d;
		else
			x = (int32_t)cl->bits.s;
		break;
	case OCast:
		x = l.u;
		if (cl->type == CAddr)
			lab = cl->label;
		break;
	default:
		if (OCmpw <= op && op <= OCmpl1) {
			if (op <= OCmpw1) {
				l.u = (uint32_t)l.u;
				r.u = (uint32_t)r.u;
			} else
				op -= OCmpl - OCmpw;
			switch (op - OCmpw) {
			case ICule: x = l.u <= r.u; break;
			case ICult: x = l.u < r.u;  break;
			case ICsle: x = l.s <= r.s; break;
			case ICslt: x = l.s < r.s;  break;
			case ICsgt: x = l.s > r.s;  break;
			case ICsge: x = l.s >= r.s; break;
			case ICugt: x = l.u > r.u;  break;
			case ICuge: x = l.u >= r.u; break;
			case ICeq:  x = l.u == r.u; break;
			case ICne:  x = l.u != r.u; break;
			default: die("unreachable");
			}
		}
		else if (OCmps <= op && op <= OCmps1) {
			switch (op - OCmps) {
			case FCle: x = l.fs <= r.fs; break;
			case FClt: x = l.fs < r.fs;  break;
			case FCgt: x = l.fs > r.fs;  break;
			case FCge: x = l.fs >= r.fs; break;
			case FCne: x = l.fs != r.fs; break;
			case FCeq: x = l.fs == r.fs; break;
			case FCo: x = l.fs < r.fs || l.fs >= r.fs; break;
			case FCuo: x = !(l.fs < r.fs || l.fs >= r.fs); break;
			default: die("unreachable");
			}
		}
		else if (OCmpd <= op && op <= OCmpd1) {
			switch (op - OCmpd) {
			case FCle: x = l.fd <= r.fd; break;
			case FClt: x = l.fd < r.fd;  break;
			case FCgt: x = l.fd > r.fd;  break;
			case FCge: x = l.fd >= r.fd; break;
			case FCne: x = l.fd != r.fd; break;
			case FCeq: x = l.fd == r.fd; break;
			case FCo: x = l.fd < r.fd || l.fd >= r.fd; break;
			case FCuo: x = !(l.fd < r.fd || l.fd >= r.fd); break;
			default: die("unreachable");
			}
		}
		else
			die("unreachable");
	}
	*res = (Con){lab ? CAddr : CBits, .bits={.i=x}};
	res->bits.i = x;
	if (lab)
		strcpy(res->label, lab);
}

static void
foldflt(Con *res, int op, int w, Con *cl, Con *cr)
{
	float xs, ls, rs;
	double xd, ld, rd;

	if (cl->type != CBits || cr->type != CBits)
		err("invalid address operand for '%s'", opdesc[op].name);
	if (w)  {
		ld = cl->bits.d;
		rd = cr->bits.d;
		switch (op) {
		case OAdd: xd = ld + rd; break;
		case OSub: xd = ld - rd; break;
		case ODiv: xd = ld / rd; break;
		case OMul: xd = ld * rd; break;
		case OSitof: xd = cl->bits.i; break;
		case OExts: xd = cl->bits.s; break;
		case OCast: xd = ld; break;
		default: die("unreachable");
		}
		*res = (Con){CBits, .bits={.d=xd}, .flt=2};
	} else {
		ls = cl->bits.s;
		rs = cr->bits.s;
		switch (op) {
		case OAdd: xs = ls + rs; break;
		case OSub: xs = ls - rs; break;
		case ODiv: xs = ls / rs; break;
		case OMul: xs = ls * rs; break;
		case OSitof: xs = cl->bits.i; break;
		case OTruncd: xs = cl->bits.d; break;
		case OCast: xs = ls; break;
		default: die("unreachable");
		}
		*res = (Con){CBits, .bits={.s=xs}, .flt=1};
	}
}

static int
opfold(int op, int cls, Con *cl, Con *cr, Fn *fn)
{
	int nc;
	Con c;

	if ((op == ODiv || op == OUDiv
	|| op == ORem || op == OURem) && czero(cr, KWIDE(cls)))
		err("null divisor in '%s'", opdesc[op].name);
	if (cls == Kw || cls == Kl)
		foldint(&c, op, cls == Kl, cl, cr);
	else
		foldflt(&c, op, cls == Kd, cl, cr);
	if (c.type == CBits)
		nc = getcon(c.bits.i, fn).val;
	else {
		nc = fn->ncon;
		vgrow(&fn->con, ++fn->ncon);
	}
	assert(!(cls == Ks || cls == Kd) || c.flt);
	fn->con[nc] = c;
	return nc;
}
