(ns clojure.math
  "Mathematical functions matching the Java `java.lang.Math` API
   exposed by Clojure 1.11+. Thin wrappers around mino's `math-*`
   C primitives so portable Clojure code that calls
   `clojure.math/sqrt`, `clojure.math/log`, etc. just works.")

;; --- Constants ---------------------------------------------------------------

(def PI 3.141592653589793)
(def E  2.718281828459045)

;; --- Trigonometry ------------------------------------------------------------

(defn sin  "Returns the sine of n (in radians)." [a]
  (math-sin a))

(defn cos  "Returns the cosine of n (in radians)." [a]
  (math-cos a))

(defn tan  "Returns the tangent of n (in radians)." [a]
  (math-tan a))

(defn asin "Returns the arc-sine of n (-1 <= n <= 1); result in [-PI/2, PI/2]." [a]
  (math-asin a))

(defn acos "Returns the arc-cosine of n (-1 <= n <= 1); result in [0, PI]." [a]
  (math-acos a))

(defn atan "Returns the arc-tangent of n; result in [-PI/2, PI/2]." [a]
  (math-atan a))

(defn atan2 "Returns atan2(y, x): the angle in radians between the positive x-axis and (x, y)." [y x]
  (math-atan2 y x))

(defn sinh "Returns the hyperbolic sine of n." [x] (math-sinh x))
(defn cosh "Returns the hyperbolic cosine of n." [x] (math-cosh x))
(defn tanh "Returns the hyperbolic tangent of n." [x] (math-tanh x))

;; --- Logarithms / exponentials -----------------------------------------------

(defn sqrt    "Returns the square root of n." [a] (math-sqrt a))
(defn cbrt    "Returns the cube root of n." [a]   (math-cbrt a))
(defn log     "Returns the natural logarithm (base e) of n." [a] (math-log a))
(defn log10   "Returns the base-10 logarithm of n." [a] (math-log10 a))
(defn log1p   "Returns ln(1 + n), accurate for small n." [x] (math-log1p x))
(defn exp     "Returns e^n." [a] (math-exp a))
(defn expm1   "Returns exp(n) - 1, accurate for small n." [x] (math-expm1 x))
(defn pow     "Returns base raised to the power of exp." [a b] (math-pow a b))

;; --- Rounding ----------------------------------------------------------------

(defn floor "Returns the largest double <= n and equal to a mathematical integer." [a]
  (math-floor a))

(defn ceil  "Returns the smallest double >= n and equal to a mathematical integer." [a]
  (math-ceil a))

(defn round "Returns the closest long to n, rounding half-up away from zero." [a]
  (math-round a))

(defn rint "Returns the double closest to n and equal to a mathematical
   integer, rounding ties to the even integer." [a]
  (math-rint a))

;; --- Integer division --------------------------------------------------------

(defn floor-div
  "Integer division rounding toward negative infinity: the largest
   integer <= the exact quotient of x and y."
  [x y]
  (if (and (int? x) (int? y))
    (quot (- x (mod x y)) y)
    (throw (ex-info "floor-div expects longs" {:x x :y y}))))

(defn floor-mod
  "Floor modulus of x and y: x - (floor-div x y) * y. Has the sign of
   the divisor y."
  [x y]
  (if (and (int? x) (int? y))
    (mod x y)
    (throw (ex-info "floor-mod expects longs" {:x x :y y}))))

;; --- Exact (overflow-checked) long arithmetic --------------------------------

(defn add-exact
  "Returns the sum of x and y. Note: unlike Clojure on the JVM, does
   not throw on long overflow -- promotes to bignum instead."
  [x y]
  (if (and (int? x) (int? y))
    (+ x y)
    (throw (ex-info "add-exact expects longs" {:x x :y y}))))

(defn subtract-exact
  "Returns the difference of x and y. Note: unlike Clojure on the JVM,
   does not throw on long overflow -- promotes to bignum instead."
  [x y]
  (if (and (int? x) (int? y))
    (- x y)
    (throw (ex-info "subtract-exact expects longs" {:x x :y y}))))

(defn multiply-exact
  "Returns the product of x and y. Note: unlike Clojure on the JVM,
   does not throw on long overflow -- promotes to bignum instead."
  [x y]
  (if (and (int? x) (int? y))
    (* x y)
    (throw (ex-info "multiply-exact expects longs" {:x x :y y}))))

(defn increment-exact
  "Returns x incremented by 1. Note: unlike Clojure on the JVM, does
   not throw on long overflow -- promotes to bignum instead."
  [a]
  (if (int? a)
    (inc a)
    (throw (ex-info "increment-exact expects a long" {:x a}))))

(defn decrement-exact
  "Returns x decremented by 1. Note: unlike Clojure on the JVM, does
   not throw on long overflow -- promotes to bignum instead."
  [a]
  (if (int? a)
    (dec a)
    (throw (ex-info "decrement-exact expects a long" {:x a}))))

(defn negate-exact
  "Returns the negation of x. Note: unlike Clojure on the JVM, does
   not throw on long overflow -- promotes to bignum instead."
  [a]
  (if (int? a)
    (- a)
    (throw (ex-info "negate-exact expects a long" {:x a}))))

;; --- Angle conversion --------------------------------------------------------

(defn to-radians "Converts the angle a (in degrees) to radians." [deg] (math-to-radians deg))
(defn to-degrees "Converts the angle a (in radians) to degrees." [r] (math-to-degrees r))

;; --- Misc --------------------------------------------------------------------

(defn signum "Returns -1.0, 0.0, or 1.0 depending on the sign of n; preserves -0.0." [d]
  (math-signum d))

(defn hypot  "Returns sqrt(a^2 + b^2) avoiding intermediate overflow." [x y]
  (math-hypot x y))

(defn copy-sign "Returns a value with the magnitude of mag and the sign of sgn." [magnitude sign]
  (math-copy-sign magnitude sign))

(defn next-up   "Returns the next representable double greater than n." [d]
  (math-next-up d))

(defn next-down "Returns the next representable double less than n." [d]
  (math-next-down d))

(defn next-after "Returns the double adjacent to start in the direction
   of direction." [start direction]
  (math-next-after start direction))

(defn ulp "Returns the size of an ulp (unit in last place) of n." [d]
  (math-ulp d))

(defn scalb
  "Returns n scaled by 2 to the power of the integer scale-factor:
   n * 2^scale-factor, computed as a single rounding."
  [d scaleFactor]
  (math-scalb d scaleFactor))

(defn get-exponent
  "Returns the unbiased binary exponent of n. Zero and subnormals
   report MIN_EXPONENT - 1 (-1023); NaN and infinities report
   MAX_EXPONENT + 1 (1024)."
  [d]
  (math-get-exponent d))

(defn IEEE-remainder "Returns the IEEE 754 remainder of a/b." [dividend divisor]
  (math-ieee-remainder dividend divisor))

(defn random
  "Returns a positive double between 0.0 (inclusive) and 1.0 (exclusive),
   chosen pseudorandomly with approximately uniform distribution."
  []
  (rand))
