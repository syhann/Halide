#ifndef HALIDE_PATTERN_MATCH_INTRINSICS_H
#define HALIDE_PATTERN_MATCH_INTRINSICS_H

/** \file
 * Tools to replace common patterns with more readily recognizable intrinsics.
 */

#include "IR.h"

namespace Halide {
namespace Internal {

/** Replace common arithmetic patterns with intrinsics. */
Stmt pattern_match_intrinsics(Stmt s);
Expr pattern_match_intrinsics(Expr e);

/** Try to rewrite an expression as an Add expression instead of an intrinsic. */
Expr as_add(const Expr &a);
/** Try to rewrite an expression as a Mul expression instead of an intrinsic. */
Expr as_mul(const Expr &a);

/** Implement intrinsics with non-intrinsic using equivalents. */
Expr lower_widening_add(const Expr &a, const Expr &b);
Expr lower_widening_mul(const Expr &a, const Expr &b);
Expr lower_widening_sub(const Expr &a, const Expr &b);
Expr lower_widening_shift_left(const Expr &a, const Expr &b);

Expr lower_rounding_shift_right(const Expr &a, const Expr &b);
Expr lower_rounding_shift_left(const Expr &a, const Expr &b);

Expr lower_saturating_add(const Expr &a, const Expr &b);
Expr lower_saturating_sub(const Expr &a, const Expr &b);

Expr lower_halving_add(const Expr &a, const Expr &b);
Expr lower_rounding_halving_add(const Expr &a, const Expr &b);
Expr lower_halving_sub(const Expr &a, const Expr &b);
Expr lower_rounding_halving_sub(const Expr &a, const Expr &b);

Expr lower_mulhi_shr(const Expr &a, const Expr &b, const Expr &shift);
Expr lower_sorted_avg(const Expr &a, const Expr &b);

Expr lower_abs(const Expr &a);
Expr lower_absd(const Expr &a, const Expr &b);

/** Implement any arithmetic intrinsic. */
Expr lower_intrinsic(const Call *op);

}  // namespace Internal
}  // namespace Halide

#endif