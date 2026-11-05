// Save states (#203, ADR 0105): the CG value types as state file values, shared by the CG and
// UIKit serializers (snapshot/snapshot_internal.h has the recipe).
#ifndef MAPLETHIEF_RUNTIME_CG_CG_SNAPSHOT_H
#define MAPLETHIEF_RUNTIME_CG_CG_SNAPSHOT_H

#include <vector>

#include "cg/cg_objects.h"
#include "snapshot/snapshot_internal.h"

namespace mt_cg::snap {

inline void put(mt_snap::Writer &w, const Color &c) {
    w.f32(c.r);
    w.f32(c.g);
    w.f32(c.b);
    w.f32(c.a);
}
inline bool get(mt_snap::Reader &r, Color *c) {
    return r.f32(&c->r) && r.f32(&c->g) && r.f32(&c->b) && r.f32(&c->a);
}

inline void put(mt_snap::Writer &w, const Point &p) {
    w.f64(p.x);
    w.f64(p.y);
}
inline bool get(mt_snap::Reader &r, Point *p) { return r.f64(&p->x) && r.f64(&p->y); }

inline void put(mt_snap::Writer &w, const Rect &v) {
    w.f64(v.x);
    w.f64(v.y);
    w.f64(v.w);
    w.f64(v.h);
}
inline bool get(mt_snap::Reader &r, Rect *v) {
    return r.f64(&v->x) && r.f64(&v->y) && r.f64(&v->w) && r.f64(&v->h);
}

inline void put(mt_snap::Writer &w, const Affine &a) {
    w.f64(a.a);
    w.f64(a.b);
    w.f64(a.c);
    w.f64(a.d);
    w.f64(a.tx);
    w.f64(a.ty);
}
inline bool get(mt_snap::Reader &r, Affine *a) {
    return r.f64(&a->a) && r.f64(&a->b) && r.f64(&a->c) && r.f64(&a->d) && r.f64(&a->tx) &&
           r.f64(&a->ty);
}

inline void put(mt_snap::Writer &w, const Font &f) {
    w.boolean(f.bold);
    w.f32(f.size);
}
inline bool get(mt_snap::Reader &r, Font *f) { return r.boolean(&f->bold) && r.f32(&f->size); }

inline void put(mt_snap::Writer &w, const Path &p) {
    w.u32(static_cast<mt_snap::u32>(p.subpaths.size()));
    for (const Subpath &s : p.subpaths) {
        w.boolean(s.closed);
        w.u32(static_cast<mt_snap::u32>(s.points.size()));
        for (const Point &pt : s.points) {
            put(w, pt);
        }
    }
}
inline bool get(mt_snap::Reader &r, Path *p) {
    mt_snap::u32 n = 0;
    if (!r.u32(&n) || !r.count_fits(n, 5)) {
        return false;
    }
    p->subpaths.resize(n);
    for (Subpath &s : p->subpaths) {
        mt_snap::u32 m = 0;
        if (!r.boolean(&s.closed) || !r.u32(&m) || !r.count_fits(m, 16)) {
            return false;
        }
        s.points.resize(m);
        for (Point &pt : s.points) {
            if (!get(r, &pt)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace mt_cg::snap

#endif // MAPLETHIEF_RUNTIME_CG_CG_SNAPSHOT_H
