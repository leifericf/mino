/*
 * prim/image.c -- Clojure-level primitives for save-lisp-and-die.
 *
 * Thin wrappers around mino_save_image / mino_load_image_into
 * so the image save/load API is callable from Clojure.
 */

#include "prim/internal.h"
#include "mino.h"

/* (save-image path) -> nil */
static mino_val *prim_save_image(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val *path_val;
    const char *path_str;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "save-image requires one argument");
    }
    path_val = args->as.cons.car;
    if (mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "save-image: path must be a string");
    }
    path_str = path_val->as.s.data;
    if (mino_save_image(S, path_str) != 0) return NULL;
    return mino_nil(S);
}

/* (load-image-into path) -> nil */
static mino_val *prim_load_image(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val *path_val;
    const char *path_str;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "load-image-into requires one argument");
    }
    path_val = args->as.cons.car;
    if (mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "load-image-into: path must be a string");
    }
    path_str = path_val->as.s.data;
    if (mino_load_image_into(S, path_str) != 0) return NULL;
    return mino_nil(S);
}

static const mino_prim_def k_prims_image[] = {
    {"save-image",      prim_save_image,
     "Save the full runtime state to an image file."},
    {"load-image-into", prim_load_image,
     "Load an image file into the current state."},
};
static const size_t k_prims_image_count =
    sizeof(k_prims_image) / sizeof(k_prims_image[0]);

void mino_install_image_prims(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table(S, core_env, "clojure.core",
                       k_prims_image, k_prims_image_count);
}
