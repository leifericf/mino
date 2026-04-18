/*
 * mino_jni.c — JNI bridge for the mino C API.
 *
 * Maps Java native methods in MinoEmbed to their mino equivalents.
 * State and env handles are passed as Java longs (opaque pointers).
 * eval_string returns the printed representation of the result.
 */

#include <jni.h>
#include "mino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Capture printed representation into a C string. */
static char print_buf[4096];

static void val_to_string(mino_state_t *S, const mino_val_t *v)
{
    FILE *f = tmpfile();
    if (!f) { print_buf[0] = '\0'; return; }
    mino_print_to(S, f, v);
    long len = ftell(f);
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(print_buf)) len = sizeof(print_buf) - 1;
    rewind(f);
    fread(print_buf, 1, (size_t)len, f);
    print_buf[len] = '\0';
    fclose(f);
}

/* Java_MinoEmbed_stateNew */
JNIEXPORT jlong JNICALL
Java_MinoEmbed_stateNew(JNIEnv *jenv, jclass cls)
{
    (void)jenv; (void)cls;
    return (jlong)(uintptr_t)mino_state_new();
}

/* Java_MinoEmbed_envNew */
JNIEXPORT jlong JNICALL
Java_MinoEmbed_envNew(JNIEnv *jenv, jclass cls, jlong state)
{
    (void)jenv; (void)cls;
    return (jlong)(uintptr_t)mino_new((mino_state_t *)(uintptr_t)state);
}

/* Java_MinoEmbed_envFree */
JNIEXPORT void JNICALL
Java_MinoEmbed_envFree(JNIEnv *jenv, jclass cls, jlong state, jlong env)
{
    (void)jenv; (void)cls;
    mino_env_free((mino_state_t *)(uintptr_t)state,
                  (mino_env_t *)(uintptr_t)env);
}

/* Java_MinoEmbed_stateFree */
JNIEXPORT void JNICALL
Java_MinoEmbed_stateFree(JNIEnv *jenv, jclass cls, jlong state)
{
    (void)jenv; (void)cls;
    mino_state_free((mino_state_t *)(uintptr_t)state);
}

/* Java_MinoEmbed_evalString */
JNIEXPORT jstring JNICALL
Java_MinoEmbed_evalString(JNIEnv *jenv, jclass cls,
                          jlong state, jstring src, jlong env)
{
    (void)cls;
    mino_state_t *S = (mino_state_t *)(uintptr_t)state;
    mino_env_t   *E = (mino_env_t *)(uintptr_t)env;
    const char *csrc = (*jenv)->GetStringUTFChars(jenv, src, NULL);

    mino_val_t *result = mino_eval_string(S, csrc, E);
    (*jenv)->ReleaseStringUTFChars(jenv, src, csrc);

    if (result == NULL)
        return NULL;

    /* Print result to buffer. */
    val_to_string(S, result);

    return (*jenv)->NewStringUTF(jenv, print_buf);
}

/* Java_MinoEmbed_lastError */
JNIEXPORT jstring JNICALL
Java_MinoEmbed_lastError(JNIEnv *jenv, jclass cls, jlong state)
{
    (void)cls;
    const char *err = mino_last_error((mino_state_t *)(uintptr_t)state);
    return err ? (*jenv)->NewStringUTF(jenv, err) : NULL;
}
