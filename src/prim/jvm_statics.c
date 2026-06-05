/*
 * jvm_statics.c -- JVM Clojure surface-parity statics layer.
 *
 * Two distinct concerns share this file because they share the same
 * mechanism (a literal slash-name binding in the clojure.core env):
 *
 *   1. Value statics  -- mathematical constants and parser/printer
 *      functions whose JVM contract has no host coupling. Long/MAX_VALUE,
 *      Math/PI, Math/sqrt, Integer/parseInt etc.
 *
 *   2. Embedded-host semantic remap -- JVM Class/Member references
 *      that have a real mino-native equivalent (System/currentTimeMillis,
 *      java.util.UUID/randomUUID etc.). The Clojure-level UX matches
 *      JVM; the implementation underneath calls mino's own primitive.
 *
 * Anything depending on JVM-specific runtime state (Thread/getStackTrace,
 * System/identityHashCode, ClassLoader/getResource, ...) stays absent
 * by design and surfaces a clear MNS001 at the resolve site rather than
 * being faked into a wrong-shaped result.
 */

#include "prim/internal.h"
#include "mino.h"
#include <math.h>
#include <time.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* ------------------------------------------------------------------------- */
/* Layer 1: parsers and predicates not already in numeric_math.c            */
/* ------------------------------------------------------------------------- */

static mino_val *prim_long_parse_long(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    mino_val *s, *radix_v;
    long long radix = 10;
    char     *endp = NULL;
    long long n;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Long/parseLong requires a string argument");
    }
    s = args->as.cons.car;
    if (mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Long/parseLong: string argument required");
    }
    if (mino_is_cons(args->as.cons.cdr)) {
        radix_v = args->as.cons.cdr->as.cons.car;
        if (!mino_val_int_p(radix_v)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "Long/parseLong: radix must be an integer");
        }
        radix = mino_val_int_get(radix_v);
        if (radix < 2 || radix > 36) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "Long/parseLong: radix out of range");
        }
    }
    n = strtoll(s->as.s.data, &endp, (int)radix);
    if (endp == s->as.s.data || (endp != NULL && *endp != '\0')) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Long/parseLong: unparseable input");
    }
    return mino_int(S, n);
}

static mino_val *prim_double_parse_double(mino_state *S, mino_val *args,
                                           mino_env *env)
{
    mino_val *s;
    char     *endp = NULL;
    double    d;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Double/parseDouble requires a string argument");
    }
    s = args->as.cons.car;
    if (mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Double/parseDouble: string argument required");
    }
    d = strtod(s->as.s.data, &endp);
    if (endp == s->as.s.data) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Double/parseDouble: unparseable input");
    }
    return mino_float(S, d);
}

static mino_val *prim_double_is_nan_jvm(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Double/isNaN requires one argument");
    }
    v = args->as.cons.car;
    if (mino_type_of(v) != MINO_FLOAT && mino_type_of(v) != MINO_FLOAT32) {
        return mino_false(S);
    }
    return isnan(v->as.f) ? mino_true(S) : mino_false(S);
}

static mino_val *prim_double_is_infinite_jvm(mino_state *S, mino_val *args,
                                               mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Double/isInfinite requires one argument");
    }
    v = args->as.cons.car;
    if (mino_type_of(v) != MINO_FLOAT && mino_type_of(v) != MINO_FLOAT32) {
        return mino_false(S);
    }
    return isinf(v->as.f) ? mino_true(S) : mino_false(S);
}

/* Math/abs accepts an int or a double and preserves the type, matching
 * JVM's overload-resolution behavior. Math/min and Math/max likewise. */
static mino_val *prim_jvm_math_abs(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Math/abs requires one argument");
    }
    v = args->as.cons.car;
    if (mino_val_int_p(v)) {
        long long n = mino_val_int_get(v);
        return mino_int(S, n < 0 ? -n : n);
    }
    if (mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32) {
        return mino_float(S, fabs(v->as.f));
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "Math/abs: numeric argument required");
}

static int coerce_to_double(const mino_val *v, double *out)
{
    long long ll;
    if (mino_val_int_p((mino_val *)v)) {
        *out = (double)mino_val_int_get((mino_val *)v);
        return 1;
    }
    if (mino_type_of((mino_val *)v) == MINO_FLOAT
        || mino_type_of((mino_val *)v) == MINO_FLOAT32) {
        *out = v->as.f;
        return 1;
    }
    if (as_long((mino_val *)v, &ll)) {
        *out = (double)ll;
        return 1;
    }
    return 0;
}

static mino_val *prim_jvm_math_min(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val *va, *vb;
    double      a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Math/min requires two arguments");
    }
    va = args->as.cons.car;
    vb = args->as.cons.cdr->as.cons.car;
    /* Integer-only fast path preserves the long type. */
    if (mino_val_int_p(va) && mino_val_int_p(vb)) {
        long long aa = mino_val_int_get(va);
        long long bb = mino_val_int_get(vb);
        return mino_int(S, aa < bb ? aa : bb);
    }
    if (!coerce_to_double(va, &a) || !coerce_to_double(vb, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Math/min: numeric arguments required");
    }
    return mino_float(S, a < b ? a : b);
}

static mino_val *prim_jvm_math_max(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val *va, *vb;
    double      a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Math/max requires two arguments");
    }
    va = args->as.cons.car;
    vb = args->as.cons.cdr->as.cons.car;
    if (mino_val_int_p(va) && mino_val_int_p(vb)) {
        long long aa = mino_val_int_get(va);
        long long bb = mino_val_int_get(vb);
        return mino_int(S, aa > bb ? aa : bb);
    }
    if (!coerce_to_double(va, &a) || !coerce_to_double(vb, &b)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Math/max: numeric arguments required");
    }
    return mino_float(S, a > b ? a : b);
}

/* JVM's Math.round returns a long; mino's clojure.math/round returns
 * a double via libm's round(). The JVM-canon entry needs its own
 * wrapper. */
static mino_val *prim_jvm_math_round(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    mino_val *v;
    double      x;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Math/round requires one argument");
    }
    v = args->as.cons.car;
    if (!coerce_to_double(v, &x)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Math/round: numeric argument required");
    }
    return mino_int(S, (long long)floor(x + 0.5));
}

/* Boolean/parseBoolean: case-insensitive "true" → true, else false. */
static mino_val *prim_boolean_parse_boolean(mino_state *S, mino_val *args,
                                             mino_env *env)
{
    mino_val *v;
    const char *s;
    size_t      n;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "Boolean/parseBoolean requires a string argument");
    }
    v = args->as.cons.car;
    if (mino_type_of(v) == MINO_NIL) return mino_false(S);
    if (mino_type_of(v) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "Boolean/parseBoolean: string argument required");
    }
    s = v->as.s.data;
    n = v->as.s.len;
    if (n != 4) return mino_false(S);
    return ((s[0] == 't' || s[0] == 'T') &&
            (s[1] == 'r' || s[1] == 'R') &&
            (s[2] == 'u' || s[2] == 'U') &&
            (s[3] == 'e' || s[3] == 'E')) ? mino_true(S) : mino_false(S);
}

/* java.util.{List,Set,Map}/of route to existing collection constructors. */
static mino_val *prim_java_util_list_of(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    return prim_list(S, args, env);
}

static mino_val *prim_java_util_set_of(mino_state *S, mino_val *args,
                                         mino_env *env)
{
    return prim_hash_set(S, args, env);
}

static mino_val *prim_java_util_map_of(mino_state *S, mino_val *args,
                                         mino_env *env)
{
    return prim_hash_map(S, args, env);
}

static mino_val *prim_string_value_of(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    return prim_str(S, args, env);
}

static mino_val *prim_character_to_string(mino_state *S, mino_val *args,
                                            mino_env *env)
{
    return prim_str(S, args, env);
}

/* ------------------------------------------------------------------------- */
/* Layer 2: embedded-host semantic remap                                    */
/* ------------------------------------------------------------------------- */

static mino_val *jvm_remap_current_time_millis(mino_state *S,
                                                 mino_val *args,
                                                 mino_env *env)
{
    /* JVM contract: epoch millis as a long. mino's `time-ms` returns
     * monotonic ms as a double, which fails `(integer? ...)` and
     * isn't wall-clock anyway. Compute the value directly from the
     * host wall clock so the integer-returning JVM contract holds. */
    long long ms;
    (void)args; (void)env;
#ifdef _WIN32
    {
        FILETIME ft;
        ULARGE_INTEGER u;
        GetSystemTimeAsFileTime(&ft);
        u.LowPart  = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        /* FILETIME is 100ns ticks since 1601-01-01. Convert to ms
         * since the Unix epoch (1970-01-01) by subtracting the
         * 11644473600 second offset and scaling. */
        ms = (long long)(u.QuadPart / 10000ULL)
             - 11644473600000LL;
    }
#else
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ms = (long long)ts.tv_sec * 1000LL
             + (long long)(ts.tv_nsec / 1000000L);
    }
#endif
    return mino_int(S, ms);
}

static mino_val *jvm_remap_nano_time(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    (void)args;
    return prim_nano_time(S, mino_nil(S), env);
}

static mino_val *jvm_remap_getenv(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    return prim_getenv(S, args, env);
}

static mino_val *jvm_remap_exit(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    return prim_exit(S, args, env);
}

static mino_val *jvm_remap_get_property(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    /* mino doesn't have a JVM properties table; surface
     * :mino/unsupported rather than fake an empty answer. */
    (void)args; (void)env;
    return prim_throw_classified(S, "host", "MHO001",
        "System/getProperty: mino has no JVM properties table; "
        "use System/getenv or the embedder's capability surface");
}

static mino_val *jvm_remap_thread_sleep(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    return prim_thread_sleep(S, args, env);
}

static mino_val *jvm_remap_random_uuid(mino_state *S, mino_val *args,
                                         mino_env *env)
{
    (void)args;
    return prim_random_uuid(S, mino_nil(S), env);
}

static mino_val *jvm_remap_parse_uuid(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    return prim_parse_uuid(S, args, env);
}

/* ------------------------------------------------------------------------- */
/* Install table + entry point                                              */
/* ------------------------------------------------------------------------- */

static const mino_prim_def k_prims_jvm_statics[] = {
    /* Layer 1 -- parsers / predicates */
    {"Long/parseLong",       prim_long_parse_long,
     "Parses an integer string. JVM Long static."},
    {"Integer/parseInt",     prim_long_parse_long,
     "Alias for Long/parseLong; mino has one integer tier."},
    {"Double/parseDouble",   prim_double_parse_double,
     "Parses a floating-point string. JVM Double static."},
    {"Float/parseFloat",     prim_double_parse_double,
     "Alias for Double/parseDouble; mino has one float tier."},
    {"Double/isNaN",         prim_double_is_nan_jvm,
     "True when the argument is NaN."},
    {"Double/isInfinite",    prim_double_is_infinite_jvm,
     "True when the argument is +inf or -inf."},
    {"Boolean/parseBoolean", prim_boolean_parse_boolean,
     "Parses \"true\" (case-insensitive) to true; everything else to false."},

    /* Layer 1 -- Math methods routed through the existing
     * clojure.math C prims so Math/sqrt and clojure.math/sqrt have
     * identical behavior. */
    {"Math/sqrt",   prim_math_sqrt,   "Square root."},
    {"Math/floor",  prim_math_floor,  "Floor (rounds toward negative infinity)."},
    {"Math/ceil",   prim_math_ceil,   "Ceiling (rounds toward positive infinity)."},
    {"Math/log",    prim_math_log,    "Natural log."},
    {"Math/log10",  prim_math_log10,  "Base-10 log."},
    {"Math/exp",    prim_math_exp,    "e^x."},
    {"Math/sin",    prim_math_sin,    "Sine (radians)."},
    {"Math/cos",    prim_math_cos,    "Cosine (radians)."},
    {"Math/tan",    prim_math_tan,    "Tangent (radians)."},
    {"Math/atan",   prim_math_atan,   "Arctangent."},
    {"Math/atan2",  prim_math_atan2,  "Two-argument arctangent."},
    {"Math/pow",    prim_math_pow,    "Exponentiation."},
    /* JVM-canon variants of round/abs/min/max are different shape
     * (different return-type contract or no clojure.math equivalent),
     * so they live here. */
    {"Math/round",  prim_jvm_math_round, "Round to nearest long."},
    {"Math/abs",    prim_jvm_math_abs,   "Absolute value (preserves int/double type)."},
    {"Math/min",    prim_jvm_math_min,   "Numeric minimum of two values."},
    {"Math/max",    prim_jvm_math_max,   "Numeric maximum of two values."},

    /* Layer 1 -- java.util collection factories */
    {"java.util.List/of",  prim_java_util_list_of,
     "JVM List.of static; routes to mino's list constructor."},
    {"java.util.Set/of",   prim_java_util_set_of,
     "JVM Set.of static; routes to mino's hash-set constructor."},
    {"java.util.Map/of",   prim_java_util_map_of,
     "JVM Map.of static; routes to mino's hash-map constructor."},

    /* Layer 1 -- String / Character coercion */
    {"String/valueOf",        prim_string_value_of,
     "JVM String.valueOf; routes to mino's str."},
    {"Character/toString",    prim_character_to_string,
     "JVM Character.toString; routes to mino's str."},

    /* Layer 2 -- embedded-host semantic remap */
    {"System/currentTimeMillis", jvm_remap_current_time_millis,
     "Epoch millis from the host clock."},
    {"System/nanoTime",          jvm_remap_nano_time,
     "Monotonic nanosecond counter from the host clock."},
    {"System/getenv",            jvm_remap_getenv,
     "Reads an environment variable from the host process."},
    {"System/exit",              jvm_remap_exit,
     "Exits the host process with the given status code."},
    {"System/getProperty",       jvm_remap_get_property,
     "JVM system-properties lookup. mino has no JVM properties table; "
     "throws :mino/unsupported."},
    {"Thread/sleep",             jvm_remap_thread_sleep,
     "Suspends the current thread for the given number of milliseconds."},
    {"java.util.UUID/randomUUID", jvm_remap_random_uuid,
     "Generates a random UUID v4."},
    {"java.util.UUID/fromString", jvm_remap_parse_uuid,
     "Parses a UUID from its canonical string form."},
};

static const size_t k_prims_jvm_statics_count =
    sizeof(k_prims_jvm_statics) / sizeof(k_prims_jvm_statics[0]);

void mino_install_jvm_statics(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    /* Function-shaped statics share the normal install table path. */
    prim_install_table(S, core_env, "clojure.core",
                       k_prims_jvm_statics, k_prims_jvm_statics_count);

    /* Value-shaped statics: literal slash-name bindings to a fresh
     * value cell; the env's literal-binding probe in
     * eval_qualified_symbol finds them on lookup. */
    env_bind(S, core_env, "Long/MAX_VALUE",    mino_int(S, 9223372036854775807LL));
    env_bind(S, core_env, "Long/MIN_VALUE",    mino_int(S, (long long)(-9223372036854775807LL - 1)));
    env_bind(S, core_env, "Integer/MAX_VALUE", mino_int(S, 2147483647LL));
    env_bind(S, core_env, "Integer/MIN_VALUE", mino_int(S, -2147483648LL));
    env_bind(S, core_env, "Short/MAX_VALUE",   mino_int(S, 32767));
    env_bind(S, core_env, "Short/MIN_VALUE",   mino_int(S, -32768));
    env_bind(S, core_env, "Byte/MAX_VALUE",    mino_int(S, 127));
    env_bind(S, core_env, "Byte/MIN_VALUE",    mino_int(S, -128));
    env_bind(S, core_env, "Double/MAX_VALUE",  mino_float(S, 1.7976931348623157e+308));
    env_bind(S, core_env, "Double/MIN_VALUE",  mino_float(S, 4.9e-324));
    env_bind(S, core_env, "Double/POSITIVE_INFINITY", mino_float(S, 1.0 / 0.0));
    env_bind(S, core_env, "Double/NEGATIVE_INFINITY", mino_float(S, -1.0 / 0.0));
    env_bind(S, core_env, "Double/NaN",        mino_float(S, 0.0 / 0.0));
    env_bind(S, core_env, "Float/MAX_VALUE",   mino_float(S, 3.4028235e+38));
    env_bind(S, core_env, "Float/MIN_VALUE",   mino_float(S, 1.4e-45));
    env_bind(S, core_env, "Float/POSITIVE_INFINITY", mino_float(S, 1.0 / 0.0));
    env_bind(S, core_env, "Float/NEGATIVE_INFINITY", mino_float(S, -1.0 / 0.0));
    env_bind(S, core_env, "Float/NaN",         mino_float(S, 0.0 / 0.0));
    env_bind(S, core_env, "Math/PI",           mino_float(S, 3.141592653589793));
    env_bind(S, core_env, "Math/E",            mino_float(S, 2.718281828459045));
    env_bind(S, core_env, "Boolean/TRUE",      mino_true(S));
    env_bind(S, core_env, "Boolean/FALSE",     mino_false(S));
}
