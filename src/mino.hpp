/*
 * mino.hpp -- thin C++ RAII wrappers over the C API.
 *
 * Header-only. Include after mino.h:
 *
 *   #include "mino.h"
 *   #include "mino.hpp"
 *
 * The wrappers cover the three common ownership patterns:
 *
 *   mino::state   -- owning handle for a mino_state.
 *   mino::env     -- owning handle for a mino_env created in a state.
 *   mino::pin     -- GC-rooted handle for a mino_val; releases on destruct.
 *
 * Everything else stays in the C API. Construct values via the C
 * constructors (mino_int, mino_string, ...), pass them where the C
 * API expects mino_val *, and only wrap in a `pin` when you need to
 * keep a value alive across a GC boundary.
 *
 * Move-only by design. No reference counting, no implicit copy. If
 * you need to share, copy the underlying mino_val * (which is just
 * a pointer) and pin separately on each side.
 *
 * Designed to compile against C++14 and later with no external
 * dependencies beyond the C API. Exception-safe: every destructor
 * is noexcept and tolerates a NULL underlying pointer.
 */

#ifndef MINO_HPP
#define MINO_HPP

#include "mino.h"

#include <stdexcept>
#include <string>

namespace mino {

/* ----------------------------------------------------------------- */
/* exception                                                          */
/* ----------------------------------------------------------------- */

/* Thrown by the helpers in this header when a C-API call returns
 * a failure indicator (NULL return + mino_last_error set, or -1 from
 * an _ex call). Carries the diagnostic text mino_last_error
 * returned. Catch by reference if you don't need to copy. */
class error : public std::runtime_error {
public:
    explicit error(const std::string& msg)
        : std::runtime_error(msg) {}
};

/* ----------------------------------------------------------------- */
/* state                                                              */
/* ----------------------------------------------------------------- */

class state {
public:
    state() : ptr_(mino_state_new()) {
        if (ptr_ == nullptr) {
            throw error("mino_state_new failed");
        }
    }
    ~state() noexcept {
        if (ptr_ != nullptr) {
            mino_state_free(ptr_);
        }
    }

    state(const state&)            = delete;
    state& operator=(const state&) = delete;

    state(state&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    state& operator=(state&& other) noexcept {
        if (this != &other) {
            if (ptr_ != nullptr) mino_state_free(ptr_);
            ptr_       = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    mino_state* raw() const noexcept { return ptr_; }
    operator mino_state*() const noexcept { return ptr_; }

private:
    mino_state* ptr_;
};

/* ----------------------------------------------------------------- */
/* env                                                                */
/* ----------------------------------------------------------------- */

class env {
public:
    /* Construct a fresh env in `s`. The state must outlive the env;
     * destructing the env frees its frame and releases any closures
     * reachable only through it. */
    explicit env(state& s) : state_(&s), ptr_(mino_env_new(s)) {
        if (ptr_ == nullptr) {
            throw error("mino_env_new failed");
        }
    }

    /* Wrap an existing env pointer without taking ownership. Useful
     * when the env was constructed via mino_env_new_default and
     * the caller wants to drive eval through a C++-facing API. */
    static env borrow(state& s, mino_env* e) noexcept {
        env out;
        out.state_  = &s;
        out.ptr_    = e;
        out.owning_ = false;
        return out;
    }

    ~env() noexcept {
        if (owning_ && ptr_ != nullptr && state_ != nullptr) {
            mino_env_free(*state_, ptr_);
        }
    }

    env(const env&)            = delete;
    env& operator=(const env&) = delete;

    env(env&& other) noexcept
        : state_(other.state_),
          ptr_(other.ptr_),
          owning_(other.owning_)
    {
        other.ptr_    = nullptr;
        other.owning_ = false;
    }
    env& operator=(env&& other) noexcept {
        if (this != &other) {
            if (owning_ && ptr_ != nullptr && state_ != nullptr) {
                mino_env_free(*state_, ptr_);
            }
            state_        = other.state_;
            ptr_          = other.ptr_;
            owning_       = other.owning_;
            other.ptr_    = nullptr;
            other.owning_ = false;
        }
        return *this;
    }

    mino_env* raw() const noexcept { return ptr_; }
    operator mino_env*() const noexcept { return ptr_; }

    /* Convenience for the common (set, get) cases. */
    void set(const char* name, mino_val* val) noexcept {
        if (state_ != nullptr) mino_env_set(*state_, ptr_, name, val);
    }
    mino_val* get(const char* name) const noexcept {
        return mino_env_get(ptr_, name);
    }

private:
    env() : state_(nullptr), ptr_(nullptr), owning_(false) {}

    state*   state_;
    mino_env* ptr_;
    bool      owning_ = true;
};

/* ----------------------------------------------------------------- */
/* pin -- GC-rooted value handle                                      */
/* ----------------------------------------------------------------- */

/* Wraps a mino_ref so the underlying value survives garbage
 * collection. Use when you need to keep a mino_val * alive across
 * eval calls or other points where GC may run. Destructing the pin
 * releases the root; the value may then be collected.
 *
 * Move-only. `release` extracts the raw mino_ref* if the caller
 * wants to hand it back to the C API; the wrapper becomes empty. */
class pin {
public:
    pin() : state_(nullptr), ref_(nullptr) {}

    pin(state& s, mino_val* v)
        : state_(&s), ref_(mino_ref_new(s, v))
    {
        if (ref_ == nullptr) {
            throw error("mino_ref_new failed");
        }
    }

    ~pin() noexcept {
        if (ref_ != nullptr && state_ != nullptr) {
            mino_unref(*state_, ref_);
        }
    }

    pin(const pin&)            = delete;
    pin& operator=(const pin&) = delete;

    pin(pin&& other) noexcept
        : state_(other.state_), ref_(other.ref_)
    {
        other.state_ = nullptr;
        other.ref_   = nullptr;
    }
    pin& operator=(pin&& other) noexcept {
        if (this != &other) {
            if (ref_ != nullptr && state_ != nullptr) {
                mino_unref(*state_, ref_);
            }
            state_       = other.state_;
            ref_         = other.ref_;
            other.state_ = nullptr;
            other.ref_   = nullptr;
        }
        return *this;
    }

    mino_val* deref() const noexcept {
        return ref_ != nullptr ? mino_deref(ref_) : nullptr;
    }
    operator mino_val*() const noexcept { return deref(); }

    mino_ref* release() noexcept {
        mino_ref* r = ref_;
        ref_   = nullptr;
        state_ = nullptr;
        return r;
    }

    bool empty() const noexcept { return ref_ == nullptr; }

private:
    state*    state_;
    mino_ref* ref_;
};

/* ----------------------------------------------------------------- */
/* eval helpers                                                       */
/* ----------------------------------------------------------------- */

/* mino_eval_string with C++-shape error handling. Returns the result
 * on success; throws mino::error with the diagnostic text on failure.
 * The returned mino_val * is a borrowed pointer (caller may pin it). */
inline mino_val* eval_string(state& s, env& e, const char* src) {
    mino_val* out = mino_eval_string(s, src, e);
    if (out == nullptr) {
        const char* msg = mino_last_error(s);
        throw error(msg != nullptr ? msg : "mino_eval_string failed");
    }
    return out;
}

/* mino_load_file with C++-shape error handling. */
inline mino_val* load_file(state& s, env& e, const char* path) {
    mino_val* out = mino_load_file(s, path, e);
    if (out == nullptr) {
        const char* msg = mino_last_error(s);
        throw error(msg != nullptr ? msg : "mino_load_file failed");
    }
    return out;
}

/* Print a value to a heap-allocated std::string. Routes through
 * mino_print_to_buf with a 4 KiB working buffer that grows on
 * overflow. Useful for embedding mino output in C++ host messages
 * without piping through stdout. */
inline std::string print_to_string(state& s, mino_val* v) {
    char     small[4096];
    int      n = mino_print_to_buf(s, v, small, sizeof small);
    if (n < 0) return std::string();
    if ((size_t)n + 1u < sizeof small) {
        return std::string(small, (size_t)n);
    }
    /* Output may have been truncated. Grow the buffer and retry. */
    size_t cap = sizeof small * 2u;
    for (int i = 0; i < 8; i++) {
        std::string buf(cap, '\0');
        n = mino_print_to_buf(s, v, &buf[0], cap);
        if (n < 0) return std::string();
        if ((size_t)n + 1u < cap) {
            buf.resize((size_t)n);
            return buf;
        }
        cap *= 2u;
    }
    /* Give up; return the latest truncated form. */
    std::string buf(cap, '\0');
    n = mino_print_to_buf(s, v, &buf[0], cap);
    if (n < 0) return std::string();
    buf.resize((size_t)n < cap ? (size_t)n : cap - 1);
    return buf;
}

} /* namespace mino */

#endif /* MINO_HPP */
