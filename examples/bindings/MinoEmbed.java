/*
 * MinoEmbed.java — embedding mino from Java via JNI.
 *
 * A thin JNI bridge exposes the core mino operations: create a
 * runtime, evaluate code, and extract results. The same event
 * processing scenario runs through the mino script.
 *
 * Build:
 *   make
 *   # Compile Java
 *   javac -d examples/bindings examples/bindings/MinoEmbed.java
 *   # Build the JNI shared library
 *   cc -std=c99 -shared -fPIC -Isrc \
 *      -I"$(java -XshowSettings:properties 2>&1 | grep java.home | awk '{print $3}')/include" \
 *      -I"$(java -XshowSettings:properties 2>&1 | grep java.home | awk '{print $3}')/include/darwin" \
 *      -o examples/bindings/libminojni.dylib \
 *      examples/bindings/mino_jni.c src/[a-z]*.o -lm
 *   # Run
 *   java -Djava.library.path=examples/bindings -cp examples/bindings MinoEmbed
 */

public class MinoEmbed {

    /* Native methods bridging to the mino C API. */
    private static native long   stateNew();
    private static native long   envNew(long state);
    private static native void   envFree(long state, long env);
    private static native void   stateFree(long state);
    private static native String evalString(long state, String src, long env);
    private static native String lastError(long state);

    static {
        System.loadLibrary("minojni");
    }

    /* Processing script: same as the C and C++ examples. */
    private static final String SCRIPT =
        "(defn avg [xs]\n" +
        "  (/ (reduce + xs) (count xs)))\n" +
        "\n" +
        "(defn summarize [[device readings]]\n" +
        "  [device {:count (count readings)\n" +
        "           :avg   (avg (map :value readings))}])\n" +
        "\n" +
        "(->> events\n" +
        "     (filter #(= (:type %) :temp))\n" +
        "     (group-by :device)\n" +
        "     (map summarize)\n" +
        "     (into (sorted-map)))\n";

    /* Event data built as a mino vector literal. */
    private static final String EVENTS =
        "(def events\n" +
        "  [{:type :temp     :device \"sensor-01\" :value 21.3 :ts 1000}\n" +
        "   {:type :humidity :device \"sensor-01\" :value 45.0 :ts 1001}\n" +
        "   {:type :temp     :device \"sensor-02\" :value 19.8 :ts 1002}\n" +
        "   {:type :temp     :device \"sensor-01\" :value 22.1 :ts 1003}\n" +
        "   {:type :temp     :device \"sensor-02\" :value 20.4 :ts 1004}\n" +
        "   {:type :temp     :device \"sensor-01\" :value 22.9 :ts 1005}])\n";

    public static void main(String[] args) {
        long state = stateNew();
        long env   = envNew(state);

        /* Define event data from a mino literal. */
        String r = evalString(state, EVENTS, env);
        if (r == null) {
            System.err.println("error: " + lastError(state));
            return;
        }

        /* Run the processing script. */
        String result = evalString(state, SCRIPT, env);
        if (result != null) {
            System.out.println("result: " + result);
        } else {
            System.err.println("error: " + lastError(state));
        }

        envFree(state, env);
        stateFree(state);
    }
}
