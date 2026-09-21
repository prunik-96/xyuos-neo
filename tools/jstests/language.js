// The engine, checked against itself. Every line here states what the
// language says should happen; anything that does not is printed.
//
// Written in the subset the engine claims to support, so running it is both
// the test and a demonstration of what that subset is.

var total = 0, fails = 0;

function eq(got, want, what) {
    total++;
    if (got !== want) {
        fails++;
        console.log("FAIL " + what + ": got <" + got + "> want <" + want + ">");
    }
}

// --- numbers and their printing --------------------------------------------
eq(1 + 1, 2, "addition");
eq(0.1 + 0.2 === 0.3, false, "floating point is floating point");
eq(String(1), "1", "integers print without a point");
eq(String(1.5), "1.5", "fractions print");
eq(String(1e21), "1e+21", "big numbers use exponents");
eq(String(-0.5), "-0.5", "negatives");
eq(String(1/3), "0.3333333333333333", "shortest round trip");
eq(String(NaN), "NaN", "NaN prints");
eq(String(1/0), "Infinity", "infinity prints");
eq(7 % 3, 1, "modulo");
eq(-7 % 3, -1, "modulo keeps the sign of the left");
eq(2 ** 10, 1024, "power");
eq(5 / 2, 2.5, "division is not integer division");
eq((1).toFixed(2), "1.00", "toFixed");
eq((255).toString(16), "ff", "toString with a radix");
eq(parseInt("42px"), 42, "parseInt stops at the first non-digit");
eq(parseInt("0x1f", 16), 31, "parseInt hex");
eq(parseFloat("3.5rem"), 3.5, "parseFloat");
eq(isNaN(parseInt("abc")), true, "parseInt of nonsense");

// --- strings ----------------------------------------------------------------
var s = "Hello, world";
eq(s.length, 12, "length");
eq(s[0], "H", "index");
eq(s.charAt(1), "e", "charAt");
eq(s.charCodeAt(0), 72, "charCodeAt");
eq(s.indexOf("world"), 7, "indexOf");
eq(s.indexOf("nope"), -1, "indexOf missing");
eq(s.slice(0, 5), "Hello", "slice");
eq(s.slice(-5), "world", "slice from the end");
eq(s.substring(7), "world", "substring");
eq(s.toUpperCase(), "HELLO, WORLD", "toUpperCase");
eq("  pad  ".trim(), "pad", "trim");
eq(s.replace("world", "there"), "Hello, there", "replace");
eq("a-b-c".replaceAll("-", "+"), "a+b+c", "replaceAll");
eq("a-b-c".split("-").length, 3, "split");
eq("a-b-c".split("-")[1], "b", "split element");
eq(s.includes("lo, w"), true, "includes");
eq(s.startsWith("Hell"), true, "startsWith");
eq(s.endsWith("rld"), true, "endsWith");
eq("ab".repeat(3), "ababab", "repeat");
eq("5".padStart(3, "0"), "005", "padStart");
eq("x" + 1, "x1", "concatenation converts");
eq(1 + "x", "1x", "and the other way");
eq("b" > "a", true, "strings compare");

// --- truth and equality -------------------------------------------------------
eq(1 == "1", true, "loose equality converts");
eq(1 === "1", false, "strict equality does not");
eq(null == undefined, true, "null and undefined are loosely equal");
eq(null === undefined, false, "but not strictly");
eq(!!"", false, "the empty string is false");
eq(!!"0", true, "but the string zero is true");
eq(!!0, false, "zero is false");
eq(!![], true, "an array is true");
eq(typeof undefined, "undefined", "typeof undefined");
eq(typeof null, "object", "typeof null is a famous mistake");
eq(typeof 1, "number", "typeof number");
eq(typeof "s", "string", "typeof string");
eq(typeof {}, "object", "typeof object");
eq(typeof eq, "function", "typeof function");
eq(typeof notDeclaredAnywhere, "undefined", "typeof of an unknown name");
eq(null ?? "d", "d", "nullish coalescing");
eq(0 ?? "d", 0, "nullish does not catch zero");
eq(0 || "d", "d", "or does");
eq(1 && 2, 2, "and returns the second");

// --- arrays -------------------------------------------------------------------
var a = [3, 1, 2];
eq(a.length, 3, "array length");
eq(a[0], 3, "array index");
a.push(4);
eq(a.length, 4, "push");
eq(a.pop(), 4, "pop returns");
eq(a.join("-"), "3-1-2", "join");
eq(a.slice(1).join(""), "12", "array slice");
eq(a.indexOf(1), 1, "array indexOf");
eq(a.includes(2), true, "array includes");
eq([1, 2, 3].map(function (x) { return x * 2; }).join(","), "2,4,6", "map");
eq([1, 2, 3, 4].filter(function (x) { return x % 2 === 0; }).join(","), "2,4", "filter");
eq([1, 2, 3].reduce(function (t, x) { return t + x; }, 0), 6, "reduce");
eq([1, 2, 3].reduce(function (t, x) { return t + x; }), 6, "reduce without a seed");
eq([1, 2, 3].find(function (x) { return x > 1; }), 2, "find");
eq([1, 2, 3].findIndex(function (x) { return x > 1; }), 1, "findIndex");
eq([1, 2, 3].some(function (x) { return x > 2; }), true, "some");
eq([1, 2, 3].every(function (x) { return x > 0; }), true, "every");
eq([3, 1, 2].sort().join(""), "123", "sort");
eq([3, 1, 20].sort(function (x, y) { return x - y; }).join(","), "1,3,20", "sort with a comparator");
eq([1, 2].concat([3]).length, 3, "concat");
eq([1, 2, 3].reverse().join(""), "321", "reverse");
eq([[1, 2], [3]].flat().join(""), "123", "flat");
eq([1, 2, 3].at(-1), 3, "at from the end");
eq(Array.isArray([]), true, "isArray");
eq(Array.from("abc").length, 3, "Array.from a string");
var sp = [1, 2, 3];
sp.splice(1, 1);
eq(sp.join(""), "13", "splice removes");
sp.splice(1, 0, 9, 9);
eq(sp.join(""), "1993", "splice inserts");
eq(sp.shift(), 1, "shift");
sp.unshift(0);
eq(sp.join(""), "0993", "unshift");

// --- objects ---------------------------------------------------------------------
var o = { a: 1, "b": 2, 3: "three" };
eq(o.a, 1, "property");
eq(o["b"], 2, "subscript");
eq(o[3], "three", "numeric key");
o.c = 5;
eq(o.c, 5, "assignment adds");
eq(Object.keys(o).length, 4, "Object.keys");
eq(Object.values({ x: 7 })[0], 7, "Object.values");
eq(o.hasOwnProperty("a"), true, "hasOwnProperty");
eq(("a" in o), true, "in");
eq(o.missing, undefined, "a missing property is undefined");
var merged = Object.assign({}, { p: 1 }, { q: 2 });
eq(merged.p + merged.q, 3, "Object.assign");
var shorthandX = 4;
var sh = { shorthandX };
eq(sh.shorthandX, 4, "shorthand property");
var nested = { inner: { deep: [1, { x: 2 }] } };
eq(nested.inner.deep[1].x, 2, "nesting");
eq(nested.missing?.deep, undefined, "optional chaining");

// --- functions and closures --------------------------------------------------------
function add(x, y) { return x + y; }
eq(add(2, 3), 5, "a function");
eq(isNaN(add(2)), true, "a missing argument makes the sum NaN");
var addArrow = (x, y) => x + y;
eq(addArrow(2, 3), 5, "an arrow function");
var square = x => x * x;
eq(square(4), 16, "a one-argument arrow");
function counter() {
    var n = 0;
    return function () { n++; return n; };
}
var c1 = counter(), c2 = counter();
c1(); c1();
eq(c1(), 3, "a closure keeps its own count");
eq(c2(), 1, "and another one is separate");
function withDefault(x, y) { if (y === undefined) y = 10; return x + y; }
eq(withDefault(1), 11, "a default");
function defParam(x, y = 7) { return x + y; }
eq(defParam(1), 8, "a written default");
eq((function () { return arguments.length; })(1, 2, 3), 3, "arguments");
function outer() { return inner(); function inner() { return "hoisted"; } }
eq(outer(), "hoisted", "function declarations are hoisted");
eq([1, 2, 3].map(x => x + 1).join(""), "234", "an arrow as a callback");
var applied = add.apply(null, [4, 5]);
eq(applied, 9, "apply");
eq(add.call(null, 6, 7), 13, "call");

// --- this, new, prototypes -----------------------------------------------------------
var obj = { v: 42, get: function () { return this.v; } };
eq(obj.get(), 42, "this in a method");
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.sum = function () { return this.x + this.y; };
var pt = new Point(3, 4);
eq(pt.x, 3, "a constructor sets a field");
eq(pt.sum(), 7, "a prototype method");
var arrowThis = { v: 1, go: function () { var f = () => this.v; return f(); } };
eq(arrowThis.go(), 1, "an arrow keeps the enclosing this");

// --- control flow ------------------------------------------------------------------
var acc = 0;
for (var i = 0; i < 5; i++) acc += i;
eq(acc, 10, "for");
acc = 0;
for (var k of [1, 2, 3]) acc += k;
eq(acc, 6, "for-of");
var names = "";
for (var key in { a: 1, b: 2 }) names += key;
eq(names, "ab", "for-in");
acc = 0;
var w = 0;
while (w < 4) { w++; if (w === 2) continue; acc += w; }
eq(acc, 8, "while with continue");
acc = 0;
do { acc++; } while (acc < 3);
eq(acc, 3, "do-while");
acc = 0;
for (var q = 0; q < 10; q++) { if (q === 3) break; acc++; }
eq(acc, 3, "break");
function sw(x) {
    switch (x) {
    case 1: return "one";
    case 2:
    case 3: return "few";
    default: return "many";
    }
}
eq(sw(1), "one", "switch");
eq(sw(3), "few", "switch fallthrough");
eq(sw(9), "many", "switch default");
eq((1 > 0) ? "y" : "n", "y", "the conditional operator");

// --- errors ----------------------------------------------------------------------
var caught = "";
try { throw new Error("boom"); }
catch (e) { caught = e.message; }
eq(caught, "boom", "throw and catch");
var order = "";
try { order += "a"; throw "x"; }
catch (e) { order += "b"; }
finally { order += "c"; }
eq(order, "abc", "finally runs");
var thrownString = "";
try { null.x; } catch (e) { thrownString = "caught"; }
eq(thrownString, "caught", "reading a property of null throws");

// --- Math and JSON -----------------------------------------------------------------
eq(Math.max(1, 5, 3), 5, "Math.max");
eq(Math.min(1, 5, 3), 1, "Math.min");
eq(Math.abs(-3), 3, "Math.abs");
eq(Math.floor(2.7), 2, "Math.floor");
eq(Math.ceil(2.1), 3, "Math.ceil");
eq(Math.round(2.5), 3, "Math.round rounds halves up");
eq(Math.round(-2.5), -2, "and -2.5 goes to -2");
eq(Math.sqrt(16), 4, "Math.sqrt");
eq(Math.pow(2, 8), 256, "Math.pow");
eq(Math.trunc(-2.7), -2, "Math.trunc");
eq(Math.sign(-5), -1, "Math.sign");
var r = Math.random();
eq(r >= 0 && r < 1, true, "Math.random is in range");
eq(JSON.stringify({ a: 1, b: "x" }), '{"a":1,"b":"x"}', "JSON.stringify");
eq(JSON.stringify([1, "two", null]), '[1,"two",null]', "JSON.stringify an array");
eq(JSON.parse('{"n":5}').n, 5, "JSON.parse");
eq(JSON.parse('[1,2,3]').length, 3, "JSON.parse an array");
eq(JSON.parse('{"s":"a\\nb"}').s.length, 3, "JSON.parse an escape");

// --- operators in bulk ---------------------------------------------------------------
var n = 5;
n += 3; eq(n, 8, "+=");
n -= 2; eq(n, 6, "-=");
n *= 2; eq(n, 12, "*=");
n /= 4; eq(n, 3, "/=");
n %= 2; eq(n, 1, "%=");
var pre = 1;
eq(++pre, 2, "prefix increment returns the new value");
eq(pre++, 2, "postfix returns the old one");
eq(pre, 3, "and then increments");
eq(6 & 3, 2, "bitwise and");
eq(6 | 3, 7, "bitwise or");
eq(6 ^ 3, 5, "bitwise xor");
eq(1 << 4, 16, "shift left");
eq(16 >> 2, 4, "shift right");
eq(~5, -6, "bitwise not");
var nn = null;
nn ??= "set"; eq(nn, "set", "??=");
var tt = 0;
tt ||= 9; eq(tt, 9, "||=");
var uu = 1;
uu &&= 5; eq(uu, 5, "&&=");

// --- a real-ish program ----------------------------------------------------------------
function wordFrequencies(text) {
    var words = text.toLowerCase().split(" ");
    var counts = {};
    words.forEach(function (w) {
        w = w.replace(".", "").replace(",", "");
        if (!w) return;
        counts[w] = (counts[w] || 0) + 1;
    });
    return counts;
}
var freq = wordFrequencies("the cat and the hat and the bat");
eq(freq["the"], 3, "counting words");
eq(freq["and"], 2, "counting words again");
eq(Object.keys(freq).length, 5, "distinct words");

function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
eq(fib(20), 6765, "recursion");

var sorted = [5, 3, 9, 1].sort((x, y) => x - y).map(String).join("<");
eq(sorted, "1<3<5<9", "a chain of methods");

console.log(fails === 0
    ? ("all " + total + " checks pass")
    : (fails + " of " + total + " checks FAILED"));
