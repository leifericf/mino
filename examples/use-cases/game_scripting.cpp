/*
 * game_scripting.cpp — entity system with scripted behaviors.
 *
 * A game engine exposes its entity system through typed handles.
 * Scripts define behaviors, queries, and commands. Step limits
 * protect the host from runaway player-authored code. Entity
 * state is queried through getters, never mutated directly by
 * the script.
 *
 * Build:
 *   make
 *   c++ -std=c++17 -Isrc -o examples/use-cases/game_scripting \
 *       examples/use-cases/game_scripting.cpp src/[a-z]*.o -lm
 */

#include "mino.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

/* ── Expose ────────────────────────────────────────────────────────── */

/* Entities have a name, position, health, and tags. The C++ side
 * owns all mutation. Scripts read entity state through getters
 * and issue commands that the host validates and applies. */

struct Entity {
    std::string name;
    double x, y;
    int hp, max_hp;
    std::vector<std::string> tags;
};

static std::vector<Entity> world;

static int find_entity(const char *name, size_t len)
{
    for (size_t i = 0; i < world.size(); i++)
        if (world[i].name.size() == len &&
            strncmp(world[i].name.c_str(), name, len) == 0)
            return (int)i;
    return -1;
}

/* Return an entity as a mino map snapshot. */
static mino_val_t *entity_to_map(mino_state_t *S, const Entity &e)
{
    /* Build tags vector. */
    std::vector<mino_val_t *> tag_vals;
    for (auto &t : e.tags)
        tag_vals.push_back(mino_keyword(S, t.c_str()));
    mino_val_t *tags = mino_vector(S, tag_vals.data(), tag_vals.size());

    mino_val_t *ks[5], *vs[5];
    ks[0] = mino_keyword(S, "name"); vs[0] = mino_string(S, e.name.c_str());
    ks[1] = mino_keyword(S, "x");    vs[1] = mino_float(S, e.x);
    ks[2] = mino_keyword(S, "y");    vs[2] = mino_float(S, e.y);
    ks[3] = mino_keyword(S, "hp");   vs[3] = mino_int(S, e.hp);
    ks[4] = mino_keyword(S, "tags"); vs[4] = tags;
    return mino_map(S, ks, vs, 5);
}

/* Host function: look up entity by name, return snapshot. */
static mino_val_t *host_entity(mino_state_t *S, mino_val_t *args,
                               mino_env_t *)
{
    const char *name;
    size_t len;
    if (!mino_is_cons(args) ||
        !mino_to_string(mino_car(args), &name, &len))
        return mino_nil(S);
    int idx = find_entity(name, len);
    if (idx < 0) return mino_nil(S);
    return entity_to_map(S, world[(size_t)idx]);
}

/* Host function: list all entities as a vector of maps. */
static mino_val_t *host_entities(mino_state_t *S, mino_val_t *,
                                 mino_env_t *)
{
    std::vector<mino_ref_t *> refs;
    for (auto &e : world)
        refs.push_back(mino_ref(S, entity_to_map(S, e)));

    std::vector<mino_val_t *> items;
    for (auto *r : refs)
        items.push_back(mino_deref(r));
    mino_val_t *result = mino_vector(S, items.data(), items.size());
    for (auto *r : refs)
        mino_unref(S, r);
    return result;
}

/* Host function: move an entity by dx, dy. */
static mino_val_t *host_move(mino_state_t *S, mino_val_t *args,
                             mino_env_t *)
{
    const char *name;
    size_t len;
    if (!mino_is_cons(args) ||
        !mino_to_string(mino_car(args), &name, &len))
        return mino_nil(S);
    args = mino_cdr(args);
    long long dx = 0, dy = 0;
    if (mino_is_cons(args)) { mino_to_int(mino_car(args), &dx); args = mino_cdr(args); }
    if (mino_is_cons(args)) { mino_to_int(mino_car(args), &dy); }
    int idx = find_entity(name, len);
    if (idx < 0) return mino_nil(S);
    world[(size_t)idx].x += dx;
    world[(size_t)idx].y += dy;
    return entity_to_map(S, world[(size_t)idx]);
}

/* Host function: apply damage to an entity. */
static mino_val_t *host_damage(mino_state_t *S, mino_val_t *args,
                               mino_env_t *)
{
    const char *name;
    size_t len;
    if (!mino_is_cons(args) ||
        !mino_to_string(mino_car(args), &name, &len))
        return mino_nil(S);
    long long amount = 0;
    if (mino_is_cons(mino_cdr(args)))
        mino_to_int(mino_car(mino_cdr(args)), &amount);
    int idx = find_entity(name, len);
    if (idx < 0) return mino_nil(S);
    world[(size_t)idx].hp -= (int)amount;
    if (world[(size_t)idx].hp < 0) world[(size_t)idx].hp = 0;
    return mino_int(S, world[(size_t)idx].hp);
}

/* ── Script ────────────────────────────────────────────────────────── */

/* Scripts define queries and commands over the entity system.
 * Keywords like `:hp` and `:tags` act as data accessors. Sets act
 * as predicates for tag matching. */

static const char *script =
    ";; Find entities within a radius of a point.\n"
    "(defn nearby [x y radius]\n"
    "  (let [r2 (* radius radius)]\n"
    "    (->> (all-entities)\n"
    "         (filter (fn [e]\n"
    "                   (let [dx (- (:x e) x)\n"
    "                         dy (- (:y e) y)]\n"
    "                     (<= (+ (* dx dx) (* dy dy)) r2)))))))\n"
    "\n"
    ";; Find all entities with a given tag.\n"
    "(defn tagged [tag]\n"
    "  (->> (all-entities)\n"
    "       (filter (fn [e] (some #{tag} (:tags e))))))\n"
    "\n"
    ";; Summary: name and HP of all hostile entities.\n"
    "(defn hostile-report []\n"
    "  (->> (tagged :hostile)\n"
    "       (map (fn [e] [(:name e) (:hp e)]))\n"
    "       (into (sorted-map))))\n"
    "\n"
    ";; Run a scripted scenario.\n"
    "(println \"--- entities ---\")\n"
    "(println (mapv :name (all-entities)))\n"
    "\n"
    "(println \"\\n--- nearby (0,0) r=15 ---\")\n"
    "(println (mapv :name (nearby 0 0 15)))\n"
    "\n"
    "(println \"\\n--- hostile report ---\")\n"
    "(println (hostile-report))\n"
    "\n"
    "(move-entity \"goblin-1\" 5 0)\n"
    "(damage-entity \"goblin-1\" 30)\n"
    "\n"
    "(println \"\\n--- after combat ---\")\n"
    "(println (hostile-report))\n";

/* ── Embed ─────────────────────────────────────────────────────────── */

int main()
{
    /* Populate the game world. */
    world = {
        {"player",   0.0,   0.0, 100, 100, {"player", "friendly"}},
        {"goblin-1", 8.0,   3.0,  45,  45, {"hostile", "goblin"}},
        {"goblin-2", 25.0, 10.0,  40,  40, {"hostile", "goblin"}},
        {"merchant", 5.0,  -2.0,  80,  80, {"friendly", "npc"}},
        {"dragon",   50.0, 50.0, 500, 500, {"hostile", "boss"}},
    };

    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    /* Step limit: protect against runaway player scripts. */
    mino_set_limit(S, MINO_LIMIT_STEPS, 100000);

    /* Register entity functions. */
    mino_register_fn(S, env, "entity",        host_entity);
    mino_register_fn(S, env, "all-entities",  host_entities);
    mino_register_fn(S, env, "move-entity",   host_move);
    mino_register_fn(S, env, "damage-entity", host_damage);

    mino_val_t *result = mino_eval_string(S, script, env);
    if (!result)
        fprintf(stderr, "error: %s\n", mino_last_error(S));

    mino_env_free(S, env);
    mino_state_free(S);
}
