(ns clojure.math
  "Mathematical functions matching the Java `java.lang.Math` API
   exposed by Clojure 1.11+. Thin wrappers around mino's `math-*`
   C primitives so portable Clojure code that calls
   `clojure.math/sqrt`, `clojure.math/log`, etc. just works.")

;; --- Constants ---------------------------------------------------------------

(def PI 3.141592653589793)
(def E  2.718281828459045)

;; --- Trigonometry ------------------------------------------------------------

(defn sin  "Returns the sine of n (in radians)." [n]
  (math-sin n))

(defn cos  "Returns the cosine of n (in radians)." [n]
  (math-cos n))

(defn tan  "Returns the tangent of n (in radians)." [n]
  (math-tan n))

(defn asin "Returns the arc-sine of n (-1 <= n <= 1); result in [-PI/2, PI/2]." [n]
  (math-asin n))

(defn acos "Returns the arc-cosine of n (-1 <= n <= 1); result in [0, PI]." [n]
  (math-acos n))

(defn atan "Returns the arc-tangent of n; result in [-PI/2, PI/2]." [n]
  (math-atan n))

(defn atan2 "Returns atan2(y, x): the angle in radians between the positive x-axis and (x, y)." [y x]
  (math-atan2 y x))

(defn sinh "Returns the hyperbolic sine of n." [n] (math-sinh n))
(defn cosh "Returns the hyperbolic cosine of n." [n] (math-cosh n))
(defn tanh "Returns the hyperbolic tangent of n." [n] (math-tanh n))

;; --- Logarithms / exponentials -----------------------------------------------

(defn sqrt    "Returns the square root of n." [n] (math-sqrt n))
(defn cbrt    "Returns the cube root of n." [n]   (math-cbrt n))
(defn log     "Returns the natural logarithm (base e) of n." [n] (math-log n))
(defn log10   "Returns the base-10 logarithm of n." [n] (math-log10 n))
(defn log1p   "Returns ln(1 + n), accurate for small n." [n] (math-log1p n))
(defn exp     "Returns e^n." [n] (math-exp n))
(defn expm1   "Returns exp(n) - 1, accurate for small n." [n] (math-expm1 n))
(defn pow     "Returns base raised to the power of exp." [base exp] (math-pow base exp))

;; --- Rounding ----------------------------------------------------------------

(defn floor "Returns the largest double <= n and equal to a mathematical integer." [n]
  (math-floor n))

(defn ceil  "Returns the smallest double >= n and equal to a mathematical integer." [n]
  (math-ceil n))

(defn round "Returns the closest long to n, rounding half-up away from zero." [n]
  (math-round n))

(defn rint "Returns the double closest to n and equal to a mathematical
   integer, rounding ties to the even integer." [n]
  (math-rint n))

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
  "Returns the sum of x and y; throws on long overflow."
  [x y]
  (if (and (int? x) (int? y))
    (+ x y)
    (throw (ex-info "add-exact expects longs" {:x x :y y}))))

(defn subtract-exact
  "Returns the difference of x and y; throws on long overflow."
  [x y]
  (if (and (int? x) (int? y))
    (- x y)
    (throw (ex-info "subtract-exact expects longs" {:x x :y y}))))

(defn multiply-exact
  "Returns the product of x and y; throws on long overflow."
  [x y]
  (if (and (int? x) (int? y))
    (* x y)
    (throw (ex-info "multiply-exact expects longs" {:x x :y y}))))

(defn increment-exact
  "Returns x incremented by 1; throws on long overflow."
  [x]
  (if (int? x)
    (inc x)
    (throw (ex-info "increment-exact expects a long" {:x x}))))

(defn decrement-exact
  "Returns x decremented by 1; throws on long overflow."
  [x]
  (if (int? x)
    (dec x)
    (throw (ex-info "decrement-exact expects a long" {:x x}))))

(defn negate-exact
  "Returns the negation of x; throws on long overflow."
  [x]
  (if (int? x)
    (- x)
    (throw (ex-info "negate-exact expects a long" {:x x}))))

;; --- Angle conversion --------------------------------------------------------

(defn to-radians "Converts the angle a (in degrees) to radians." [a] (math-to-radians a))
(defn to-degrees "Converts the angle a (in radians) to degrees." [a] (math-to-degrees a))

;; --- Misc --------------------------------------------------------------------

(defn signum "Returns -1.0, 0.0, or 1.0 depending on the sign of n; preserves -0.0." [n]
  (math-signum n))

(defn hypot  "Returns sqrt(a^2 + b^2) avoiding intermediate overflow." [a b]
  (math-hypot a b))

(defn copy-sign "Returns a value with the magnitude of mag and the sign of sgn." [mag sgn]
  (math-copy-sign mag sgn))

(defn next-up   "Returns the next representable double greater than n." [n]
  (math-next-up n))

(defn next-down "Returns the next representable double less than n." [n]
  (math-next-down n))

(defn next-after "Returns the double adjacent to start in the direction
   of direction." [start direction]
  (math-next-after start direction))

(defn ulp "Returns the size of an ulp (unit in last place) of n." [n]
  (math-ulp n))

(defn scalb
  "Returns n scaled by 2 to the power of the integer scale-factor:
   n * 2^scale-factor, computed as a single rounding."
  [n scale-factor]
  (math-scalb n scale-factor))

(defn get-exponent
  "Returns the unbiased binary exponent of n. Zero and subnormals
   report MIN_EXPONENT - 1 (-1023); NaN and infinities report
   MAX_EXPONENT + 1 (1024)."
  [n]
  (math-get-exponent n))

(defn IEEE-remainder "Returns the IEEE 754 remainder of a/b." [a b]
  (math-ieee-remainder a b))

(defn random
  "Returns a positive double between 0.0 (inclusive) and 1.0 (exclusive),
   chosen pseudorandomly with approximately uniform distribution."
  []
  (rand))
