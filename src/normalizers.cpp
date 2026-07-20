#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include <mim/axm.h>
#include <mim/world.h>

#include "mim/plug/core/core.h"
#include "mim/plug/layout/layout.h"

namespace mim::plug::layout {

namespace {

/// A literal iter `(extent, stride, axis)`: `x ↦ (x·stride)@axis` for `x ∈ [0, extent)`.
struct It {
    u64 extent, stride;
    const Def* axis;
    bool operator==(const It&) const = default;
};

using Its = std::vector<It>;
/// A literal coordinate/offset: formal sum `Σ cᵢ@aᵢ` as (coefficient, axis) terms.
using Terms = std::vector<std::pair<u64, const Def*>>;

/// A literal layout `(D, R, O)`.
struct Val {
    Its d, r;
    Terms o;
    bool operator==(const Val&) const = default;
};

/// Deterministic axis order: annex axes by their (stable) flags, anything else by gid.
std::pair<u64, u64> axis_key(const Def* axis) {
    if (auto axm = axis->isa<Axm>()) return {0, axm->flags()};
    return {1, axis->gid()};
}

std::optional<It> parse_iter(const Def* def) {
    auto [e, s, a] = def->projs<3>();
    auto le        = Lit::isa(e);
    auto ls        = Lit::isa(s);
    if (!le || !ls) return {};
    return It{*le, *ls, a};
}

/// The element counts must come from `lt`'s implicit arguments:
/// `«1; T»` normalizes to `T`, so they cannot be recovered from the tuples themselves.
std::optional<Its> parse_iters(const Def* def, u64 n) {
    Its its;
    its.reserve(n);
    for (u64 i = 0; i < n; ++i) {
        auto it = parse_iter(def->proj(n, i));
        if (!it) return {};
        its.push_back(*it);
    }
    return its;
}

std::optional<Terms> parse_terms(const Def* def, u64 n) {
    Terms terms;
    terms.reserve(n);
    for (u64 i = 0; i < n; ++i) {
        auto [c, a] = def->proj(n, i)->projs<2>();
        auto lc     = Lit::isa(c);
        if (!lc) return {};
        terms.emplace_back(*lc, a);
    }
    return terms;
}

/// `imp` is `lt`'s implicit argument `(nd, nr, no)`, `arg` its explicit `(d, r, o)`.
std::optional<Val> parse_parts(const Def* imp, const Def* arg) {
    auto [nd, nr, no] = imp->projs<3>();
    auto lnd          = Lit::isa(nd);
    auto lnr          = Lit::isa(nr);
    auto lno          = Lit::isa(no);
    if (!lnd || !lnr || !lno) return {};
    auto [d, r, o] = arg->projs<3>();
    auto its_d     = parse_iters(d, *lnd);
    auto its_r     = parse_iters(r, *lnr);
    auto trm_o     = parse_terms(o, *lno);
    if (!its_d || !its_r || !trm_o) return {};
    return Val{std::move(*its_d), std::move(*its_r), std::move(*trm_o)};
}

/// Parses a (stuck, canonical) `%layout.lt` application into a Val.
std::optional<Val> parse(const Def* def) {
    auto lt_app = Axm::isa<lt>(def);
    if (!lt_app) return {};
    return parse_parts(lt_app->decurry()->arg(), lt_app->arg());
}

/// Merge same-axis terms, drop zero terms, sort by axis.
void canonicalize(Terms& terms) {
    std::ranges::sort(terms, [](auto& a, auto& b) { return axis_key(a.second) < axis_key(b.second); });
    Terms res;
    for (auto& [c, a] : terms)
        if (!res.empty() && res.back().second == a)
            res.back().first += c;
        else
            res.emplace_back(c, a);
    std::erase_if(res, [](auto& t) { return t.first == 0; });
    terms = std::move(res);
}

/// Axe's canonicalization: D0/D1 on the shard, C0/C2 on the (sorted) replicas, merged/sorted offset.
/// All rules preserve the extent products of `d` and `r`.
void canonicalize(Val& v) {
    // D0: drop unit-extent shard iters.
    std::erase_if(v.d, [](auto& it) { return it.extent == 1; });
    // D1: merge adjacent same-axis iters when the major one's stride equals the minor one's footprint.
    for (size_t i = 0; i + 1 < v.d.size();) {
        auto& hi = v.d[i];
        auto& lo = v.d[i + 1];
        if (hi.axis == lo.axis && hi.stride == lo.extent * lo.stride) {
            lo.extent *= hi.extent;
            v.d.erase(v.d.begin() + i);
            if (i > 0) --i;
        } else {
            ++i;
        }
    }
    // C0: drop unit-extent replicas; sort for a canonical multiset order.
    std::erase_if(v.r, [](auto& it) { return it.extent == 1; });
    std::ranges::sort(v.r, [](auto& a, auto& b) {
        return std::tuple(axis_key(a.axis), a.stride, a.extent) < std::tuple(axis_key(b.axis), b.stride, b.extent);
    });
    // C2 (saturated case only): merge same-axis replicas when the coarser stride equals the finer footprint.
    // The aliasing case (stride_hi < extent_lo·stride_lo) violates the gap condition and is left untouched.
    for (size_t i = 0; i + 1 < v.r.size();) {
        auto& lo = v.r[i];
        auto& hi = v.r[i + 1];
        if (lo.axis == hi.axis && hi.stride == lo.extent * lo.stride) {
            lo.extent *= hi.extent;
            v.r.erase(v.r.begin() + i + 1);
        } else {
            ++i;
        }
    }
    canonicalize(v.o);
}

/// Axis-wise span (Axe Lemma C.1): the footprint extent of `v` along `axis` (offset-independent).
u64 span_of(const Val& v, const Def* axis) {
    u64 sp = 1;
    for (auto& it : v.d)
        if (it.axis == axis) sp += it.stride * (it.extent - 1);
    for (auto& it : v.r)
        if (it.axis == axis) sp += it.stride * (it.extent - 1);
    return sp;
}

const Def* emit_iters(World& world, const Its& its) {
    DefVec ops;
    ops.reserve(its.size());
    for (auto& it : its)
        ops.push_back(world.tuple({world.lit_nat(it.extent), world.lit_nat(it.stride), it.axis}));
    return world.tuple(ops);
}

const Def* emit_terms(World& world, const Terms& terms) {
    DefVec ops;
    ops.reserve(terms.size());
    for (auto& [c, a] : terms)
        ops.push_back(world.tuple({world.lit_nat(c), a}));
    return world.tuple(ops);
}

/// Rebuilds `%layout.lt @(|d|, |r|, |o|) (d, r, o)`; normalization re-runs and finds it canonical.
const Def* emit(World& world, const Val& v) {
    auto imp = world.tuple({world.lit_nat(v.d.size()), world.lit_nat(v.r.size()), world.lit_nat(v.o.size())});
    auto arg = world.tuple({emit_iters(world, v.d), emit_iters(world, v.r), emit_terms(world, v.o)});
    return world.app(world.app(world.annex<lt>(), imp), arg);
}

const Def* emit_coord(World& world, const Terms& terms) {
    return world.app(world.app(world.annex<coord>(), world.lit_nat(terms.size())), emit_terms(world, terms));
}

/// Rank-1 Kronecker product: `a`'s contribution scaled by `b`'s axis-wise span, `b` minor.
Val tile_val(const Val& a, const Val& b) {
    auto scale = [&](It it) {
        it.stride *= span_of(b, it.axis);
        return it;
    };
    Val res;
    res.d.reserve(a.d.size() + b.d.size());
    for (auto& it : a.d)
        res.d.push_back(scale(it));
    res.d.insert(res.d.end(), b.d.begin(), b.d.end());
    res.r.reserve(a.r.size() + b.r.size());
    for (auto& it : a.r)
        res.r.push_back(scale(it));
    res.r.insert(res.r.end(), b.r.begin(), b.r.end());
    res.o = b.o;
    for (auto [c, ax] : a.o)
        res.o.emplace_back(c * span_of(b, ax), ax);
    canonicalize(res);
    return res;
}

} // namespace

/// `Π_{i<n} arg#i` as a `%core.nat.mul` chain; reduces even for symbolic elements.
const Def* normalize_prod(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();
    auto n      = Lit::isa(callee->as<App>()->arg());
    if (!n) return {};
    const Def* res = world.lit_nat(1);
    for (u64 i = 0; i < *n; ++i)
        res = world.call(core::nat::mul, Defs{res, arg->proj(*n, i)});
    return res;
}

/// `Π_{i<n} extent (arg#i)`; like normalize_prod, but over the iters' extents.
const Def* normalize_dom(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();
    auto n      = Lit::isa(callee->as<App>()->arg());
    if (!n) return {};
    const Def* res = world.lit_nat(1);
    for (u64 i = 0; i < *n; ++i)
        res = world.call(core::nat::mul, Defs{res, arg->proj(*n, i)->proj(3, 0)});
    return res;
}

/// Canonicalizes a fully literal layout; leaves anything symbolic stuck.
const Def* normalize_lt(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();
    auto val    = parse_parts(callee->as<App>()->arg(), arg);
    if (!val) return {};
    auto canon = *val;
    canonicalize(canon);
    if (canon == *val) return {};
    return emit(world, canon);
}

/// Merges same-axis terms, drops zero terms, and sorts, so equal coordinates are hash-consed.
const Def* normalize_coord(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();
    auto n      = Lit::isa(callee->as<App>()->arg());
    if (!n) return {};
    auto terms = parse_terms(arg, *n);
    if (!terms) return {};
    auto canon = *terms;
    canonicalize(canon);
    if (canon == *terms) return {};
    return emit_coord(world, canon);
}

const Def* normalize_tile(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    auto [a, b] = arg->projs<2>();
    auto va     = parse(a);
    auto vb     = parse(b);
    if (!va || !vb) return {};
    return emit(world, tile_val(*va, *vb));
}

/// Exact division: recovers `c` with `a = c ⊗ b` by peeling `b`'s shard iters off `a`'s minor end
/// (splitting fused iters as needed), un-scaling the remainder by `b`'s span, and subtracting
/// replicas/offsets. The result is verified by re-tiling; any failure leaves the application stuck.
const Def* normalize_untile(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    auto [a, b] = arg->projs<2>();
    auto va     = parse(a);
    auto vb     = parse(b);
    if (!va || !vb) return {};

    Val c;
    c.d = va->d;
    // Peel b.d minor-first off the back of c.d.
    for (auto it = vb->d.rbegin(); it != vb->d.rend(); ++it) {
        if (c.d.empty()) return {};
        auto& back = c.d.back();
        if (back.axis != it->axis || back.stride != it->stride || back.extent % it->extent != 0) return {};
        if (back.extent == it->extent)
            c.d.pop_back();
        else // split the fused iter and peel off its minor half
            back = It{back.extent / it->extent, it->extent * it->stride, back.axis};
    }
    // Un-scale the remaining shard iters by b's span.
    for (auto& it : c.d) {
        auto sp = span_of(*vb, it.axis);
        if (it.stride % sp != 0) return {};
        it.stride /= sp;
    }
    // Remove b's replicas (exact multiset match), un-scale the rest.
    c.r = va->r;
    for (auto& it : vb->r) {
        auto pos = std::ranges::find(c.r, it);
        if (pos == c.r.end()) return {};
        c.r.erase(pos);
    }
    for (auto& it : c.r) {
        auto sp = span_of(*vb, it.axis);
        if (it.stride % sp != 0) return {};
        it.stride /= sp;
    }
    // Subtract b's offset, un-scale the difference.
    Terms oa = va->o, ob = vb->o;
    canonicalize(oa);
    canonicalize(ob);
    for (auto& [cf_b, ax_b] : ob) {
        auto pos = std::ranges::find_if(oa, [ax = ax_b](auto& t) { return t.second == ax; });
        if (pos == oa.end() || pos->first < cf_b) return {};
        pos->first -= cf_b;
    }
    for (auto& [cf, ax] : oa) {
        auto sp = span_of(*vb, ax);
        if (cf % sp != 0) return {};
        cf /= sp;
    }
    c.o = std::move(oa);
    canonicalize(c);

    // Verify: c ⊗ b must reproduce a.
    auto ca = *va;
    canonicalize(ca);
    if (tile_val(c, *vb) != ca) return {};
    return emit(world, c);
}

/// Slices the region `[b, b+t)` out of the row-major view `s`:
/// group the shard iters by `s` (splitting as needed), then per dimension keep the trailing full levels
/// covered by `t`, narrow one level, and move the region corner's leading digits into the offset.
const Def* normalize_slice(const Def* type, const Def* callee, const Def* arg) {
    auto& world         = type->world();
    auto [l, s, bgn, t] = arg->projs<4>();
    auto vl             = parse(l);
    auto rk             = Lit::isa(callee->as<App>()->arg()->proj(3, 2)); // {E R: Nat, rk: Nat}
    if (!vl || !rk) return {};

    std::vector<u64> vs, vb, vt;
    for (auto [vec, def] : {
             std::pair{&vs,   s},
             {&vb, bgn},
             {&vt,   t}
    }) {
        for (u64 i = 0; i < *rk; ++i) {
            auto lit = Lit::isa(def->proj(*rk, i));
            if (!lit) return {};
            vec->push_back(*lit);
        }
    }

    // Group-by-shape (Axe Alg. 1, simplified): consume iters major-first, splitting on overshoot.
    Its work = vl->d;
    std::reverse(work.begin(), work.end()); // stack; back() is the most significant remaining iter
    std::vector<Its> blocks(*rk);
    for (u64 i = 0; i < *rk; ++i) {
        u64 need = vs[i];
        while (need > 1) {
            if (work.empty()) return {};
            auto it = work.back();
            work.pop_back();
            if (it.extent <= need) {
                if (need % it.extent != 0) return {};
                blocks[i].push_back(it);
                need /= it.extent;
            } else { // split: keep the major `need` digits, push the minor rest back
                if (it.extent % need != 0) return {};
                auto minor = it.extent / need;
                blocks[i].push_back(It{need, minor * it.stride, it.axis});
                work.push_back(It{minor, it.stride, it.axis});
                need = 1;
            }
        }
    }
    if (!work.empty()) return {};

    // Per dimension: t must cover trailing full levels plus one narrowed digit range.
    Val res;
    res.r = vl->r;
    res.o = vl->o;
    for (u64 i = 0; i < *rk; ++i) {
        auto& block = blocks[i];
        if (vt[i] == 0 || vb[i] + vt[i] > vs[i]) return {};
        // Find the narrowed level k−1: t = m · Π_{j≥k} extent_j with 1 ≤ m ≤ extent_{k−1}.
        size_t k   = block.size();
        u64 suffix = 1;
        while (k > 0 && vt[i] > block[k - 1].extent * suffix) {
            suffix *= block[k - 1].extent;
            --k;
        }
        if (vt[i] % suffix != 0) return {};
        u64 m = vt[i] / suffix; // == 1 whenever k == 0
        // Delinearize the region corner: trailing digits must be zero,
        // the narrowed level's digit range must fit, leading digits go into the offset.
        u64 rem = vb[i];
        for (size_t j = block.size(); j-- > 0;) {
            u64 digit = rem % block[j].extent;
            rem /= block[j].extent;
            if (j >= k) {
                if (digit != 0) return {};
            } else if (j + 1 == k) {
                if (digit + m > block[j].extent) return {};
                res.o.emplace_back(digit * block[j].stride, block[j].axis);
            } else {
                res.o.emplace_back(digit * block[j].stride, block[j].axis);
            }
        }
        // Result iters of this dimension: the narrowed level (if non-trivial), then the trailing full levels.
        if (m > 1) res.d.push_back(It{m, block[k - 1].stride, block[k - 1].axis});
        for (size_t j = k; j < block.size(); ++j)
            res.d.push_back(block[j]);
    }
    canonicalize(res);
    return emit(world, res);
}

/// Closed-form axis-wise span.
const Def* normalize_span(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    auto [l, a] = arg->projs<2>();
    auto vl     = parse(l);
    if (!vl) return {};
    return world.lit_nat(span_of(*vl, a));
}

/// Evaluates a literal layout at a literal index: all replica coordinates in lexicographic order.
const Def* normalize_eval(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    auto [l, x] = arg->projs<2>();
    auto vl     = parse(l);
    auto lx     = Lit::isa(x);
    if (!vl || !lx) return {};

    Terms base = vl->o;
    u64 rem    = *lx;
    for (auto it = vl->d.rbegin(); it != vl->d.rend(); ++it) { // minor-first delinearization
        base.emplace_back((rem % it->extent) * it->stride, it->axis);
        rem /= it->extent;
    }
    if (rem != 0) return {};

    u64 reps = 1;
    for (auto& it : vl->r)
        reps *= it.extent;
    DefVec coords;
    coords.reserve(reps);
    for (u64 k = 0; k < reps; ++k) {
        Terms terms = base;
        u64 r_rem   = k;
        for (auto it = vl->r.rbegin(); it != vl->r.rend(); ++it) {
            terms.emplace_back((r_rem % it->extent) * it->stride, it->axis);
            r_rem /= it->extent;
        }
        canonicalize(terms);
        coords.push_back(emit_coord(world, terms));
    }
    return world.tuple(coords);
}

MIM_layout_NORMALIZER_IMPL

} // namespace mim::plug::layout
