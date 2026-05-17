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

(defn IEEE-remainder "Returns the IEEE 754 remainder of a/b." [a b]
  (math-ieee-remainder a b))
